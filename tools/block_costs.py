#!/usr/bin/env python3
"""Guest cycle cost of each basic block of a function, as the framework's compiled code
charges it (one span per block: instructions + 2 per load). A hand-written rewrite charges
the blocks of the path it reproduces (a2_charge), so the game keeps the original's timing.
Overlay code ran in the interpreter (per-instruction timing with the instruction cache): for
it the spans are an estimate.

    python3 tools/block_costs.py [--module game00] func_8001A028 [func_...]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import a2lift as al


def blocks(name, module="main"):
    for mod, path, skip, base, asm in al.modules():
        if mod != module:
            continue
        for fname, rows in al.parse_asm(asm):
            if fname != name:
                continue
            insns = [al.Insn(a, w, t) for a, w, t in rows]
            image = open(path, "rb").read()[skip:]

            def read_word(addr):
                off = addr - base
                return struct.unpack_from("<I", image, off)[0] if 0 <= off <= len(image) - 4 else None
            lf = al.Lifter(name, insns, insns[0].addr, insns[-1].addr + 4,
                           al.parse_jump_tables(os.path.dirname(asm)), set(), read_word)
            lead = set(lf.leaders())
            out, cur = [], []
            for k, i in enumerate(insns):
                if i.addr in lead and cur:
                    out.append(cur)
                    cur = []
                cur.append(i)
                if k > 0 and insns[k - 1].is_cti():
                    out.append(cur)
                    cur = []
            if cur:
                out.append(cur)
            return out
    raise SystemExit(f"{name}: not found in {module}")


def main():
    args = sys.argv[1:]
    module = "main"
    if args[:1] == ["--module"]:
        module, args = args[1], args[2:]
    for name in args:
        print(name)
        for b in blocks(name, module):
            last = b[-2] if len(b) >= 2 and b[-2].is_cti() else b[-1]
            print(f"  {b[0].addr:08X}  {len(b):3} insns  span {al.span_cost(b):3}   ends: {last.text[:48]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
