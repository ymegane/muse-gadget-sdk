#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Render an idle dashboard (indoor climate + weather) as an image for
`display.draw_file` or `display.draw_url` on a Link with a colour screen.

Meant to run on the agent's side of the link: the agent collects the data,
writes a JSON payload, renders it with this script, and draws the result with
the existing display commands. No firmware change is needed.

Payload (every field optional except what you want shown):

    {
      "updated_at": "2026-01-01T12:00:00+09:00",
      "indoor": [
        {"label": "Room A", "temperature_c": 24.0, "humidity_pct": 45},
        {"label": "Room B", "temperature_c": 23.5, "humidity_pct": 42}
      ],
      "outdoor": {
        "temperature_c": 13.0, "feels_like_c": 12.0,
        "humidity_pct": 50, "precipitation_probability_pct": 10,
        "condition": "clear", "is_day": true
      }
    }

`condition` is one of: clear, partly_cloudy, cloudy, fog, drizzle, rain,
snow, thunderstorm. The first indoor entry is drawn large; the rest as rows.
`is_day: false` switches to a dark palette and a moon.

Usage:
    dashboard_image.py payload.json out.jpg [--width 320] [--height 240]
                       [--font path/to/font.ttf] [--bold-font path/to/bold.ttf]

The default size is the M5Stack CoreS3 screen. Output is a baseline JPEG for a
.jpg/.jpeg name (the device's ROM decoder handles baseline only), otherwise
PNG, which is handy for previewing. Labels may be in any script the font
covers; pass --font for CJK labels.
"""

import argparse
import datetime
import json
import math
import os

from PIL import Image, ImageDraw, ImageFont

# Fonts tried in order when --font is not given. The first one found wins.
REGULAR_FONTS = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]
BOLD_FONTS = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]

DAY = {
    "bg": (246, 244, 238),
    "panel": (255, 255, 255),
    "text": (28, 30, 36),
    "muted": (110, 114, 124),
    "accent": (226, 120, 40),
    "humid": (40, 120, 200),
    "sun": (250, 176, 30),
    "cloud": (180, 188, 200),
    "rain": (50, 120, 220),
    "line": (222, 220, 212),
}
NIGHT = {
    "bg": (16, 18, 28),
    "panel": (30, 34, 48),
    "text": (232, 234, 240),
    "muted": (140, 146, 162),
    "accent": (255, 160, 90),
    "humid": (110, 170, 240),
    "sun": (236, 228, 190),  # the moon
    "cloud": (110, 118, 136),
    "rain": (110, 160, 240),
    "line": (48, 52, 68),
}


def find_font(explicit, candidates):
    for path in ([explicit] if explicit else []) + candidates:
        if path and os.path.exists(path):
            return path
    return None


class Fonts:
    def __init__(self, regular, bold):
        self.regular = find_font(regular, REGULAR_FONTS)
        self.bold = find_font(bold, BOLD_FONTS) or self.regular
        self.cache = {}

    def get(self, size, bold=False):
        key = (size, bold)
        if key not in self.cache:
            path = self.bold if bold else self.regular
            if path:
                self.cache[key] = ImageFont.truetype(path, size)
            else:
                self.cache[key] = ImageFont.load_default(size)
        return self.cache[key]


def fmt_temp(value, decimals=1):
    if value is None:
        return "--"
    return f"{value:.{decimals}f}°"


def fmt_pct(value):
    return "--%" if value is None else f"{round(value)}%"


def fmt_time(iso):
    if not iso:
        return "--:--"
    try:
        return datetime.datetime.fromisoformat(iso).strftime("%H:%M")
    except ValueError:
        return iso[:16]


def text_w(draw, text, font):
    return draw.textlength(text, font=font)


def fit_text(draw, text, font_for_size, size, max_w, bold=False, min_size=10):
    """The largest font no bigger than `size` that fits `text` in `max_w`."""
    while size > min_size and text_w(draw, text, font_for_size(size, bold)) > max_w:
        size -= 1
    return font_for_size(size, bold)


# ---- Weather icons, drawn with shapes so no emoji font is needed ------------


def draw_sun(d, cx, cy, r, c):
    d.ellipse([cx - r * 0.55, cy - r * 0.55, cx + r * 0.55, cy + r * 0.55], fill=c["sun"])
    for i in range(8):
        a = i * math.pi / 4
        x0, y0 = cx + math.cos(a) * r * 0.72, cy + math.sin(a) * r * 0.72
        x1, y1 = cx + math.cos(a) * r * 0.98, cy + math.sin(a) * r * 0.98
        d.line([x0, y0, x1, y1], fill=c["sun"], width=max(2, int(r * 0.12)))


def draw_moon(d, cx, cy, r, c):
    d.ellipse([cx - r * 0.7, cy - r * 0.7, cx + r * 0.7, cy + r * 0.7], fill=c["sun"])
    o = r * 0.38
    d.ellipse([cx - r * 0.7 + o, cy - r * 0.7 - o * 0.5,
               cx + r * 0.7 + o, cy + r * 0.7 - o * 0.5], fill=c["panel"])


def draw_cloud(d, cx, cy, r, c, colour=None):
    col = colour or c["cloud"]
    d.ellipse([cx - r * 0.9, cy - r * 0.15, cx - r * 0.1, cy + r * 0.55], fill=col)
    d.ellipse([cx - r * 0.5, cy - r * 0.55, cx + r * 0.35, cy + r * 0.3], fill=col)
    d.ellipse([cx + r * 0.05, cy - r * 0.25, cx + r * 0.9, cy + r * 0.55], fill=col)
    d.rectangle([cx - r * 0.5, cy + r * 0.1, cx + r * 0.5, cy + r * 0.55], fill=col)


def draw_drops(d, cx, cy, r, c, snow=False):
    for i, dx in enumerate((-0.45, 0.0, 0.45)):
        x = cx + dx * r
        y = cy + r * (0.72 + (0.12 if i == 1 else 0))
        if snow:
            s = r * 0.1
            d.ellipse([x - s, y - s, x + s, y + s], fill=c["text"] if c is NIGHT else c["cloud"])
        else:
            d.line([x + r * 0.08, y - r * 0.1, x - r * 0.06, y + r * 0.16],
                   fill=c["rain"], width=max(2, int(r * 0.1)))


def draw_bolt(d, cx, cy, r, c):
    pts = [(cx + r * 0.1, cy + r * 0.3), (cx - r * 0.2, cy + r * 0.75),
           (cx + r * 0.02, cy + r * 0.72), (cx - r * 0.12, cy + r * 1.05),
           (cx + r * 0.28, cy + r * 0.6), (cx + r * 0.05, cy + r * 0.62)]
    d.polygon(pts, fill=c["accent"])


def draw_fog(d, cx, cy, r, c):
    for i in range(3):
        y = cy - r * 0.3 + i * r * 0.32
        inset = r * (0.1 if i == 1 else 0.3)
        d.line([cx - r * 0.85 + inset, y, cx + r * 0.85 - inset, y],
               fill=c["cloud"], width=max(3, int(r * 0.16)))


def draw_weather_icon(d, cx, cy, r, condition, is_day, c):
    celestial = draw_sun if is_day else draw_moon
    cond = (condition or "").lower()
    if cond == "clear":
        celestial(d, cx, cy, r, c)
    elif cond == "partly_cloudy":
        celestial(d, cx - r * 0.3, cy - r * 0.3, r * 0.75, c)
        draw_cloud(d, cx + r * 0.1, cy + r * 0.15, r * 0.8, c)
    elif cond == "fog":
        draw_fog(d, cx, cy, r, c)
    elif cond in ("drizzle", "rain"):
        draw_cloud(d, cx, cy - r * 0.2, r, c)
        draw_drops(d, cx, cy - r * 0.2, r, c)
    elif cond == "snow":
        draw_cloud(d, cx, cy - r * 0.2, r, c)
        draw_drops(d, cx, cy - r * 0.2, r, c, snow=True)
    elif cond == "thunderstorm":
        draw_cloud(d, cx, cy - r * 0.25, r, c)
        draw_bolt(d, cx, cy - r * 0.25, r, c)
    else:  # cloudy or unknown
        draw_cloud(d, cx, cy, r, c)


# ---- Layout ------------------------------------------------------------------


def render(payload, width, height, fonts):
    outdoor = payload.get("outdoor") or {}
    indoor = payload.get("indoor") or []
    is_day = outdoor.get("is_day", True)
    c = DAY if is_day else NIGHT

    # Lay out on a 320x240 grid and scale everything else from it.
    s = min(width / 320, height / 240)

    def px(v):
        return int(round(v * s))

    img = Image.new("RGB", (width, height), c["bg"])
    d = ImageDraw.Draw(img)
    pad = px(8)
    split = int(width * 0.56)

    # Indoor: main room large on the left.
    main = indoor[0] if indoor else {}
    x0, y = pad + px(4), pad + px(2)
    d.text((x0, y), main.get("label", "Indoor"),
           font=fit_text(d, main.get("label", "Indoor"), fonts.get, px(18), split - x0 - pad),
           fill=c["muted"])
    y += px(22)
    big = fit_text(d, fmt_temp(main.get("temperature_c")), fonts.get, px(62),
                   split - x0 - pad, bold=True)
    d.text((x0 - px(2), y), fmt_temp(main.get("temperature_c")), font=big, fill=c["text"])
    y += px(70)
    hum = fmt_pct(main.get("humidity_pct"))
    d.ellipse([x0, y + px(6), x0 + px(14), y + px(20)], fill=c["humid"])
    d.text((x0 + px(20), y), hum, font=fonts.get(px(24), True), fill=c["humid"])

    # Other rooms as rows under the main one.
    y = px(132)
    d.line([pad, y, split - pad, y], fill=c["line"], width=1)
    row_h = px(24)
    max_rows = max(0, (height - px(30) - y) // row_h)
    for room in indoor[1:1 + max_rows]:
        y += px(4)
        label_font = fonts.get(px(15))
        val = f"{fmt_temp(room.get('temperature_c'))}  {fmt_pct(room.get('humidity_pct'))}"
        val_font = fonts.get(px(16), True)
        val_w = text_w(d, val, val_font)
        label = room.get("label", "")
        label_font = fit_text(d, label, fonts.get, px(15), split - pad * 2 - val_w - px(10))
        d.text((x0, y + px(2)), label, font=label_font, fill=c["muted"])
        d.text((split - pad - val_w, y), val, font=val_font, fill=c["text"])
        y += row_h - px(4)

    # Outdoor panel on the right.
    px0, py0, px1, py1 = split, pad, width - pad, height - px(30)
    d.rounded_rectangle([px0, py0, px1, py1], radius=px(12), fill=c["panel"])
    pw = px1 - px0
    icx, icy, ir = px0 + pw / 2, py0 + px(40), px(30)
    draw_weather_icon(d, icx, icy, ir, outdoor.get("condition"), is_day, c)

    t = fmt_temp(outdoor.get("temperature_c"), 0)
    tf = fit_text(d, t, fonts.get, px(40), pw - px(12), bold=True)
    d.text((icx - text_w(d, t, tf) / 2, py0 + px(76)), t, font=tf, fill=c["text"])

    small = fonts.get(px(13))
    lines = []
    if outdoor.get("feels_like_c") is not None:
        lines.append(("feels", fmt_temp(outdoor.get("feels_like_c"), 0), c["text"]))
    if outdoor.get("precipitation_probability_pct") is not None:
        lines.append(("rain", fmt_pct(outdoor.get("precipitation_probability_pct")), c["rain"]))
    if outdoor.get("humidity_pct") is not None:
        lines.append(("hum", fmt_pct(outdoor.get("humidity_pct")), c["humid"]))
    ly = py0 + px(126)
    for name, value, colour in lines[:3]:
        d.text((px0 + px(10), ly), name, font=small, fill=c["muted"])
        vf = fonts.get(px(15), True)
        d.text((px1 - px(10) - text_w(d, value, vf), ly - px(1)), value, font=vf, fill=colour)
        ly += px(20)

    # Footer: last update, so stale data is obvious.
    foot = f"updated {fmt_time(payload.get('updated_at'))}"
    ff = fonts.get(px(13))
    d.text((width - pad - text_w(d, foot, ff), height - px(22)), foot, font=ff, fill=c["muted"])
    return img


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("payload", help="JSON payload file, or - for stdin")
    parser.add_argument("out")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--font", help="TrueType/OpenType font for text")
    parser.add_argument("--bold-font", help="font for numbers (defaults to a bold one)")
    args = parser.parse_args()

    if args.payload == "-":
        import sys
        payload = json.load(sys.stdin)
    else:
        with open(args.payload, encoding="utf-8") as f:
            payload = json.load(f)

    img = render(payload, args.width, args.height, Fonts(args.font, args.bold_font))
    if args.out.lower().endswith((".jpg", ".jpeg")):
        img.save(args.out, "JPEG", quality=90, progressive=False)
    else:
        img.save(args.out)


if __name__ == "__main__":
    main()
