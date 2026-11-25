#!/usr/bin/env python3
"""Locate the controller state in guest RAM by diffing RAM with a button held and released.

Run through tools/run_scenario.py:  run_scenario.py find_pad_buffer:main
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay

RAM = 0x80000000
SIZE = 0x200000


def dump(rt):
    out = bytearray()
    step = 0x80000
    for off in range(0, SIZE, step):
        out += rt.read_ram(RAM + off, step)
    return bytes(out)


def main(rt):
    play_to_gameplay.main(rt)
    rt.hold("none")
    rt.wait_frames(8)
    base = dump(rt)
    results = {}
    for btn in ("LEFT", "RIGHT", "UP", "CROSS"):
        rt.hold(btn)
        rt.wait_frames(8)
        held = dump(rt)
        rt.hold("none")
        rt.wait_frames(8)
        results[btn] = held
    bit = {"LEFT": 7, "RIGHT": 5, "UP": 4, "CROSS": 14}
    # a halfword that is 0xFFFF-ish neutral and clears exactly the button's bit when held
    cands = None
    for btn, held in results.items():
        found = set()
        for i in range(0, SIZE - 1, 2):
            a = base[i] | (base[i + 1] << 8)
            b = held[i] | (held[i + 1] << 8)
            if a != b and (a ^ b) == (1 << bit[btn]) and (a >> bit[btn]) & 1:
                found.add(i)
        print(btn, len(found), "halfwords clear bit", bit[btn])
        cands = found if cands is None else cands & found
    print("common to all buttons:", [hex(RAM + i) for i in sorted(cands)][:20])
    # also candidates where the inverse (active high) pattern is used
    return sorted(cands)


if __name__ == "__main__":
    print("run via tools/run_scenario.py find_pad_buffer:main")
