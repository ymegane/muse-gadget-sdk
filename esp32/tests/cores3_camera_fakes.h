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

/* The bits of ESP-IDF, the CoreS3 BSP, esp_video, V4L2 and esp_new_jpeg that
 * boards/cores3_camera.c uses, for test_cores3_camera.py. Every header it
 * includes is this one; the calls go to cores3_camera_harness.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

typedef int esp_err_t;
#define ESP_OK                   0
#define ESP_FAIL                 -1
#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
const char *esp_err_to_name(esp_err_t err);

#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t err_rc_ = (x); \
        if (err_rc_ != ESP_OK) return err_rc_; } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) return (err); } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, ...) do { esp_err_t err_rc_ = (x); \
        if (err_rc_ != ESP_OK) { ret = err_rc_; goto goto_tag; } } while (0)
#define ESP_GOTO_ON_FALSE(a, err, goto_tag, tag, ...) do { if (!(a)) { ret = (err); goto goto_tag; } } while (0)

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT   2
#define heap_caps_malloc(size, caps) malloc(size)

int64_t esp_timer_get_time(void);

/* The CoreS3 BSP and esp_video */
#define BSP_CAMERA_DEVICE "/dev/video2"
typedef struct { int unused; } bsp_camera_cfg_t;
esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg);   /* rails on, esp_video_init */
esp_err_t esp_video_deinit(void);

/* V4L2 */
enum {
    VIDIOC_G_FMT = 1, VIDIOC_S_FMT, VIDIOC_REQBUFS, VIDIOC_QUERYBUF, VIDIOC_QBUF,
    VIDIOC_DQBUF, VIDIOC_STREAMON, VIDIOC_STREAMOFF, VIDIOC_S_DQBUF_TIMEOUT,
};
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP            1
#define V4L2_PIX_FMT_RGB565         0x50424752
#define V4L2_PIX_FMT_RGB565X        0x52424752
#define V4L2_PIX_FMT_YUYV           0x56595559
#define V4L2_PIX_FMT_GREY           0x59455247
#define V4L2_BUF_FLAG_ERROR         0x40
struct v4l2_pix_format { uint32_t width, height, pixelformat; };
struct v4l2_format { uint32_t type; union { struct v4l2_pix_format pix; } fmt; };
struct v4l2_requestbuffers { uint32_t count, type, memory; };
struct v4l2_buffer {
    uint32_t index, type, bytesused, flags, memory;
    union { uint32_t offset; } m;
    uint32_t length;
};

/* POSIX calls the driver makes on the video device. */
#define O_RDONLY   0
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1
int fake_open(const char *path, int flags);
int fake_close(int fd);
int fake_ioctl(int fd, unsigned long request, void *arg);
void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset);
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mmap fake_mmap

/* esp_new_jpeg */
typedef enum {
    JPEG_PIXEL_FORMAT_YCbYCr = 1, JPEG_PIXEL_FORMAT_RGB565_BE, JPEG_PIXEL_FORMAT_RGB565_LE,
} jpeg_pixel_format_t;
typedef enum { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1, JPEG_ERR_NO_MEM = -2 } jpeg_error_t;
typedef enum { JPEG_SUBSAMPLE_420 = 3 } jpeg_subsampling_t;
typedef void *jpeg_enc_handle_t;
typedef struct {
    int width, height;
    jpeg_pixel_format_t src_type;
    jpeg_subsampling_t subsampling;
    uint8_t quality;
} jpeg_enc_config_t;
#define DEFAULT_JPEG_ENC_CONFIG() { .width = 320, .height = 240, .quality = 40 }
jpeg_error_t jpeg_enc_open(jpeg_enc_config_t *info, jpeg_enc_handle_t *jpeg_enc);
jpeg_error_t jpeg_enc_process(const jpeg_enc_handle_t jpeg_enc, const uint8_t *in_buf, int inbuf_size,
                              uint8_t *out_buf, int outbuf_size, int *out_size);
jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t jpeg_enc);

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
