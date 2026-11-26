#!/usr/bin/env python3
"""Recognise PsyQ library code by signature and name its functions.

Signatures: lab313ru/psx_psyq_signatures (one JSON per library and SDK version; each object
file is a byte pattern with its relocations masked, plus its labels). Clone it next to the
repository; it is not part of it:
    git clone --depth 1 https://github.com/lab313ru/psx_psyq_signatures ../psx_psyq_signatures

    python3 tools/psyq_sigs.py [--sigs ../psx_psyq_signatures] [--report decomp-build/psyq_report.txt]

For every module (main executable, GAME/EVENT overlays, PACK1 code modules) and every SDK
version, each object signature is searched at word-aligned positions. Per library, the version
that covers the most bytes is kept (games often mix library versions). An object is accepted
when it matches exactly once in the module and does not overlap a longer accepted match.

Writes symbols/psyq_names.txt (main executable) and symbols/<module>_psyq.txt (overlays), and
symbols/psyq_objects.json (every accepted object: module, library, version, address, size),
which tools/progress.py uses to tell library code from game code.
"""
import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict

MIN_LITERAL = 24              # signatures with fewer fixed bytes are too generic to trust
LOCAL_LABEL = re.compile(r"^(loc|text|data|lab|jpt|def|switchD|caseD|sub|locret)_[0-9A-Fa-f]+$")


def modules():
    out = [("main", "extracted/files/SLES_026.01", 0x800, 0x80017000)]
    for name in ("GAME00", "GAME01", "GAME02", "GAME03", "EVENT00"):
        out.append((name.lower(), f"extracted/files/{name}.BIN", 0, 0x80084ED0))
    for p in sorted(glob.glob("config/pack1_*.yaml")):
        idx = os.path.basename(p)[6:8]
        src = glob.glob(f"extracted/archives/PACK1/00{idx}.*")
        if src:
            out.append((f"pack1_{idx}", src[0], 0, 0x80149AE8))
    return out


class Sig:
    __slots__ = ("lib", "obj", "size", "literal", "anchor", "anchor_off", "regex", "labels")

    def __init__(self, lib, obj, sig, labels):
        toks = sig.split()
        self.lib, self.obj, self.size, self.labels = lib, obj, len(toks), labels
        self.literal = sum(t != "??" for t in toks)
        best, run_start, best_off = b"", None, 0
        for i, t in enumerate(toks + ["??"]):          # longest run of fixed bytes = search anchor
            if t != "??":
                if run_start is None:
                    run_start = i
            elif run_start is not None:
                if i - run_start > len(best):
                    best = bytes(int(x, 16) for x in toks[run_start:i])
                    best_off = run_start
                run_start = None
        self.anchor, self.anchor_off = best, best_off
        self.regex = re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)]))
                                         for t in toks), re.DOTALL)

    def find(self, data):
        hits, start = [], 0
        while True:
            k = data.find(self.anchor, start)
            if k < 0:
                return hits
            pos = k - self.anchor_off
            if pos >= 0 and pos % 4 == 0 and self.regex.match(data, pos):
                hits.append(pos)
            start = k + 1


def load_version(path):
    sigs = []
    for p in sorted(glob.glob(os.path.join(path, "*.json"))):
        lib = os.path.basename(p)[:-5]
        for o in json.load(open(p)):
            if o.get("sig"):
                s = Sig(lib, o["name"], o["sig"], o.get("labels", []))
                if s.literal >= MIN_LITERAL and s.anchor:
                    sigs.append(s)
    return sigs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sigs", default="../psx_psyq_signatures")
    ap.add_argument("--report", default="decomp-build/psyq_report.txt")
    a = ap.parse_args()
    versions = sorted(d for d in os.listdir(a.sigs) if d.isdigit())
    print(f"loading {len(versions)} SDK versions ...", file=sys.stderr)
    sigsets = {v: load_version(os.path.join(a.sigs, v)) for v in versions}

    mods = modules()
    data = {}
    for name, path, skip, base in mods:
        data[name] = open(path, "rb").read()[skip:]

    # matches[module][(lib, version)] = list of (pos, sig) with a unique hit
    covered = defaultdict(lambda: defaultdict(int))     # (lib, version) -> bytes covered, all modules
    found = defaultdict(list)
    for v, sigs in sigsets.items():
        for s in sigs:
            for name, _, _, _ in mods:
                hits = s.find(data[name])
                if len(hits) == 1:
                    found[(name, s.lib, v)].append((hits[0], s))
                    covered[s.lib][v] += s.size
    best_version = {lib: max(vs.items(), key=lambda kv: (kv[1], kv[0]))[0] for lib, vs in covered.items()}

    accepted = defaultdict(list)                        # module -> [(pos, sig, version)]
    for (name, lib, v), lst in found.items():
        if best_version[lib] == v:
            accepted[name].extend((pos, s, v) for pos, s in lst)
    objects, report = [], []
    names = defaultdict(dict)                           # module -> addr -> name
    for name, _, _, base in mods:
        taken = []                                      # accepted intervals, longest first
        for pos, s, v in sorted(accepted[name], key=lambda t: -t[1].size):
            if any(pos < e and pos + s.size > b for b, e in taken):
                continue
            taken.append((pos, pos + s.size))
            objects.append({"module": name, "lib": s.lib, "obj": s.obj, "version": v,
                            "addr": f"{base + pos:#010x}", "size": s.size})
            for lab in s.labels:
                n = lab["name"]
                if not LOCAL_LABEL.match(n):
                    names[name][base + pos + lab["offset"]] = n
    objects.sort(key=lambda o: (o["module"] != "main", o["module"], o["addr"]))

    os.makedirs("symbols", exist_ok=True)
    for name, _, _, _ in mods:
        if not names[name]:
            continue
        out = "symbols/psyq_names.txt" if name == "main" else f"symbols/{name}_psyq.txt"
        with open(out, "w") as f:
            f.write("// Generated by tools/psyq_sigs.py (PsyQ signatures) - do not edit.\n")
            for addr in sorted(names[name]):
                f.write(f"{names[name][addr]} = {addr:#010X}; // type:func\n".replace("0X", "0x"))
    json.dump(objects, open("symbols/psyq_objects.json", "w"), indent=1)

    per_lib = defaultdict(lambda: [0, 0, None, None])   # lib -> objects, bytes, lo, hi (main)
    for o in objects:
        key = (o["module"], o["lib"])
        r = per_lib[key]
        r[0] += 1
        r[1] += o["size"]
        a0 = int(o["addr"], 16)
        r[2] = a0 if r[2] is None else min(r[2], a0)
        r[3] = a0 + o["size"] if r[3] is None else max(r[3], a0 + o["size"])
    for (mod, lib), (n, size, lo, hi) in sorted(per_lib.items(), key=lambda kv: (kv[0][0] != "main", kv[1][2])):
        report.append(f"{mod:9} {lib:14} v{best_version[lib]:>4}  {n:3} objects  {size:7} bytes  {lo:#010x}-{hi:#010x}")
    total_names = sum(len(v) for v in names.values())
    report.append(f"{len(objects)} objects, {total_names} function names "
                  f"({', '.join(f'{m}: {len(v)}' for m, v in names.items() if v)})")
    os.makedirs(os.path.dirname(a.report), exist_ok=True)
    open(a.report, "w").write("\n".join(report) + "\n")
    print("\n".join(report))


if __name__ == "__main__":
    main()
