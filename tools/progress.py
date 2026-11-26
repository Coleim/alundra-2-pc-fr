#!/usr/bin/env python3
"""Decompilation progress per module: game code vs library code, names, rewritten functions.

Library code = the PsyQ library section of the main executable: everything from the first
object recognised by tools/psyq_sigs.py (symbols/psyq_objects.json) upwards, since the linker
places the libraries after the game objects. "sig" counts the functions inside a recognised
object; "other" the rest of the section (small stubs, static functions, the sound driver).
Library code is replaced by native code in phase 4; game code is rewritten in phase 3.

A function counts as rewritten when src/ contains a C file defining it
(`/* REWRITTEN: func_XXXXXXXX */` marker comment, or a symbol listed in symbols/rewritten.txt).
"""
import glob
import json
import os
import re
import sys

MODULES = {"main": "decomp/asm/main/3110.s"}
for _p in sorted(glob.glob("decomp/asm/*/code.s")):
    MODULES[os.path.basename(os.path.dirname(_p))] = _p

GLABEL = re.compile(r"^glabel\s+(\S+)")
ADDR = re.compile(r"^\s+/\* [0-9A-F]+ ([0-9A-F]{8}) ")


def functions(path):
    """[(name, address)] in file order."""
    out = []
    try:
        for line in open(path):
            m = GLABEL.match(line)
            if m:
                out.append([m.group(1), None])
                continue
            m = ADDR.match(line)
            if m and out and out[-1][1] is None:
                out[-1][1] = int(m.group(1), 16)
    except FileNotFoundError:
        pass
    return [(n, a) for n, a in out if a is not None]


def library_objects():
    try:
        objs = json.load(open("symbols/psyq_objects.json"))
    except FileNotFoundError:
        return {}
    by_mod = {}
    for o in objs:
        a = int(o["addr"], 16)
        by_mod.setdefault(o["module"], []).append((a, a + o["size"]))
    return by_mod


def main():
    rewritten = set()
    for p in glob.glob("src/**/*.c", recursive=True):
        rewritten.update(re.findall(r"REWRITTEN:\s*(\S+)", open(p).read()))
    if os.path.exists("symbols/rewritten.txt"):
        rewritten.update(l.split()[0] for l in open("symbols/rewritten.txt") if l.strip())
    objs = library_objects()
    lib_start = min((lo for lo, _ in objs.get("main", [])), default=None)

    cols = ("functions", "game", "lib-sig", "lib-other", "named", "rewritten")
    print(f"{'module':10} " + " ".join(f"{c:>9}" for c in cols))
    tot = dict.fromkeys(cols, 0)
    for name, asm in MODULES.items():
        fs = functions(asm)
        iv = objs.get(name, [])
        row = dict.fromkeys(cols, 0)
        for fn, a in fs:
            row["functions"] += 1
            row["named"] += not fn.startswith("func_")
            row["rewritten"] += fn in rewritten
            if any(lo <= a < hi for lo, hi in iv):
                row["lib-sig"] += 1
            elif name == "main" and lib_start is not None and a >= lib_start:
                row["lib-other"] += 1
            else:
                row["game"] += 1
        print(f"{name:10} " + " ".join(f"{row[c]:>9}" for c in cols))
        for c in cols:
            tot[c] += row[c]
    print(f"{'total':10} " + " ".join(f"{tot[c]:>9}" for c in cols))
    print(f"game code rewritten: {tot['rewritten']} of {tot['game']} "
          f"({100.0 * tot['rewritten'] / max(tot['game'], 1):.2f}%); "
          f"library: {tot['lib-sig'] + tot['lib-other']} functions "
          f"({tot['lib-sig']} recognised by signature)")


if __name__ == "__main__":
    sys.exit(main())
