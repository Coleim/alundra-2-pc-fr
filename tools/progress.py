#!/usr/bin/env python3
"""Decompilation progress per module: game code vs library code, names, rewritten functions.

Library code = the PsyQ library section of the main executable: everything from the first
object recognised by tools/psyq_sigs.py (symbols/psyq_objects.json) upwards, since the linker
places the libraries after the game objects. "sig" counts the functions inside a recognised
object; "other" the rest of the section (small stubs, static functions, the sound driver).
Library code is replaced by native code in phase 4; game code is rewritten in phase 3.

A function counts as rewritten when src/ contains a C file defining it
(`/* REWRITTEN: func_XXXXXXXX */` marker comment, A2_REWRITE in src/game/, or a symbol listed in
symbols/rewritten.txt), and as lifted when tools/a2lift.py translated it (src/game/lifted/).
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
    """[(name, address)] in file order (alignment padding left out)."""
    out, words = [], {}
    try:
        for line in open(path):
            m = GLABEL.match(line)
            if m:
                out.append([m.group(1), None])
                continue
            m = ADDR.match(line)
            if m and out:
                if out[-1][1] is None:
                    out[-1][1] = int(m.group(1), 16)
                w = re.match(r"^\s+/\* [0-9A-F]+ [0-9A-F]{8} ([0-9A-F]{8}) \*/", line)
                if w and w.group(1) != "00000000":
                    words[out[-1][0]] = 1
    except FileNotFoundError:
        pass
    return [(n, a) for n, a in out if a is not None and n in words]


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
    rewritten, lifted = set(), set()
    for p in glob.glob("src/**/*.c", recursive=True):
        text = open(p).read()
        if "/lifted/" in p:
            lifted.update(re.findall(r'\{ 0x[0-9A-F]+u, "(\w+)"', text))
            continue
        rewritten.update(re.findall(r"REWRITTEN:\s*(\S+)", text))
        rewritten.update(re.findall(r"A2_REWRITE(?:_OVERLAY)?\(\s*(\w+)\s*,", text))
    if os.path.exists("symbols/rewritten.txt"):
        rewritten.update(l.split()[0] for l in open("symbols/rewritten.txt") if l.strip())
    objs = library_objects()
    starts = [lo for lo, _ in objs.get("main", [])]
    if os.path.exists("symbols/bios_thunks.txt"):      # libapi BIOS-call stubs open the section
        starts += [int(m, 16) for m in re.findall(r"=\s*(0x[0-9A-Fa-f]+)", open("symbols/bios_thunks.txt").read())]
    lib_start = min(starts, default=None)

    cols = ("functions", "game", "lib-sig", "lib-other", "named", "rewritten", "lifted")
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
            row["lifted"] += fn in lifted and fn not in rewritten
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
    in_c = tot["rewritten"] + tot["lifted"]
    print(f"game code in C: {in_c} of {tot['game']} ({100.0 * in_c / max(tot['game'], 1):.1f}%): "
          f"{tot['rewritten']} rewritten by hand, {tot['lifted']} lifted; "
          f"library: {tot['lib-sig'] + tot['lib-other']} functions "
          f"({tot['lib-sig']} recognised by signature)")


if __name__ == "__main__":
    sys.exit(main())
