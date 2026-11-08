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

// Host harness for display.show_dashboard's payload parser. The runner
// extracts the production parser (main/dashboard_command.c) and the condition
// names (components/muse/muse_dashboard.c) into dashboard_parser.inc.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "muse_dashboard.h"

#include "dashboard_parser.inc"

static bool parse(const char *json, muse_dashboard_data_t *d, const char **error) {
    cJSON *payload = cJSON_Parse(json);
    *error = NULL;
    bool ok = dashboard_parse(payload, d, error);
    cJSON_Delete(payload);
    return ok;
}

int main(void) {
    muse_dashboard_data_t d;
    const char *error;

    // The payload from issue #4 and esp32/tools/dashboard_sample.json.
    assert(parse("{\"updated_at\":\"2026-01-01T12:00:00+09:00\","
                 "\"indoor\":[{\"label\":\"Room A\",\"temperature_c\":24.0,\"humidity_pct\":45},"
                 "{\"label\":\"Room B\",\"temperature_c\":23.5,\"humidity_pct\":42}],"
                 "\"outdoor\":{\"temperature_c\":13.0,\"feels_like_c\":12.0,\"humidity_pct\":50,"
                 "\"precipitation_probability_pct\":10,\"condition\":\"clear\",\"is_day\":true}}",
                 &d, &error));
    assert(strcmp(d.updated, "12:00") == 0);
    assert(d.rooms == 2);
    assert(strcmp(d.room[0].label, "Room A") == 0);
    assert(fabsf(d.room[1].temperature_c - 23.5f) < 0.01f);
    assert(d.room[1].humidity_pct == 42);
    assert(d.outdoor && fabsf(d.outdoor_c - 13.0f) < 0.01f);
    assert(fabsf(d.feels_like_c - 12.0f) < 0.01f);
    assert(d.outdoor_humidity_pct == 50 && d.precipitation_pct == 10);
    assert(d.condition == MUSE_DASHBOARD_CLEAR && d.is_day);

    // Numbers and booleans as strings; missing fields; unknown condition.
    assert(parse("{\"outdoor\":{\"temperature_c\":\"-3.5\",\"is_day\":\"false\","
                 "\"condition\":\"hail\",\"extra\":1}}", &d, &error));
    assert(d.rooms == 0 && d.outdoor && fabsf(d.outdoor_c + 3.5f) < 0.01f);
    assert(isnan(d.feels_like_c) && d.outdoor_humidity_pct == -1 && d.precipitation_pct == -1);
    assert(!d.is_day && d.condition == MUSE_DASHBOARD_UNKNOWN);
    assert(d.updated[0] == '\0');

    // Out-of-range values read as missing; at most MUSE_DASHBOARD_ROOMS rooms.
    assert(parse("{\"indoor\":[{\"temperature_c\":500,\"humidity_pct\":120},{},{},{},{},{}]}",
                 &d, &error));
    assert(d.rooms == MUSE_DASHBOARD_ROOMS && !d.outdoor);
    assert(isnan(d.room[0].temperature_c) && d.room[0].humidity_pct == -1);
    assert(d.room[0].label[0] == '\0');

    // A long CJK label is cut on a character boundary.
    char json[512];
    char label[200] = "";
    for (int i = 0; i < 20; i++) strcat(label, "\xE5\xAF\x9D");   // 寝, 3 bytes each
    snprintf(json, sizeof(json), "{\"indoor\":[{\"label\":\"%s\"}]}", label);
    assert(parse(json, &d, &error));
    size_t n = strlen(d.room[0].label);
    assert(n < MUSE_DASHBOARD_LABEL_MAX && n % 3 == 0 && n > 0);

    // Unusable payloads.
    assert(!parse("{}", &d, &error) && error);
    assert(!parse("[1,2]", &d, &error) && error);
    assert(!parse("{\"indoor\":\"Room A\"}", &d, &error) && error);

    puts("dashboard parser OK");
    return 0;
}
