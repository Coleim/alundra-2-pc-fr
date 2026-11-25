#!/usr/bin/env python3
"""Extract every file of a PS1 CD image (MODE2/2352 .bin) and split the game's
archives into numbered sub-files.

  python3 tools/extract_disc.py alundra.cue [--out extracted]

Output (all of it is game data: keep it out of Git):
  <out>/files/...                 ISO 9660 files; XA/STR files as raw 2352-byte sectors
  <out>/archives/<NAME>/NNNN.bin  entries of PACK0/PACK1/PAKVCAST/PAKOMAKE (.PAK) and GRP_FR.GP
  <out>/manifest.json             sector map, file list, archive entry table
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

SECTOR = 2352
USER_OFF = 24
USER = 2048


class Disc:
    def __init__(self, bin_path: Path):
        self.f = open(bin_path, "rb")
        self.size = bin_path.stat().st_size
        if self.size % SECTOR:
            raise SystemExit(f"{bin_path}: not a multiple of {SECTOR} (need MODE2/2352)")

    def raw(self, lba: int, count: int = 1) -> bytes:
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def user(self, lba: int, size: int) -> bytes:
        out = bytearray()
        for i in range((size + USER - 1) // USER):
            self.f.seek((lba + i) * SECTOR + USER_OFF)
            out += self.f.read(USER)
        return bytes(out[:size])


def walk(disc: Disc, lba: int, size: int, path: str, out: list):
    data = disc.user(lba, size)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // USER + 1) * USER
            continue
        rec = data[i:i + n]
        elba, = struct.unpack("<I", rec[2:6])
        esize, = struct.unpack("<I", rec[10:14])
        flags = rec[25]
        nl = rec[32]
        name = rec[33:33 + nl]
        sysuse = rec[33 + nl + ((nl + 1) % 2):]
        xa_attr = struct.unpack(">H", sysuse[4:6])[0] if len(sysuse) >= 14 and sysuse[6:8] == b"XA" else 0
        if name not in (b"\x00", b"\x01"):
            nm = name.decode("ascii", "replace").split(";")[0]
            if flags & 2:
                walk(disc, elba, esize, f"{path}/{nm}", out)
            else:
                out.append({"path": f"{path}/{nm}".lstrip("/"), "lba": elba, "size": esize,
                            "raw": bool(xa_attr & 0x3000)})
        i += n


def split_table(blob: bytes, entry_words: int):
    """Archives start with a table of u32 byte offsets (non-increasing slots are unused)."""
    offs = list(struct.unpack_from(f"<{entry_words}I", blob, 0))
    return offs


def split_pak(blob: bytes):
    first = struct.unpack_from("<I", blob, 0)[0]
    words = first // 4
    offs = struct.unpack_from(f"<{words}I", blob, 0)
    entries = []
    for i, o in enumerate(offs):
        if o == 0 or o >= len(blob):
            continue
        nxt = len(blob)
        for p in offs[i + 1:]:
            if p > o:
                nxt = min(p, len(blob)) if p <= len(blob) else len(blob)
                break
        entries.append((i, o, nxt - o))
    return entries


def sniff(b: bytes) -> str:
    h = b[:16]
    if h[:4] == b"\x10\x00\x00\x00":
        return "tim"
    if h[:4] == b"\x60\x00\x00\x00":
        return "psx-xx"
    if h[:4] == b"\x41\x00\x00\x00":
        return "tmd"
    if h[:4] == b"\x00\x00\x01\xba":
        return "mpeg"
    if h[:2] == b"VA" or h[:4] == b"pQES":
        return "vab-seq"
    return "bin"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cue")
    ap.add_argument("--out", default="extracted")
    a = ap.parse_args()

    cue = Path(a.cue)
    m = re.search(r'FILE "(.*)" BINARY', cue.read_text())
    if not m:
        raise SystemExit("no FILE line in cue")
    disc = Disc(cue.parent / m.group(1))
    out = Path(a.out)

    pvd = disc.user(16, USER)
    if pvd[1:6] != b"CD001":
        raise SystemExit("not an ISO 9660 PlayStation disc")
    root = pvd[156:190]
    rlba, = struct.unpack("<I", root[2:6])
    rsize, = struct.unpack("<I", root[10:14])
    files = []
    walk(disc, rlba, rsize, "", files)

    manifest = {"sectors": disc.size // SECTOR, "files": [], "archives": {}}
    (out / "files").mkdir(parents=True, exist_ok=True)
    for fe in files:
        dst = out / "files" / fe["path"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        if fe["raw"]:
            nsec = (fe["size"] + USER - 1) // USER
            with open(dst, "wb") as fo:
                disc.f.seek(fe["lba"] * SECTOR)
                fo.write(disc.f.read(nsec * SECTOR))
            fe["stored_as"] = "raw2352"
        else:
            dst.write_bytes(disc.user(fe["lba"], fe["size"]))
            fe["stored_as"] = "user2048"
        manifest["files"].append(fe)
        print(f"  {fe['path']:32} {fe['size']:>11,} {fe['stored_as']}")

    for fe in files:
        name = fe["path"].rsplit("/", 1)[-1]
        if not (name.endswith(".PAK") or name.endswith(".GP")):
            continue
        blob = (out / "files" / fe["path"]).read_bytes()
        d = out / "archives" / name.rsplit(".", 1)[0]
        d.mkdir(parents=True, exist_ok=True)
        entries = split_pak(blob)
        rows = []
        for idx, off, size in entries:
            if size <= 0:
                continue
            chunk = blob[off:off + size]
            kind = sniff(chunk)
            (d / f"{idx:04d}.{kind}").write_bytes(chunk)
            rows.append({"index": idx, "offset": off, "size": size, "kind": kind})
        manifest["archives"][name] = rows
        kinds = {}
        for r in rows:
            kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
        print(f"  {name}: {len(rows)} entries {kinds}")

    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"done -> {out}")


if __name__ == "__main__":
    sys.exit(main())
