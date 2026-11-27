#!/usr/bin/env python3
"""Draft C for a function with m2c, from the splat assembly.

    python3 tools/decompile.py func_8001BF70 [more functions...]

Finds the module that defines each function and runs m2c (installed in .venv-decomp:
pip install git+https://github.com/matt-kempster/m2c.git). The output is a starting point:
the rewrite in src/ is written by hand from it and the assembly, then checked with A2_VERIFY.
"""
import glob
import os
import subprocess
import sys

M2C = os.path.join(os.path.dirname(sys.executable), "m2c")


def module_of(name):
    for path in ["decomp/asm/main/3110.s"] + sorted(glob.glob("decomp/asm/*/code.s")):
        with open(path) as f:
            if f"glabel {name}\n" in f.read():
                return path
    return None


def main(names):
    rc = 0
    for name in names:
        path = module_of(name)
        if not path:
            print(f"// {name}: not found in decomp/asm", file=sys.stderr)
            rc = 1
            continue
        print(f"// {name} ({path})")
        # read-only data of the same module (jump tables, constants)
        extra = sorted(glob.glob(os.path.join(os.path.dirname(path), "data", "*.s")))
        out = subprocess.run([M2C, "--target", "mips-gcc-c", "--valid-syntax", "-f", name, path] + extra,
                             capture_output=True, text=True)
        print(out.stdout or out.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
