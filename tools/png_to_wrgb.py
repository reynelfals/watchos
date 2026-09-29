#!/usr/bin/env python3
"""Convert an image to 128x128 little-endian RGB565 WRGB."""
from pathlib import Path
import struct, sys
from PIL import Image, ImageOps

def rgb565(r,g,b):
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3)

def convert(src, dst, size=128):
    im=Image.open(src).convert("RGB")
    im=ImageOps.fit(im,(size,size),method=Image.Resampling.LANCZOS,centering=(0.5,0.5))
    with open(dst,"wb") as f:
        f.write(b"WRGB")
        f.write(struct.pack("<HH",size,size))
        for r,g,b in im.getdata(): f.write(struct.pack("<H",rgb565(r,g,b)))

if __name__=="__main__":
    if len(sys.argv) not in (3,4):
        raise SystemExit("usage: png_to_wrgb.py INPUT OUTPUT [SIZE]")
    convert(sys.argv[1],sys.argv[2],int(sys.argv[3]) if len(sys.argv)==4 else 128)
