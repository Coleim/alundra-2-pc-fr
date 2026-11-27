#!/usr/bin/env python3
"""Call graph of the split code (decomp/asm), to find who calls what.

    python3 tools/callgraph.py callers VSync SetGeomScreen
    python3 tools/callgraph.py callees func_8001A99C
    python3 tools/callgraph.py tree func_8001A99C --depth 3

Functions are named as in the split (func_XXXXXXXX, library names); calls are the direct `jal`
instructions (calls through pointers do not appear).
"""
import argparse
import collections
import os
import re

ASM = "decomp/asm"


def load():
    funcs = {}                          # name -> (module, address)
    calls = collections.defaultdict(list)
    for root, _, files in os.walk(ASM):
        if "/data" in root or "/nonmatchings" in root and False:
            pass
        for f in sorted(files):
            if not f.endswith(".s") or "data" in f:
                continue
            module = os.path.relpath(root, ASM).split(os.sep)[0]
            cur = None
            for line in open(os.path.join(root, f), errors="replace"):
                m = re.match(r"glabel (\w+)", line)
                if m:
                    cur = m.group(1)
                    continue
                if cur is None:
                    continue
                m = re.match(r"\s*/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/", line)
                if m and cur not in funcs:
                    funcs[cur] = (module, int(m.group(1), 16))
                m = re.search(r"\bjal\s+(\w+)\s*$", line)
                if m:
                    calls[(module, cur)].append(m.group(1))
    return funcs, calls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["callers", "callees", "tree"])
    ap.add_argument("names", nargs="+")
    ap.add_argument("--depth", type=int, default=2)
    a = ap.parse_args()
    funcs, calls = load()
    if a.mode == "callers":
        for n in a.names:
            who = collections.Counter()
            for (mod, f), cs in calls.items():
                k = cs.count(n)
                if k:
                    who[f"{mod}:{f}"] += k
            print(f"{n}: {sum(who.values())} calls from {len(who)} functions")
            for w, k in sorted(who.items()):
                print(f"   {w} x{k}")
    elif a.mode == "callees":
        for n in a.names:
            for (mod, f), cs in calls.items():
                if f == n:
                    print(f"{mod}:{f}: " + " ".join(dict.fromkeys(cs)))
    else:
        def tree(n, d, seen, pad=""):
            for (mod, f), cs in calls.items():
                if f != n:
                    continue
                for c in dict.fromkeys(cs):
                    print(f"{pad}  {c}")
                    if d > 1 and c not in seen and c.startswith("func_"):
                        seen.add(c)
                        tree(c, d - 1, seen, pad + "  ")
                break
        for n in a.names:
            print(n)
            tree(n, a.depth, {n})


if __name__ == "__main__":
    main()
