#!/usr/bin/env python3
"""Turn a runtime `cd_read_log` dump into a map of (file offset -> RAM address, size).

The log interleaves 12-byte header reads with 2048-byte data DMAs, and its `lba`
field is the seek position of the command, not of each sector.

  python3 tools/summarize_cd_loads.py cd_read_log.json extracted/manifest.json symbols/cd_loads.json
"""
import json
import sys


def main():
    log = json.load(open(sys.argv[1]))["entries"]
    files = sorted(json.load(open(sys.argv[2]))["files"], key=lambda f: f["lba"])

    def which(lba):
        for f in files:
            if f["lba"] <= lba < f["lba"] + (f["size"] + 2047) // 2048:
                return f["path"], (lba - f["lba"]) * 2048
        return "?unmapped", lba * 2048

    loads = []
    for e in log:
        if e["size"] <= 16:
            continue
        lba, dest, size = e["lba"], int(e["dest"], 16), e["size"]
        if loads and loads[-1]["lba"] == lba and loads[-1]["dest"] + loads[-1]["size"] == dest:
            loads[-1]["size"] += size
        else:
            name, off = which(lba)
            loads.append({"name": name, "lba": lba, "off": off, "dest": dest | 0x80000000, "size": size})
    uniq = {(l["name"], l["off"], l["dest"], l["size"]) for l in loads if l["name"] != "A2STR.STR"}
    out = [dict(zip(("name", "off", "dest", "size"), u)) for u in sorted(uniq)]
    json.dump(out, open(sys.argv[3], "w"), indent=0)
    print(f"{len(log)} DMA entries -> {len(out)} distinct loads")


if __name__ == "__main__":
    sys.exit(main())
