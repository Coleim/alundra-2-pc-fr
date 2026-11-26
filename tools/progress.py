#!/usr/bin/env python3
"""Decompilation progress per module: named vs automatic names, and rewritten functions.

A function counts as rewritten when src/ contains a C file defining it
(`/* REWRITTEN: func_XXXXXXXX */` marker comment, or a symbol listed in symbols/rewritten.txt).
"""
import glob
import os
import re
import sys

MODULES = {"main": "decomp/asm/main/3110.s", "game00": "decomp/asm/game00/code.s",
           "event00": "decomp/asm/event00/code.s", "game01": "decomp/asm/game01/code.s",
           "game02": "decomp/asm/game02/code.s", "game03": "decomp/asm/game03/code.s"}


def labels(path):
    try:
        return re.findall(r"^glabel\s+(\S+)", open(path).read(), re.M)
    except FileNotFoundError:
        return []


def main():
    rewritten = set()
    for p in glob.glob("src/**/*.c", recursive=True):
        rewritten.update(re.findall(r"REWRITTEN:\s*(\S+)", open(p).read()))
    if os.path.exists("symbols/rewritten.txt"):
        rewritten.update(l.split()[0] for l in open("symbols/rewritten.txt") if l.strip())
    tot = [0, 0, 0]
    print(f"{'module':10} {'functions':>9} {'named':>6} {'auto':>6} {'rewritten':>10}")
    for name, asm in MODULES.items():
        ls = labels(asm)
        auto = sum(1 for l in ls if l.startswith("func_"))
        done = sum(1 for l in ls if l in rewritten)
        print(f"{name:10} {len(ls):>9} {len(ls) - auto:>6} {auto:>6} {done:>10}")
        tot[0] += len(ls); tot[1] += len(ls) - auto; tot[2] += done
    print(f"{'total':10} {tot[0]:>9} {tot[1]:>6} {tot[0] - tot[1]:>6} {tot[2]:>10}"
          f"   ({100.0 * tot[2] / max(tot[0], 1):.1f}% rewritten)")


if __name__ == "__main__":
    sys.exit(main())
