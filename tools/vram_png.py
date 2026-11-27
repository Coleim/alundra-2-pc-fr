#!/usr/bin/env python3
"""Turn a VRAM dump of the standalone engine (alundra2 --dump-vram FILE: 1024x512 pixels,
15-bit little-endian) into a PNG.

  python3 tools/vram_png.py vram.bin out.png [--area X Y W H] [--scale N]
"""
import argparse
import struct

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vram")
    ap.add_argument("png")
    ap.add_argument("--area", type=int, nargs=4, metavar=("X", "Y", "W", "H"))
    ap.add_argument("--scale", type=int, default=1)
    a = ap.parse_args()
    data = open(a.vram, "rb").read()
    px = struct.unpack("<%dH" % (len(data) // 2), data)
    img = Image.new("RGB", (1024, 512))
    img.putdata([((p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3) for p in px])
    if a.area:
        x, y, w, h = a.area
        img = img.crop((x, y, x + w, y + h))
    if a.scale > 1:
        img = img.resize((img.width * a.scale, img.height * a.scale), Image.NEAREST)
    img.save(a.png)


if __name__ == "__main__":
    main()
