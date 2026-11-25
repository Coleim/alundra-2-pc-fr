#!/usr/bin/env python3
"""Drive the recompiled runtime (debug build, TCP port 4370) from scripts.

Library use:   from runtime_ctl import Runtime
CLI use:       runtime_ctl.py frame | shot out.png | press BTN[+BTN] [frames] | turbo on|off
"""
import json
import socket
import sys
import time

BITS = {"SELECT": 0, "L3": 1, "R3": 2, "START": 3, "UP": 4, "RIGHT": 5, "DOWN": 6, "LEFT": 7,
        "L2": 8, "R2": 9, "L1": 10, "R1": 11, "TRIANGLE": 12, "CIRCLE": 13, "CROSS": 14, "SQUARE": 15}


def pad_word(spec: str) -> int:
    """'CROSS+UP' -> active-low 16-bit pad word."""
    word = 0xFFFF
    if spec.lower() in ("", "none"):
        return word
    for name in spec.upper().split("+"):
        word &= ~(1 << BITS[name]) & 0xFFFF
    return word


class Stalled(RuntimeError):
    """The emulation thread stopped advancing frames."""


class Runtime:
    def __init__(self, port=4370, host="127.0.0.1", stall_s=45.0, on_stall=None):
        self.addr = (host, port)
        self.stall_s = stall_s
        self.on_stall = on_stall

    def send(self, obj, timeout=60.0):
        s = socket.create_connection(self.addr, timeout=timeout)
        try:
            s.sendall((json.dumps(obj) + "\n").encode())
            buf = bytearray()
            depth = 0
            started = False
            in_str = esc = False
            pos = 0
            while True:
                chunk = s.recv(1 << 20)
                if not chunk:
                    break
                buf += chunk
                while pos < len(buf):
                    c = buf[pos]
                    pos += 1
                    if in_str:
                        if esc:
                            esc = False
                        elif c == 0x5C:
                            esc = True
                        elif c == 0x22:
                            in_str = False
                    elif c == 0x22:
                        in_str = True
                    elif c == 0x7B:
                        depth += 1
                        started = True
                    elif c == 0x7D:
                        depth -= 1
                        if started and depth == 0:
                            return json.loads(buf[:pos].decode())
            return json.loads(buf.decode() or "{}")
        finally:
            s.close()

    def frame(self):
        return self.send({"cmd": "frame"}).get("frame")

    def wait_frames(self, n, poll=0.02):
        start = self.frame()
        last, last_t = start, time.time()
        while True:
            f = self.frame() or 0
            if f >= start + n:
                return
            if f != last:
                last, last_t = f, time.time()
            elif time.time() - last_t > self.stall_s:
                if self.on_stall:
                    self.on_stall(f)
                raise Stalled(f"no frame progress for {self.stall_s:.0f}s at frame {f}")
            time.sleep(poll)

    def press(self, spec, hold=4, then_wait=20):
        self.send({"cmd": "press", "buttons": pad_word(spec), "frames": hold})
        self.wait_frames(hold + then_wait)

    def hold(self, spec):
        if spec.lower() == "none":
            self.send({"cmd": "clear_input"})
        else:
            self.send({"cmd": "set_input", "buttons": "%04X" % pad_word(spec)})

    def turbo(self, on):
        self.send({"cmd": "turbo", "enabled": 1 if on else 0})

    def shot(self, path):
        return self.send({"cmd": "screenshot", "path": path})

    def read_ram(self, addr, length):
        r = self.send({"cmd": "read_ram", "addr": "0x%08X" % addr, "len": length}, timeout=120)
        h = r.get("hex") or r.get("data") or ""
        return bytes.fromhex(h)


def main(argv):
    rt = Runtime()
    op = argv[0]
    if op == "frame":
        print(rt.frame())
    elif op == "shot":
        print(rt.shot(argv[1]))
    elif op == "press":
        rt.press(argv[1], int(argv[2]) if len(argv) > 2 else 4)
    elif op == "turbo":
        rt.turbo(argv[1] == "on")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
