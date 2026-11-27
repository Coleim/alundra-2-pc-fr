#!/usr/bin/env python3
"""Play the reference session under several configurations at once and compare each with the
original code.

    python3 tools/validate_session.py [--replay R] [--out decomp-build/validate] [--stop N]

Configurations: original (A2_OVERRIDES=0), lifted (all translated code: the hand-written
rewrites are off, their translated versions run), full (everything on, as shipped). Each run keeps its runtime log next to its trace.
"""
import argparse
import glob
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import replay_diff as rd

def hand_written():
    """Functions rewritten by hand (A2_REWRITE in src/game/, and the heading override)."""
    names = ["func_800A09C0"]
    for p in sorted(glob.glob("src/game/*.c")):
        names += re.findall(r"A2_REWRITE(?:_OVERLAY)?\(\s*(\w+)\s*,", open(p).read())
    return ",".join(names)


HAND = hand_written()
CONFIGS = {
    "original": ["A2_OVERRIDES=0"],
    "lifted": ["A2_ANALOG=0", "A2_DISABLE=" + HAND],
    "full": ["A2_ANALOG=0"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", default="saves/replays/Alundra_2-boot-20261126T091826Z.psxrpl")
    ap.add_argument("--out", default="decomp-build/validate")
    ap.add_argument("--stop", type=int)
    ap.add_argument("--configs", default="original,lifted,full")
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--port", type=int, default=4450, help="first debug port (one per configuration)")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE added to every configuration")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    names = a.configs.split(",")
    traces = {n: os.path.join(a.out, f"{n}.trace") for n in names}
    if not a.reuse:
        threads = []
        for k, n in enumerate(names):
            def go(n=n, port=a.port + k):
                rd.run("build-dev/alundra_2", "game.toml", a.replay, CONFIGS[n] + a.env, traces[n], a.stop,
                       port=port)
            t = threading.Thread(target=go)
            t.start()
            threads.append(t)
            time.sleep(15)                      # simultaneous starts can be refused
        for t in threads:
            t.join()
    ref = rd.read_trace(traces["original"])
    for n in names:
        if n == "original":
            continue
        cur = rd.read_trace(traces[n])
        print(f"== {n}: {len(cur)} frames (original {len(ref)})")
        for level in ("gpu", "vram", "ram", "cycle"):
            d = rd.first_difference(ref, cur, level)
            print(f"   {level:5} {'identical' if d is None else 'first difference at frame %d' % d[0]}")
        log = traces[n] + ".log"
        if os.path.exists(log):
            bad = [l.strip() for l in open(log, errors="replace") if "a2.lift" in l or "FATAL" in l]
            for l in bad[:3]:
                print("   log:", l[:160])
    return 0


if __name__ == "__main__":
    sys.exit(main())
