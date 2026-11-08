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

#include "muse_dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"

static const char *TAG = "dashboard";

/* Same layout as esp32/tools/dashboard_image.py, which renders it as an image
 * for display.draw_file: drawn for 320x240 and scaled to the screen. */

#if CONFIG_LV_FONT_MONTSERRAT_48
#define FONT_MAIN (&lv_font_montserrat_48)
#else
#define FONT_MAIN (&lv_font_montserrat_28)
#endif
#if CONFIG_LV_FONT_MONTSERRAT_36
#define FONT_OUTDOOR (&lv_font_montserrat_36)
#else
#define FONT_OUTDOOR (&lv_font_montserrat_28)
#endif

#if CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define CJK_FONT muse_font_cjk_noto_16
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define CJK_FONT muse_font_cjk_16
#endif

#if CONFIG_HOMEHUB_DASHBOARD_JAPANESE
#define LABEL_FEELS "\xE4\xBD\x93\xE6\x84\x9F"   /* 体感 */
#define LABEL_RAIN "\xE9\x99\x8D\xE6\xB0\xB4"    /* 降水 */
#define LABEL_HUMID "\xE6\xB9\xBF\xE5\xBA\xA6"   /* 湿度 */
#else
#define LABEL_FEELS "feels"
#define LABEL_RAIN "rain"
#define LABEL_HUMID "hum"
#endif

typedef struct {
    uint32_t bg, panel, text, muted, accent, humid, sun, moon, cloud, rain, line;
} palette_t;

/* Dark day and night: a light screen glares in a room. is_day only picks
 * the sun or the moon. */
static const palette_t DARK = {
    0x10121c, 0x1e2230, 0xe8eaf0, 0x8c92a2, 0xffa05a,
    0x6eaaf0, 0xfab01e, 0xece4be, 0x6e7688, 0x6ea0f0, 0x303444,
};

/* Shared with any task: the latest data and when it came. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_dashboard_data_t s_data;
static int64_t s_received_us;   /* 0: nothing since boot */
static bool s_cached;           /* s_data is the last data from before boot */
static bool s_dirty;
static volatile bool s_active;  /* the tile is visible */

/* LVGL task only. */
static lv_obj_t *s_tile;
static int s_w, s_h;
static float s_scale = 1;
static lv_obj_t *s_main_label, *s_main_temp, *s_main_humid, *s_main_humid_dot, *s_rule;
static lv_obj_t *s_row_label[MUSE_DASHBOARD_ROOMS - 1], *s_row_value[MUSE_DASHBOARD_ROOMS - 1];
static lv_obj_t *s_panel, *s_icon, *s_out_temp;
static lv_obj_t *s_out_name[3], *s_out_value[3];
static lv_obj_t *s_footer;
static lv_obj_t *s_clock;
static int s_clock_min = -1;    /* minute of the day shown */
static int s_drawn_icon = -1;   /* condition * 2 + is_day last drawn */
static bool s_drawn_stale;

static int S(float v) { return (int)lroundf(v * s_scale); }

muse_dashboard_condition_t muse_dashboard_condition(const char *name)
{
    static const char *const NAMES[] = {
        [MUSE_DASHBOARD_CLEAR] = "clear",
        [MUSE_DASHBOARD_PARTLY_CLOUDY] = "partly_cloudy",
        [MUSE_DASHBOARD_CLOUDY] = "cloudy",
        [MUSE_DASHBOARD_FOG] = "fog",
        [MUSE_DASHBOARD_DRIZZLE] = "drizzle",
        [MUSE_DASHBOARD_RAIN] = "rain",
        [MUSE_DASHBOARD_SNOW] = "snow",
        [MUSE_DASHBOARD_THUNDERSTORM] = "thunderstorm",
    };
    for (int i = 1; name && i < (int)(sizeof(NAMES) / sizeof(NAMES[0])); i++) {
        if (strcmp(name, NAMES[i]) == 0) return (muse_dashboard_condition_t)i;
    }
    return MUSE_DASHBOARD_UNKNOWN;
}

/* The latest data survives a restart in NVS, so the tile has something to
 * show at boot. It shows as old until new data comes: there is no telling
 * how old it is. */
#define NVS_NS "dashboard"
#define NVS_KEY "last"

static void save(const muse_dashboard_data_t *data)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, data, sizeof(*data)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    muse_dashboard_data_t d;
    size_t len = sizeof(d);
    /* A blob of another size is from a firmware with another layout. */
    if (nvs_get_blob(h, NVS_KEY, &d, &len) == ESP_OK && len == sizeof(d)) {
        d.rooms = d.rooms < 0 ? 0 : d.rooms > MUSE_DASHBOARD_ROOMS ? MUSE_DASHBOARD_ROOMS : d.rooms;
        for (int i = 0; i < MUSE_DASHBOARD_ROOMS; i++) d.room[i].label[MUSE_DASHBOARD_LABEL_MAX - 1] = '\0';
        d.updated[sizeof(d.updated) - 1] = '\0';
        portENTER_CRITICAL(&s_lock);
        s_data = d;
        s_cached = true;
        s_dirty = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "showing the last data from before the restart");
    }
    nvs_close(h);
}

bool muse_dashboard_set(const muse_dashboard_data_t *data)
{
    portENTER_CRITICAL(&s_lock);
    s_data = *data;
    s_received_us = esp_timer_get_time();
    s_cached = false;
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    save(data);
    return s_tile != NULL;
}

static bool fresh(int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    int64_t at = s_received_us;
    portEXIT_CRITICAL(&s_lock);
    return at && now - at < (int64_t)CONFIG_HOMEHUB_DASHBOARD_FRESH_MIN * 60 * 1000000;
}

bool muse_dashboard_keeps_awake(void)
{
    return s_active && !muse_state_on_battery() && fresh(esp_timer_get_time());
}

bool muse_dashboard_active(void)
{
    return s_active;
}

/* ---- Text --------------------------------------------------------------- */

#define DEG "\xC2\xB0"

/* Without %f: newlib-nano builds may not format floats. */
static void fmt_temp(char *out, size_t len, float c, bool decimal)
{
    if (isnan(c)) {
        snprintf(out, len, "--" DEG);
        return;
    }
    long t = lroundf(c * (decimal ? 10 : 1));
    const char *sign = t < 0 ? "-" : "";
    t = labs(t);
    if (decimal) {
        snprintf(out, len, "%s%ld.%ld" DEG, sign, t / 10, t % 10);
    } else {
        snprintf(out, len, "%s%ld" DEG, sign, t);
    }
}

static void fmt_pct(char *out, size_t len, int pct)
{
    if (pct < 0) {
        snprintf(out, len, "--%%");
    } else {
        snprintf(out, len, "%d%%", pct);
    }
}

static const lv_font_t *label_font(void)
{
#ifdef CJK_FONT
    static lv_font_t font;   /* Montserrat, falling back to CJK for room names */
    if (!font.get_glyph_dsc) {
        font = lv_font_montserrat_16;
        font.fallback = &CJK_FONT;
    }
    return &font;
#else
    return &lv_font_montserrat_16;
#endif
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                      lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, "");
    return l;
}

/* ---- Weather icons, from plain shapes ------------------------------------- */

/* Points for the icon's lines: LVGL keeps pointers to them. */
static lv_point_precise_t s_pts[24];
static int s_npts;

static lv_obj_t *blob(lv_obj_t *parent, float cx, float cy, float r, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, (int)lroundf(cx - r), (int)lroundf(cy - r));
    lv_obj_set_size(o, (int)lroundf(2 * r), (int)lroundf(2 * r));
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void box(lv_obj_t *parent, float x0, float y0, float x1, float y1, uint32_t color)
{
    lv_obj_t *o = blob(parent, 0, 0, 0, color);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_pos(o, (int)lroundf(x0), (int)lroundf(y0));
    lv_obj_set_size(o, (int)lroundf(x1 - x0), (int)lroundf(y1 - y0));
}

static void line(lv_obj_t *parent, int n, const float *xy, float width, uint32_t color)
{
    if (s_npts + n > (int)(sizeof(s_pts) / sizeof(s_pts[0]))) return;
    lv_point_precise_t *p = &s_pts[s_npts];
    for (int i = 0; i < n; i++) {
        p[i].x = (lv_value_precise_t)xy[2 * i];
        p[i].y = (lv_value_precise_t)xy[2 * i + 1];
    }
    s_npts += n;
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, p, n);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_line_width(l, (int)lroundf(width < 2 ? 2 : width), 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
}

static void sun(lv_obj_t *p, float cx, float cy, float r, const palette_t *c)
{
    blob(p, cx, cy, r * 0.55f, c->sun);
    for (int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4;
        float xy[] = { cx + cosf(a) * r * 0.75f, cy + sinf(a) * r * 0.75f,
                       cx + cosf(a) * r * 0.98f, cy + sinf(a) * r * 0.98f };
        line(p, 2, xy, r * 0.12f, c->sun);
    }
}

static void moon(lv_obj_t *p, float cx, float cy, float r, const palette_t *c)
{
    blob(p, cx, cy, r * 0.7f, c->moon);
    blob(p, cx + r * 0.38f, cy - r * 0.19f, r * 0.7f, c->panel);
}

static void cloud(lv_obj_t *p, float cx, float cy, float r, const palette_t *c)
{
    blob(p, cx - r * 0.5f, cy + r * 0.2f, r * 0.4f, c->cloud);
    blob(p, cx - r * 0.07f, cy - r * 0.12f, r * 0.43f, c->cloud);
    blob(p, cx + r * 0.47f, cy + r * 0.15f, r * 0.42f, c->cloud);
    box(p, cx - r * 0.5f, cy + r * 0.1f, cx + r * 0.5f, cy + r * 0.6f, c->cloud);
}

static void drops(lv_obj_t *p, float cx, float cy, float r, const palette_t *c, bool snow)
{
    for (int i = 0; i < 3; i++) {
        float x = cx + (i - 1) * 0.45f * r;
        float y = cy + r * (0.78f + (i == 1 ? 0.12f : 0));
        if (snow) {
            blob(p, x, y, r * 0.1f, c->muted);
        } else {
            float xy[] = { x + r * 0.08f, y - r * 0.1f, x - r * 0.06f, y + r * 0.16f };
            line(p, 2, xy, r * 0.1f, c->rain);
        }
    }
}

static void draw_icon(muse_dashboard_condition_t cond, bool day, const palette_t *c)
{
    lv_obj_clean(s_icon);
    s_npts = 0;
    float sz = (float)lv_obj_get_width(s_icon);
    float cx = sz / 2, cy = sz / 2, r = sz / 2 - 1;
    void (*celestial)(lv_obj_t *, float, float, float, const palette_t *) = day ? sun : moon;
    switch (cond) {
    case MUSE_DASHBOARD_CLEAR:
        celestial(s_icon, cx, cy, r, c);
        break;
    case MUSE_DASHBOARD_PARTLY_CLOUDY:
        celestial(s_icon, cx - r * 0.3f, cy - r * 0.3f, r * 0.7f, c);
        cloud(s_icon, cx + r * 0.1f, cy + r * 0.15f, r * 0.8f, c);
        break;
    case MUSE_DASHBOARD_FOG:
        for (int i = 0; i < 3; i++) {
            float y = cy - r * 0.3f + i * r * 0.32f, in = r * (i == 1 ? 0.1f : 0.3f);
            float xy[] = { cx - r * 0.85f + in, y, cx + r * 0.85f - in, y };
            line(s_icon, 2, xy, r * 0.16f, c->cloud);
        }
        break;
    case MUSE_DASHBOARD_DRIZZLE:
    case MUSE_DASHBOARD_RAIN:
    case MUSE_DASHBOARD_SNOW:
        cloud(s_icon, cx, cy - r * 0.2f, r, c);
        drops(s_icon, cx, cy - r * 0.2f, r, c, cond == MUSE_DASHBOARD_SNOW);
        break;
    case MUSE_DASHBOARD_THUNDERSTORM: {
        cloud(s_icon, cx, cy - r * 0.25f, r, c);
        float y = cy - r * 0.25f;
        float xy[] = { cx + r * 0.1f, y + r * 0.35f, cx - r * 0.15f, y + r * 0.72f,
                       cx + r * 0.1f, y + r * 0.72f, cx - r * 0.1f, y + r * 1.05f };
        line(s_icon, 4, xy, r * 0.12f, c->accent);
        break;
    }
    default:
        cloud(s_icon, cx, cy, r, c);
        break;
    }
}

/* ---- Tile ----------------------------------------------------------------- */

static void on_tap(lv_event_t *e)
{
    (void)e;
    muse_ui_show_face();
}

void muse_dashboard_build(lv_obj_t *tile, int w, int h)
{
    s_tile = tile;
    s_w = w;
    s_h = h;
    s_scale = fminf(w / 320.0f, h / 240.0f);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, on_tap, LV_EVENT_SHORT_CLICKED, NULL);

    int pad = S(8), x0 = S(12), split = (int)(w * 0.56f);
    s_main_label = text(tile, label_font(), x0, S(10), split - x0 - pad, LV_TEXT_ALIGN_LEFT);
    s_main_temp = text(tile, FONT_MAIN, x0 - S(2), S(32), 0, LV_TEXT_ALIGN_LEFT);
    s_main_humid_dot = blob(tile, x0 + S(6), S(110), S(6), 0);
    s_main_humid = text(tile, &lv_font_montserrat_20, x0 + S(18), S(99), 0, LV_TEXT_ALIGN_LEFT);
    s_rule = blob(tile, 0, 0, 0, 0);
    lv_obj_set_style_radius(s_rule, 0, 0);
    lv_obj_set_pos(s_rule, pad, S(132));
    lv_obj_set_size(s_rule, split - 2 * pad, 1);
    for (int i = 0; i < MUSE_DASHBOARD_ROOMS - 1; i++) {
        int y = S(138) + i * S(24);
        s_row_label[i] = text(tile, label_font(), x0, y + S(2), S(80), LV_TEXT_ALIGN_LEFT);
        s_row_value[i] = text(tile, &lv_font_montserrat_16, split - pad - S(96), y + S(2), S(96),
                              LV_TEXT_ALIGN_RIGHT);
    }

    s_panel = blob(tile, 0, 0, 0, 0);
    lv_obj_set_style_radius(s_panel, S(12), 0);
    lv_obj_set_pos(s_panel, split, pad);
    int pw = w - pad - split, ph = h - S(30) - pad;
    lv_obj_set_size(s_panel, pw, ph);
    int icon = S(60);
    s_icon = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, icon, icon);
    lv_obj_set_pos(s_icon, (pw - icon) / 2, S(10));
    lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_CLICKABLE);
    s_out_temp = text(s_panel, FONT_OUTDOOR, 0, S(76), pw, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        int y = S(126) + i * S(20);
        s_out_name[i] = text(s_panel, label_font(), S(10), y - S(1), 0, LV_TEXT_ALIGN_LEFT);
        s_out_value[i] = text(s_panel, &lv_font_montserrat_16, pw - S(10) - S(60), y - S(1), S(60),
                              LV_TEXT_ALIGN_RIGHT);
    }

    /* The time, counted on from updated_at: the device has no clock of its own. */
    s_clock = text(tile, &lv_font_montserrat_20, x0, h - S(28), 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(DARK.text), 0);
    /* Right of the page dots, which sit at the bottom centre. */
    s_footer = text(tile, &lv_font_montserrat_14, w / 2 + S(30), h - S(22), w / 2 - S(30) - pad,
                    LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_footer, "--:--");
    lv_obj_set_style_bg_color(tile, lv_color_hex(DARK.bg), 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(DARK.muted), 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_main_humid_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "tile %dx%d", w, h);
    load();
}

static void color(lv_obj_t *o, uint32_t c)
{
    lv_obj_set_style_text_color(o, lv_color_hex(c), 0);
}

static void render(const muse_dashboard_data_t *d, bool stale)
{
    const palette_t *c = &DARK;
    char buf[48], pct[12];

    lv_obj_set_style_bg_color(s_tile, lv_color_hex(c->bg), 0);
    lv_obj_remove_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_rule, lv_color_hex(c->line), 0);

    const muse_dashboard_room_t *main = d->rooms ? &d->room[0] : NULL;
    lv_label_set_text(s_main_label, main && main->label[0] ? main->label : "Indoor");
    color(s_main_label, c->muted);
    fmt_temp(buf, sizeof(buf), main ? main->temperature_c : NAN, true);
    lv_label_set_text(s_main_temp, buf);
    color(s_main_temp, c->text);
    fmt_pct(pct, sizeof(pct), main ? main->humidity_pct : -1);
    lv_label_set_text(s_main_humid, pct);
    color(s_main_humid, c->humid);
    lv_obj_remove_flag(s_main_humid_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_main_humid_dot, lv_color_hex(c->humid), 0);

    for (int i = 0; i < MUSE_DASHBOARD_ROOMS - 1; i++) {
        const muse_dashboard_room_t *r = i + 1 < d->rooms ? &d->room[i + 1] : NULL;
        if (!r) {
            lv_label_set_text(s_row_label[i], "");
            lv_label_set_text(s_row_value[i], "");
            continue;
        }
        lv_label_set_text(s_row_label[i], r->label);
        color(s_row_label[i], c->muted);
        fmt_temp(buf, sizeof(buf), r->temperature_c, true);
        fmt_pct(pct, sizeof(pct), r->humidity_pct);
        strncat(buf, "  ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, pct, sizeof(buf) - strlen(buf) - 1);
        lv_label_set_text(s_row_value[i], buf);
        color(s_row_value[i], c->text);
    }

    lv_obj_set_flag(s_panel, LV_OBJ_FLAG_HIDDEN, !d->outdoor);
    if (d->outdoor) {
        lv_obj_set_style_bg_color(s_panel, lv_color_hex(c->panel), 0);
        int icon = (int)d->condition * 2 + d->is_day;
        if (icon != s_drawn_icon) {
            draw_icon(d->condition, d->is_day, c);
            s_drawn_icon = icon;
        }
        fmt_temp(buf, sizeof(buf), d->outdoor_c, false);
        lv_label_set_text(s_out_temp, buf);
        color(s_out_temp, c->text);
        int n = 0;
        if (!isnan(d->feels_like_c)) {
            fmt_temp(buf, sizeof(buf), d->feels_like_c, false);
            lv_label_set_text(s_out_name[n], LABEL_FEELS);
            lv_label_set_text(s_out_value[n], buf);
            color(s_out_value[n++], c->text);
        }
        if (d->precipitation_pct >= 0) {
            fmt_pct(pct, sizeof(pct), d->precipitation_pct);
            lv_label_set_text(s_out_name[n], LABEL_RAIN);
            lv_label_set_text(s_out_value[n], pct);
            color(s_out_value[n++], c->rain);
        }
        if (d->outdoor_humidity_pct >= 0) {
            fmt_pct(pct, sizeof(pct), d->outdoor_humidity_pct);
            lv_label_set_text(s_out_name[n], LABEL_HUMID);
            lv_label_set_text(s_out_value[n], pct);
            color(s_out_value[n++], c->humid);
        }
        for (int i = 0; i < 3; i++) {
            color(s_out_name[i], c->muted);
            if (i >= n) {
                lv_label_set_text(s_out_name[i], "");
                lv_label_set_text(s_out_value[i], "");
            }
        }
    }

    /* When the data was made; orange once it is old. */
    snprintf(buf, sizeof(buf), "%s", d->updated[0] ? d->updated : "--:--");
    lv_label_set_text(s_footer, buf);
    color(s_footer, stale ? c->accent : c->muted);
    s_drawn_stale = stale;
}

static float idle_threshold_s(void)
{
    float after = CONFIG_HOMEHUB_DASHBOARD_IDLE_S;
    int sleep = muse_settings_sleep_s();
    /* Come back before the screen would sleep. */
    if (sleep > 0 && sleep <= after) after = sleep / 2.0f;
    return after < 5 ? 5 : after;
}

void muse_dashboard_tick(lv_obj_t *tv, bool busy)
{
    if (!s_tile || !tv) return;
    int64_t now = esp_timer_get_time();
    bool is_fresh = fresh(now);

    portENTER_CRITICAL(&s_lock);
    bool have = s_received_us != 0 || s_cached;
    bool redraw = s_dirty || (have && s_drawn_stale != !is_fresh);
    muse_dashboard_data_t d;
    if (redraw) {
        d = s_data;
        s_dirty = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (redraw) render(&d, !is_fresh);

    portENTER_CRITICAL(&s_lock);
    int64_t at = s_received_us;
    int base = s_data.updated_s;
    portEXIT_CRITICAL(&s_lock);
    int min = -1;
    if (at && base >= 0) {
        min = (int)(((int64_t)base + (now - at) / 1000000) % 86400) / 60;
    }
    if (min != s_clock_min) {
        char clock[8] = "";
        if (min >= 0) snprintf(clock, sizeof(clock), "%02d:%02d", min / 60, min % 60);
        lv_label_set_text(s_clock, clock);
        s_clock_min = min;
    }

    s_active = lv_tileview_get_tile_active(tv) == s_tile;
    if (s_active || busy || !is_fresh || muse_state_on_battery() || muse_state_asleep()) return;
    float in_mode;
    if (muse_state_mode(&in_mode) != MUSE_MODE_IDLE) return;
    float idle = fminf(in_mode, muse_state_idle_secs());
    if (idle < idle_threshold_s()) return;
    ESP_LOGI(TAG, "idle %d s on USB power: showing the dashboard", (int)idle);
    lv_tileview_set_tile(tv, s_tile, LV_ANIM_ON);
    s_active = true;
}
