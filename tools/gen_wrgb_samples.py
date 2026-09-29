#!/usr/bin/env python3
"""Generate sample WRGB thumbs for Squish ID (LittleFS samples)."""
from __future__ import annotations
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data" / "apps" / "squish_id" / "img"

SAMPLES = [
    ("Aaliyah", (40, 120, 200), (180, 230, 255)),
    ("Ally", (255, 140, 170), (255, 220, 230)),
    ("Aarin", (210, 180, 140), (255, 240, 210)),
    ("Aaron", (80, 160, 90), (200, 255, 180)),
    ("Abbitt", (120, 90, 60), (220, 190, 140)),
    ("Abby", (90, 60, 160), (200, 180, 255)),
]

def slugify(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name.lower())

def rgb_to_rgb565(r: int, g: int, b: int) -> int:
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
                f.write(struct.pack("<H", rgb_to_rgb565(r, g, b)))

def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 48
        )
        font_sm = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18
        )
    except Exception:
        font = ImageFont.load_default()
        font_sm = font
    for name, bg, accent in SAMPLES:
        im = Image.new("RGB", (128, 128), bg)
        dr = ImageDraw.Draw(im)
        dr.ellipse((16, 16, 112, 112), fill=accent)
        initial = name[0].upper()
        bbox = dr.textbbox((0, 0), initial, font=font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        dr.text(((128 - tw) / 2, (128 - th) / 2 - 4), initial, fill=bg, font=font)
        bbox2 = dr.textbbox((0, 0), name, font=font_sm)
        tw2 = bbox2[2] - bbox2[0]
        dr.text(((128 - tw2) / 2, 104), name, fill=(20, 20, 30), font=font_sm)
        out = OUT / f"{slugify(name)}.wrgb"
        write_wrgb(out, im)
        print(out, out.stat().st_size)

if __name__ == "__main__":
    main()
