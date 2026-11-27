#!/usr/bin/env python3
"""Record a second reference session with more of the game: combat buttons, jumps, the start
menu and its sub-menus, then more walking.

    PSX_REPLAY_RECORD_BOOT=1 PSX_REPLAY_RECORD_FRAMES=40000 PSX_REPLAY_EXIT_AT_END=1 A2_OVERRIDES=0 \\
        python3 tools/run_scenario.py --retries 0 explore_session:main
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay

MOVES = ["UP", "RIGHT", "DOWN", "LEFT", "UP+RIGHT", "DOWN+LEFT"]
ACTIONS = ["SQUARE", "CROSS", "CIRCLE", "TRIANGLE", "R1", "L1"]


def menu_tour(rt):
    """Open the start menu, walk through its entries and sub-menus, close it."""
    rt.press("START", 4, 60)
    for _ in range(4):
        for step in ("DOWN", "CROSS", "RIGHT", "DOWN", "CIRCLE", "TRIANGLE"):
            rt.press(step, 4, 40)
    rt.press("START", 4, 90)
    rt.press("TRIANGLE", 4, 60)


def main(rt):
    try:
        play_to_gameplay.main(rt)
        rt.turbo(True)
        cycle = 0
        while True:
            for k, move in enumerate(MOVES):
                rt.hold(move)
                rt.wait_frames(45)
                rt.hold("none")
                rt.press(ACTIONS[(k + cycle) % len(ACTIONS)], 4, 30)
                rt.press(ACTIONS[(k + cycle + 1) % len(ACTIONS)], 4, 30)
            if cycle % 2 == 0:
                menu_tour(rt)
            cycle += 1
    except ConnectionError:
        return "runtime exited (frame budget reached)"
