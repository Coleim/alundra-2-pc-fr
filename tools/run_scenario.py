#!/usr/bin/env python3
"""Start the debug runtime, run a scenario against it, and diagnose stalls.

  python3 tools/run_scenario.py --exe build-dev/alundra_2 --game game.toml \
      [--retries 3] [--out decomp-build/scenario] module:function

The scenario is `tools/<module>.py` exposing `main(rt)`.  On a stall the process is
sampled (all thread stacks, via macOS `sample`) before being killed and restarted.
"""
import argparse
import importlib
import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from runtime_ctl import Runtime, Stalled


def start(exe, game, extra):
    log = open(os.path.join(extra["out"], "runtime.log"), "w")
    p = subprocess.Popen([exe, "--headless", "--game", game], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        time.sleep(0.2)
        try:
            Runtime().frame()
            return p
        except OSError:
            if p.poll() is not None:
                raise SystemExit("runtime exited at startup")
    raise SystemExit("debug server never came up")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build-dev/alundra_2")
    ap.add_argument("--game", default="game.toml")
    ap.add_argument("--retries", type=int, default=2)
    ap.add_argument("--out", default="decomp-build/scenario")
    ap.add_argument("scenario")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    mod, _, fn = a.scenario.partition(":")
    scenario = getattr(importlib.import_module(mod), fn or "main")

    for attempt in range(1, a.retries + 2):
        p = start(a.exe, a.game, {"out": a.out})

        def sample(frame):
            dst = os.path.join(a.out, f"stall-attempt{attempt}-frame{frame}.txt")
            subprocess.run(["sample", str(p.pid), "3", "-file", dst], capture_output=True)
            print(f"stall at frame {frame}: thread stacks in {dst}")

        rt = Runtime(on_stall=sample)
        try:
            result = scenario(rt)
            print("scenario ok", result if result is not None else "")
            p.send_signal(signal.SIGTERM)
            return 0
        except Stalled as e:
            print(f"attempt {attempt}: {e}")
        finally:
            if p.poll() is None:
                p.kill()
            p.wait()
    return 1


if __name__ == "__main__":
    sys.exit(main())
