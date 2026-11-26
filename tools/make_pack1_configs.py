#!/usr/bin/env python3
"""splat configs for the code modules stored in PACK1.PAK (all loaded at 0x80149AE8).

Code range of each entry = first function prologue .. last `jr $ra` (+ delay slot).
"""
import glob
import struct
import subprocess
import sys

BASE = 0x80149AE8
ENTRIES = [1, 2, 11, 13, 17, 18, 21, 23, 24, 25, 26, 30, 31, 32, 33, 34, 35]

for idx in ENTRIES:
    path = glob.glob(f"extracted/archives/PACK1/{idx:04d}.*")[0]
    d = open(path, "rb").read()
    w = struct.unpack("<%dI" % (len(d) // 4), d[:len(d) // 4 * 4])
    pro = [i * 4 for i, x in enumerate(w) if (x >> 16) == 0x27BD and (x & 0x8000)]
    jr = [i * 4 for i, x in enumerate(w) if x == 0x03E00008]
    lo, hi = pro[0], jr[-1] + 8
    subprocess.run([sys.executable, "tools/make_overlay_config.py", f"PACK1_{idx:02d}", "--src", path,
                    "--vram", hex(BASE), "--code", hex(lo), hex(hi)], check=True, stdout=subprocess.DEVNULL)
print(f"{len(ENTRIES)} configs written")
