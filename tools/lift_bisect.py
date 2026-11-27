#!/usr/bin/env python3
"""Find lifted functions whose timing or behaviour differs from the original, by bisection.

Runs the reference replay up to a VSync index with the original code (once), then with half
of the candidate lifted functions enabled, keeping the half that still differs.

    python3 tools/lift_bisect.py --stop 40 [--module main] [--extra A2_DISABLE=...]
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import replay_diff as rd

REPLAY = "saves/replays/Alundra_2-boot-20261126T091826Z.psxrpl"


def lifted_names(prefix):
    names = []
    for p in sorted(os.listdir("src/game/lifted")):
        if p.startswith(prefix) and p.endswith(".c"):
            names += re.findall(r'\{ 0x[0-9A-F]+u, "(\w+)"', open(os.path.join("src/game/lifted", p)).read())
    return names


def differs(a_trace, b_trace):
    a, b = rd.read_trace(a_trace), rd.read_trace(b_trace)
    for level in ("cycle", "ram", "gpu"):
        if rd.first_difference(a, b, level):
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stop", type=int, default=40)
    ap.add_argument("--module", default="main")
    ap.add_argument("--hand", default="", help="hand-written functions to disable (comma list)")
    ap.add_argument("--out", default="decomp-build/bisect")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rd.ISOLATE = 4430
    ta = os.path.join(a.out, "a.trace")
    rd.run("build-dev/alundra_2", "game.toml", REPLAY, ["A2_OVERRIDES=0"], ta, a.stop)
    cands = lifted_names(a.module + "_")
    allnames = lifted_names("")
    print(f"{len(cands)} candidates")
    step = 0
    while len(cands) > 1:
        half = cands[: len(cands) // 2]
        disable = [n for n in allnames if n not in half]
        if a.hand:
            disable += a.hand.split(",")
        tb = os.path.join(a.out, f"b{step}.trace")
        rd.run("build-dev/alundra_2", "game.toml", REPLAY,
               ["A2_ANALOG=0", "A2_DISABLE=" + ",".join(disable)], tb, a.stop)
        if differs(ta, tb):
            cands = half
        else:
            cands = cands[len(cands) // 2:]
        step += 1
        print(f"step {step}: {len(cands)} left ({cands[0]}..{cands[-1]})", flush=True)
    print("culprit:", cands)


if __name__ == "__main__":
    main()
