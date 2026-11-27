#!/usr/bin/env python3
"""Check every guest call of the rewritten code against the original binary.

For each a2_callN(cpu, SITE, TARGET, ...) in src/game/*.c, the original instruction at SITE
must be `jal TARGET` (TARGET may be a #define of the same file). Also checks that each
A2_REWRITE address is the start of a function known to splat.

    python3 tools/check_calls.py
"""
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import a2lift

IMAGES = []
for _mod, _path, _skip, _base, _asm in a2lift.modules():
    if os.path.exists(_path):
        _data = open(_path, "rb").read()[_skip:]
        IMAGES.append((_base, _data))


def words_at(addr):
    """The word at `addr` in every module loaded over it (overlays share addresses)."""
    out = []
    for base, data in IMAGES:
        off = addr - base
        if 0 <= off <= len(data) - 4:
            out.append(struct.unpack_from("<I", data, off)[0])
    return out


def is_call(word, site, target):
    if target is None:                                  # through a pointer: jalr
        return word >> 26 == 0 and word & 0x3F == 9
    return word >> 26 == 3 and (((word & 0x03FFFFFF) << 2) | (site & 0xF0000000)) == target


def known_functions():
    starts = set()
    for path in ["decomp/asm/main/3110.s"] + glob.glob("decomp/asm/*/code.s"):
        text = open(path).read()
        for m in re.finditer(r"^glabel \S+\n(?:.*\n)*?\s+/\* [0-9A-F]+ ([0-9A-F]{8}) ", text, re.M):
            starts.add(int(m.group(1), 16))
    return starts


def main():
    bad = checked = 0
    starts = known_functions()
    for path in sorted(glob.glob("src/game/**/*.c", recursive=True)):
        src = open(path).read()
        defs = {m.group(1): int(m.group(2), 16)
                for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)u?", src)}
        for m in re.finditer(r"a2_call\d?\(\s*cpu\s*,\s*(0x[0-9A-Fa-f]+)u?\s*,\s*(\w+)", src):
            site = int(m.group(1), 16)
            name = m.group(2)
            target = int(name, 16) if name.startswith("0x") else defs.get(name)
            if target is None and name.isupper():
                print(f"{path}: call at {site:#010x}: unknown target {name}")
                bad += 1
                continue
            checked += 1
            if not any(is_call(w, site, target) for w in words_at(site)):
                print(f"{path}: call at {site:#010x} to {name}: no matching jal/jalr in the original")
                bad += 1
        for m in re.finditer(r"A2_REWRITE(?:_OVERLAY)?\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)u?", src):
            checked += 1
            if int(m.group(1), 16) not in starts:
                print(f"{path}: {m.group(1)} is not a function start")
                bad += 1
    print(f"{checked} checks, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
