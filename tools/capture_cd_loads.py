#!/usr/bin/env python3
"""Play to the first zone and dump the CD read log, then summarise it into symbols/cd_loads.json.

Run:  python3 tools/run_scenario.py capture_cd_loads:main
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay


def main(rt):
    play_to_gameplay.main(rt)
    play_to_gameplay.ensure_controllable(rt)
    for combo in ("UP", "LEFT", "DOWN", "RIGHT"):     # a little walking, more loads
        rt.hold(combo)
        rt.wait_frames(90)
        rt.hold("none")
    log = rt.send({"cmd": "cd_read_log", "tail": 65536}, timeout=180)
    os.makedirs("decomp-build", exist_ok=True)
    json.dump(log, open("decomp-build/cd_read_log.json", "w"))
    subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "summarize_cd_loads.py"),
                    "decomp-build/cd_read_log.json", "extracted/manifest.json", "symbols/cd_loads.json"],
                   check=True)
    return f"{log.get('total')} DMA entries"
