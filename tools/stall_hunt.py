#!/usr/bin/env python3
"""Look for the intermittent turbo stall: run several recorded sessions at the same time.

Each instance plays `record_session:main` on its own debug port and memory-card folder,
with a power-on replay recording, so a stalled run leaves its inputs behind
(`<out>/runN/cards/replays/*.partial.psxrpl`) for a deterministic reproduction.

  python3 tools/stall_hunt.py [--runs 4] [--frames 30000] [--exe build-dev/alundra_2_hunt]
"""
import argparse
import glob
import os
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=4)
    ap.add_argument("--frames", type=int, default=30000)
    ap.add_argument("--exe", default="build-dev/alundra_2")
    ap.add_argument("--game", default="game.toml")
    ap.add_argument("--out", default="decomp-build/stall_hunt")
    ap.add_argument("--base-port", type=int, default=4380)
    a = ap.parse_args()
    env = dict(os.environ, PSX_REPLAY_RECORD_BOOT="1", PSX_REPLAY_RECORD_FRAMES=str(a.frames),
               PSX_REPLAY_EXIT_AT_END="1", A2_OVERRIDES="0")
    procs = []
    for i in range(a.runs):
        out = os.path.join(a.out, f"run{i}")
        os.makedirs(out, exist_ok=True)
        log = open(os.path.join(out, "scenario.log"), "w")
        procs.append((out, subprocess.Popen(
            [sys.executable, "tools/run_scenario.py", "--exe", a.exe, "--game", os.path.abspath(a.game),
             "--retries", "0", "--out", out, "--port", str(a.base_port + i), "record_session:main"],
            env=env, stdout=log, stderr=subprocess.STDOUT)))
        time.sleep(8)                     # simultaneous starts race on the mod state publication
    stalls = 0
    for out, p in procs:
        p.wait()
        text = open(os.path.join(out, "scenario.log")).read()
        stalled = "stall at frame" in text or "no frame progress" in text or "did not answer" in text
        stalls += stalled
        replays = sorted(glob.glob(os.path.join(out, "**", "*.psxrpl"), recursive=True))
        last = [l for l in text.splitlines() if l.strip()][-1:] or ["(no output)"]
        print(f"{out}: {'STALL' if stalled else 'ok'} exit={p.returncode} | {last[0][:100]}")
        for r in replays:
            print(f"    {r}")
    print(f"{stalls} stall(s) in {len(procs)} runs")
    return 1 if stalls else 0


if __name__ == "__main__":
    sys.exit(main())
