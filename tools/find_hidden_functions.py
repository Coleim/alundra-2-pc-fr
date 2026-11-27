#!/usr/bin/env python3
"""Find the functions splat merged into the one before them.

splat starts a function at each symbol and at each `jal` target. A function the game only
reaches through a pointer (overlay entry table, callbacks, handler tables) gets no symbol, so
it stays glued to the end of the previous function. Such code is found by reachability: the
instructions a function cannot reach from its own entry, after its last reachable
instruction, start another function (repeated on that one). Data words that point into the
middle of a function are listed too.

    python3 tools/find_hidden_functions.py            # report
    python3 tools/find_hidden_functions.py --write    # add them to the split's symbol files

Then split again (python -m splat split config/<module>.yaml) and regenerate the C.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import a2lift as al


def reachable(lf):
    """Addresses the function reaches from its entry through its own control flow."""
    idx = {i.addr: k for k, i in enumerate(lf.insns)}
    seen = set()
    work = [lf.start]
    while work:
        a = work.pop()
        while lf.inside(a) and a not in seen and a in idx:
            seen.add(a)
            i = lf.insns[idx[a]]
            if i.is_cti():
                seen.add(a + 4)
                if i.is_branch():
                    work.append(i.branch_target())
                    a += 8
                    continue
                if i.op == 2:                               # j
                    work.append(i.target)
                    break
                if i.op == 3 or (i.op == 0 and i.fn == 9):  # calls return
                    a += 8
                    continue
                if i.op == 0 and i.fn == 8:                 # jr
                    if i.rs != 31:
                        try:
                            tbl = lf.table_for(i)
                        except (ValueError, KeyError):
                            tbl = None
                        work.extend(lf.tables.get(tbl, []))
                    break
            a += 4
    return seen


def split(name, insns, tables, starts, read_word):
    """[(name, insns)]: the function, then each function hidden in its tail."""
    out = []
    while insns:
        start, end = insns[0].addr, insns[-1].addr + 4
        lf = al.Lifter(name, insns, start, end, tables, starts, read_word)
        seen = reachable(lf)
        last = max(seen) if seen else start
        k = next((n for n, i in enumerate(insns) if i.addr > last and i.word != 0), None)
        if k is None:
            out.append((name, insns))
            break
        out.append((name, insns[:k]))
        insns = insns[k:]
        name = f"func_{insns[0].addr:08X}"
        starts.add(insns[0].addr)
    return out


def data_pointers(asm_dir, lo, hi):
    """Words of the data sections that hold an address of [lo, hi)."""
    found = {}
    for root, _, files in os.walk(asm_dir):
        for f in files:
            if not f.endswith(".s"):
                continue
            path = os.path.join(root, f)
            for line in open(path, errors="replace"):
                m = re.match(r"\s*/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/ \.word (?:0x)?([0-9A-Fa-f]{8})\b", line)
                if m:
                    v = int(m.group(2), 16)
                    if lo <= v < hi and not v & 3:
                        found.setdefault(v, int(m.group(1), 16))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    lib_base = al.lib_start()
    total_hidden = 0
    for mod, path, skip_bytes, base, asm in al.modules():
        if not os.path.exists(asm):
            continue
        funcs = al.parse_asm(asm)
        tables = al.parse_jump_tables(os.path.dirname(asm))
        image = open(path, "rb").read()[skip_bytes:]

        def read_word(addr, image=image, base=base):
            off = addr - base
            if 0 <= off <= len(image) - 4:
                return struct.unpack_from("<I", image, off)[0]
            return None
        starts = {f[1][0][0] for f in funcs}
        hidden = []
        ranges = []
        for name, rows in funcs:
            insns = [al.Insn(addr, word, text) for addr, word, text in rows]
            if mod == "main" and insns[0].addr >= lib_base:
                continue
            for n, (fname, part) in enumerate(split(name, insns, tables, starts, read_word)):
                ranges.append((part[0].addr, part[-1].addr + 4, fname))
                if n:
                    hidden.append(part[0].addr)
        lo = min(r[0] for r in ranges)
        hi = max(r[1] for r in ranges)
        jt = {t for v in tables.values() for t in v}
        known = {r[0] for r in ranges}
        inner = {}
        for v, where in data_pointers(os.path.dirname(asm), lo, hi).items():
            if v in known or v in jt:
                continue
            inner[v] = where
        total_hidden += len(hidden)
        if hidden or inner:
            print(f"{mod}: {len(hidden)} hidden functions"
                  + (f", {len(inner)} data pointers into function bodies" if inner else ""))
            for h in hidden[:6]:
                print(f"   func_{h:08X}")
            for v, where in list(inner.items())[:6]:
                print(f"   pointer {v:#010x} (at {where:#010x})")
        if a.write and hidden:
            sym = "symbols/manual.txt" if mod == "main" else f"symbols/{mod}_symbols.txt"
            have = open(sym).read() if os.path.exists(sym) else ""
            with open(sym, "a") as f:
                if mod == "main":
                    f.write("// functions only reached through pointers (tools/find_hidden_functions.py)\n")
                for h in hidden:
                    if f"0x{h:08X};" not in have:
                        f.write(f"func_{h:08X} = 0x{h:08X}; // type:func\n")
    print(f"total: {total_hidden} hidden functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
