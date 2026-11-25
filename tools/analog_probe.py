#!/usr/bin/env python3
"""Does the game honour the analog stick?  Push the left stick by different amounts and
take screenshots.  Run:  run_scenario.py --game decomp-build/game_analog.toml analog_probe:main
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay

OUT = "decomp-build/analog"


def stick(rt, lx, ly, frames=40, name=None):
    rt.send({"cmd": "set_input", "buttons": "FFFF", "lx": lx, "ly": ly, "pad_type": 1})
    rt.wait_frames(frames)
    if name:
        rt.shot(os.path.join(OUT, name))


def main(rt):
    os.makedirs(OUT, exist_ok=True)
    play_to_gameplay.main(rt)
    rt.send({"cmd": "clear_input"})
    rt.wait_frames(20)
    rt.shot(os.path.abspath(os.path.join(OUT, "0_neutral.png")))
    steps = [("1_right_full", 255, 128), ("2_right_half", 200, 128), ("3_up_full", 128, 0),
             ("4_diag_down_right", 255, 255), ("5_slight_up_right", 170, 90)]
    for name, lx, ly in steps:
        stick(rt, lx, ly, 40, os.path.abspath(os.path.join(OUT, name + ".png")))
        rt.send({"cmd": "clear_input"})
        rt.wait_frames(30)
    st = rt.send({"cmd": "pad_status"})
    print("pad_status", {k: st.get(k) for k in ("pad", "slot0")})
    return "done"
