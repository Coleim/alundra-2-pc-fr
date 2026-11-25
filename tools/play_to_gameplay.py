#!/usr/bin/env python3
"""From a freshly booted debug runtime, reach the first controllable scene of a new game.

Assumes the French build: intro -> title -> Demarrer -> difficulty (Normal) -> opening
cutscene -> first zone.  Runs in turbo, so it is quick and silent when the runtime
is started with --headless.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from runtime_ctl import Runtime


def main():
    rt = Runtime()
    rt.turbo(True)
    rt.wait_frames(5500)          # BIOS, logos, intro movie
    rt.press("START", 6, 600)     # skip to the title screen
    rt.press("CIRCLE", 4, 120)    # in case the cursor already left the first entry
    rt.press("UP", 4, 40)         # cursor on "Demarrer"
    rt.press("CROSS", 4, 200)     # new game -> difficulty
    rt.press("CROSS", 4, 3000)    # Normal -> opening cutscene
    rt.wait_frames(18000)         # cutscene and first load
    rt.turbo(False)
    print("frame", rt.frame())


if __name__ == "__main__":
    sys.exit(main())
