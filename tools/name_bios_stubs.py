#!/usr/bin/env python3
"""Name the PsyQ -> BIOS dispatch thunks of a PS-X EXE.

PsyQ reaches the kernel through three-instruction thunks:
    addiu $t2,$zero,0xA0 ; jr $t2 ; addiu $t1,$zero,<index>
The name tables come from the framework (psx-spx A0/B0/C0 function lists).

  python3 tools/name_bios_stubs.py extracted/files/SLES_026.01 \
      --names psxrecomp/recompiler/src/bios_call_names.cpp --out symbols/bios_thunks.txt
"""
import argparse
import re
import struct
import sys


def parse_tables(cpp_path):
    text = open(cpp_path).read()
    tables = {}
    for tag, key in (("kA0", 0xA0), ("kB0", 0xB0), ("kC0", 0xC0)):
        m = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % tag, text, re.S)
        names = []
        if m:
            body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
            for tok in re.findall(r'"([^"]*)"|(\bnullptr\b)', body):
                names.append(tok[0] if tok[0] != "" or not tok[1] else "")
        tables[key] = names
    return tables


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--names", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    data = open(a.exe, "rb").read()
    load, = struct.unpack_from("<I", data, 0x18)
    size, = struct.unpack_from("<I", data, 0x1C)
    body = data[0x800:0x800 + size]
    words = struct.unpack("<%dI" % (len(body) // 4), body)
    tables = parse_tables(a.names)

    found = {}
    for i in range(len(words) - 2):
        w = words[i]
        if w in (0x240A00A0, 0x240A00B0, 0x240A00C0) and words[i + 1] == 0x01400008 \
                and (words[i + 2] >> 16) == 0x2409:
            table = w & 0xFF
            idx = words[i + 2] & 0xFFFF
            names = tables.get(table, [])
            nm = names[idx] if idx < len(names) and names[idx] else f"bios_{table:02X}_{idx:02X}"
            addr = load + i * 4
            found[addr] = (table, idx, nm)

    used = {}
    with open(a.out, "w") as f:
        for addr in sorted(found):
            table, idx, nm = found[addr]
            used[nm] = used.get(nm, 0) + 1
            sym = nm if used[nm] == 1 else f"{nm}_{used[nm]}"
            f.write(f"{sym} = 0x{addr:08X}; // type:func size:12\n")
    print(f"{len(found)} BIOS thunks named -> {a.out}")
    named = sum(1 for _, _, n in found.values() if not n.startswith("bios_"))
    print(f"  {named} have a known name, {len(found) - named} generic")


if __name__ == "__main__":
    sys.exit(main())
