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

#pragma once

/*
 * Idle dashboard (CONFIG_HOMEHUB_DASHBOARD): indoor climate and the weather
 * on a tile after Settings, sent by the agent with display.show_dashboard.
 * While the device is idle and on USB power, with data received recently,
 * the UI slides to it by itself; a tap, a talk or a swipe leaves it. On
 * battery it never comes back by itself and the screen sleeps as usual.
 */

#include <stdbool.h>

#include "lvgl.h"

#define MUSE_DASHBOARD_ROOMS 4
#define MUSE_DASHBOARD_LABEL_MAX 40

typedef enum {
    MUSE_DASHBOARD_UNKNOWN = 0,
    MUSE_DASHBOARD_CLEAR,
    MUSE_DASHBOARD_PARTLY_CLOUDY,
    MUSE_DASHBOARD_CLOUDY,
    MUSE_DASHBOARD_FOG,
    MUSE_DASHBOARD_DRIZZLE,
    MUSE_DASHBOARD_RAIN,
    MUSE_DASHBOARD_SNOW,
    MUSE_DASHBOARD_THUNDERSTORM,
} muse_dashboard_condition_t;

/* A missing number is NAN (temperatures) or -1 (percentages). */
typedef struct {
    char label[MUSE_DASHBOARD_LABEL_MAX];
    float temperature_c;
    int humidity_pct;
} muse_dashboard_room_t;

typedef struct {
    char updated[8];   /* "HH:MM" from updated_at, or empty */
    int rooms;         /* entries used in room[]; the first is drawn large */
    muse_dashboard_room_t room[MUSE_DASHBOARD_ROOMS];
    bool outdoor;      /* the outdoor fields below were given */
    float outdoor_c;
    float feels_like_c;
    int outdoor_humidity_pct;
    int precipitation_pct;
    muse_dashboard_condition_t condition;
    bool is_day;
} muse_dashboard_data_t;

/* "clear", "partly_cloudy", ...: MUSE_DASHBOARD_UNKNOWN for anything else. */
muse_dashboard_condition_t muse_dashboard_condition(const char *name);

/* From any task: replaces the data and stamps it as just received. False when
 * this screen has no dashboard tile (no touch, or the UI isn't up). */
bool muse_dashboard_set(const muse_dashboard_data_t *data);

/* From any task: the dashboard is on screen with fresh data on USB power,
 * so the screen should not auto-sleep. */
bool muse_dashboard_keeps_awake(void);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Fills the dashboard tile. */
void muse_dashboard_build(lv_obj_t *tile, int w, int h);

/* Each frame while the screen is lit: redraws new data, and slides `tv` to the
 * tile once idle long enough on USB power. `busy` holds it back (an image,
 * a Settings sub-page). */
void muse_dashboard_tick(lv_obj_t *tv, bool busy);

/* The dashboard tile is the visible tile. */
bool muse_dashboard_active(void);
