#!/usr/bin/env python3
"""Play a recorded replay and print the framework's verdict (in-sync / diverged / ...).

  python3 tools/replay_check.py saves/replays/<file>.psxrpl [--exe build-dev/alundra_2] [--env A2_OVERRIDES=0]
Exit status: 0 in sync, 3 diverged, 4 not played, other = failure.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--exe", default="build-dev/alundra_2")
    ap.add_argument("--game", default="game.toml")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the runtime")
    ap.add_argument("--verdict", default="decomp-build/verdict.json")
    ap.add_argument("--log", help="save the runtime output (stdout and stderr) here")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds before the runtime is killed")
    ap.add_argument("--isolate", type=int, metavar="PORT",
                    help="own debug port and memory-card folder, to run next to other instances")
    a = ap.parse_args()
    env = dict(os.environ, PSX_REPLAY_EXIT_AT_END="1")
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    if os.path.exists(a.verdict):
        os.remove(a.verdict)
    os.makedirs(os.path.dirname(a.verdict), exist_ok=True)
    cmd = [a.exe, "--headless", "--game", os.path.abspath(a.game), "--replay", a.replay,
           "--replay-verdict", a.verdict, "--replay-fast"]
    if a.isolate:
        cards = os.path.abspath(os.path.join(os.path.dirname(a.verdict), f"cards-{a.isolate}"))
        os.makedirs(cards, exist_ok=True)
        cmd += ["--debug-port", str(a.isolate), "--memcard-dir", cards]
    for attempt in range(4):
        out = open(a.log, "w") if a.log else subprocess.DEVNULL
        try:
            p = subprocess.run(cmd, env=env, stdout=out, stderr=subprocess.STDOUT if a.log else subprocess.DEVNULL,
                               timeout=a.timeout)
        except subprocess.TimeoutExpired:
            # a fatal stop leaves the debug server up for post-mortem: the process never exits
            print(f"runtime killed after {a.timeout}s (hung or halted, see the log)")
            return 5
        # Instances started in the same seconds can refuse to start ("cannot publish state"):
        # no verdict and exit 1 right away. Try again a little later.
        if os.path.exists(a.verdict) or p.returncode != 1:
            break
        time.sleep(5 + 7 * attempt + random.random() * 5)
    try:
        v = json.load(open(a.verdict))
    except Exception as e:
        print(f"no verdict ({e}); runtime exit {p.returncode}")
        return p.returncode or 1
    keys = ("verdict", "result", "status", "frames", "frame", "diverged_at", "first_divergence", "reason")
    print("verdict:", {k: v[k] for k in keys if k in v} or {k: str(v)[:80] for k, v in list(v.items())[:8]})
    print("runtime exit", p.returncode, {0: "in sync", 3: "DIVERGED", 4: "not played"}.get(p.returncode, ""))
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
