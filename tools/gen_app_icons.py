#!/usr/bin/env python3
"""Generate 72x72 letter-tile WRGB icons for shipped apps under data/apps/*."""
from __future__ import annotations
import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "data" / "apps"

# Distinct colors per app folder (RGB)
COLORS = {
    "tilt_maze": ((30, 90, 160), (120, 200, 255)),
    "prefs_lab": ((90, 50, 140), (210, 180, 255)),
    "layout_lab": ((20, 120, 100), (140, 240, 210)),
    "counter": ((160, 70, 40), (255, 190, 140)),
    "squish_id": ((200, 80, 140), (255, 200, 230)),
    "sd_lab": ((50, 110, 50), (180, 240, 160)),
    "gpio_lab": ((110, 110, 40), (230, 230, 120)),
    "uart_chat": ((40, 90, 130), (160, 210, 255)),
    "mesh_chat": ((20, 70, 50), (120, 230, 160)),
    "kidcoder": ((180, 100, 30), (255, 210, 120)),
    "brainfuck": ((80, 40, 40), (220, 140, 140)),
    "power_lab": ((140, 40, 40), (255, 160, 120)),
    "_example_hello": ((40, 80, 140), (140, 200, 255)),
    "usb_sd": ((60, 60, 100), (180, 180, 220)),
    "waterfall": ((20, 40, 90), (80, 220, 255)),
    "pcm_lab": ((30, 70, 50), (120, 255, 180)),
    "sound_deck": ((40, 30, 90), (160, 140, 255)),
    "portal": ((20, 30, 55), (120, 180, 220)),
}

SIZE = 72


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def write_wrgb(path: Path, img: Image.Image) -> None:
    img = img.convert("RGB")
    w, h = img.size
    px = img.load()
    with path.open("wb") as f:
        f.write(b"WRGB")
        f.write(struct.pack("<HH", w, h))
        for y in range(h):
            for x in range(w):
                r, g, b = px[x, y]
                f.write(struct.pack("<H", rgb565(r, g, b)))


def letter_for(folder: str, name: str) -> str:
    if name:
        return name[0].upper()
    for ch in folder:
        if ch.isalnum():
            return ch.upper()
    return "?"


def main() -> None:
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 36
        )
    except Exception:
        font = ImageFont.load_default()

    for folder in sorted(APPS.iterdir()):
        if not folder.is_dir():
            continue
        app_json = folder / "app.json"
        if not app_json.exists():
            continue
        data = json.loads(app_json.read_text())
        name = data.get("name") or data.get("id") or folder.name
        bg, accent = COLORS.get(folder.name, ((70, 70, 90), (180, 180, 200)))
        im = Image.new("RGB", (SIZE, SIZE), bg)
        dr = ImageDraw.Draw(im)
        dr.rounded_rectangle((4, 4, SIZE - 5, SIZE - 5), radius=14, fill=accent)
        letter = letter_for(folder.name, name)
        bbox = dr.textbbox((0, 0), letter, font=font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        dr.text(((SIZE - tw) / 2, (SIZE - th) / 2 - 2), letter, fill=bg, font=font)
        out = folder / "icon.wrgb"
        write_wrgb(out, im)
        data["icon"] = "icon.wrgb"
        app_json.write_text(json.dumps(data, indent=2) + "\n")
        print(f"{out} ({out.stat().st_size} bytes)  icon in app.json")


if __name__ == "__main__":
    main()
