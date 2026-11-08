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

#include "m5stack_cores3_ltr553.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stack_monitor.h"
#include "sdkconfig.h"
#if CONFIG_HOMEHUB_M5STACK_CORES3_AUTO_BRIGHTNESS
#include "muse_ui.h"
#endif

// ---- Protocol (host-tested) -------------------------------------------------

// Lite-On LTR-553ALS-WA. Its ambient light half measures two photodiodes:
// CH0 (visible and infrared) and CH1 (infrared). The datasheet's lux formula
// weighs them by their ratio. Reading ALS_DATA_CH1_0 latches all four data
// bytes, so read 0x88-0x8B (and the status at 0x8C) in one burst.
#define LTR553_ADDR             0x23
#define LTR553_REG_ALS_CONTR    0x80   // gain [4:2], SW reset [1], active [0]
#define LTR553_REG_ALS_MEAS     0x85   // integration time [5:3], repeat rate [2:0]
#define LTR553_REG_PART_ID      0x86   // part number [7:4] = 0x9, revision [3:0]
#define LTR553_REG_MANUFAC_ID   0x87   // 0x05
#define LTR553_REG_ALS_DATA     0x88   // CH1 LSB, CH1 MSB, CH0 LSB, CH0 MSB
#define LTR553_PART_NUMBER      0x9
#define LTR553_MANUFACTURER     0x05

// 100 ms integration (code 0), a new result every 500 ms (code 3). The lux
// formula divides by the integration time in units of 100 ms.
#define LTR553_INT_MS           100
#define LTR553_MEAS_RATE        ((0 << 3) | 3)

#define LTR553_STATUS_INVALID   0x80   // ALS_PS_STATUS[7]: data not valid
#define LTR553_STATUS_GAIN(s)   (((s) >> 4) & 0x07)

// The gains the sensor supports, by their ALS_CONTR / ALS_PS_STATUS code,
// lowest first. 1x covers up to 64k lux, 96x resolves about 0.01 lux.
static const struct {
    uint8_t code;
    uint8_t gain;
} GAINS[] = {{0, 1}, {1, 2}, {2, 4}, {3, 8}, {6, 48}, {7, 96}};
#define GAIN_COUNT ((int)(sizeof(GAINS) / sizeof(GAINS[0])))

// Step the gain down above this count on either channel and up below the
// low mark, so a reading stays clear of saturation (65535) and of the
// noise floor. The marks are far enough apart that one step never crosses
// both: the largest step, 8x to 48x, multiplies the counts by 6.
#define GAIN_HIGH_COUNTS        40000
#define GAIN_LOW_COUNTS         500

static uint8_t als_contr(int gain_index) {
    return (uint8_t)((GAINS[gain_index].code << 2) | 0x01);
}

// The index in GAINS of a status gain code, or -1 for a reserved code.
static int gain_index_of_code(uint8_t code) {
    for (int i = 0; i < GAIN_COUNT; i++) {
        if (GAINS[i].code == code) return i;
    }
    return -1;
}

// The gain to measure with next, given a reading taken at gain_index.
static int next_gain_index(int gain_index, uint16_t ch0, uint16_t ch1) {
    uint16_t peak = ch0 > ch1 ? ch0 : ch1;
    if (peak > GAIN_HIGH_COUNTS && gain_index > 0) return gain_index - 1;
    if (peak < GAIN_LOW_COUNTS && gain_index < GAIN_COUNT - 1) return gain_index + 1;
    return gain_index;
}

// Illuminance from the two channels, per the LTR-553ALS datasheet. With
// mostly infrared light (ratio of 0.85 or more) the formula gives 0.
static float ltr553_lux(uint16_t ch0, uint16_t ch1, int gain, int int_ms) {
    uint32_t sum = (uint32_t)ch0 + ch1;
    if (sum == 0 || gain <= 0 || int_ms <= 0) return 0.0f;
    float ratio = (float)ch1 / (float)sum;
    float lux;
    if (ratio < 0.45f) {
        lux = 1.7743f * ch0 + 1.1059f * ch1;
    } else if (ratio < 0.64f) {
        lux = 4.2785f * ch0 - 1.9548f * ch1;
    } else if (ratio < 0.85f) {
        lux = 0.5926f * ch0 + 0.1185f * ch1;
    } else {
        lux = 0.0f;
    }
    lux /= (float)gain * ((float)int_ms / 100.0f);
    return lux > 0.0f ? lux : 0.0f;
}

// Decode the 5-byte burst at ALS_DATA (CH1 LSB/MSB, CH0 LSB/MSB, status).
// Returns false when the sensor marks the data invalid or reports a
// reserved gain; *gain_index is the gain the data was measured with.
static bool ltr553_decode(const uint8_t raw[5], uint16_t *ch0, uint16_t *ch1, int *gain_index) {
    uint8_t status = raw[4];
    if (status & LTR553_STATUS_INVALID) return false;
    int index = gain_index_of_code(LTR553_STATUS_GAIN(status));
    if (index < 0) return false;
    *ch1 = (uint16_t)(raw[0] | (raw[1] << 8));
    *ch0 = (uint16_t)(raw[2] | (raw[3] << 8));
    *gain_index = index;
    return true;
}

// Readings older than this count as missing, so a dead sensor drops out.
#define STALE_US (30LL * 1000 * 1000)

typedef struct {
    bool valid;
    float value;
    int64_t at_us;
} reading_t;

// sensors.read result: the light reading with its value, unit and age in
// seconds; with no recent reading, an error.
static cJSON *readings_result(const reading_t *light, int64_t now_us) {
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    if (!light->valid || now_us - light->at_us > STALE_US) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(error, "code", "no_readings");
        cJSON_AddStringToObject(error, "message",
                                "no light sensor reading; the LTR-553 may be missing");
        return result;
    }
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON *sensor = cJSON_AddObjectToObject(payload, "light");
    cJSON_AddNumberToObject(sensor, "value", round(light->value * 10.0) / 10.0);
    cJSON_AddStringToObject(sensor, "unit", "lux");
    cJSON_AddNumberToObject(sensor, "age_s", (double)((now_us - light->at_us) / 1000000));
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload);
    return result;
}

// ---- Auto brightness (host-tested) -------------------------------------------

// The ambient level from 0 (dark room) to 1 (well-lit room), linear in log
// lux between the two marks. The sensor sits behind the front glass and
// reads a room lit for reading at a few tens of lux, so the bright mark is
// low.
#define AMBIENT_DARK_LUX        2.0f
#define AMBIENT_BRIGHT_LUX      200.0f
// A new level replaces the one in use only when it moves this far (or hits
// an end), so a reading hovering on a step doesn't flicker the backlight.
#define AMBIENT_HYSTERESIS      0.1f
// Never dim below the brightness setting's own minimum.
#define AMBIENT_MIN_PCT         10

static inline float ambient_level(float lux) {
    if (!(lux > AMBIENT_DARK_LUX)) return 0.0f;   // also NaN
    if (lux >= AMBIENT_BRIGHT_LUX) return 1.0f;
    return logf(lux / AMBIENT_DARK_LUX) / logf(AMBIENT_BRIGHT_LUX / AMBIENT_DARK_LUX);
}

// The level to use next, given the one in use and a fresh one.
static inline float ambient_settle(float current, float fresh) {
    if (fresh <= 0.0f || fresh >= 1.0f) return fresh;
    return fabsf(fresh - current) >= AMBIENT_HYSTERESIS ? fresh : current;
}

// The backlight for the brightness setting at an ambient level: the setting
// in a bright room, sliding down to the minimum in the dark.
static inline int ambient_brightness(int pct, float level) {
    if (pct <= AMBIENT_MIN_PCT) return pct;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    return AMBIENT_MIN_PCT + (int)lroundf((float)(pct - AMBIENT_MIN_PCT) * level);
}

// ---- I2C driver --------------------------------------------------------------

#if CONFIG_HOMEHUB_M5STACK_CORES3_AUTO_BRIGHTNESS
// Every second, so the backlight follows a light switch promptly.
#define POLL_MS         1000
#else
#define POLL_MS         5000
#endif
#define BUS_WAIT_MS     1000
// A new result every 500 ms: wait for one at the new gain before reading.
#define GAIN_SETTLE_MS  1200

static const char *TAG = "link.ltr553";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static reading_t s_light;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_configured;
static int s_gain_index = 2;   // 4x: indoor light without stepping
static float s_level = -1.0f;  // ambient level in use; -1 before a reading

static esp_err_t write_reg(uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(s_dev, bytes, sizeof(bytes), 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *out, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, 100);
}

// Wait for the BSP's bus (the board's init brings it up), then add the
// sensor on it. The bus is the board's: never create it here.
static void attach(void) {
    i2c_master_bus_handle_t bus = NULL;
    while (i2c_master_get_bus_handle(CONFIG_BSP_I2C_NUM, &bus) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(BUS_WAIT_MS));
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LTR553_ADDR,
        .scl_speed_hz = 400000,
    };
    while (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// Set the measurement rate and switch the ALS on at the current gain.
static bool configure(void) {
    uint8_t id[2];
    if (read_regs(LTR553_REG_PART_ID, id, sizeof(id)) == ESP_OK &&
        ((id[0] >> 4) != LTR553_PART_NUMBER || id[1] != LTR553_MANUFACTURER)) {
        ESP_LOGW(TAG, "unexpected part/manufacturer id 0x%02x/0x%02x", id[0], id[1]);
    }
    if (write_reg(LTR553_REG_ALS_MEAS, LTR553_MEAS_RATE) != ESP_OK) return false;
    if (write_reg(LTR553_REG_ALS_CONTR, als_contr(s_gain_index)) != ESP_OK) return false;
    return true;
}

// One reading. Returns false when the sensor didn't answer or had no valid
// data; *settle is set when the gain changed and the next result is due
// sooner than the next poll.
static bool poll_once(bool *settle) {
    *settle = false;
    if (!s_configured) {
        s_configured = configure();
        if (!s_configured) return false;
        *settle = true;   // the first result is 500 ms away
        return true;
    }
    uint8_t raw[5];
    uint16_t ch0, ch1;
    int gain_index;
    if (read_regs(LTR553_REG_ALS_DATA, raw, sizeof(raw)) != ESP_OK) {
        s_configured = false;   // it may have reset: set it up again
        return false;
    }
    if (!ltr553_decode(raw, &ch0, &ch1, &gain_index)) return false;
    float lux = ltr553_lux(ch0, ch1, GAINS[gain_index].gain, LTR553_INT_MS);
    int64_t now = esp_timer_get_time();
    float was = s_level;
    float level = was < 0.0f ? ambient_level(lux) : ambient_settle(was, ambient_level(lux));
    taskENTER_CRITICAL(&s_lock);
    s_light = (reading_t){true, lux, now};
    s_level = level;
    taskEXIT_CRITICAL(&s_lock);
#if CONFIG_HOMEHUB_M5STACK_CORES3_AUTO_BRIGHTNESS
    if (level != was) {
        ESP_LOGI(TAG, "ambient %.1f lux: backlight level %.2f", lux, level);
    }
#endif
    // The lux above used the gain this result was measured with. Step only
    // from a result at the gain last set: an older one is from before it.
    if (gain_index != s_gain_index) {
        *settle = true;
        return true;
    }
    int next = next_gain_index(gain_index, ch0, ch1);
    if (next != s_gain_index && write_reg(LTR553_REG_ALS_CONTR, als_contr(next)) == ESP_OK) {
        s_gain_index = next;
        *settle = true;
    }
    return true;
}

static void sensors_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    attach();
    ESP_LOGI(TAG, "polling the LTR-553 on I2C%d at 0x%02x", CONFIG_BSP_I2C_NUM, LTR553_ADDR);
    bool logged = false;
    bool failing = false;
    for (;;) {
        bool settle;
        if (poll_once(&settle)) {
            if (!logged && s_light.valid) {
                logged = true;
                ESP_LOGI(TAG, "first reading: %.1f lux (gain %dx)",
                         s_light.value, GAINS[s_gain_index].gain);
            } else if (failing) {
                ESP_LOGI(TAG, "sensor reads recovered");
            }
            failing = false;
        } else if (!failing) {
            // Log once per run of failures, not every poll.
            failing = true;
            ESP_LOGW(TAG, "sensor read failed; will retry");
        }
        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(settle ? GAIN_SETTLE_MS : POLL_MS));
    }
}

#if CONFIG_HOMEHUB_M5STACK_CORES3_AUTO_BRIGHTNESS
// The backlight's ambient level moves at most this much per second, so a
// change in the room fades in over a couple of seconds.
#define FADE_PER_S      0.5f
// A gap longer than this between frames means the screen slept: catch up
// with the room at once instead of fading from before.
#define FADE_GAP_US     (1000LL * 1000)

// The UI's brightness filter: runs in the LVGL task every lit frame.
static int auto_brightness(int pct) {
    static float shown = -1.0f;
    static int64_t last_us;
    float level;
    int64_t at_us;
    taskENTER_CRITICAL(&s_lock);
    level = s_level;
    at_us = s_light.at_us;
    taskEXIT_CRITICAL(&s_lock);
    int64_t now = esp_timer_get_time();
    if (level < 0.0f || now - at_us > STALE_US) {
        shown = -1.0f;   // no sensor: the setting as it is
        return pct;
    }
    if (shown < 0.0f || now - last_us > FADE_GAP_US) {
        shown = level;
    } else {
        float step = FADE_PER_S * (float)(now - last_us) / 1e6f;
        shown = level > shown ? fminf(level, shown + step) : fmaxf(level, shown - step);
    }
    last_us = now;
    return ambient_brightness(pct, shown);
}
#endif

void m5stack_cores3_ltr553_init(void) {
#if CONFIG_HOMEHUB_M5STACK_CORES3_AUTO_BRIGHTNESS
    muse_ui_set_brightness_filter(auto_brightness);
#endif
    if (xTaskCreate(sensors_task, "ltr553", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the light sensor task");
    }
}

cJSON *m5stack_cores3_ltr553_command(void) {
    reading_t snapshot;
    taskENTER_CRITICAL(&s_lock);
    snapshot = s_light;
    taskEXIT_CRITICAL(&s_lock);
    return readings_result(&snapshot, esp_timer_get_time());
}
