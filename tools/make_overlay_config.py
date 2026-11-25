#!/usr/bin/env python3
"""Write a splat config for a raw overlay file loaded at a fixed RAM address.

  python3 tools/make_overlay_config.py GAME00 --vram 0x80084ED0 --code 0x2800 0x6A000
"""
import argparse
import os
import sys

TEMPLATE = """name: {name}
options:
  basename: {lc}
  target_path: extracted/files/{name}.BIN
  elf_path: decomp-build/{lc}.elf
  base_path: ..
  platform: psx
  compiler: PSYQ
  asm_path: decomp/asm/{lc}
  src_path: src/{lc}
  build_path: decomp-build
  ld_script_path: decomp-build/{lc}.ld
  ld_dependencies: True
  find_file_boundaries: False
  gp_value: 0x8005D168
  o_as_suffix: True
  use_legacy_include_asm: False
  section_order: [".rodata", ".text", ".data", ".bss"]
  symbol_addrs_path:
    - symbols/main_symbols.txt
    - symbols/{lc}_symbols.txt
  reloc_addrs_path:
    - symbols/{lc}_relocs.txt
  undefined_funcs_auto_path: decomp-build/{lc}_undefined_funcs_auto.txt
  undefined_syms_auto_path: decomp-build/{lc}_undefined_syms_auto.txt
  subalign: 2
  string_encoding: ASCII
  data_string_encoding: ASCII
  rodata_string_guesser_level: 2
  data_string_guesser_level: 2
segments:
  - name: {lc}
    type: code
    start: 0x0
    vram: {vram:#x}
    subsegments:
{subs}
  - [{size:#x}]
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--vram", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--code", type=lambda x: int(x, 0), nargs=2, required=True, metavar=("LO", "HI"))
    ap.add_argument("--root", default=".")
    a = ap.parse_args()
    lc = a.name.lower()
    size = os.path.getsize(os.path.join(a.root, "extracted/files", a.name + ".BIN"))
    lo, hi = a.code
    subs = []
    if lo > 0:
        subs.append("      - [0x0, data, head]")
    subs.append(f"      - [{lo:#x}, asm, code]")
    if hi < size:
        subs.append(f"      - [{hi:#x}, data, tail]")
    open(os.path.join(a.root, "config", lc + ".yaml"), "w").write(
        TEMPLATE.format(name=a.name, lc=lc, vram=a.vram, subs="\n".join(subs), size=size))
    for suffix in ("_symbols.txt", "_relocs.txt"):
        p = os.path.join(a.root, "symbols", lc + suffix)
        if not os.path.exists(p):
            open(p, "w").close()
    print(f"config/{lc}.yaml")


if __name__ == "__main__":
    sys.exit(main())
