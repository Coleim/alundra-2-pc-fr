#!/usr/bin/env python3
"""Push the left stick at known angles and read back the heading the game computed.

Run:  python3 tools/run_scenario.py --game decomp-build/game_analog.toml analog_heading_test:main
Expected: fine angle within +-1 unit of the stick angle (512-unit circle, 0 right, 128 up).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay


def cmd(rt, name):
    r = rt.send({"cmd": "mod_cmd", "name": name, "args": ""})
    return r.get("reply") or r.get("text") or str(r)


def parse(reply):
    return {k: int(v) for k, v in (kv.split("=") for kv in reply.split() if "=" in kv)}


def main(rt):
    play_to_gameplay.main(rt)
    play_to_gameplay.ensure_controllable(rt)
    bad = []
    for deg in (0, 20, 45, 77, 90, 135, 180, 225, 250, 315, 340):
        a = math.radians(deg)
        lx = int(round(128 + 120 * math.cos(a)))
        ly = int(round(128 - 120 * math.sin(a)))       # stick Y grows downwards
        rt.send({"cmd": "set_input", "buttons": "FFFF", "lx": lx, "ly": ly, "pad_type": 1})
        got = None
        for _ in range(10):                     # wait until the game has computed a heading
            rt.wait_frames(30)
            got = parse(cmd(rt, "a2_heading"))
            if got["input_angle"] != -2:
                break
        want = int(round(deg / 360.0 * 512)) & 0x1FF
        diff = min(abs(got["fine"] - want), 512 - abs(got["fine"] - want)) if got["fine"] >= 0 else 999
        print(f"stick {deg:3d} deg -> want {want:3d} fine {got['fine']:4d} input_angle {got['input_angle']:4d} "
              f"result {got['result']:4d} {'ok' if diff <= 1 else 'BAD'}")
        if diff > 1:
            bad.append(deg)
    rt.send({"cmd": "clear_input"})
    if bad:
        raise RuntimeError(f"wrong heading for {bad}")
    return "analog heading ok"
