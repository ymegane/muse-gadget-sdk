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

// Host LTR-553 harness: raw sensor bytes in, sensors.read result out.
// The runner extracts the production protocol code into ltr553_protocol.inc.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#include "ltr553_protocol.inc"

#define S(x) (x * 1000000LL)

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void check_gains(void) {
    assert(GAIN_COUNT == 6);
    assert(gain_index_of_code(0) == 0 && GAINS[0].gain == 1);
    assert(gain_index_of_code(6) == 4 && GAINS[4].gain == 48);
    assert(gain_index_of_code(7) == 5 && GAINS[5].gain == 96);
    // Codes 4 and 5 are reserved.
    assert(gain_index_of_code(4) == -1);
    assert(gain_index_of_code(5) == -1);
    // ALS_CONTR: gain code in [4:2], active mode in [0].
    assert(als_contr(0) == 0x01);
    assert(als_contr(2) == 0x09);
    assert(als_contr(5) == 0x1D);
}

static void check_lux(void) {
    // Each branch of the datasheet formula, at 1x and 100 ms.
    assert(near(ltr553_lux(1000, 200, 1, 100), 1995.48f));   // ratio 0.17
    assert(near(ltr553_lux(1000, 1000, 1, 100), 2323.70f));  // ratio 0.50
    assert(near(ltr553_lux(300, 700, 1, 100), 260.73f));     // ratio 0.70
    assert(ltr553_lux(100, 900, 1, 100) == 0.0f);            // ratio 0.90
    // Gain and integration time divide it.
    assert(near(ltr553_lux(1000, 200, 4, 100), 498.87f));
    assert(near(ltr553_lux(1000, 200, 1, 200), 997.74f));
    // Darkness and nonsense inputs read 0, never negative.
    assert(ltr553_lux(0, 0, 1, 100) == 0.0f);
    assert(ltr553_lux(1000, 200, 0, 100) == 0.0f);
}

static void check_decode(void) {
    uint16_t ch0, ch1;
    int gain_index;
    // CH1 LSB/MSB, CH0 LSB/MSB, then the status with gain code 2 (4x).
    uint8_t raw[5] = {0x34, 0x12, 0x78, 0x56, 2 << 4};
    assert(ltr553_decode(raw, &ch0, &ch1, &gain_index));
    assert(ch1 == 0x1234 && ch0 == 0x5678 && gain_index == 2);
    // The new-data bit doesn't matter; the invalid bit does.
    raw[4] = (2 << 4) | 0x04;
    assert(ltr553_decode(raw, &ch0, &ch1, &gain_index));
    raw[4] = (2 << 4) | LTR553_STATUS_INVALID;
    assert(!ltr553_decode(raw, &ch0, &ch1, &gain_index));
    // A reserved gain code is rejected.
    raw[4] = 4 << 4;
    assert(!ltr553_decode(raw, &ch0, &ch1, &gain_index));
}

static void check_gain_steps(void) {
    // In range: stay.
    assert(next_gain_index(2, 1000, 200) == 2);
    // Near saturation on either channel: step down, but not below 1x.
    assert(next_gain_index(2, 45000, 100) == 1);
    assert(next_gain_index(2, 100, 45000) == 1);
    assert(next_gain_index(0, 65535, 65535) == 0);
    // Dim: step up, but not above 96x.
    assert(next_gain_index(2, 100, 20) == 3);
    assert(next_gain_index(5, 10, 2) == 5);
    // One step up from a low reading never lands above the high mark.
    for (int i = 0; i < GAIN_COUNT - 1; i++) {
        uint32_t scaled = (uint32_t)(GAIN_LOW_COUNTS - 1) * GAINS[i + 1].gain / GAINS[i].gain;
        assert(scaled <= GAIN_HIGH_COUNTS);
    }
}

static void check_result(void) {
    reading_t light = {0};

    // Nothing yet: an error, not a null payload.
    cJSON *result = readings_result(&light, S(10));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON *error = cJSON_GetObjectItem(result, "error");
    assert(!strcmp(cJSON_GetObjectItem(error, "code")->valuestring, "no_readings"));
    assert(!cJSON_GetObjectItem(result, "payload"));
    cJSON_Delete(result);

    // A fresh reading rounds to one decimal and carries its age.
    light = (reading_t){true, 498.87f, S(7)};
    result = readings_result(&light, S(10));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    char *json = cJSON_PrintUnformatted(cJSON_GetObjectItem(result, "payload"));
    assert(!strcmp(json, "{\"light\":{\"value\":498.9,\"unit\":\"lux\",\"age_s\":3}}"));
    cJSON_free(json);
    cJSON_Delete(result);

    // A stale reading is an error again.
    light.at_us = S(10) - STALE_US - 1;
    result = readings_result(&light, S(10));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON_Delete(result);
}

static void check_auto_brightness(void) {
    // Log lux between the marks: dark is 0, bright is 1, their geometric
    // mean (20 lux) is halfway.
    assert(ambient_level(0.0f) == 0.0f);
    assert(ambient_level(AMBIENT_DARK_LUX) == 0.0f);
    assert(ambient_level(NAN) == 0.0f);
    assert(near(ambient_level(20.0f), 0.5f));
    assert(ambient_level(AMBIENT_BRIGHT_LUX) == 1.0f);
    assert(ambient_level(60000.0f) == 1.0f);
    // A small move keeps the level; a big one or an end replaces it.
    assert(ambient_settle(0.5f, 0.55f) == 0.5f);
    assert(ambient_settle(0.5f, 0.65f) == 0.65f);
    assert(ambient_settle(0.95f, 1.0f) == 1.0f);
    assert(ambient_settle(0.05f, 0.0f) == 0.0f);
    // The setting in a bright room, the minimum in the dark, between in between.
    assert(ambient_brightness(100, 1.0f) == 100);
    assert(ambient_brightness(100, 0.0f) == AMBIENT_MIN_PCT);
    assert(ambient_brightness(100, 0.5f) == 55);
    assert(ambient_brightness(50, 0.5f) == 30);
    assert(ambient_brightness(AMBIENT_MIN_PCT, 1.0f) == AMBIENT_MIN_PCT);
    assert(ambient_brightness(100, 2.0f) == 100);
    assert(ambient_brightness(100, -1.0f) == AMBIENT_MIN_PCT);
}

int main(void) {
    check_gains(); check_lux(); check_decode(); check_gain_steps(); check_result(); check_auto_brightness();
    puts("PASS m5stack cores3 ltr553: gains, lux formula, decode, gain steps, result envelope, stale handling, auto brightness");
    return 0;
}
