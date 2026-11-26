#!/usr/bin/env python3
"""Inventory known PS1 signatures inside the extracted archives and files.

  python3 tools/inventory_data.py > decomp-build/inventory.txt
"""
import collections
import glob
import os
import re
import struct

SIGS = {
    b"pBAV": "VAB sound bank",
    b"pQES": "SEQ music sequence",
    b"\x10\x00\x00\x00\x08\x00\x00\x00": "TIM 4/8-bit image",
    b"\x10\x00\x00\x00\x09\x00\x00\x00": "TIM 4/8-bit image with CLUT (flag 9)",
    b"\x10\x00\x00\x00\x02\x00\x00\x00": "TIM 16-bit image",
    b"\x10\x00\x00\x00\x03\x00\x00\x00": "TIM 24-bit image",
    b"\x41\x00\x00\x00": "TMD model (id 0x41)",
    b"\x00\x00\x01\xba": "MPEG pack",
}


def scan(path, name, counts, examples):
    d = open(path, "rb").read()
    for sig, label in SIGS.items():
        n = 0
        start = 0
        while True:
            k = d.find(sig, start)
            if k < 0:
                break
            if label.startswith("TMD") and k % 4:
                start = k + 1
                continue
            n += 1
            if len(examples[(name, label)]) < 3:
                examples[(name, label)].append(k)
            start = k + 4
        if n:
            counts[(name, label)] += n


def main():
    counts = collections.Counter()
    examples = collections.defaultdict(list)
    for p in sorted(glob.glob("extracted/files/**/*", recursive=True)):
        if os.path.isfile(p) and not p.endswith((".XA", ".STR")):
            scan(p, os.path.relpath(p, "extracted/files"), counts, examples)
    for (name, label), n in sorted(counts.items()):
        ex = ", ".join(hex(x) for x in examples[(name, label)])
        print(f"{name:20} {label:42} {n:6}  e.g. {ex}")
    # raw sector streams
    for xa in ("STEREO.XA", "MONO.XA"):
        p = f"extracted/files/{xa}"
        if os.path.exists(p):
            d = open(p, "rb").read(2352 * 4000)
            chans = collections.Counter((d[i + 16], d[i + 17]) for i in range(0, len(d) - 2352, 2352))
            print(f"{xa}: first 4000 sectors, (file,channel) pairs: {len(chans)} -> {sorted(chans)[:8]}")


if __name__ == "__main__":
    main()
