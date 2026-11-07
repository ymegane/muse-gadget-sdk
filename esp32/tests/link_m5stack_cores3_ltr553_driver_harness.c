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

// Host LTR-553 driver harness: the whole driver against a fake I2C bus and
// FreeRTOS. The runner supplies empty IDF headers; the fakes are below.
// stack_monitor.h is the real one, from main/.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

static int64_t fake_now_us = 1000000;
static int64_t esp_timer_get_time(void) { return fake_now_us; }

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
typedef void (*TaskFunction_t)(void *);
static int fake_delays;
static void vTaskDelay(int ticks) { (void)ticks; fake_delays++; }
static TaskFunction_t fake_task;
static int xTaskCreate(TaskFunction_t fn, const char *name, int stack, void *arg, int prio, void *handle) {
    (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    fake_task = fn;
    return pdPASS;
}

typedef int i2c_port_num_t;
#define CONFIG_BSP_I2C_NUM 1
#define I2C_ADDR_BIT_LEN_7 0
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
typedef struct fake_bus *i2c_master_bus_handle_t;
typedef struct fake_dev *i2c_master_dev_handle_t;
static struct fake_bus { int unused; } fake_bus;
static struct fake_dev { int unused; } fake_dev;

static int fake_bus_missing;       // calls that find no bus yet
static bool fake_sensor_answers;   // the LTR-553 ACKs
static bool fake_device_added;
static uint8_t fake_writes[16][2];
static int fake_write_count;
static uint8_t fake_raw[5];

static esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port, i2c_master_bus_handle_t *bus) {
    assert(port == CONFIG_BSP_I2C_NUM);
    if (fake_bus_missing > 0) {
        fake_bus_missing--;
        return ESP_ERR_INVALID_STATE;
    }
    *bus = &fake_bus;
    return ESP_OK;
}
static esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                           i2c_master_dev_handle_t *dev) {
    assert(bus == &fake_bus);
    assert(cfg->device_address == 0x23);
    fake_device_added = true;
    *dev = &fake_dev;
    return ESP_OK;
}
static esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len, int timeout) {
    (void)timeout;
    assert(dev == &fake_dev && len == 2);
    if (!fake_sensor_answers) return ESP_FAIL;
    assert(fake_write_count < 16);
    memcpy(fake_writes[fake_write_count++], buf, 2);
    return ESP_OK;
}
static esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *wbuf, size_t wlen,
                                             uint8_t *rbuf, size_t rlen, int timeout) {
    (void)timeout;
    assert(dev == &fake_dev && wlen == 1);
    if (!fake_sensor_answers) return ESP_FAIL;
    if (wbuf[0] == 0x86) {
        assert(rlen == 2);
        rbuf[0] = 0x92;   // part 0x9, revision 2
        rbuf[1] = 0x05;
        return ESP_OK;
    }
    assert(wbuf[0] == 0x88 && rlen == 5);
    memcpy(rbuf, fake_raw, 5);
    return ESP_OK;
}

#include "m5stack_cores3_ltr553.c"

void stack_monitor_poll(stack_monitor_t *state) { (void)state; }

static void set_raw(uint16_t ch0, uint16_t ch1, uint8_t gain_code) {
    fake_raw[0] = (uint8_t)ch1;
    fake_raw[1] = (uint8_t)(ch1 >> 8);
    fake_raw[2] = (uint8_t)ch0;
    fake_raw[3] = (uint8_t)(ch0 >> 8);
    fake_raw[4] = (uint8_t)((gain_code << 4) | 0x04);
}

static bool wrote(int index, uint8_t reg, uint8_t value) {
    return index < fake_write_count && fake_writes[index][0] == reg && fake_writes[index][1] == value;
}

int main(void) {
    bool settle;

    // The task starts at once; attach() waits for the board's bus.
    m5stack_cores3_ltr553_init();
    assert(fake_task == sensors_task);
    fake_bus_missing = 2;
    attach();
    assert(fake_device_added);
    assert(fake_delays == 2);

    // Silent at first: the poll fails and sensors.read has no readings.
    fake_sensor_answers = false;
    assert(!poll_once(&settle));
    cJSON *result = m5stack_cores3_ltr553_command();
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON_Delete(result);

    // Once it answers: 100 ms / 500 ms, then ALS on at 4x.
    fake_sensor_answers = true;
    assert(poll_once(&settle) && settle);
    assert(fake_write_count == 2);
    assert(wrote(0, 0x85, 0x03));
    assert(wrote(1, 0x80, 0x09));

    // An indoor reading at 4x: stays at 4x.
    set_raw(1000, 200, 2);
    assert(poll_once(&settle) && !settle);
    assert(fake_write_count == 2);
    result = m5stack_cores3_ltr553_command();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    char *json = cJSON_PrintUnformatted(cJSON_GetObjectItem(result, "payload"));
    assert(!strcmp(json, "{\"light\":{\"value\":498.9,\"unit\":\"lux\",\"age_s\":0}}"));
    cJSON_free(json);
    cJSON_Delete(result);

    // Bright light near saturation: step down to 2x.
    set_raw(50000, 8000, 2);
    assert(poll_once(&settle) && settle);
    assert(fake_write_count == 3 && wrote(2, 0x80, 0x05));

    // A result still measured at 4x is used with its own gain but
    // doesn't step again.
    assert(poll_once(&settle) && settle);
    assert(fake_write_count == 3);
    result = m5stack_cores3_ltr553_command();
    json = cJSON_PrintUnformatted(cJSON_GetObjectItem(result, "payload"));
    assert(!strcmp(json, "{\"light\":{\"value\":24390.6,\"unit\":\"lux\",\"age_s\":0}}"));
    cJSON_free(json);
    cJSON_Delete(result);

    // The sensor drops off the bus: it is set up again, at the current gain.
    fake_sensor_answers = false;
    assert(!poll_once(&settle));
    fake_sensor_answers = true;
    assert(poll_once(&settle) && settle);
    assert(fake_write_count == 5);
    assert(wrote(3, 0x85, 0x03));
    assert(wrote(4, 0x80, 0x05));

    // The reading ages out after 30 s without a new one.
    fake_now_us += 31LL * 1000 * 1000;
    result = m5stack_cores3_ltr553_command();
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON_Delete(result);

    puts("PASS m5stack cores3 ltr553 driver: waits for the bus, retries, configures, steps gain, re-inits");
    return 0;
}
