#!/usr/bin/env python3
"""Find where two runs of the same replay stop matching.

Run A is the reference (usually the original code), run B the candidate (rewritten code).
Both play the replay headless with the traces of src/a2_diag.c, compared at every waiting
VSync call, level by level:
  ram    hash of each 4 KiB page of main RAM (game state)
  gpu    what the game sent to the GPU during the frame (ordering tables, draw
         environments, VRAM transfers)
  vram   the whole VRAM (the picture)
  cycle  guest cycle count at the VSync call (timing)

  python3 tools/replay_diff.py saves/replays/<file>.psxrpl \
      --a A2_OVERRIDES=0 --b A2_ANALOG=0 [--stop 14000] [--bytes] [--gpu]

--bytes  replays both runs again up to the first RAM difference and lists the differing words.
--gpu    replays both runs again and diffs the GPU journals of the first frame that differs.
Exit status: 0 identical (cycles aside), 1 different, 2 failure.
"""
import argparse
import difflib
import os
import struct
import subprocess
import sys

PAGE = 4096
RAM_BASE = 0x80000000


ISOLATE = None


def run(exe, game, replay, env_pairs, trace, stop=None, dump_at=None, gpu_dump_at=None):
    env = dict(os.environ, A2_DIAG_TRACE=trace, PSX_REPLAY_EXIT_AT_END="1")
    for kv in env_pairs:
        k, _, v = kv.partition("=")
        env[k] = v
    if stop is not None:
        env["A2_DIAG_STOP"] = str(stop)
    if dump_at:
        env["A2_DIAG_DUMP_AT"] = ",".join(str(i) for i in dump_at)
    if gpu_dump_at:
        env["A2_DIAG_GPU_DUMP_AT"] = ",".join(str(i) for i in gpu_dump_at)
    cmd = [exe, "--headless", "--game", os.path.abspath(game), "--replay", replay, "--replay-fast"]
    if ISOLATE:
        cards = os.path.abspath(os.path.join(os.path.dirname(trace), "cards"))
        os.makedirs(cards, exist_ok=True)
        cmd += ["--debug-port", str(ISOLATE), "--memcard-dir", cards]
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_trace(path):
    """{index: dict(cycle, gpu, items, vram, pages)}"""
    data = open(path, "rb").read()
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0x54523241:                       # "A2RT": index, cycle, pages
        pages, = struct.unpack_from("<I", data, 4)
        fields, off = 3, 8
    elif magic == 0x32523241:                     # "A2R2": + gpu hash, gpu items, vram hash
        pages, fields = struct.unpack_from("<2I", data, 4)
        off = 12
    else:
        raise SystemExit(f"{path}: not a trace")
    size = 4 * (fields + pages)
    recs = {}
    for o in range(off, len(data) - size + 1, size):
        w = struct.unpack_from(f"<{fields + pages}I", data, o)
        r = {"cycle": w[1] | (w[2] << 32), "pages": w[fields:]}
        if fields >= 6:
            r.update(gpu=w[3], items=w[4], vram=w[5])
        recs[w[0]] = r
    return recs


def first_difference(a, b, level):
    for i in sorted(set(a) & set(b)):
        ra, rb = a[i], b[i]
        if level == "ram":
            pages = [p for p in range(len(ra["pages"])) if ra["pages"][p] != rb["pages"][p]]
            if pages:
                return i, pages
        elif level == "gpu":
            if "gpu" in ra and (ra["gpu"], ra["items"]) != (rb["gpu"], rb["items"]):
                return i, (ra["items"], rb["items"])
        elif level in ra and ra[level] != rb[level]:
            return i, (ra[level], rb[level])
    return None


def page_ranges(pages):
    out, start, prev = [], None, None
    for p in list(pages) + [None]:
        if start is not None and (p is None or p != prev + 1):
            out.append(f"{RAM_BASE + start * PAGE:#010x}-{RAM_BASE + (prev + 1) * PAGE - 1:#010x}")
            start = None
        if p is not None and start is None:
            start = p
        prev = p
    return out


def byte_diff(ram_a, ram_b, pages, limit):
    rows = []
    for p in pages:
        for off in range(p * PAGE, (p + 1) * PAGE, 4):
            wa, wb = ram_a[off:off + 4], ram_b[off:off + 4]
            if wa != wb:
                rows.append((RAM_BASE + off, int.from_bytes(wa, "little"), int.from_bytes(wb, "little")))
    for addr, wa, wb in rows[:limit]:
        print(f"    {addr:#010x}: A={wa:#010x} B={wb:#010x}")
    if len(rows) > limit:
        print(f"    ... {len(rows) - limit} more differing words")
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--a", action="append", default=[], help="KEY=VALUE for the reference run")
    ap.add_argument("--b", action="append", default=[], help="KEY=VALUE for the candidate run")
    ap.add_argument("--stop", type=int, help="stop both runs after this VSync index")
    ap.add_argument("--bytes", action="store_true", help="list the differing RAM words at the first RAM difference")
    ap.add_argument("--gpu", action="store_true", help="diff the GPU journals at the first GPU difference")
    ap.add_argument("--limit", type=int, default=24, help="lines to print per detail")
    ap.add_argument("--exe", default="build-dev/alundra_2")
    ap.add_argument("--game", default="game.toml")
    ap.add_argument("--out", default="decomp-build/replay_diff")
    ap.add_argument("--reuse", action="store_true", help="compare existing traces without replaying")
    ap.add_argument("--isolate", type=int, metavar="PORT",
                    help="own debug port and memory-card folder, to run next to other instances")
    a = ap.parse_args()
    global ISOLATE
    ISOLATE = a.isolate
    os.makedirs(a.out, exist_ok=True)
    ta, tb = os.path.join(a.out, "a.trace"), os.path.join(a.out, "b.trace")
    if not a.reuse:
        run(a.exe, a.game, a.replay, a.a, ta, a.stop)
        run(a.exe, a.game, a.replay, a.b, tb, a.stop)
    ra, rb = read_trace(ta), read_trace(tb)
    print(f"VSync records: A={len(ra)} B={len(rb)}")
    diffs = {}
    for level in ("ram", "gpu", "vram", "cycle"):
        d = first_difference(ra, rb, level)
        diffs[level] = d
        if d is None:
            print(f"  {level:5} identical")
        elif level == "ram":
            print(f"  {level:5} first difference at VSync {d[0]}: {len(d[1])} page(s) "
                  f"{', '.join(page_ranges(d[1])[:8])}")
        elif level == "gpu":
            print(f"  {level:5} first difference at VSync {d[0]}: items A={d[1][0]} B={d[1][1]}")
        elif level == "cycle":
            print(f"  {level:5} first difference at VSync {d[0]}: A={d[1][0]} B={d[1][1]} ({d[1][1] - d[1][0]:+d})")
        else:
            print(f"  {level:5} first difference at VSync {d[0]}")
    if diffs["ram"] or diffs["cycle"]:
        common = sorted(set(ra) & set(rb))
        pages, frames, drift = {}, 0, [rb[i]["cycle"] - ra[i]["cycle"] for i in common]
        for i in common:
            d = [p for p, (x, y) in enumerate(zip(ra[i]["pages"], rb[i]["pages"])) if x != y]
            frames += bool(d)
            for p in d:
                pages[p] = pages.get(p, 0) + 1
        print(f"  whole session: RAM differs in {frames} of {len(common)} frames; "
              f"cycle drift min {min(drift):+d} max {max(drift):+d}, last {drift[-1]:+d}")
        for p, n in sorted(pages.items(), key=lambda kv: -kv[1])[:10]:
            print(f"    page {RAM_BASE + p * PAGE:#010x}: {n} frames")
    if a.bytes and diffs["ram"]:
        i, pages = diffs["ram"]
        run(a.exe, a.game, a.replay, a.a, ta + ".dump", i, [i])
        run(a.exe, a.game, a.replay, a.b, tb + ".dump", i, [i])
        print(f"  differing RAM words at VSync {i}:")
        byte_diff(open(f"{ta}.dump.{i}.ram", "rb").read(), open(f"{tb}.dump.{i}.ram", "rb").read(),
                  pages, a.limit)
    if a.gpu and diffs["gpu"]:
        i = diffs["gpu"][0]
        run(a.exe, a.game, a.replay, a.a, ta + ".gpu", i, None, [i])
        run(a.exe, a.game, a.replay, a.b, tb + ".gpu", i, None, [i])
        ja = open(f"{ta}.gpu.{i}.gpu.txt").read().splitlines()
        jb = open(f"{tb}.gpu.{i}.gpu.txt").read().splitlines()
        print(f"  GPU journal of frame {i}: A={len(ja)} items, B={len(jb)} items")
        lines = list(difflib.unified_diff(ja, jb, "A", "B", n=1, lineterm=""))
        for line in lines[:a.limit]:
            print("    " + line)
        if len(lines) > a.limit:
            print(f"    ... {len(lines) - a.limit} more diff lines")
    return 0 if all(diffs[k] is None for k in ("ram", "gpu", "vram")) else 1


if __name__ == "__main__":
    sys.exit(main())
