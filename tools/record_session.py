#!/usr/bin/env python3
"""Record a power-on replay (PSXRTI3) of a short play session.

Run through run_scenario.py with the recorder enabled:
  PSX_REPLAY_RECORD_BOOT=1 PSX_REPLAY_RECORD_FRAMES=26000 PSX_REPLAY_EXIT_AT_END=1 A2_OVERRIDES=0 \\
      python3 tools/run_scenario.py record_session:main
The runtime exits by itself when the frame budget is reached.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay

BUDGET = int(os.environ.get("PSX_REPLAY_RECORD_FRAMES", "26000"))
WALK = ["UP", "RIGHT", "UP+RIGHT", "DOWN", "LEFT", "DOWN+LEFT", "UP+LEFT", "DOWN+RIGHT"]


def main(rt):
    try:
        play_to_gameplay.main(rt)
        while True:                                  # keep walking until the budget ends the run
            for combo in WALK:
                rt.hold(combo)
                rt.wait_frames(60)
                rt.hold("none")
                rt.wait_frames(15)
    except OSError:
        return "runtime exited (frame budget reached)"
