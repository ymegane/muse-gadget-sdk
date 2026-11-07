/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * The M5Stack CoreS3's GC0308 camera, on the ESP32-S3's DVP port through
 * esp_video. The BSP's bsp_camera_start() switches the camera's rails on and
 * brings esp_video up on the board's I2C bus; the sensor's frames (YUYV by
 * default, or RGB565) are compressed in software with esp_new_jpeg, as the
 * S3 has no JPEG encoder. esp_video is up only while a capture runs, as on
 * the P4 3.5 (p4_csi_camera.c). The profile runs the sensor at 320x240: at
 * 640x480 the DVP driver's copy out of its DMA buffer fell behind with Wi-Fi
 * up, and frames came out in shuffled bands.
 */
#include "cores3_camera.h"
#include "camera.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "mbedtls/base64.h"

#define BUFFERS       2
#define SETTLE_US     1500000  /* auto exposure and white balance catch up */
#define FRAME_WAIT_S  2        /* a stream that stops sending frames */
#define GRAB_LIMIT_US (SETTLE_US + 3000000)   /* one that sends only broken ones */
/* The JPEG has to fit in this: a 380 KB one, about 510 KB as base64, made
 * Muse time out (p4_csi_camera.c). A camera frame here is far under it. */
#define JPEG_MAX_BYTES (150 * 1024)
static const int QUALITIES[] = { 80, 60, 40, 20 };

static const char *TAG = "cores3.camera";
static int s_fd = -1;
static uint8_t *s_buf[BUFFERS];
static int s_w, s_h;
static jpeg_pixel_format_t s_src;
/* A stream that would not stop may still write to its buffers: keep them. */
static bool s_stuck;

/* The encoder's input format for a V4L2 pixel format, or 0. */
static jpeg_pixel_format_t jpeg_source(uint32_t pixelformat)
{
    switch (pixelformat) {
    case V4L2_PIX_FMT_YUYV:    return JPEG_PIXEL_FORMAT_YCbYCr;
    case V4L2_PIX_FMT_RGB565X: return JPEG_PIXEL_FORMAT_RGB565_BE;
    case V4L2_PIX_FMT_RGB565:  return JPEG_PIXEL_FORMAT_RGB565_LE;
    default:                   return 0;
    }
}

static void close_camera(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_video deinit: %s", esp_err_to_name(err));
    }
}

static esp_err_t open_camera(void)
{
    /* Camera rails on, then esp_video on the BSP's I2C bus. */
    ESP_RETURN_ON_ERROR(bsp_camera_start(NULL), TAG, "camera start");
    esp_err_t ret = ESP_FAIL;
    /* Without a sensor esp_video_init succeeds but registers no device. */
    s_fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    ESP_GOTO_ON_FALSE(s_fd >= 0, ESP_ERR_NOT_FOUND, fail, TAG, "no camera sensor");
    struct timeval wait = { .tv_sec = FRAME_WAIT_S };
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &wait) == 0, ESP_FAIL, fail, TAG, "DQBUF timeout");
    /* The sensor's default format, from sdkconfig: DVP can't convert. */
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, fail, TAG, "G_FMT");
    s_src = jpeg_source(fmt.fmt.pix.pixelformat);
    ESP_GOTO_ON_FALSE(s_src, ESP_ERR_NOT_SUPPORTED, fail, TAG, "pixel format 0x%08x",
                      (unsigned)fmt.fmt.pix.pixelformat);
    s_w = fmt.fmt.pix.width;
    s_h = fmt.fmt.pix.height;
    struct v4l2_requestbuffers req = {
        .count = BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, fail, TAG, "REQBUFS");
    for (int i = 0; i < BUFFERS; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &b) == 0, ESP_FAIL, fail, TAG, "QUERYBUF");
        /* esp_video's MAP_FAILED is NULL */
        s_buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        ESP_GOTO_ON_FALSE(s_buf[i], ESP_ERR_NO_MEM, fail, TAG, "mmap");
    }
    ESP_LOGI(TAG, "%dx%d, pixel format 0x%08x", s_w, s_h, (unsigned)fmt.fmt.pix.pixelformat);
    return ESP_OK;
fail:
    close_camera();
    return ret;
}

static esp_err_t init(void)
{
    return ESP_OK;
}

/* One JPEG of the frame at the first of QUALITIES that fits the cap. */
static esp_err_t encode(const uint8_t *frame, uint32_t frame_len, uint8_t **jpeg, uint32_t *len)
{
    /* A JPEG of a camera frame is far smaller than the frame. */
    int cap = (int)frame_len / 2;
    *jpeg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*jpeg) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < sizeof(QUALITIES) / sizeof(QUALITIES[0]); i++) {
        jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
        cfg.width = s_w;
        cfg.height = s_h;
        cfg.src_type = s_src;
        cfg.subsampling = JPEG_SUBSAMPLE_420;
        cfg.quality = QUALITIES[i];
        jpeg_enc_handle_t enc = NULL;
        if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
            return ESP_ERR_NO_MEM;
        }
        int out = 0;
        jpeg_error_t err = jpeg_enc_process(enc, frame, (int)frame_len, *jpeg, cap, &out);
        jpeg_enc_close(enc);
        if (err == JPEG_ERR_OK) {
            ESP_LOGI(TAG, "JPEG quality %d: %d bytes", QUALITIES[i], out);
            if (out <= JPEG_MAX_BYTES) {
                *len = (uint32_t)out;
                return ESP_OK;
            }
        } else if (err != JPEG_ERR_NO_MEM) {
            /* NO_MEM is an output buffer too small: try a lower quality. */
            return ESP_FAIL;
        }
    }
    return ESP_ERR_INVALID_SIZE;
}

/* The frame after SETTLE_US, encoded. */
static esp_err_t grab(struct v4l2_buffer *b, uint8_t **jpeg, uint32_t *len)
{
    int64_t start = esp_timer_get_time(), until = start + SETTLE_US;
    for (int frames = 0;; frames++) {
        if (ioctl(s_fd, VIDIOC_DQBUF, b) != 0 || esp_timer_get_time() - start > GRAB_LIMIT_US) {
            return ESP_ERR_TIMEOUT;
        }
        bool whole = !(b->flags & V4L2_BUF_FLAG_ERROR) && b->bytesused;
        if (whole && esp_timer_get_time() >= until) {
            ESP_LOGI(TAG, "kept frame %d", frames);
            break;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, b) != 0) {
            return ESP_FAIL;
        }
    }
    return encode(s_buf[b->index], b->bytesused, jpeg, len);
}

static esp_err_t capture(camera_frame_t *out)
{
    if (s_stuck) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_RETURN_ON_ERROR(open_camera(), TAG, "open");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < BUFFERS && err == ESP_OK; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        err = ioctl(s_fd, VIDIOC_QBUF, &b) == 0 ? ESP_OK : ESP_FAIL;
    }
    bool streaming = err == ESP_OK && ioctl(s_fd, VIDIOC_STREAMON, &type) == 0;
    uint8_t *jpeg = NULL;
    uint32_t len = 0;
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
    if (!streaming) {
        err = ESP_FAIL;
    } else {
        err = grab(&b, &jpeg, &len);
        if (ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0 && ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0) {
            s_stuck = true;
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (!s_stuck) {
        close_camera();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
        free(jpeg);
        return err;
    }
    *out = (camera_frame_t){ .jpeg = jpeg, .len = len, .width = s_w, .height = s_h, .priv = jpeg };
    return ESP_OK;
}

static void release(camera_frame_t *frame)
{
    free(frame->priv);
}

static const camera_driver_t s_driver = {
    .name = "GC0308 (M5Stack CoreS3)",
    .max_width = 320,
    .max_height = 240,
    .init = init,
    .capture = capture,
    .release = release,
};

void cores3_camera_register(void)
{
    camera_register(&s_driver);
}

bool cores3_camera_capture(char **jpeg_base64, const char **error)
{
    *jpeg_base64 = NULL;
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        *error = err == ESP_ERR_NO_MEM ? "camera memory allocation failed"
               : err == ESP_ERR_INVALID_STATE ? "camera busy"
               : err == ESP_ERR_NOT_FOUND ? "no camera"
               : err == ESP_ERR_NOT_SUPPORTED ? "camera pixel format not supported"
               : err == ESP_ERR_TIMEOUT ? "camera sent no frame"
               : err == ESP_ERR_INVALID_SIZE ? "frame over 150 KB even at low quality"
               : err == ESP_ERR_INVALID_RESPONSE ? "camera stream stuck: restart the device"
                                             : "camera capture failed";
        return false;
    }
    size_t cap = (frame.len + 2) / 3 * 4 + 1, len = 0;
    *jpeg_base64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (*jpeg_base64 && mbedtls_base64_encode((unsigned char *)*jpeg_base64, cap, &len,
                                              frame.jpeg, frame.len) != 0) {
        free(*jpeg_base64);
        *jpeg_base64 = NULL;
    }
    camera_release(&frame);
    *error = *jpeg_base64 ? NULL : "camera memory allocation failed";
    return *jpeg_base64 != NULL;
}
