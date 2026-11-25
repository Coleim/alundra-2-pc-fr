#!/usr/bin/env python3
"""Shadow-verify the rewritten heading function (src/input_override.c).

Run with the original executed alongside:
  A2_VERIFY=1 A2_ANALOG=0 python3 tools/run_scenario.py verify_heading:main
Passes when a2.heading.calls > 0 and a2.heading.mismatch == 0.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay

COMBOS = ["UP", "RIGHT", "UP+RIGHT", "DOWN", "DOWN+RIGHT", "LEFT", "UP+LEFT", "DOWN+LEFT",
          "UP", "LEFT", "DOWN", "RIGHT"]


def counters(rt):
    r = rt.send({"cmd": "mod_counters"})
    out = {}
    for c in r.get("counters", []):
        out[c["name"]] = c["count"]
    return out, r


def main(rt):
    play_to_gameplay.main(rt)
    rt.hold("none")
    rt.wait_frames(20)
    for combo in COMBOS:
        rt.hold(combo)
        rt.wait_frames(45)
        rt.hold("none")
        rt.wait_frames(10)
    c, raw = counters(rt)
    print("counters", c)
    calls = c.get("a2.heading.calls", 0)
    bad = c.get("a2.heading.mismatch", 0)
    if calls == 0:
        raise RuntimeError(f"override never ran: {raw}")
    if bad:
        raise RuntimeError(f"{bad} mismatches in {calls} calls")
    return f"{calls} calls, 0 mismatches"
