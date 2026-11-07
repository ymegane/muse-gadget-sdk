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

/* boards/cores3_camera.c for test_cores3_camera.py, against a scripted
 * esp_video DVP device and esp_new_jpeg: every capture, failed or not, leaves
 * esp_video torn down, the encoder gets the sensor's own pixel format, and
 * the quality steps down until the JPEG fits. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cores3_camera_fakes.h"
#include "cores3_camera.h"

#define W 640
#define H 480
#define FD 7

static bool sensor, frames = true, video_live;
static uint32_t pixelformat = V4L2_PIX_FMT_YUYV;
static int video_inits, video_deinits, opens, closes, encoders_live, encodes;
static uint32_t jpeg_len_at_quality_ten;   /* scripted size: this x quality/10 */
static uint32_t jpeg_len_fixed;            /* or this, whatever the quality */
static int last_quality;
static jpeg_pixel_format_t last_src;
static int64_t now_us;
static uint8_t buffers[2][W * H * 2];

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg)
{
    assert(cfg == NULL);
    assert(!video_live);   /* never set up over a live one */
    video_live = true;
    video_inits++;
    return ESP_OK;
}

esp_err_t esp_video_deinit(void)
{
    assert(video_live);
    video_live = false;
    video_deinits++;
    return ESP_OK;
}

int fake_open(const char *path, int flags)
{
    assert(video_live && strcmp(path, "/dev/video2") == 0 && flags == O_RDONLY);
    if (!sensor) return -1;   /* esp_video_init found no sensor */
    opens++;
    return FD;
}

int fake_close(int fd)
{
    assert(fd == FD);
    closes++;
    return 0;
}

int fake_ioctl(int fd, unsigned long request, void *arg)
{
    assert(fd == FD && video_live);
    struct v4l2_buffer *b = arg;
    switch (request) {
    case VIDIOC_S_FMT:
        assert(!"DVP takes the sensor's default format");
        return -1;
    case VIDIOC_G_FMT: {
        struct v4l2_format *f = arg;
        f->fmt.pix.width = W;
        f->fmt.pix.height = H;
        f->fmt.pix.pixelformat = pixelformat;
        return 0;
    }
    case VIDIOC_QUERYBUF:
        assert(b->index < 2);
        b->length = sizeof(buffers[0]);
        b->m.offset = b->index;
        return 0;
    case VIDIOC_DQBUF:
        if (!frames) return -1;   /* the DQBUF timeout */
        now_us += 62500;          /* 16 fps */
        b->index = (uint32_t)(now_us / 62500) % 2;
        b->bytesused = sizeof(buffers[0]);
        b->flags = 0;
        return 0;
    default:
        return 0;
    }
}

void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset)
{
    (void)addr;
    (void)prot;
    (void)flags;
    assert(fd == FD && len == sizeof(buffers[0]) && offset >= 0 && offset < 2);
    return buffers[offset];
}

jpeg_error_t jpeg_enc_open(jpeg_enc_config_t *info, jpeg_enc_handle_t *jpeg_enc)
{
    assert(info->width == W && info->height == H && info->subsampling == JPEG_SUBSAMPLE_420);
    last_quality = info->quality;
    last_src = info->src_type;
    encoders_live++;
    *jpeg_enc = &encoders_live;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_enc_process(const jpeg_enc_handle_t jpeg_enc, const uint8_t *in_buf, int inbuf_size,
                              uint8_t *out_buf, int outbuf_size, int *out_size)
{
    assert(jpeg_enc && in_buf && inbuf_size == W * H * 2 && out_buf);
    encodes++;
    uint32_t len = jpeg_len_fixed ? jpeg_len_fixed : jpeg_len_at_quality_ten * (uint32_t)last_quality / 10;
    if (len > (uint32_t)outbuf_size) return JPEG_ERR_NO_MEM;   /* out_buf too small */
    *out_size = (int)len;
    out_buf[0] = 0xFF;
    out_buf[1] = 0xD8;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t jpeg_enc)
{
    assert(jpeg_enc);
    encoders_live--;
    return JPEG_ERR_OK;
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (slen + 2) / 3 * 4;
    *olen = need;
    if (dlen < need + 1) return -1;
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16 | (i + 1 < slen ? src[i + 1] << 8 : 0)
                     | (i + 2 < slen ? src[i + 2] : 0);
        dst[o++] = abc[v >> 18 & 63];
        dst[o++] = abc[v >> 12 & 63];
        dst[o++] = i + 1 < slen ? abc[v >> 6 & 63] : '=';
        dst[o++] = i + 2 < slen ? abc[v & 63] : '=';
    }
    dst[o] = 0;
    return 0;
}

/* One capture; checks that it left nothing of esp_video behind. */
static bool capture(char **b64, const char **error)
{
    *error = NULL;
    bool ok = cores3_camera_capture(b64, error);
    assert(!video_live && video_inits == video_deinits);
    assert(opens == closes && encoders_live == 0);
    assert(ok == (*b64 != NULL) && ok == (*error == NULL));
    return ok;
}

int main(void)
{
    char *b64 = NULL;
    const char *error = NULL;
    cores3_camera_register();

    /* No sensor answering: esp_video still comes up, and has to go. */
    sensor = false;
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    assert(video_inits == 2);

    /* YUYV that fits at quality 60: 80 is over 150 KB. */
    sensor = true;
    jpeg_len_at_quality_ten = 20000;
    int64_t start = now_us;
    assert(capture(&b64, &error));
    assert(last_quality == 60 && last_src == JPEG_PIXEL_FORMAT_YCbYCr && now_us - start >= 1500000);
    assert(strncmp(b64, "/9", 2) == 0 && strlen(b64) == (120000 + 2) / 3 * 4);
    free(b64);

    /* Big-endian RGB565 goes to the encoder as such. */
    pixelformat = V4L2_PIX_FMT_RGB565X;
    jpeg_len_at_quality_ten = 1000;
    assert(capture(&b64, &error));
    assert(last_quality == 80 && last_src == JPEG_PIXEL_FORMAT_RGB565_BE);
    free(b64);

    /* A format the encoder can't take is refused before streaming. */
    pixelformat = V4L2_PIX_FMT_GREY;
    int before = encodes;
    assert(!capture(&b64, &error) && strcmp(error, "camera pixel format not supported") == 0);
    assert(encodes == before);
    pixelformat = V4L2_PIX_FMT_YUYV;

    /* Over 150 KB even at quality 20. */
    jpeg_len_fixed = 200000;
    assert(!capture(&b64, &error) && strcmp(error, "frame over 150 KB even at low quality") == 0);
    assert(last_quality == 20);

    /* Over the output buffer at every quality: the same answer, no crash. */
    jpeg_len_fixed = W * H;
    before = encodes;
    assert(!capture(&b64, &error) && strcmp(error, "frame over 150 KB even at low quality") == 0);
    assert(encodes - before == 4);
    jpeg_len_fixed = 0;

    /* A sensor that sends nothing. */
    frames = false;
    assert(!capture(&b64, &error) && strcmp(error, "camera sent no frame") == 0);

    assert(video_inits == 8);
    printf("ok\n");
    return 0;
}
