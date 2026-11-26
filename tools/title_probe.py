#!/usr/bin/env python3
"""Reach the title screen, move the cursor up, and log what happens."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import screen
import play_to_gameplay as p


def main(rt):
    rt.turbo(True)
    rt.wait_frames(4500)
    p.to_title(rt)
    rt.wait_frames(300)
    os.makedirs("decomp-build/title", exist_ok=True)
    print("start row", screen.menu_cursor_row(rt), flush=True)
    for i in range(3):
        rt.press("UP", 4, 40)
        print("after UP", i, "row", screen.menu_cursor_row(rt), flush=True)
    rt.press("CROSS", 4, 200)
    rt.shot(os.path.abspath("decomp-build/title/after_cross.png"))
    print("pad", rt.send({"cmd": "pad_status"}).get("slot0"))
    return "ok"
