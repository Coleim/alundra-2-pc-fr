#!/usr/bin/env python3
"""Dump the controller records while the left stick is pushed right (analog mode)."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import play_to_gameplay


def u32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def main(rt):
    play_to_gameplay.main(rt)
    for frames in (5, 60, 240):
        rt.send({"cmd": "set_input", "buttons": "FFFF", "lx": 255, "ly": 128, "pad_type": 1})
        rt.wait_frames(frames)
        buf = rt.read_ram(0x8006AED0, 16)
        rec = rt.read_ram(0x8006AF20, 0x110)
        inp = rt.read_ram(0x80060C40, 0x18)
        print(f"after {frames} frames: pad buffer {buf.hex(' ')}")
        print(f"   record: state(+4)={struct.unpack_from('<H', rec, 4)[0]} mode(+0xC)={rec[0xC]} "
              f"held(+0x1C)={struct.unpack_from('<I', rec, 0x1C)[0]:#06x} "
              f"sticks rx={u32(rec,0xFC)} ry={u32(rec,0x100)} lx={u32(rec,0x104)} ly={u32(rec,0x108)}")
        print("   heading cmd:", rt.send({"cmd": "mod_cmd", "name": "a2_heading", "args": ""}))
        print(f"   input : held={struct.unpack_from('<H', inp, 2)[0]:#06x} dir={u32(inp,8)} angle={u32(inp,0x10)}")
    print("pad_status", rt.send({"cmd": "pad_status"}).get("slot0"))
    return "diag done"
