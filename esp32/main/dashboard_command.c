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

// See dashboard_command.h. The payload is the one esp32/tools/dashboard_image.py
// renders for display.draw_file, so the two stay interchangeable.

#include "dashboard_command.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "muse_dashboard.h"

// ---- Parser (host-tested) ---------------------------------------------------

// Parameters arrive as Muse wrote them: numbers may also come as strings.
static bool dash_number(const cJSON *obj, const char *key, double *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return isfinite(*out);
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        char *end;
        *out = strtod(item->valuestring, &end);
        return *end == '\0' && isfinite(*out);
    }
    return false;
}

static float dash_temp(const cJSON *obj, const char *key) {
    double v;
    return dash_number(obj, key, &v) && v > -100 && v < 100 ? (float)v : NAN;
}

static int dash_pct(const cJSON *obj, const char *key) {
    double v;
    if (!dash_number(obj, key, &v) || v < 0 || v > 100) return -1;
    return (int)lround(v);
}

static bool dash_bool(const cJSON *obj, const char *key, bool fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcmp(item->valuestring, "true") == 0) return true;
        if (strcmp(item->valuestring, "false") == 0) return false;
    }
    return fallback;
}

// Copies at most `cap - 1` bytes without cutting a UTF-8 character in two.
static void dash_copy_utf8(char *out, size_t cap, const char *in) {
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

// Fills `out` from the payload object. False with `error` set when the payload
// is not usable at all; unknown fields are ignored.
static bool dashboard_parse(const cJSON *payload, muse_dashboard_data_t *out,
                            const char **error) {
    memset(out, 0, sizeof(*out));
    out->is_day = true;
    if (!cJSON_IsObject(payload)) {
        *error = "the dashboard payload must be a JSON object";
        return false;
    }

    // "2026-01-01T12:00:00+09:00" -> "12:00", as the sender's local time.
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(payload, "updated_at");
    if (cJSON_IsString(at) && at->valuestring && strlen(at->valuestring) >= 16 &&
        at->valuestring[10] == 'T') {
        memcpy(out->updated, at->valuestring + 11, 5);
        out->updated[5] = '\0';
    }

    const cJSON *indoor = cJSON_GetObjectItemCaseSensitive(payload, "indoor");
    if (!cJSON_IsArray(indoor)) indoor = NULL;
    const cJSON *room;
    cJSON_ArrayForEach(room, indoor) {
        if (out->rooms >= MUSE_DASHBOARD_ROOMS) break;
        if (!cJSON_IsObject(room)) continue;
        muse_dashboard_room_t *r = &out->room[out->rooms++];
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(room, "label");
        dash_copy_utf8(r->label, sizeof(r->label),
                       cJSON_IsString(label) && label->valuestring ? label->valuestring : "");
        r->temperature_c = dash_temp(room, "temperature_c");
        r->humidity_pct = dash_pct(room, "humidity_pct");
    }

    const cJSON *outdoor = cJSON_GetObjectItemCaseSensitive(payload, "outdoor");
    out->outdoor = cJSON_IsObject(outdoor);
    out->outdoor_c = NAN;
    out->feels_like_c = NAN;
    out->outdoor_humidity_pct = -1;
    out->precipitation_pct = -1;
    if (out->outdoor) {
        out->outdoor_c = dash_temp(outdoor, "temperature_c");
        out->feels_like_c = dash_temp(outdoor, "feels_like_c");
        out->outdoor_humidity_pct = dash_pct(outdoor, "humidity_pct");
        out->precipitation_pct = dash_pct(outdoor, "precipitation_probability_pct");
        const cJSON *cond = cJSON_GetObjectItemCaseSensitive(outdoor, "condition");
        out->condition = muse_dashboard_condition(
            cJSON_IsString(cond) ? cond->valuestring : NULL);
        out->is_day = dash_bool(outdoor, "is_day", true);
    }

    if (!out->rooms && !out->outdoor) {
        *error = "give indoor (a list of rooms) or outdoor (the weather)";
        return false;
    }
    return true;
}

// ---- Command ------------------------------------------------------------------

#ifndef DASHBOARD_HOST_TEST

static cJSON *dash_error(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

cJSON *dashboard_command(const cJSON *params) {
    // The payload as a JSON string (as advertised), or its fields given
    // directly as parameters.
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "payload");
    cJSON *parsed = NULL;
    const cJSON *payload = params;
    if (cJSON_IsString(text) && text->valuestring) {
        parsed = cJSON_Parse(text->valuestring);
        if (!parsed) return dash_error("invalid_param", "payload is not valid JSON");
        payload = parsed;
    } else if (cJSON_IsObject(text)) {
        payload = text;
    }

    muse_dashboard_data_t *data = malloc(sizeof(*data));
    if (!data) {
        cJSON_Delete(parsed);
        return dash_error("out_of_memory", "failed to allocate");
    }
    const char *error = NULL;
    bool ok = dashboard_parse(payload, data, &error);
    cJSON_Delete(parsed);
    if (!ok) {
        free(data);
        return dash_error("invalid_param", error);
    }
    bool shown = muse_dashboard_set(data);
    int rooms = data->rooms;
    bool outdoor = data->outdoor;
    free(data);
    if (!shown) {
        return dash_error("unsupported", "this screen has no dashboard (it needs a touch screen)");
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *out = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddNumberToObject(out, "rooms", rooms);
    cJSON_AddBoolToObject(out, "outdoor", outdoor);
    cJSON_AddBoolToObject(out, "on_screen", muse_dashboard_active());
    return result;
}

void dashboard_command_describe(cJSON *commands) {
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(
        command, "description",
        "Update the dashboard: a screen with indoor temperature and humidity "
        "and the current weather, one swipe past Settings. While the device "
        "is idle and on USB power it slides back to the dashboard by itself "
        "after a minute, as long as the data is under "
        "30 minutes old, so send fresh data every 10 to 15 minutes. A tap or "
        "a talk leaves it. On battery it never comes back by itself. "
        "Does not wake a sleeping screen. payload is a JSON object: "
        "{\"updated_at\": ISO 8601 local time, \"indoor\": [{\"label\", "
        "\"temperature_c\", \"humidity_pct\"}, ...] (up to 4; the first is "
        "shown large), \"outdoor\": {\"temperature_c\", \"feels_like_c\", "
        "\"humidity_pct\", \"precipitation_probability_pct\", \"condition\": "
        "clear|partly_cloudy|cloudy|fog|drizzle|rain|snow|thunderstorm, "
        "\"is_day\": false for a night palette and a moon}. Every field is "
        "optional; give indoor or outdoor.");
    cJSON *required = cJSON_AddObjectToObject(command, "required");
    cJSON *payload = cJSON_AddObjectToObject(required, "payload");
    cJSON_AddStringToObject(payload, "type", "string");
    cJSON_AddStringToObject(payload, "description", "The dashboard data as a JSON object, in a string.");
    cJSON_AddObjectToObject(command, "optional");
    cJSON_AddItemToObject(commands, "display.show_dashboard", command);
}

#endif  // DASHBOARD_HOST_TEST
