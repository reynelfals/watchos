#!/usr/bin/env python3
"""Desktop WatchOS Lua host (layout preview + tilt/IMU for tilt_maze).

SAFE_INSET=18, CORNER_INSET=52 per simulator contract.
Keeps SD/squish helpers; adds watch.imu(), watch.ball(), tilt pad, autoplay.
"""

from __future__ import annotations

import math
import struct
import os
import time
from dataclasses import dataclass, field
from typing import Any, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFont

LCD_W = 410
LCD_H = 502
SAFE_INSET = 18
CORNER_INSET = 52
SCALE = 2
PANEL_W = 280
CANVAS_W = LCD_W * SCALE + PANEL_W  # 1100
CANVAS_H = LCD_H * SCALE  # 1004
TILT_MAX_G = 0.7


@dataclass
class Button:
    label: str
    cb: Any = None


@dataclass
class BallSprite:
    x: int
    y: int
    r: int
    color: Tuple[int, int, int]


@dataclass
class HostState:
    title: str = ""
    text: str = ""
    bg: Tuple[int, int, int] = (8, 10, 16)
    layout: str = "column"
    layout_cols: int = 2
    buttons: List[Button] = field(default_factory=list)
    fills: List[Tuple[int, int, int, int, int, int, int]] = field(default_factory=list)
    circles: List[Tuple[int, int, int, int, int, int]] = field(default_factory=list)
    labels: List[Tuple[int, int, str, int, int, int]] = field(default_factory=list)
    panels: List[Tuple[int, int, int, int, int, int, int]] = field(default_factory=list)
    progress: List[Tuple[int, int, int, int, int, int]] = field(default_factory=list)
    sliders: List[Tuple] = field(default_factory=list)
    image_box: Optional[Tuple[int, int, int, int]] = None
    ball: Optional[BallSprite] = None
    # canvas: (x, y, w, h, pixels RGB list len w*h)
    canvas: Optional[Tuple[int, int, int, int, list]] = None


class WatchHost:
    def __init__(self, apps_root: str):
        self.apps_root = apps_root
        self.state = HostState()
        self._sd: dict[str, bytes] = {}
        self._sd_mounted = False
        self._usb_msc = False
        self._usb_msc_status = "USB SD idle (sim)"
        self._tick = None
        self._boot_ms = int(time.time() * 1000)
        self._sim_ms: Optional[int] = None  # if set, millis() uses this (shot mode)
        self.imu_ax = 0.0
        self.imu_ay = 0.0
        self.imu_az = 1.0
        self.imu_gx = 0.0
        self.imu_gy = 0.0
        self.imu_gz = 0.0
        self.tilt_max_g = TILT_MAX_G
        self._prefs: dict[str, str] = {}
        self._brightness = 75
        self._idle_timeout = 60
        self._wifi = "off"
        self._on_back = None
        self._on_long_press = None
        self._gesture = None
        self._touch_delta = {"dx": 0, "dy": 0, "pressed": False, "long": False}
        self._gpio: dict[int, int] = {}
        self._mic_running = False
        self._mic_rate = 16000
        self._mic_frame = 0
        self._mic_pcm_pos = 0  # sample index for synthetic PCM
        self.mic_tone_hz = 880.0  # synthetic tone for waterfall / pcm_lab sim
        self._speaker_running = False
        self._speaker_rate = 16000
        self._speaker_last_path = None
        self._speaker_vol_raw = 0xBF  # ES8311 reg32 max = 0 dB; session only
        self._progress_vals: dict[int, int] = {}
        self._prog_id = 0
        self._slider_id = 0
        self._notif_app_id = None
        self._notif_text = None
        self._alert_uh_oh_count = 0

    # ---- clock / tick -------------------------------------------------------

    def millis(self) -> int:
        if self._sim_ms is not None:
            return int(self._sim_ms)
        return int(time.time() * 1000) - self._boot_ms

    def advance_ms(self, ms: int) -> None:
        """Advance simulated clock (for headless --shot)."""
        if self._sim_ms is None:
            self._sim_ms = self.millis()
        self._sim_ms = int(self._sim_ms) + int(ms)

    def use_sim_clock(self, start_ms: int = 0) -> None:
        self._sim_ms = int(start_ms)

    def on_tick(self, cb=None) -> None:
        self._tick = cb

    def fire_tick(self) -> None:
        if self._tick is not None:
            self._tick()

    # ---- IMU / tilt ---------------------------------------------------------

    def set_tilt(self, ax: float = 0.0, ay: float = 0.0, az: float | None = None) -> None:
        self.imu_ax = float(ax)
        self.imu_ay = float(ay)
        if az is None:
            n2 = self.imu_ax * self.imu_ax + self.imu_ay * self.imu_ay
            self.imu_az = max(0.2, 1.0 - 0.5 * n2)
        else:
            self.imu_az = float(az)

    def set_gyro(self, gx: float = 0.0, gy: float = 0.0, gz: float = 0.0) -> None:
        """Synthetic gyro rates in degrees/second (matches QMI8658 / watch.imu)."""
        self.imu_gx = float(gx)
        self.imu_gy = float(gy)
        self.imu_gz = float(gz)

    def autoplay_tilt(self, t: float) -> None:
        """Deterministic elliptical tilt for --shot (t in seconds-ish)."""
        ax = 0.35 * math.sin(t * 1.7)
        ay = 0.45 * math.cos(t * 1.1)
        self.set_tilt(ax=ax, ay=ay)
        # Mild yaw-rate for portal pan demos when autoplaying
        self.set_gyro(gx=0.0, gy=0.0, gz=25.0 * math.sin(t * 0.9))

    def imu(self):
        """Return plain dict; run.py wraps as lua.table.
        ax..az in g, gx..gz in dps (same as firmware QMI8658 path).
        """
        return {
            "ax": float(self.imu_ax),
            "ay": float(self.imu_ay),
            "az": float(self.imu_az),
            "gx": float(self.imu_gx),
            "gy": float(self.imu_gy),
            "gz": float(self.imu_gz),
        }

    def ball(self, x, y, r, R, G, B) -> None:
        """Persistent ball sprite; x/y/r may be floats (rounded like firmware pixArg)."""
        self.state.ball = BallSprite(
            int(round(float(x))),
            int(round(float(y))),
            max(1, int(round(float(r)))),
            (int(R) & 255, int(G) & 255, int(B) & 255),
        )

    # ---- core UI ------------------------------------------------------------

    def set_title(self, t: str) -> None:
        self.state.title = str(t or "")

    def set_text(self, t: str) -> None:
        self.state.text = str(t or "")

    def append_text(self, t: str) -> None:
        self.state.text = (self.state.text or "") + str(t or "")

    def clear(self) -> None:
        self.state.text = ""

    def clear_ui(self) -> None:
        self.state.buttons.clear()
        self.state.fills.clear()
        self.state.circles.clear()
        self.state.labels.clear()
        self.state.panels.clear()
        self.state.progress.clear()
        self.state.sliders.clear()
        self.state.image_box = None
        self.state.ball = None
        self.state.canvas = None
        self.state.text = ""

    def width(self) -> int:
        return LCD_W

    def height(self) -> int:
        return LCD_H

    def delay(self, _ms: int = 0) -> None:
        return None

    def fill(self, r: int, g: int, b: int) -> None:
        self.state.bg = (int(r), int(g), int(b))

    def rect(self, x: int, y: int, w: int, h: int, r: int, g: int, b: int) -> None:
        self.state.fills.append((int(x), int(y), int(w), int(h), int(r), int(g), int(b)))

    def circle(self, x: int, y: int, rad: int, r: int, g: int, b: int) -> None:
        self.state.circles.append((int(x), int(y), int(rad), int(r), int(g), int(b)))

    def text_at(self, x: int, y: int, text: str, r: int = 255, g: int = 255, b: int = 255) -> None:
        self.state.labels.append((int(x), int(y), str(text), int(r), int(g), int(b)))

    def gfx_clear(self) -> None:
        self.state.fills.clear()
        self.state.circles.clear()
        self.state.labels.clear()
        self.state.panels.clear()
        self.state.progress.clear()
        self.state.canvas = None
        # persistent ball survives gfx_clear (matches device ball layer)

    def button_layout(self, mode: str, cols: int = 2) -> bool:
        self.state.layout = str(mode or "column")
        c = int(cols) if cols is not None else 2
        if c < 1:
            c = 1
        if c > 4:
            c = 4
        self.state.layout_cols = c
        return True

    def button(self, label: str, cb=None) -> bool:
        if len(self.state.buttons) >= 10:
            return False
        self.state.buttons.append(Button(str(label), cb))
        return True

    def back(self) -> None:
        if self._on_back is not None:
            self._on_back()

    def battery(self):
        return 87

    def safe_inset(self) -> int:
        return SAFE_INSET

    def corner_inset(self) -> int:
        return CORNER_INSET

    def vibrate(self) -> None:
        return None

    # ---- SD / squish (unchanged contract) -----------------------------------

    def sd_ready(self) -> bool:
        return self._sd_mounted

    def sd_mount(self) -> bool:
        self._sd_mounted = True
        return True

    def sd_unmount(self) -> bool:
        self._sd_mounted = False
        self._usb_msc = False
        self._usb_msc_status = "USB SD idle (sim)"
        return True


    def usb_msc_active(self) -> bool:
        return bool(getattr(self, "_usb_msc", False))

    def usb_msc_enter(self) -> bool:
        self._sd_mounted = True
        self._usb_msc = True
        self._usb_msc_status = (
            "USB SD active — PC should see drive. Tap Exit before unplug."
        )
        return True

    def usb_msc_exit(self) -> bool:
        self._usb_msc = False
        self._usb_msc_status = "USB SD exited — SD remounted (sim)"
        return True

    def usb_msc_status(self) -> str:
        return getattr(self, "_usb_msc_status", "USB SD idle (sim)")


    def sd_info(self):
        if not self._sd_mounted:
            return None
        return {"total_mb": 15180.0, "used_mb": 120.0, "total_bytes": 0, "used_bytes": 0}

    def sd_list(self, path: str):
        if not self._sd_mounted:
            return None
        path = (path or "/sd").rstrip("/") or "/sd"
        out = []
        # Dynamic entries from _sd store
        prefix = path + "/"
        seen = set()
        for k, v in list(self._sd.items()):
            if k.startswith(prefix):
                rest = k[len(prefix):]
                if not rest or rest == ".dir":
                    continue
                name = rest.split("/", 1)[0]
                if name in seen:
                    continue
                seen.add(name)
                full = prefix + name
                is_dir = any(
                    kk == full + "/.dir" or kk.startswith(full + "/")
                    for kk in self._sd
                )
                size = len(v) if (not is_dir and isinstance(v, (bytes, bytearray))) else 0
                if not is_dir and full in self._sd and isinstance(self._sd[full], (bytes, bytearray)):
                    size = len(self._sd[full])
                out.append({"name": name, "size": size, "is_dir": is_dir})
        if path == "/sd" and not out:
            out = [
                {"name": "squish", "size": 0, "is_dir": True},
                {"name": "watchos_sd_lab.txt", "size": 16, "is_dir": False},
            ]
        out.sort(key=lambda e: (not e["is_dir"], e["name"]))
        return out

    def sd_read(self, path: str, max_bytes: int = 256):
        if not self._sd_mounted:
            return None
        data = self._sd.get(path)
        if data is None:
            return None
        if isinstance(data, str):
            data = data.encode("utf-8")
        chunk = data[: int(max_bytes)]
        try:
            return chunk.decode("utf-8")
        except Exception:
            return chunk

    def sd_write(self, path: str, data) -> bool:
        if not self._sd_mounted:
            return False
        if isinstance(data, str):
            self._sd[path] = data.encode("utf-8")
        else:
            self._sd[path] = bytes(data)
        return True

    def sd_mkdir(self, path: str) -> bool:
        if not self._sd_mounted:
            return False
        self._sd[path.rstrip("/") + "/.dir"] = b""
        return True

    def sd_put_lfs(self, lfs_src: str, sd_dst: str) -> bool:
        if not self._sd_mounted:
            return False
        if not lfs_src.startswith("/apps/") or ".." in lfs_src or ".." in sd_dst:
            return False
        if not sd_dst.startswith("/sd/"):
            return False
        host = self._map_lfs(lfs_src)
        if not host or not os.path.isfile(host):
            return False
        with open(host, "rb") as f:
            self._sd[sd_dst] = f.read()
        return True

    def lfs_list(self, path: str):
        if not path.startswith("/apps/") or ".." in path:
            return None
        host = self._map_lfs(path)
        if not host or not os.path.isdir(host):
            return None
        out = []
        for name in sorted(os.listdir(host)):
            full = os.path.join(host, name)
            out.append(
                {
                    "name": name,
                    "size": os.path.getsize(full) if os.path.isfile(full) else 0,
                    "is_dir": os.path.isdir(full),
                }
            )
        return out

    def image_exists(self, path: str) -> bool:
        if path.startswith("/sd/"):
            return path in self._sd
        host = self._map_lfs(path)
        return bool(host and os.path.isfile(host))

    def image(self, path: str, x: int, y: int, max_w: int = 160, max_h: int = 160) -> bool:
        if not self.image_exists(path):
            return False
        self.state.image_box = (int(x), int(y), int(max_w), int(max_h))
        return True

    def image_clear(self) -> None:
        self.state.image_box = None

    def squish_image_path(self, name: str):
        slug = "".join(c.lower() if c.isalnum() else "_" for c in (name or ""))
        if not slug:
            return None
        sd = f"/sd/squish/img/{slug}.wrgb"
        lfs = f"/apps/squish_id/img/{slug}.wrgb"
        print(f"squish_image_path: try SD {sd}")
        if sd in self._sd:
            print(f"squish_image_path: HIT SD {sd}")
            return sd
        print(f"squish_image_path: miss SD, try LFS {lfs}")
        if self.image_exists(lfs):
            print(f"squish_image_path: HIT LFS {lfs}")
            return lfs
        print(f"squish_image_path: MISS both for slug={slug}")
        return None

    def squish_count(self) -> int:
        return 194

    def squish_search(self, q: str):
        return []

    def letter_pad(self, cb=None) -> bool:
        return True

    def _map_lfs(self, path: str) -> Optional[str]:
        if not path.startswith("/apps/"):
            return None
        rel = path[len("/apps/") :]
        return os.path.join(self.apps_root, rel)

    # ---- render -------------------------------------------------------------

    def _fonts(self):
        try:
            font_t = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20
            )
            font_b = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14
            )
            font_btn = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13
            )
            font_sm = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12
            )
        except Exception:
            font_t = ImageFont.load_default()
            font_b = font_t
            font_btn = font_t
            font_sm = font_t
        return font_t, font_b, font_btn, font_sm

    def render_lcd(self) -> Image.Image:
        img = Image.new("RGB", (LCD_W, LCD_H), self.state.bg)
        draw = ImageDraw.Draw(img)
        font_t, font_b, font_btn, _font_sm = self._fonts()

        title_y = SAFE_INSET + 8
        draw.text(
            (LCD_W // 2, title_y),
            self.state.title,
            fill=(62, 224, 240),
            font=font_t,
            anchor="mt",
        )

        if self.state.layout == "grid2":
            cols = int(getattr(self.state, "layout_cols", 2) or 2)
            nbtn = max(1, len(self.state.buttons))
            rows = (nbtn + cols - 1) // cols
            btn_band = 70 + rows * 48
            if btn_band < 160:
                btn_band = 160
            if btn_band > 320:
                btn_band = 320
        else:
            btn_band = 230
        body_top = title_y + 28
        body_bottom = LCD_H - btn_band - 8
        body_left = SAFE_INSET + 6
        body_right = LCD_W - SAFE_INSET - 6
        draw.rectangle(
            [body_left - 2, body_top - 2, body_right + 2, body_bottom],
            outline=(40, 48, 64),
        )
        y = body_top
        for line in (self.state.text or "").split("\n"):
            if y + 16 > body_bottom:
                draw.text((body_left, y), "...", fill=(200, 200, 210), font=font_b)
                break
            draw.text((body_left, y), line[:42], fill=(244, 247, 251), font=font_b)
            y += 16

        if self.state.image_box:
            x, iy, w, h = self.state.image_box
            draw.rectangle([x, iy, x + w, iy + h], outline=(120, 180, 255), width=2)
            draw.text((x + 8, iy + h // 2), "IMG", fill=(120, 180, 255), font=font_b)

        if self.state.canvas:
            cx, cy, cw, ch, pixels = self.state.canvas
            buf = Image.new("RGB", (cw, ch))
            buf.putdata(pixels)
            img.paste(buf, (cx, cy))

        for (x, y0, w, h, r0, g0, b0) in self.state.fills:
            draw.rectangle([x, y0, x + w, y0 + h], fill=(r0, g0, b0))
        for (x, y0, rad, r0, g0, b0) in self.state.circles:
            draw.ellipse([x - rad, y0 - rad, x + rad, y0 + rad], fill=(r0, g0, b0))
        for (x, y0, w, h, r0, g0, b0) in self.state.panels:
            draw.rounded_rectangle([x, y0, x + w, y0 + h], radius=12, fill=(r0, g0, b0))
        for (_pid, x, y0, w, h, val) in self.state.progress:
            draw.rectangle([x, y0, x + w, y0 + h], fill=(26, 36, 48), outline=(40, 60, 80))
            fw = max(0, int(w * (val / 100.0)))
            if fw:
                draw.rectangle([x, y0, x + fw, y0 + h], fill=(61, 220, 151))
        for (_sid, x, y0, w, h, _a, _b, _cb) in self.state.sliders:
            mid = y0 + h // 3
            draw.rounded_rectangle([x, mid, x + w, y0 + 2 * h // 3], radius=4, fill=(40, 50, 70))
            draw.ellipse([x + w // 2 - 8, y0, x + w // 2 + 8, y0 + h], fill=(80, 180, 255))
        for (x, y0, text, r0, g0, b0) in self.state.labels:
            draw.text((x, y0), text, fill=(r0, g0, b0), font=font_b)

        # Persistent ball on top of maze shapes
        if self.state.ball:
            b = self.state.ball
            draw.ellipse(
                [b.x - b.r, b.y - b.r, b.x + b.r, b.y + b.r], fill=b.color
            )

        n = len(self.state.buttons)
        if n:
            if self.state.layout == "grid2":
                cols = int(getattr(self.state, "layout_cols", 2) or 2)
                if cols < 1:
                    cols = 1
                if cols > 4:
                    cols = 4
                gap_x, gap_y = 10, 8
                avail = LCD_W - 2 * SAFE_INSET - (cols - 1) * gap_x
                bw = max(72, min(170, avail // cols))
                bh = 44 if cols >= 4 else 40
                rows = (n + cols - 1) // cols
                total_h = rows * bh + (rows - 1) * gap_y
                start_y = LCD_H - SAFE_INSET - 70 - total_h
                start_x = (LCD_W - (cols * bw + (cols - 1) * gap_x)) // 2
                for i, btn in enumerate(self.state.buttons):
                    row, col = divmod(i, cols)
                    x = start_x + col * (bw + gap_x)
                    y0 = start_y + row * (bh + gap_y)
                    draw.rounded_rectangle(
                        [x, y0, x + bw, y0 + bh], radius=10, fill=(28, 39, 64)
                    )
                    draw.text(
                        (x + bw // 2, y0 + bh // 2),
                        btn.label,
                        fill=(244, 247, 251),
                        font=font_btn,
                        anchor="mm",
                    )
            else:
                bw, bh = 280, 44
                gap = 8
                total_h = n * bh + (n - 1) * gap
                start_y = LCD_H - SAFE_INSET - 70 - total_h
                x = (LCD_W - bw) // 2
                for i, btn in enumerate(self.state.buttons):
                    y0 = start_y + i * (bh + gap)
                    draw.rounded_rectangle(
                        [x, y0, x + bw, y0 + bh], radius=10, fill=(28, 39, 64)
                    )
                    draw.text(
                        (x + bw // 2, y0 + bh // 2),
                        btn.label,
                        fill=(244, 247, 251),
                        font=font_btn,
                        anchor="mm",
                    )

        r = CORNER_INSET
        draw.arc([0, 0, 2 * r, 2 * r], 180, 270, fill=(80, 80, 100), width=2)
        draw.arc([LCD_W - 2 * r, 0, LCD_W, 2 * r], 270, 360, fill=(80, 80, 100), width=2)
        draw.arc([0, LCD_H - 2 * r, 2 * r, LCD_H], 90, 180, fill=(80, 80, 100), width=2)
        draw.arc(
            [LCD_W - 2 * r, LCD_H - 2 * r, LCD_W, LCD_H], 0, 90, fill=(80, 80, 100), width=2
        )
        draw.rectangle(
            [SAFE_INSET, SAFE_INSET, LCD_W - SAFE_INSET, LCD_H - SAFE_INSET],
            outline=(60, 90, 120),
        )
        return img


    # ---- API expand (layout / input / prefs / net / stubs) -------------------

    def label(self, x, y, text, r=244, g=247, b=251) -> bool:
        self.state.labels.append((int(x), int(y), str(text), int(r), int(g), int(b)))
        return True

    def panel(self, x, y, w, h, r=20, g=28, b=40) -> bool:
        self.state.panels.append((int(x), int(y), int(w), int(h), int(r), int(g), int(b)))
        return True

    def progress(self, x, y, w, h, value=0):
        pid = self._prog_id % 4
        self._prog_id += 1
        value = max(0, min(100, int(value)))
        self._progress_vals[pid] = value
        self.state.progress = [p for p in self.state.progress if p[0] != pid]
        self.state.progress.append((pid, int(x), int(y), int(w), int(h), value))
        return pid

    def progress_set(self, pid, value) -> bool:
        pid = int(pid)
        value = max(0, min(100, int(value)))
        self._progress_vals[pid] = value
        updated = False
        newp = []
        for p in self.state.progress:
            if p[0] == pid:
                newp.append((pid, p[1], p[2], p[3], p[4], value))
                updated = True
            else:
                newp.append(p)
        self.state.progress = newp
        return updated

    def slider(self, x, y, w, h, minV=0, maxV=100, value=0, cb=None):
        sid = self._slider_id % 4
        self._slider_id += 1
        self.state.sliders.append((sid, int(x), int(y), int(w), int(h), int(minV), int(maxV), cb))
        return sid

    def z_raise(self, which="all") -> None:
        return None

    def on_back(self, fn=None) -> None:
        self._on_back = fn

    def on_long_press(self, fn=None) -> None:
        self._on_long_press = fn

    def gesture(self):
        g = self._gesture
        self._gesture = None
        return g

    def touch_delta(self):
        return dict(self._touch_delta)

    def sim_swipe(self, typ: str, dx: int = 100, dy: int = 0) -> None:
        self._gesture = {"type": typ, "dx": int(dx), "dy": int(dy)}

    def sim_long_press(self, x: int = 100, y: int = 100) -> None:
        if self._on_long_press:
            self._on_long_press(int(x), int(y))

    def prefs_get(self, key: str, default=None):
        if key in self._prefs:
            return self._prefs[key]
        return default

    def prefs_set(self, key: str, value: str) -> bool:
        if not key or len(key) > 15 or len(str(value)) > 128:
            return False
        self._prefs[str(key)] = str(value)
        return True

    def sd_remove(self, path: str) -> bool:
        if not self._sd_mounted or self._usb_msc:
            return False
        if not str(path).startswith("/sd/") or ".." in path:
            return False
        for k in list(self._sd.keys()):
            if k == path or k.startswith(str(path).rstrip("/") + "/"):
                del self._sd[k]
        return True

    def sd_rename(self, src: str, dst: str) -> bool:
        if not self._sd_mounted or self._usb_msc:
            return False
        if not src.startswith("/sd/") or not dst.startswith("/sd/"):
            return False
        if ".." in src or ".." in dst or src not in self._sd:
            return False
        self._sd[dst] = self._sd.pop(src)
        return True

    def now(self):
        t = time.localtime()
        return {
            "epoch": int(time.time()),
            "year": t.tm_year,
            "month": t.tm_mon,
            "day": t.tm_mday,
            "hour": t.tm_hour,
            "min": t.tm_min,
            "sec": t.tm_sec,
            "wday": t.tm_wday + 1,
            "rtc": False,
        }

    def set_alarm(self, *args):
        return (False, "planned")

    def wifi_state(self) -> str:
        return self._wifi

    def wifi_connect(self, ssid: str, password: str = "", timeout_ms: int = 12000):
        if not ssid:
            return (False, "no_ssid")
        self._wifi = "on"
        return (True, "127.0.0.1")

    def wifi_disconnect(self) -> bool:
        self._wifi = "off"
        return True

    def http_get(self, url: str, max_bytes: int = 32768):
        if self._wifi != "on":
            return (None, "wifi_off")
        max_bytes = max(0, min(int(max_bytes), 65536))
        body = f"sim-http-ok:{url}"[:max_bytes]
        return (body, 200)

    def audio_ready(self) -> bool:
        return True

    def mic_start(self, opts=None):
        rate = 16000
        if isinstance(opts, dict):
            rate = int(opts.get("rate", rate) or rate)
        elif opts is not None:
            try:
                rate = int(opts)
            except Exception:
                rate = 16000
        if rate < 8000:
            rate = 8000
        if rate > 48000:
            rate = 48000
        self._mic_running = True
        self._mic_rate = rate
        self._mic_frame = 0
        self._mic_pcm_pos = 0
        return True

    def mic_stop(self) -> bool:
        self._mic_running = False
        return True

    def _synth_pcm(self, n: int) -> list[int]:
        """Synthetic mono int16: noise floor + optional tone (mic_tone_hz)."""
        rate = float(self._mic_rate or 16000)
        tone = float(self.mic_tone_hz or 0.0)
        out: list[int] = []
        for i in range(n):
            t = (self._mic_pcm_pos + i) / rate
            # Cheap deterministic "noise" without RNG state
            noise = 0.05 * math.sin(t * 917.0) + 0.035 * math.sin(t * 1301.3 + 0.7)
            s = noise
            if tone > 0.0:
                s += 0.35 * math.sin(2.0 * math.pi * tone * t)
            v = int(max(-32767, min(32767, round(s * 32767.0))))
            out.append(v)
        self._mic_pcm_pos += n
        self._mic_frame += 1
        return out

    def mic_info(self):
        return {
            "sample_rate": int(self._mic_rate) if self._mic_running else 0,
            "bits": 16,
            "channels": 1,
            "running": bool(self._mic_running),
        }

    def mic_read(self, max_samples: int = 256):
        """Return (pcm_bytes LE int16, count) or (None, err) if mic not started."""
        if not self._mic_running:
            return (None, "mic_not_running")
        n = max(1, min(1024, int(max_samples if max_samples is not None else 256)))
        samples = self._synth_pcm(n)
        pcm = struct.pack("<" + "h" * n, *samples)
        return (pcm, n)

    def mic_read_table(self, max_samples: int = 256):
        """Return (list[int16], count) or (None, err) if mic not started."""
        if not self._mic_running:
            return (None, "mic_not_running")
        n = max(1, min(1024, int(max_samples if max_samples is not None else 256)))
        samples = self._synth_pcm(n)
        return (samples, n)

    def mic_spectrum(self, bins: int = 64):
        bins = max(8, min(128, int(bins)))
        if not self._mic_running:
            return (None, "mic_not_running")
        self._mic_frame += 1
        t = self._mic_frame * 0.05
        out = []
        # Noise floor + moving tone peak + low rumble
        peak_bin = int((0.35 + 0.25 * math.sin(t * 0.7)) * (bins - 1))
        tone = max(0.0, min(1.0, abs(math.sin(t * 2.0))))
        for i in range(bins):
            noise = 0.04 + 0.03 * abs(math.sin(t * 3.1 + i * 0.4))
            rumble = 0.12 * math.exp(-((i / max(1, bins * 0.15)) ** 2))
            dist = abs(i - peak_bin)
            spike = tone * math.exp(-(dist * dist) / 8.0)
            v = noise + rumble + spike
            if v > 1.0:
                v = 1.0
            out.append(float(v))
        return out

    def mic_fft(self, bins: int = 64):
        return self.mic_spectrum(bins)

    def _wav_header(self, data_bytes: int, rate: int = 16000, ch: int = 1, bits: int = 16) -> bytes:
        import struct as _st
        byte_rate = rate * ch * bits // 8
        block = ch * bits // 8
        riff = 36 + data_bytes
        return _st.pack(
            "<4sI4s4sIHHIIHH4sI",
            b"RIFF",
            riff,
            b"WAVE",
            b"fmt ",
            16,
            1,
            ch,
            rate,
            byte_rate,
            block,
            bits,
            b"data",
            data_bytes,
        )

    def mic_record_file(self, path: str, seconds: int = 3, rate: int = 16000):
        if not path or not str(path).startswith("/sd/"):
            return (False, "bad_path")
        if not self._sd_mounted:
            self.sd_mount()
        seconds = max(1, min(20, int(seconds or 3)))
        rate = int(rate or 16000)
        if rate not in (8000, 16000, 48000):
            rate = 16000
        # Ensure parent dir marker
        parent = str(path).rsplit("/", 1)[0]
        self.sd_mkdir(parent)
        was = self._mic_running
        self.mic_start({"rate": rate})
        n = seconds * rate
        samples = self._synth_pcm(n)
        pcm = struct.pack("<" + "h" * n, *samples)
        blob = self._wav_header(len(pcm), rate, 1, 16) + pcm
        self._sd[path] = blob
        if not was:
            self.mic_stop()
        return True

    def mic_record(self, *args):
        if args and isinstance(args[0], str):
            secs = args[1] if len(args) > 1 else 3
            rate = args[2] if len(args) > 2 else 16000
            return self.mic_record_file(args[0], secs, rate)
        return (None, "use_mic_spectrum_or_mic_read")

    def speaker_ready(self) -> bool:
        return True

    def speaker_start(self, opts=None):
        rate = 16000
        if isinstance(opts, dict):
            rate = int(opts.get("rate", rate) or rate)
        elif opts is not None:
            try:
                rate = int(opts)
            except Exception:
                rate = 16000
        if self._mic_running:
            self.mic_stop()
        self._speaker_running = True
        self._speaker_rate = rate
        return True

    def speaker_write(self, pcm) -> bool:
        if not self._speaker_running:
            return (False, "speaker_not_running")
        if not isinstance(pcm, (bytes, bytearray, str)):
            return (False, "bad_pcm")
        raw = pcm.encode("latin1") if isinstance(pcm, str) else bytes(pcm)
        if len(raw) == 0 or (len(raw) & 3) != 0:
            return (False, "bad_pcm")
        return True

    def speaker_stop(self) -> bool:
        self._speaker_running = False
        return True

    @staticmethod
    def _vol_pct_to_raw(percent: int) -> int:
        percent = max(0, min(100, int(percent)))
        return (percent * 0xBF + 50) // 100

    @staticmethod
    def _vol_raw_to_pct(raw: int) -> int:
        raw = max(0, min(0xBF, int(raw)))
        return (raw * 100 + 0xBF // 2) // 0xBF

    def speaker_volume(self, percent=None):
        """Get/set speaker volume 0..100% (maps to ES8311 0x00..0xBF). Session only."""
        if percent is None:
            return self._vol_raw_to_pct(self._speaker_vol_raw)
        try:
            pct = int(percent)
        except Exception:
            return (False, "bad_args")
        pct = max(0, min(100, pct))
        self._speaker_vol_raw = self._vol_pct_to_raw(pct)
        return True

    def speaker_volume_raw(self, raw=None):
        """Get/set raw ES8311 DAC reg 0x32 (0..0xBF). Lab helper."""
        if raw is None:
            return int(self._speaker_vol_raw) & 0xFF
        try:
            v = int(raw)
        except Exception:
            return (False, "bad_args")
        self._speaker_vol_raw = max(0, min(0xBF, v))
        return True

    def speaker_play(self, path: str = None, *args):
        if not path:
            return (False, "bad_path")
        if not str(path).startswith("/sd/"):
            return (False, "bad_path")
        if not self._sd_mounted:
            self.sd_mount()
        data = self._sd.get(path)
        if data is None:
            return (False, "sd_open_failed")
        if self._mic_running:
            self.mic_stop()
        self._speaker_last_path = path
        self._speaker_running = False  # play is one-shot (blocking)
        return True

    # ---- canvas (RGB buffer for waterfall) ---------------------------------

    @staticmethod
    def _heat_rgb(v: float):
        v = max(0.0, min(1.0, float(v)))
        x = v * 4.0
        seg = int(x)
        t = x - seg
        stops = [
            ((0, 0, 40), (0, 80, 255)),
            ((0, 80, 255), (0, 220, 180)),
            ((0, 220, 180), (180, 255, 40)),
            ((180, 255, 40), (255, 120, 0)),
            ((255, 120, 0), (255, 40, 40)),
        ]
        if seg >= 4:
            return stops[4][1]
        (r0, g0, b0), (r1, g1, b1) = stops[seg]
        return (
            int(r0 + (r1 - r0) * t),
            int(g0 + (g1 - g0) * t),
            int(b0 + (b1 - b0) * t),
        )

    def canvas(self, x, y, w, h) -> bool:
        w, h = max(8, int(w)), max(8, int(h))
        pixels = [(4, 6, 12)] * (w * h)
        self.state.canvas = (int(x), int(y), w, h, pixels)
        return True

    def canvas_clear(self, r=0, g=0, b=0) -> bool:
        c = self.state.canvas
        if not c:
            return False
        x, y, w, h, pixels = c
        col = (int(r) & 255, int(g) & 255, int(b) & 255)
        for i in range(w * h):
            pixels[i] = col
        self.state.canvas = (x, y, w, h, pixels)
        return True

    def canvas_scroll(self, dy: int) -> bool:
        c = self.state.canvas
        if not c:
            return False
        x, y, w, h, pixels = c
        dy = int(dy)
        if dy == 0:
            return True
        newp = [(0, 0, 0)] * (w * h)
        if dy < 0:
            shift = -dy
            if shift < h:
                for row in range(h - shift):
                    for col in range(w):
                        newp[row * w + col] = pixels[(row + shift) * w + col]
        else:
            shift = dy
            if shift < h:
                for row in range(shift, h):
                    for col in range(w):
                        newp[row * w + col] = pixels[(row - shift) * w + col]
        self.state.canvas = (x, y, w, h, newp)
        return True

    def canvas_row(self, y_row, mags) -> bool:
        c = self.state.canvas
        if not c:
            return False
        x, y, w, h, pixels = c
        y_row = int(y_row)
        if y_row < 0 or y_row >= h:
            return False
        if hasattr(mags, "values"):
            # lupa table
            n = len(mags)
            vals = [float(mags[i + 1]) for i in range(n)]
        else:
            vals = [float(v) for v in list(mags)]
        n = len(vals)
        if n < 1:
            return False
        for col in range(w):
            i0 = (col * n) // w
            if i0 >= n:
                i0 = n - 1
            pixels[y_row * w + col] = self._heat_rgb(vals[i0])
        self.state.canvas = (x, y, w, h, pixels)
        return True

    def canvas_pixel(self, px, py, r, g, b) -> bool:
        c = self.state.canvas
        if not c:
            return False
        x, y, w, h, pixels = c
        px, py = int(px), int(py)
        if px < 0 or py < 0 or px >= w or py >= h:
            return False
        pixels[py * w + px] = (int(r) & 255, int(g) & 255, int(b) & 255)
        self.state.canvas = (x, y, w, h, pixels)
        return True

    # ---- Meshtastic BLE mesh (sim loopback) ----
    def ble_ready(self) -> bool:
        if not hasattr(self, "_ble_st"):
            self._ble_st = "idle"
            self._ble_hits = [
                {"addr": "AA:BB:CC:DD:EE:01", "name": "Meshtastic_sim", "rssi": -55},
                {"addr": "AA:BB:CC:DD:EE:02", "name": "T1000-E", "rssi": -68},
            ]
            self._ble_conn = None
            self._mesh_q = []
            self._mesh_ch = 1  # default Family secondary
            self._mesh_alert_beep = True
            self._mesh_alert_vibrate = True
            self._mesh_bg_alerts = []
            self._notif_app_id = None
            self._notif_text = None
            self._alert_uh_oh_count = 0
        return True

    def mesh_channel(self, n=None):
        self.ble_ready()
        if n is None:
            return int(self._mesh_ch)
        n = max(0, min(7, int(n)))
        self._mesh_ch = n
        return n

    def mesh_alert_beep(self, on=None):
        self.ble_ready()
        if on is None:
            return bool(self._mesh_alert_beep)
        self._mesh_alert_beep = bool(on)
        return self._mesh_alert_beep

    def mesh_alert_vibrate(self, on=None):
        self.ble_ready()
        if on is None:
            return bool(self._mesh_alert_vibrate)
        self._mesh_alert_vibrate = bool(on)
        return self._mesh_alert_vibrate

    def mesh_inject_peer(self, text, from_id="!simpeer"):
        """Sim helper: inbound TEXT → queue + optional bg alert record."""
        self.ble_ready()
        self._mesh_q.append({"from": from_id, "text": str(text), "time": 0})
        self._mesh_bg_alerts.append(str(text))
        if self._mesh_alert_beep:
            self.alert_uh_oh()
        if self._mesh_alert_vibrate:
            print("[sim] mesh alert VIBRATE")
        self.notify("mesh_chat", text)
        return True

    def notify(self, app_id, text=""):
        """Generic notification banner associated with app_id (swipe-down opens it)."""
        self._notif_app_id = str(app_id or "")
        self._notif_text = str(text or "")
        print(f"[sim] notify app={self._notif_app_id!r} text={self._notif_text!r}")
        return True

    def notif_clear(self):
        self._notif_app_id = None
        self._notif_text = None
        return True

    def notif_app_id(self):
        return self._notif_app_id

    def beep(self, ms=100, hz=880):
        print(f"[sim] beep ms={int(ms)} hz={int(hz)} (vol={self.speaker_volume()}%)")
        return True

    def alert_uh_oh(self):
        """ICQ-style ascending then descending two-tone alert."""
        self._alert_uh_oh_count = int(getattr(self, "_alert_uh_oh_count", 0)) + 1
        # Motif mirrors firmware: 660→990 (asc) → 520 (desc), full volume.
        print("[sim] alert UH-OH 660Hz/85ms → 990Hz/95ms → 520Hz/180ms (vol=100%)")
        return True

    def sim_notif_swipe_down(self):
        """Sim: swipe-down on active notification → set launch intent + return app id."""
        if not self._notif_app_id:
            return None
        app = self._notif_app_id
        text = self._notif_text or ""
        self._launch_action = "reply"
        self._launch_text = str(text)
        self.notif_clear()
        print(f"[sim] notif swipe-down → open {app!r} launch=reply text={text!r}")
        return app

    def launch_action(self):
        """One-shot pending launch action (e.g. reply from notif swipe)."""
        a = getattr(self, "_launch_action", None)
        self._launch_action = None
        return a

    def launch_text(self):
        """One-shot pending launch text (notif body)."""
        t = getattr(self, "_launch_text", None)
        self._launch_text = None
        return t

    def ble_start(self, *args):
        self.ble_ready()
        self._ble_st = "scanning"
        return True

    def ble_stop(self) -> bool:
        self.ble_ready()
        self._ble_st = "idle"
        self._ble_conn = None
        return True

    def ble_status(self) -> str:
        self.ble_ready()
        return self._ble_st

    def ble_scan(self, timeout_ms=4000):
        self.ble_ready()
        self._ble_st = "idle"
        return list(self._ble_hits)

    def ble_connect(self, addr, pin=None):
        self.ble_ready()
        if not addr:
            self._ble_st = "error"
            return (False, "bad_addr")
        # pin nil/omit = NO_PIN; any 4-6 digit string accepted in sim
        if pin is not None and pin != "":
            s = str(pin)
            if not s.isdigit() or not (4 <= len(s) <= 6):
                self._ble_st = "error"
                return (False, "bad_pin")
        self._ble_conn = str(addr)
        self._ble_st = "connected"
        self._mesh_q.append(
            {"from": "!sim0001", "text": "[sim] connected " + str(addr), "time": 0}
        )
        return True

    def ble_disconnect(self) -> bool:
        self.ble_ready()
        self._ble_conn = None
        self._ble_st = "idle"
        return True

    def mesh_connected(self) -> bool:
        self.ble_ready()
        return self._ble_conn is not None and self._ble_st == "connected"

    def mesh_send(self, text):
        self.ble_ready()
        if not self.mesh_connected():
            return (False, "not_connected")
        if text is None or str(text) == "":
            return (False, "empty")
        # Loopback: outbound echo + fake peer reply for UI testing
        self._mesh_q.append({"from": "!me", "text": str(text), "time": 0})
        self._mesh_q.append(
            {"from": "!simpeer", "text": "ack: " + str(text)[:40], "time": 0}
        )
        return True

    def mesh_poll(self):
        self.ble_ready()
        out = list(self._mesh_q)
        self._mesh_q.clear()
        return out

    def speech_to_text(self, secs: int = 3):
        """Sim: deterministic fake phrase so Mesh Chat Speak UI is testable."""
        _ = secs
        url = self._prefs.get("stt_url") if hasattr(self, "_prefs") else None
        # Always return a fake transcript in sim (UI path); note when URL missing
        # via second path only if explicitly requested — sim always succeeds.
        return "on my way"

    def install_zip(self, path: str, app_id: str, dest: str = "lfs"):
        if not app_id or ".." in app_id:
            return (False, "bad app id")
        if dest not in ("lfs", "sd"):
            return (False, "dest must be lfs|sd")
        self._prefs[f"zip_{app_id}"] = f"{dest}:{path}"
        return (True, "ok")

    def brightness(self, percent=None):
        if percent is None:
            return int(self._brightness)
        percent = max(0, min(100, int(percent)))
        self._brightness = percent
        return percent

    def idle_timeout(self, sec=None):
        if sec is None:
            return int(self._idle_timeout)
        sec = max(0, min(3600, int(sec)))
        self._idle_timeout = sec
        return sec

    def sprite(self, path, x, y, fw, fh, frame=0, max_w=None, max_h=None) -> bool:
        mw = int(max_w or fw)
        mh = int(max_h or fh)
        self.state.image_box = (int(x), int(y), min(int(fw), mw), min(int(fh), mh))
        return True

    def image_frame(self, path, x, y, fw, fh, frame=0, max_w=None, max_h=None) -> bool:
        return self.sprite(path, x, y, fw, fh, frame, max_w, max_h)

    _GPIO_SAFE = {10, 16, 19, 20}

    def gpio_pwm(self, pin, duty, freq=5000):
        pin = int(pin)
        if pin not in self._GPIO_SAFE:
            return (False, "pin_not_allowed")
        self._gpio[pin] = max(0, min(255, int(duty)))
        return True

    def gpio_adc(self, pin):
        pin = int(pin)
        if pin not in self._GPIO_SAFE:
            return (None, "pin_not_allowed")
        return int(self._gpio.get(pin, 0))

    def gpio_write(self, pin, val) -> bool:
        pin = int(pin)
        if pin not in self._GPIO_SAFE:
            return False
        self._gpio[pin] = 1 if val else 0
        return True

    def gpio_read(self, pin):
        pin = int(pin)
        if pin not in self._GPIO_SAFE:
            return None
        return int(self._gpio.get(pin, 0))

    def time(self):
        n = self.now()
        return {k: n[k] for k in ("hour", "min", "sec", "day", "month", "year", "wday")}

    def battery_pct(self):
        return self.battery()

    def battery_mv(self):
        return 4000

    def charging(self):
        return False

    def usb_power(self):
        return True

    def battery_connected(self):
        return True

    def touch(self):
        if self._touch_delta.get("pressed"):
            return {"x": 100, "y": 100, "pressed": True}
        return None


    def _tilt_pad_geom(self):
        cx = LCD_W * SCALE + PANEL_W // 2
        cy = 200
        rad = 90
        return cx, cy, rad

    def render(self) -> Image.Image:
        """Full canvas: 2x LCD + tilt pad panel (matches historical 1100x1004 shots)."""
        canvas = Image.new("RGB", (CANVAS_W, CANVAS_H), (24, 28, 38))
        lcd = self.render_lcd().resize((LCD_W * SCALE, LCD_H * SCALE), Image.NEAREST)
        canvas.paste(lcd, (0, 0))
        draw = ImageDraw.Draw(canvas)
        _ft, _fb, _fbtn, font_sm = self._fonts()
        try:
            font_title = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 22
            )
        except Exception:
            font_title = font_sm

        x0 = LCD_W * SCALE
        draw.line([(x0, 0), (x0, CANVAS_H)], fill=(50, 60, 80), width=2)
        draw.text((x0 + 24, 24), "Tilt pad", fill=(80, 180, 255), font=font_title)
        draw.text((x0 + 24, 56), "Drag the stick to tip", fill=(140, 150, 170), font=font_sm)
        draw.text((x0 + 24, 76), "Arrows still work / --shot autoplay", fill=(140, 150, 170), font=font_sm)

        cx, cy, rad = self._tilt_pad_geom()
        draw.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=(40, 50, 70), outline=(70, 90, 120), width=2)
        draw.line([(cx - rad, cy), (cx + rad, cy)], fill=(60, 70, 90), width=1)
        draw.line([(cx, cy - rad), (cx, cy + rad)], fill=(60, 70, 90), width=1)
        sx = int(cx + (self.imu_ay / self.tilt_max_g) * rad)
        sy = int(cy + (self.imu_ax / self.tilt_max_g) * rad)
        draw.ellipse([sx - 16, sy - 16, sx + 16, sy + 16], fill=(80, 180, 255), outline=(255, 255, 255), width=2)
        draw.text(
            (x0 + 24, 300),
            f"ax={self.imu_ax:+.2f}  ay={self.imu_ay:+.2f}  az={self.imu_az:+.2f}",
            fill=(244, 247, 251),
            font=font_sm,
        )
        draw.text(
            (x0 + 24, 318),
            f"gx={self.imu_gx:+.1f}  gy={self.imu_gy:+.1f}  gz={self.imu_gz:+.1f} dps",
            fill=(180, 200, 220),
            font=font_sm,
        )
        zb = [x0 + 40, 345, x0 + PANEL_W - 40, 385]
        draw.rounded_rectangle(zb, radius=10, fill=(28, 39, 64), outline=(60, 90, 140))
        draw.text(
            ((zb[0] + zb[2]) // 2, (zb[1] + zb[3]) // 2),
            "Center / Zero",
            fill=(244, 247, 251),
            font=font_sm,
            anchor="mm",
        )
        draw.text(
            (x0 + 16, CANVAS_H - 36),
            "Swipe right on watch = back",
            fill=(140, 150, 170),
            font=font_sm,
        )
        return canvas



    # ---- Background jobs (sim: complete after a few poll ticks) -------------
    # Firmware pins the worker to the non-UI core; sim just defers results.

    def job_start(self, name, opts=None):
        import math, random
        opts = opts or {}
        bins = int(opts.get("bins", 64)) if isinstance(opts, dict) else 64
        if bins < 8:
            bins = 8
        if bins > 128:
            bins = 128
        name = str(name or "")
        if name not in ("fft", "mic_fft", "rms"):
            return None, "unknown_job"
        if not hasattr(self, "_jobs"):
            self._jobs = {}
            self._job_next = 1
        jid = self._job_next
        self._job_next += 1
        kind = "fft" if name in ("fft", "mic_fft") else "rms"
        # complete after 2 tick polls
        self._jobs[jid] = {
            "status": "queued",
            "kind": kind,
            "bins": bins,
            "ticks_left": 2,
            "result": None,
            "err": None,
        }
        return jid, None

    def job_status(self, jid):
        j = getattr(self, "_jobs", {}).get(int(jid)) if jid else None
        return j["status"] if j else "unknown"

    def _job_advance(self):
        jobs = getattr(self, "_jobs", None)
        if not jobs:
            return
        import math, random
        for j in jobs.values():
            if j["status"] in ("done", "error", "cancelled"):
                continue
            if j["status"] == "queued":
                j["status"] = "running"
            j["ticks_left"] -= 1
            if j["ticks_left"] > 0:
                continue
            if j["kind"] == "fft":
                n = j["bins"]
                j["result"] = [max(0.0, min(1.0, random.random() * 0.3 + (0.7 if i == n // 4 else 0))) for i in range(n)]
            else:
                j["result"] = float(random.uniform(100.0, 4000.0))
            j["status"] = "done"

    def job_result(self, jid):
        self._job_advance()
        j = getattr(self, "_jobs", {}).get(int(jid)) if jid else None
        if not j:
            return None, "unknown_id"
        if j["status"] == "cancelled":
            return None, "cancelled"
        if j["status"] == "error":
            return None, j.get("err") or "error"
        if j["status"] != "done":
            return None, "not_ready"
        res = j["result"]
        # consume
        del self._jobs[int(jid)]
        return res, None

    def job_cancel(self, jid):
        j = getattr(self, "_jobs", {}).get(int(jid)) if jid else None
        if not j:
            return False
        j["status"] = "cancelled"
        return True

# WASM apps: firmware-only (wasm3). This sim does not load .wasm; see docs/WASM_APPS.md.
