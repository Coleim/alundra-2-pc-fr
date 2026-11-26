#!/usr/bin/env python3
"""Name PsyQ library functions of the main executable from their debug strings.

A function that loads the address of "ResetGraph(%d)..." is ResetGraph.  Only functions
that still carry an automatic func_XXXXXXXX name are renamed.

  python3 tools/name_by_strings.py extracted/files/SLES_026.01 decomp/asm/main/3110.s \
      --out symbols/lib_names.txt
"""
import argparse
import re
import struct
import sys

# substring of the string literal -> function name (library function that prints it)
STRING_TO_NAME = [
    ("ResetGraph(%d)", "ResetGraph"), ("SetGraphDebug:level", "SetGraphDebug"),
    ("SetGrapQue(%d)", "SetGraphQue"), ("DrawSyncCallback(%08x)", "DrawSyncCallback"),
    ("SetDispMask(%d)", "SetDispMask"), ("DrawSync(%d)", "DrawSync"),
    ("ClearOTagR(%08x", "ClearOTagR"), ("ClearOTag(%08x", "ClearOTag"),
    ("VSync: timeout", "v_wait"), ("MDEC_rest:bad option", "MDEC_rest"),
    ("CD_sync", "CD_sync"), ("CD_ready", "CD_ready"), ("CD_init:", "CD_init"),
    ("CD_datasync", "CD_datasync"), ("DsSearchFile: searching", "DsSearchFile"),
    ("DS_newmedia: Read error", "DS_newmedia"), ("libmcrd: event overflow", "mcrd_event_overflow"),
]


def parse_functions(asm_path):
    """[(vram, name)] for every glabel in a splat-generated asm file."""
    funcs = []
    pending = None
    with open(asm_path) as f:
        for line in f:
            m = re.match(r"\s*glabel\s+(\S+)", line)
            if m:
                pending = m.group(1)
                continue
            if pending:
                m = re.match(r"\s*/\*\s*[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})\s", line)
                if m:
                    funcs.append((int(m.group(1), 16), pending))
                    pending = None
    return sorted(funcs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("asm")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    d = open(a.exe, "rb").read()
    load, = struct.unpack_from("<I", d, 0x18)
    size, = struct.unpack_from("<I", d, 0x1C)
    body = d[0x800:0x800 + size]
    words = struct.unpack("<%dI" % (len(body) // 4), body)

    strings = {}
    for m in re.finditer(rb"[ -~\r\n\t]{4,}\x00", body):
        s = m.group()[:-1].decode()
        for sub, name in STRING_TO_NAME:
            if sub in s:
                strings[load + m.start()] = (name, s)

    funcs = parse_functions(a.asm)
    starts = [v for v, _ in funcs]

    def owner(addr):
        lo, hi = 0, len(starts) - 1
        best = None
        while lo <= hi:
            mid = (lo + hi) // 2
            if starts[mid] <= addr:
                best, lo = mid, mid + 1
            else:
                hi = mid - 1
        return funcs[best] if best is not None else None

    hits = {}
    for i, w in enumerate(words):
        if (w >> 26) == 0x0F:                       # lui rt, hi
            rt = (w >> 16) & 31
            hi = (w & 0xFFFF) << 16
            for j in range(i + 1, min(i + 8, len(words))):
                w2 = words[j]
                op = w2 >> 26
                if op in (0x09, 0x0D) and ((w2 >> 21) & 31) == rt:    # addiu/ori rt, rt, lo
                    lo = w2 & 0xFFFF
                    val = hi + (lo - 0x10000 if (op == 0x09 and lo & 0x8000) else lo)
                    if val in strings:
                        own = owner(load + i * 4)
                        if own:
                            hits.setdefault(own[0], set()).add(strings[val][0])
                    break
    names = {}
    with open(a.out, "w") as f:
        for vram, label in funcs:
            if vram in hits and label.startswith("func_"):
                cands = sorted(hits[vram])
                nm = cands[0]
                note = "" if len(cands) == 1 else f" (also: {','.join(cands[1:])})"
                if nm in names.values():
                    nm = f"{nm}_{vram:08X}"
                names[vram] = nm
                f.write(f"{nm} = 0x{vram:08X}; // type:func{note}\n")
    print(f"{len(strings)} anchor strings, {len(names)} functions named -> {a.out}")
    for v, n in sorted(names.items()):
        print(f"  {v:#010x} {n}")


if __name__ == "__main__":
    sys.exit(main())
