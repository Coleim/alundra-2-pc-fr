#!/usr/bin/env python3
"""From a freshly booted debug runtime, reach the first controllable scene of a new game.

Works from what is on screen, not from frame counts: it finds the title cursor, picks
"Demarrer", accepts the default difficulty and waits for the in-game HUD.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from runtime_ctl import Runtime
import screen

TITLE_START_Y = 112   # cursor row of "Demarrer" on the French title screen


def to_title(rt):
    for _ in range(12):
        if screen.menu_cursor_row(rt, samples=3, gap=5) is not None:
            return
        rt.press("START", 6, 240)
    raise RuntimeError("title screen never appeared")


def pick_new_game(rt):
    for _ in range(8):
        row = screen.menu_cursor_row(rt)
        if row is None:
            rt.wait_frames(40)
            continue
        if row == 0:
            return
        rt.press("UP", 4, 40)
    raise RuntimeError("could not select the new game entry")


def main(rt=None):
    rt = rt or Runtime()
    rt.turbo(True)
    rt.wait_frames(4500)
    to_title(rt)
    rt.wait_frames(300)
    pick_new_game(rt)
    rt.press("CROSS", 4, 240)          # difficulty menu
    rt.press("CROSS", 4, 600)          # Normal
    for _ in range(80):                # opening cutscene and first load
        rt.wait_frames(600)
        if screen.hud_present(screen.grab(rt)):
            break
    else:
        raise RuntimeError("gameplay HUD never appeared")
    rt.wait_frames(120)
    rt.turbo(False)
    return rt.frame()


if __name__ == "__main__":
    print("frame", main())
