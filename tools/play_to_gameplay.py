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
    snapshot_failure(rt, "fail_to_title.png")
    raise RuntimeError("title screen never appeared")


def snapshot_failure(rt, name):
    os.makedirs("decomp-build", exist_ok=True)
    rt.shot(os.path.abspath(os.path.join("decomp-build", name)))


def pick_new_game(rt):
    for _ in range(8):
        row = screen.menu_cursor_row(rt)
        if row is None:
            rt.wait_frames(40)
            continue
        if row == 0:
            return
        rt.press("UP", 4, 40)
    snapshot_failure(rt, "fail_pick_new_game.png")
    raise RuntimeError("could not select the new game entry")


def start_new_game(rt):
    """Title -> difficulty screen, retrying from the load screen if the wrong entry was picked."""
    for _ in range(6):
        pick_new_game(rt)
        rt.press("CROSS", 4, 260)
        kind = screen.screen_kind(screen.grab(rt))
        if kind == "difficulty":
            return
        if kind == "load":
            rt.press("CIRCLE", 4, 200)
        rt.wait_frames(120)
    snapshot_failure(rt, "fail_start_new_game.png")
    raise RuntimeError("never reached the difficulty screen")


def main(rt=None):
    rt = rt or Runtime()
    rt.turbo(True)
    rt.wait_frames(4500)
    to_title(rt)
    rt.wait_frames(300)
    start_new_game(rt)
    rt.press("CROSS", 4, 600)          # Normal
    for _ in range(80):                # opening cutscene and first load
        rt.wait_frames(600)
        img = screen.grab(rt)
        if screen.screen_kind(img) == "title" and screen.hud_present(img):
            break
    else:
        raise RuntimeError("gameplay HUD never appeared")
    rt.wait_frames(120)
    rt.turbo(False)
    return rt.frame()


def ensure_controllable(rt, tries=40):
    """Press a direction until the heading override has run, i.e. Flint answers the pad.

    Advances dialogue boxes with CROSS in between.  Needs the a2_heading debug command."""
    for _ in range(tries):
        rt.hold("RIGHT")
        rt.wait_frames(40)
        r = rt.send({"cmd": "mod_cmd", "name": "a2_heading", "args": ""})
        rt.hold("none")
        if "input_angle=-2" not in (r.get("reply") or "input_angle=-2"):
            return True
        rt.press("CROSS", 4, 150)
    snapshot_failure(rt, "fail_controllable.png")
    raise RuntimeError("Flint never became controllable")


if __name__ == "__main__":
    print("frame", main())
