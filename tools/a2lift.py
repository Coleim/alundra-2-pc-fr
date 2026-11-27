#!/usr/bin/env python3
"""a2lift: translate the game's MIPS functions into C, exactly, instruction by instruction.

The C keeps the original's semantics (registers in CPUState, guest memory, delayed loads,
multiply/divide, GTE) and the framework's timing model for compiled code (one span charge
per basic block, an interrupt check at every control transfer), so a replay stays identical
to the original. Calls go through the guest dispatcher (a2_jal): the callee may be original,
hand-written or lifted.

    python3 tools/a2lift.py [--modules main,game00,...] [--out src/game/lifted]

Functions already rewritten by hand (A2_REWRITE in src/game/*.c, plus func_800A09C0) are
skipped, as are the PsyQ library functions of the main executable. A function the
translator cannot prove exact (code outside its bounds, unknown instructions, coprocessor 0,
syscall/break, a delayed-load hazard across a branch, ...) is left to the original code; the
reasons are counted in the report.
"""
import argparse
import bisect
import collections
import glob
import hashlib
import os
import re
import struct
import sys

def lib_start():
    """Start of the PsyQ library section of the main executable: the first recognised
    object or BIOS-call stub (libapi), whichever comes first."""
    import json
    starts = [int(o["addr"], 16) for o in json.load(open("symbols/psyq_objects.json")) if o["module"] == "main"]
    for line in open("symbols/bios_thunks.txt"):
        m = re.match(r"\s*\w+\s*=\s*(0x[0-9A-Fa-f]+)", line)
        if m:
            starts.append(int(m.group(1), 16))
    return min(starts)
REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
       "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp",
       "fp", "ra"]
GTE_DATA_READ_HELPER = 0xB08F8FAA
GTE_DATA_WRITE_HELPER = 0xF08FFFAA
GTE_CTRL_HELPER = 0xEC101010


def modules():
    out = [("main", "extracted/files/SLES_026.01", 0x800, 0x80017000, "decomp/asm/main/3110.s")]
    for name in ("GAME00", "GAME01", "GAME02", "GAME03", "EVENT00"):
        lc = name.lower()
        out.append((lc, f"extracted/files/{name}.BIN", 0, 0x80084ED0, f"decomp/asm/{lc}/code.s"))
    for p in sorted(glob.glob("config/pack1_*.yaml")):
        idx = os.path.basename(p)[6:8]
        src = glob.glob(f"extracted/archives/PACK1/00{idx}.*")
        if src:
            out.append((f"pack1_{idx}", src[0], 0, 0x80149AE8, f"decomp/asm/pack1_{idx}/code.s"))
    # AL22 libraries (PACK0 entries 106-109): four variants, loaded at 0x80010000
    for p in sorted(glob.glob("config/al22_*.yaml")):
        idx = os.path.basename(p)[5:8]
        out.append((f"al22_{idx}", f"extracted/archives/PACK0/0{idx}.bin", 0, 0x80010000,
                    f"decomp/asm/al22_{idx}/code.s"))
    return out


# ---------------------------------------------------------------- decoding

class Insn:
    __slots__ = ("addr", "word", "op", "rs", "rt", "rd", "sa", "fn", "imm", "uimm", "target", "text")

    def __init__(self, addr, word, text=""):
        self.addr, self.word, self.text = addr, word, text
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.fn = word & 63
        self.uimm = word & 0xFFFF
        self.imm = self.uimm - 0x10000 if self.uimm & 0x8000 else self.uimm
        self.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)

    # classification
    def is_branch(self):              # conditional, PC-relative
        return self.op in (4, 5, 6, 7) or (self.op == 1 and self.rt in (0, 1, 0x10, 0x11))

    def is_jump(self):
        return self.op in (2, 3) or (self.op == 0 and self.fn in (8, 9))

    def is_cti(self):
        return self.is_branch() or self.is_jump()

    def branch_target(self):
        return (self.addr + 4 + (self.imm << 2)) & 0xFFFFFFFF

    def is_load(self):                # CPU loads with a delay slot (incl. lwl/lwr)
        return 0x20 <= self.op <= 0x26

    def is_delayed_read(self):        # instructions whose GPR result arrives one instruction late
        return self.is_load() or (self.op == 0x12 and self.rs in (0, 2))

    def dest(self):
        """GPR written (0 if none)."""
        op, fn = self.op, self.fn
        if op == 0:
            if fn in (0, 2, 3, 4, 6, 7, 0x10, 0x12, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B):
                return self.rd
            if fn == 9:
                return self.rd
            return 0
        if op == 3:
            return 31
        if op == 1 and self.rt in (0x10, 0x11):
            return 31
        if 8 <= op <= 0xF or 0x20 <= op <= 0x26:
            return self.rt
        if op == 0x12 and self.rs in (0, 2):
            return self.rt
        return 0

    def sources(self):
        """GPRs read."""
        op, fn = self.op, self.fn
        if op == 0:
            if fn in (0, 2, 3):
                return {self.rt}
            if fn in (4, 6, 7):
                return {self.rs, self.rt}
            if fn in (8, 9):
                return {self.rs}
            if fn in (0x11, 0x13):
                return {self.rs}
            if fn in (0x18, 0x19, 0x1A, 0x1B) or 0x20 <= fn <= 0x2B:
                return {self.rs, self.rt}
            return set()
        if op == 1:
            return {self.rs}
        if op in (4, 5):
            return {self.rs, self.rt}
        if op in (6, 7):
            return {self.rs}
        if 8 <= op <= 0xE:
            return {self.rs}
        if op in (0x22, 0x26):                 # lwl/lwr merge with rt
            return {self.rs, self.rt}
        if 0x20 <= op <= 0x26:
            return {self.rs}
        if 0x28 <= op <= 0x2E:
            return {self.rs, self.rt}
        if op == 0x12 and self.rs in (4, 6):
            return {self.rt}
        if op in (0x32, 0x3A):
            return {self.rs}
        return set()


class Unsupported(Exception):
    pass


def span_cost(insns):
    return sum(1 + (2 if (0x20 <= i.op <= 0x26 or i.op == 0x32) else 0) for i in insns)


# ---------------------------------------------------------------- input parsing

GLABEL = re.compile(r"^glabel (\S+)")
LINE = re.compile(r"^\s+/\* [0-9A-F]+ ([0-9A-F]{8}) ([0-9A-F]{8}) \*/\s*(.*)$")


def parse_asm(path):
    """[(name, [(addr, word, text)])] in file order."""
    funcs, cur = [], None
    for line in open(path):
        m = GLABEL.match(line)
        if m:
            cur = (m.group(1), [])
            funcs.append(cur)
            continue
        if line.startswith("endlabel"):
            cur = None
            continue
        m = LINE.match(line)
        if m and cur is not None:
            raw = bytes.fromhex(m.group(2))
            funcs[-1][1].append((int(m.group(1), 16), struct.unpack("<I", raw)[0], m.group(3).strip()))
    return [f for f in funcs if f[1]]


def parse_jump_tables(asm_dir):
    tables = {}
    for p in glob.glob(os.path.join(asm_dir, "data", "*.s")):
        cur = None
        for line in open(p):
            m = re.match(r"^dlabel (jtbl_([0-9A-F]{8}))", line)
            if m:
                cur = int(m.group(2), 16)
                tables[cur] = []
                continue
            if line.startswith("enddlabel"):
                cur = None
                continue
            if cur is not None:
                m = re.search(r"\.word (?:\.L|0x)([0-9A-F]{8})", line)
                if m and int(m.group(1), 16) >= 0x80000000:
                    tables[cur].append(int(m.group(1), 16))
                elif ".word" in line and tables[cur]:
                    cur = None               # padding after the table
    return tables


def handwritten():
    names = {"func_800A09C0"}
    for p in glob.glob("src/game/*.c"):
        names.update(re.findall(r"A2_REWRITE(?:_OVERLAY)?\(\s*(\w+)\s*,", open(p).read()))
    return names


# ---------------------------------------------------------------- C emission

def r(n):
    return "0u" if n == 0 else REG[n]


def hx(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


def simm(i):
    return f"{i.imm}" if i.imm >= 0 else f"- {-i.imm}"


def addr_expr(i):
    base = r(i.rs)
    if i.imm == 0:
        return base
    return f"{base} + {i.imm}" if i.imm > 0 else f"{base} - {-i.imm}"


def note(i):
    m = re.search(r"%(?:hi|lo)\((\w+)\)", i.text)
    return f"  /* {m.group(1)} */" if m else ""


def assign(rd, expr):
    if rd == 0:
        return f"(void)({expr});"
    return f"{REG[rd]} = {expr};"


PRECISE = False                              # set per module: overlays follow the interpreter's model


def load_value(i):
    """C expression of the value a delayed-read instruction produces."""
    a = addr_expr(i)
    op = i.op
    t = f", {i.rt}u, 0x{i.word:08X}u" if PRECISE else ""
    if op == 0x20:
        return f"LB({a}{t})"
    if op == 0x21:
        return f"LH({a}{t})"
    if op == 0x23:
        return f"LW({a}{t})"
    if op == 0x24:
        return f"LBU({a}{t})"
    if op == 0x25:
        return f"LHU({a}{t})"
    if op == 0x22:
        return f"LWL({a}, {r(i.rt)}{t})" if PRECISE else f"LWL({a}, {r(i.rt)}, {i.rt})"
    if op == 0x26:
        return f"LWR({a}, {r(i.rt)}{t})" if PRECISE else f"LWR({a}, {r(i.rt)}, {i.rt})"
    if op == 0x12 and i.rs == 0:                # mfc2
        return f"GTE_MFC2({i.rd})" if (GTE_DATA_READ_HELPER >> i.rd) & 1 else f"cpu->gte_data[{i.rd}]"
    if op == 0x12 and i.rs == 2:                # cfc2
        return f"GTE_CFC2({i.rd})" if (GTE_CTRL_HELPER >> i.rd) & 1 else f"cpu->gte_ctrl[{i.rd}]"
    raise Unsupported(f"load {i.word:08X}")


def stmt(i):
    """C statement for a non-control-transfer, non-delayed-read instruction."""
    op, fn, rs, rt, rd = i.op, i.fn, i.rs, i.rt, i.rd
    if i.word == 0:
        return "/* nop */"
    if op == 0:
        if fn == 0:
            return assign(rd, f"{r(rt)} << {i.sa}")
        if fn == 2:
            return assign(rd, f"{r(rt)} >> {i.sa}")
        if fn == 3:
            return assign(rd, f"(uint32_t)((int32_t){r(rt)} >> {i.sa})")
        if fn == 4:
            return assign(rd, f"{r(rt)} << ({r(rs)} & 31)")
        if fn == 6:
            return assign(rd, f"{r(rt)} >> ({r(rs)} & 31)")
        if fn == 7:
            return assign(rd, f"(uint32_t)((int32_t){r(rt)} >> ({r(rs)} & 31))")
        if fn == 0x10:
            return f"MULDIV_STALL(); {assign(rd, 'cpu->hi')}"
        if fn == 0x12:
            return f"MULDIV_STALL(); {assign(rd, 'cpu->lo')}"
        if fn == 0x11:
            return f"cpu->hi = {r(rs)};"
        if fn == 0x13:
            return f"cpu->lo = {r(rs)};"
        if fn == 0x18:
            return f"MULT({r(rs)}, {r(rt)});"
        if fn == 0x19:
            return f"MULTU({r(rs)}, {r(rt)});"
        if fn == 0x1A:
            return f"DIV({r(rs)}, {r(rt)});"
        if fn == 0x1B:
            return f"DIVU({r(rs)}, {r(rt)});"
        if fn == 0x21:
            return assign(rd, f"{r(rs)} + {r(rt)}")
        if fn == 0x23:
            return assign(rd, f"{r(rs)} - {r(rt)}")
        if fn == 0x24:
            return assign(rd, f"{r(rs)} & {r(rt)}")
        if fn == 0x25:
            return assign(rd, f"{r(rs)} | {r(rt)}")
        if fn == 0x26:
            return assign(rd, f"{r(rs)} ^ {r(rt)}")
        if fn == 0x27:
            return assign(rd, f"~({r(rs)} | {r(rt)})")
        if fn == 0x2A:
            return assign(rd, f"(int32_t){r(rs)} < (int32_t){r(rt)}")
        if fn == 0x2B:
            return assign(rd, f"{r(rs)} < {r(rt)}")
        if fn == 0x0D:                                # break: compiler checks (division by zero)
            return f"return a2_break(cpu, 0x{(i.word >> 6) & 0xFFFFF:X}u, {hx(i.addr)});"
        raise Unsupported(f"special fn {fn:#x}")      # add/sub (trapping), syscall
    if op == 9:
        if rs == 0:
            return assign(rt, hx(i.imm)) + note(i)
        if i.imm == 0:
            return assign(rt, r(rs)) + note(i)
        return assign(rt, f"{r(rs)} + {i.imm}" if i.imm > 0 else f"{r(rs)} - {-i.imm}") + note(i)
    if op == 0xA:
        return assign(rt, f"(int32_t){r(rs)} < {i.imm}")
    if op == 0xB:
        return assign(rt, f"{r(rs)} < {hx(i.imm)}")
    if op == 0xC:
        return assign(rt, f"{r(rs)} & 0x{i.uimm:X}u")
    if op == 0xD:
        return assign(rt, f"{r(rs)} | 0x{i.uimm:X}u")
    if op == 0xE:
        return assign(rt, f"{r(rs)} ^ 0x{i.uimm:X}u")
    if op == 0xF:
        return assign(rt, hx(i.uimm << 16)) + note(i)
    if op == 0x28:
        return f"SB({addr_expr(i)}, {r(rt)});" + note(i)
    if op == 0x29:
        return f"SH({addr_expr(i)}, {r(rt)});" + note(i)
    if op == 0x2B:
        return f"SW({addr_expr(i)}, {r(rt)});" + note(i)
    if op == 0x2A:
        return f"SWL({addr_expr(i)}, {r(rt)});"
    if op == 0x2E:
        return f"SWR({addr_expr(i)}, {r(rt)});"
    if op == 0x12:
        if i.rs == 4:
            return f"GTE_MTC2({i.rd}, {r(rt)});" if (GTE_DATA_WRITE_HELPER >> i.rd) & 1 \
                else f"GTE_STALL(); cpu->gte_data[{i.rd}] = {r(rt)};"
        if i.rs == 6:
            return f"GTE_CTC2({i.rd}, {r(rt)});" if (GTE_CTRL_HELPER >> i.rd) & 1 \
                else f"GTE_STALL(); cpu->gte_ctrl[{i.rd}] = {r(rt)};"
        if i.rs & 0x10:
            return f"GTE_CMD(0x{i.word & 0x1FFFFFF:07X});"
        raise Unsupported(f"cop2 {i.word:08X}")
    if op == 0x32:
        if (GTE_DATA_WRITE_HELPER >> rt) & 1:
            return f"LWC2_HELPER({rt}, {addr_expr(i)});"
        return f"LWC2({rt}, {addr_expr(i)});"
    if op == 0x3A:
        if (GTE_DATA_READ_HELPER >> rt) & 1:
            return f"SWC2_HELPER({rt}, {addr_expr(i)});"
        return f"SWC2({rt}, {addr_expr(i)});"
    if op == 0x2F:
        return "/* cache */"
    raise Unsupported(f"opcode {op:#x}")


def branch_cond(i):
    rs, rt = r(i.rs), r(i.rt)
    if i.op == 4:
        return f"{rs} == {rt}"
    if i.op == 5:
        return f"{rs} != {rt}"
    if i.op == 6:
        return f"(int32_t){rs} <= 0"
    if i.op == 7:
        return f"(int32_t){rs} > 0"
    if i.op == 1 and i.rt in (0, 0x10):
        return f"(int32_t){rs} < 0"
    if i.op == 1 and i.rt in (1, 0x11):
        return f"(int32_t){rs} >= 0"
    raise Unsupported("branch")


class Lifter:
    def __init__(self, name, insns, start, end, tables, all_starts, read_word=None, entries=None):
        self.name, self.insns, self.start, self.end = name, insns, start, end
        self.entries = sorted(set(entries or [start]))
        self.tables, self.all_starts = tables, all_starts
        self.read_word = read_word
        self.by_addr = {i.addr: i for i in insns}
        self.labels_used = set()
        self.external = set()          # branch targets in another function (conditional tail calls)
        self.sync_labels = set()       # successors of a branch whose delay slot leaves a load pending

    def inside(self, a):
        return self.start <= a < self.end

    def table_for(self, jr):
        """Jump table used by this `jr`. The dispatch is `addu rX, rX, BASE; lw rX, 0(rX);
        jr rX`; BASE was set from a table symbol (lui/addiu %hi/%lo), maybe much earlier.
        Tables splat did not label are read from the module image, bounded by the
        `sltiu` range check when there is one, and only while entries point inside the
        function."""
        k = self.insns.index(jr)
        best = None
        adder = None
        for i in reversed(self.insns[max(0, k - 6):k]):
            if i.op == 0 and i.fn == 0x21 and i.rd == jr.rs:
                adder = i
                break
        if adder is not None:
            # the operand whose last definition before the add is `addiu r, r, %lo(SYM)`
            ka = self.insns.index(adder)
            for reg in (adder.rs, adder.rt):
                for i in reversed(self.insns[:ka]):
                    if i.dest() == reg:
                        m = re.search(r"%lo\((?:jtbl|D)_([0-9A-F]{8})\)", i.text)
                        if m and i.op == 9:
                            best = int(m.group(1), 16)
                        break
                if best is not None:
                    break
        if best is None:
            for i in self.insns:
                if i.addr >= jr.addr:
                    break
                m = re.search(r"%lo\(jtbl_([0-9A-F]{8})\)", i.text)
                if m:
                    best = int(m.group(1), 16)
        if best is not None and best not in self.tables and self.read_word:
            limit = 1024
            for i in reversed(self.insns[max(0, k - 12):k]):
                if i.op == 0xB:              # sltiu range check
                    limit = i.uimm
                    break
            entries = []
            for n in range(limit):
                w = self.read_word(best + 4 * n)
                if w is None or not self.inside(w) or w & 3:
                    break
                entries.append(w)
            if entries:
                self.tables[best] = entries
        return best

    def leaders(self):
        lead = {self.start} | set(self.entries)
        for i in self.insns:
            if i.is_branch():
                t = i.branch_target()
                if not self.inside(t):
                    if i.op == 1 and i.rt in (0x10, 0x11):
                        raise Unsupported(f"branch and link out of function at {i.addr:08X}")
                    self.external.add(t)       # shared code of hand-written assembly
                else:
                    lead.add(t)
                lead.add(i.addr + 8)
            elif i.is_jump():
                if i.op == 2 and self.inside(i.target):
                    lead.add(i.target)
                if i.op == 0 and i.fn == 8 and i.rs != 31:
                    tbl = self.table_for(i)
                    if tbl is None or tbl not in self.tables:
                        raise Unsupported(f"jr {REG[i.rs]} without jump table at {i.addr:08X}")
                    for t in self.tables[tbl]:
                        if not self.inside(t):
                            raise Unsupported(f"jump table target outside at {i.addr:08X}")
                        lead.add(t)
                lead.add(i.addr + 8)
        return sorted(a for a in lead if self.inside(a))

    def falls_through(self):
        """The code runs past the last instruction into the next function (splat cut there)."""
        last = self.insns[-2] if len(self.insns) >= 2 else None
        return not (last is not None and last.is_jump() and last.op != 3 and not (last.op == 0 and last.fn == 9))

    def check(self):
        if self.falls_through() and self.end not in self.all_starts:
            raise Unsupported("code falls past the end of the function")
        for i in self.insns:
            if i.is_cti():
                d = self.by_addr.get(i.addr + 4)
                if d is None or d.is_cti():
                    raise Unsupported(f"delay slot at {i.addr + 4:08X}")
            if i.op in (0x10, 0x11, 0x13) or (i.op == 0 and i.fn in (0x0C, 0x20, 0x22)) or i.op == 8:
                raise Unsupported(f"instruction {i.word:08X} at {i.addr:08X}")

    def pending_load_slots(self):
        """Branches whose delay slot loads a register that a successor's first instruction
        reads or writes (it must see the old value), or that leave the function: the load is
        armed in the framework's load-delay pipeline, and the successors commit it."""
        self.armed = set()
        for i in self.insns:
            if not i.is_branch():
                continue
            d = self.by_addr.get(i.addr + 4)
            if d is None or not (d.is_delayed_read() and d.dest()):
                continue
            succ = [i.branch_target(), i.addr + 8]
            if not any(not self.inside(x) or self.hazard(d, self.by_addr.get(x)) for x in succ):
                continue
            self.armed.add(i.addr)
            for x in succ:
                if self.inside(x):
                    first = self.by_addr.get(x)
                    if first is None or first.is_cti() or \
                            (first.is_delayed_read() and self.hazard(first, self.by_addr.get(x + 4))):
                        raise Unsupported(f"pending load into a branch or load at {i.addr:08X}")
                    self.sync_labels.add(x)

    # ---- emission
    def emit(self):
        self.check()
        leaders = self.leaders()
        lead_set = set(leaders)
        self.lead_set = lead_set
        self.pending_load_slots()
        # further entries: a load the caller left pending is committed there too
        self.sync_labels.update(e for e in self.entries if e != self.start)
        self.labels_used.update(self.entries)
        lines = []
        out = lines.append
        idx = {i.addr: k for k, i in enumerate(self.insns)}
        k = 0
        n = len(self.insns)
        sync = 2                                   # commit a load pending from the caller
        while k < n:
            blk_start = self.insns[k].addr
            # block extent: up to a CTI (+ delay slot) or the next leader
            j = k
            while j < n:
                ins = self.insns[j]
                if ins.is_cti():
                    j += 2
                    break
                j += 1
                if j < n and self.insns[j].addr in lead_set:
                    break
            block = self.insns[k:j]
            out(f"L_{blk_start:08X}:")
            if not PRECISE:
                out(f"    A2_SPAN({span_cost(block)});")
            cancel = None
            if blk_start in self.sync_labels:
                sync = 2                           # a load from the branch delay slot is pending
                cancel = block[0].dest()           # an instruction writing its register wins
            m = 0
            while m < len(block):
                ins = block[m]
                pre = "    "
                if sync:
                    pre += "A2_SYNC(); "
                    sync -= 1
                if ins.is_cti():
                    delay = block[m + 1]
                    lines.extend(self.emit_cti(ins, delay, pre, lead_set))
                    if ins.op == 3 or (ins.op == 0 and ins.fn == 9) or (ins.op == 1 and ins.rt in (0x10, 0x11)):
                        sync = 2                    # after a call: the callee may leave a pending load
                    m += 2
                    continue
                if ins.is_delayed_read() and ins.dest():
                    nxt = block[m + 1] if m + 1 < len(block) else self.by_addr.get(ins.addr + 4)
                    lines.extend(self.emit_delayed(ins, nxt, pre, block, m))
                    if cancel and m == 0:          # a later load of the same register wins
                        out(f"    psx_load_value_cancel(cpu, {cancel}u);")
                    m += 2 if (nxt is not None and m + 1 < len(block) and self.hazard(ins, nxt)) else 1
                    continue
                out(f"{pre}{self.timing(ins)}{stmt(ins)}")
                if cancel and m == 0:              # it overwrites the pending load's register
                    out(f"    psx_load_value_cancel(cpu, {cancel}u);")
                m += 1
            k = j
        return lines

    def timing(self, ins):
        """Per-instruction timing of the interpreter's model (precise mode only): the
        I-cache fetch at line starts and block leaders, a step boundary otherwise, and the
        interlock step for every non-load instruction (loads time themselves)."""
        if not PRECISE:
            return ""
        t = f"F(0x{ins.addr:08X}u); " if (ins.addr in self.lead_set or (ins.addr & 0xC) == 0) \
            else f"B(0x{ins.addr:08X}u); "
        if not (0x20 <= ins.op <= 0x26):
            t += f"S(0x{ins.word:08X}u); "
        return t

    def hazard(self, ld, nxt):
        """True if the instruction after a delayed read sees the old value (or overwrites it)."""
        if nxt is None:
            return False
        rt = ld.dest()
        if nxt.op in (0x22, 0x26) and nxt.rt == rt and ld.op in (0x22, 0x26):
            return False                           # lwl/lwr pair merges with the pending value
        return rt in nxt.sources() or nxt.dest() == rt

    def emit_delayed(self, ld, nxt, pre, block, m):
        rt = ld.dest()
        val = load_value(ld)
        stall = ""
        if ld.op == 0x12:
            stall = f"GTE_READ({rt}); "
        tm = self.timing(ld)
        if nxt is None or not self.hazard(ld, nxt):
            return [f"{pre}{tm}{stall}{REG[rt]} = {val};{note(ld)}"]
        if nxt.is_cti():
            raise Unsupported(f"load hazard into a branch at {ld.addr:08X}")
        if nxt.is_delayed_read() and nxt.dest():
            raise Unsupported(f"chained delayed reads at {ld.addr:08X}")
        out = [f"{pre}{tm}{stall}{{ uint32_t ld_ = {val};"]
        out.append(f"      {self.timing(nxt)}{stmt(nxt)}  /* sees the old {REG[rt]} (load delay) */")
        if nxt.dest() != rt:
            out.append(f"      {REG[rt]} = ld_; }}")
        else:
            out.append("      (void)ld_; }")
        return out

    def delay_stmt(self, d, after_call):
        """Statement for a delay slot. A delayed read there is armed for the next instruction."""
        tm = self.timing(d)
        if d.is_delayed_read() and d.dest():
            stall = f"GTE_READ({d.dest()}); " if d.op == 0x12 else ""
            if after_call:
                return f"{tm}{stall}psx_load_value_arm(cpu, {d.dest()}u, {load_value(d)});"
            return None
        return tm + stmt(d)

    def goto(self, target, pre):
        self.labels_used.add(target)
        return [f"{pre}A2_EDGE({hx(target)}); goto L_{target:08X};"]

    def emit_cti(self, i, d, pre, lead_set):
        out = []
        nxt = i.addr + 8
        if i.is_branch():
            link = i.op == 1 and i.rt in (0x10, 0x11)
            if link:
                raise Unsupported(f"branch and link at {i.addr:08X}")
            t = i.branch_target()
            ds = self.delay_stmt(d, False)
            if i.addr in self.armed:
                ds = self.delay_stmt(d, True)
            elif ds is None:
                # delayed read in a branch delay slot: both successors must not use it
                for s in (t, nxt):
                    first = self.by_addr.get(s)
                    if first is not None and self.hazard(d, first):
                        raise Unsupported(f"load hazard after branch at {i.addr:08X}")
                ds = f"{self.timing(d)}{'GTE_READ(%d); ' % d.dest() if d.op == 0x12 else ''}{REG[d.dest()]} = {load_value(d)};"
            out.append(f"{pre}{self.timing(i)}{{ int c_ = {branch_cond(i)};")
            out.append(f"      {ds}")
            self.labels_used.update((t, nxt))
            if self.inside(nxt) and not self.inside(t):
                out.append(f"      if (c_) {{ A2_EDGE({hx(t)}); a2_tail(cpu, {hx(t)}); return 1; }}")
                out.append(f"      A2_EDGE({hx(nxt)}); goto L_{nxt:08X}; }}")
            elif self.inside(nxt):
                out.append(f"      if (c_) {{ A2_EDGE({hx(t)}); goto L_{t:08X}; }}")
                out.append(f"      A2_EDGE({hx(nxt)}); goto L_{nxt:08X}; }}")
            else:
                raise Unsupported(f"branch falls out of function at {i.addr:08X}")
            return out
        if i.op == 2:                               # j
            t = i.target
            ds = self.delay_stmt(d, not self.inside(t))
            if ds is None:
                raise Unsupported(f"load in j delay slot at {i.addr:08X}")
            out.append(f"{pre}{self.timing(i)}{ds}")
            if self.inside(t):
                out.extend(self.goto(t, "    "))
            else:                                   # tail call
                out.append(f"    A2_EDGE({hx(t)}); a2_tail(cpu, {hx(t)}); return 1;")
            return out
        if i.op == 3:                               # jal
            t = i.target
            out.append(f"{pre}{self.timing(i)}ra = {hx(nxt)};")
            out.append(f"    {self.delay_stmt(d, True)}")
            out.append(f"    A2_EDGE({hx(t)}); a2_jal(cpu, {hx(t)}, {hx(nxt)});")
            if not self.inside(nxt):
                raise Unsupported(f"call at the end of the function at {i.addr:08X}")
            return out
        if i.op == 0 and i.fn == 9:                 # jalr
            out.append(f"{pre}{self.timing(i)}{{ uint32_t t_ = {r(i.rs)};")
            if i.rd:
                out.append(f"      {REG[i.rd]} = {hx(nxt)};")
            out.append(f"      {self.delay_stmt(d, True)}")
            out.append(f"      A2_EDGE(t_); a2_jal(cpu, t_, {hx(nxt)}); }}")
            return out
        if i.op == 0 and i.fn == 8:                 # jr
            if i.rs == 31:
                out.append(f"{pre}{self.timing(i)}{{ uint32_t t_ = ra;")
                out.append(f"      {self.delay_stmt(d, True)}")
                out.append("      A2_EDGE(t_); A2_RETURN(t_); }")
                return out
            tbl = self.table_for(i)
            targets = list(dict.fromkeys(self.tables[tbl]))
            ds = self.delay_stmt(d, False)
            if ds is None:
                raise Unsupported(f"load in jr delay slot at {i.addr:08X}")
            out.append(f"{pre}{self.timing(i)}{{ uint32_t t_ = {r(i.rs)};")
            out.append(f"      {ds}")
            out.append(f"      switch (t_) {{  /* jtbl_{tbl:08X} */")
            for t in targets:
                self.labels_used.add(t)
                out.append(f"      case {hx(t)}: A2_EDGE({hx(t)}); goto L_{t:08X};")
            out.append("      default: return a2_bad_jump(cpu, t_);")
            out.append("      } }")
            return out
        raise Unsupported("cti")


# ---------------------------------------------------------------- driver

def guard_words(insns):
    """First words of the function (up to 4) as the residency guard."""
    return [i.word for i in insns[:4]]


def data_pointers(asm_dir):
    """Values of the module's data words (function pointers among them)."""
    found = set()
    for root, _, files in os.walk(asm_dir):
        for f in files:
            if f.endswith(".s"):
                for line in open(os.path.join(root, f), errors="replace"):
                    m = re.search(r"\.word (?:0x)?([0-9A-Fa-f]{8})\b", line)
                    if m:
                        found.add(int(m.group(1), 16))
    return found


def units(bodies, names, skip, pointers=()):
    """Group the functions into units lifted as one C body. A function that jumps into
    another one (a branch or `j`, not a call) and is jumped back into from it forms a loop
    across functions (hand-written assembly): such functions, and the ones between them,
    become one unit, so the jumps stay jumps instead of nesting calls. A jump into the middle
    of a function adds an entry to its unit."""
    spans = [(b[0].addr, b[-1].addr + 4) for b in bodies]
    starts = [s for s, _ in spans]

    def owner(a):
        k = bisect.bisect_right(starts, a) - 1
        return k if k >= 0 and spans[k][0] <= a < spans[k][1] else None
    edges = collections.defaultdict(set)
    inner = collections.defaultdict(set)
    # a data word pointing inside a function, at an instruction: an entry reached through a
    # pointer (callbacks such as a shared "return 0" epilogue)
    for v in pointers:
        j = owner(v)
        if j is not None and v != spans[j][0] and not v & 3:
            inner[j].add(v)
    for k, b in enumerate(bodies):
        lo, hi = spans[k]
        for i in b:
            if i.is_branch() and not (i.op == 1 and i.rt in (0x10, 0x11)):
                t = i.branch_target()
            elif i.op == 2:
                t = i.target
            else:
                continue
            if lo <= t < hi:
                continue
            j = owner(t)
            if j is None:
                continue
            edges[k].add(j)
            if t != spans[j][0]:
                inner[j].add(t)
    # strongly connected components (iterative Tarjan)
    index, low, on, stack, comp = {}, {}, set(), [], {}
    counter = [0]
    for root in range(len(bodies)):
        if root in index:
            continue
        work = [(root, iter(sorted(edges[root])))]
        index[root] = low[root] = counter[0]; counter[0] += 1
        stack.append(root); on.add(root)
        while work:
            v, it = work[-1]
            w = next(it, None)
            if w is not None:
                if w not in index:
                    index[w] = low[w] = counter[0]; counter[0] += 1
                    stack.append(w); on.add(w)
                    work.append((w, iter(sorted(edges[w]))))
                elif w in on:
                    low[v] = min(low[v], index[w])
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[v])
            if low[v] == index[v]:
                while True:
                    w = stack.pop(); on.discard(w)
                    comp[w] = v
                    if w == v:
                        break
    groups = collections.defaultdict(list)
    for k in range(len(bodies)):
        groups[comp[k]].append(k)
    member_of = list(range(len(bodies)))
    for g in groups.values():
        if len(g) > 1:
            for k in range(min(g), max(g) + 1):     # the functions in between join too
                member_of[k] = min(g)
    out = collections.OrderedDict()
    for k in range(len(bodies)):
        out.setdefault(member_of[k], []).append(k)
    result = []
    for members in out.values():
        entries = set()
        for k in members:
            entries.add(spans[k][0])
            entries.update(inner[k])
        result.append({"members": members, "entries": sorted(entries)})
    return result


def distinguishing_checks(entries):
    """Variants loaded at the same address may start with the same words (shared prologue,
    jump slots...): give each one further (address, word) checks, anywhere in its body, that
    tell it apart from every other variant with the same first words."""
    by_addr = collections.defaultdict(list)
    for e in entries:
        by_addr[e["addr"]].append(e)
    for group in by_addr.values():
        for e in group:
            e["checks"] = []
            others = [o for o in group if o is not e and o["words"][:4] == e["words"][:4]]
            while others:
                best, best_n = None, 0
                for a, w in e["mem"].items():
                    n = sum(1 for o in others if a in o["mem"] and o["mem"][a] != w)
                    if n > best_n:
                        best, best_n = a, n
                if best is None or len(e["checks"]) == 4:
                    print(f"  warning: {e['cname']}: cannot tell apart from {[o['cname'] for o in others]}")
                    break
                e["checks"].append(((best - e["addr"]) // 4, e["mem"][best]))
                others = [o for o in others if not (best in o["mem"] and o["mem"][best] != e["mem"][best])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modules", default="")
    ap.add_argument("--out", default="src/game/lifted")
    ap.add_argument("--per-file", type=int, default=120)
    a = ap.parse_args()
    wanted = set(filter(None, a.modules.split(",")))
    skip = handwritten()
    os.makedirs(a.out, exist_ok=True)
    for p in glob.glob(os.path.join(a.out, "*.c")):
        os.remove(p)

    stats = collections.Counter()
    reasons = collections.Counter()
    entries = []                                # dicts: addr, mod, name, cname, words
    texts = collections.defaultdict(list)       # module -> C text of each lifted function
    seen = {}
    global PRECISE
    lib_base = lib_start()
    for mod, path, skip_bytes, base, asm in modules():
        if wanted and mod not in wanted:
            continue
        PRECISE = mod != "main"              # main: compiled code (spans); overlays: interpreter
        if not os.path.exists(asm):
            continue
        funcs = parse_asm(asm)
        tables = parse_jump_tables(os.path.dirname(asm))
        image = open(path, "rb").read()[skip_bytes:]

        def read_word(addr, image=image, base=base):
            off = addr - base
            if 0 <= off <= len(image) - 4:
                return struct.unpack_from("<I", image, off)[0]
            return None
        starts = {f[1][0][0] for f in funcs}
        bodies, names = [], []
        for name, rows in funcs:
            insns = [Insn(addr, word, text) for addr, word, text in rows]
            start, end = insns[0].addr, insns[-1].addr + 4
            if mod == "main" and start >= lib_base:
                continue
            if all(i.word == 0 for i in insns):     # alignment padding, not a function
                continue
            stats["functions"] += 1
            if name == funcs[-1][0]:
                # last function of the split: the code may run past the configured end of the
                # code segment; take the rest from the image, up to its `jr ra` + delay slot
                probe = Lifter(name, insns, start, end, tables, starts, read_word)
                if probe.falls_through():
                    pc = end
                    while pc - start < 0x4000:
                        w = read_word(pc)
                        if w is None:
                            break
                        insns.append(Insn(pc, w))
                        pc += 4
                        if len(insns) >= 2 and insns[-2].op == 0 and insns[-2].fn == 8 and insns[-2].rs == 31:
                            break
            if name in skip:
                stats["hand-written"] += 1
            bodies.append(insns)
            names.append(name)
        # data words pointing into code, other than jump table entries (labels of their switch)
        jt = {t for v in tables.values() for t in v}
        for b in bodies:                        # tables splat did not label, as the lifter reads them
            lf_probe = Lifter("probe", b, b[0].addr, b[-1].addr + 4, tables, starts, read_word)
            for i in b:
                if i.op == 0 and i.fn == 8 and i.rs != 31:
                    try:
                        jt.update(tables.get(lf_probe.table_for(i), []))
                    except (ValueError, KeyError, IndexError):
                        pass
        pointers = data_pointers(os.path.dirname(asm)) - jt
        for unit in units(bodies, names, skip, pointers):
            insns = [i for k in unit["members"] for i in bodies[k]]
            lo, hi = insns[0].addr, insns[-1].addr + 4
            body_hash = hashlib.sha1(b"".join(struct.pack("<I", i.word) for i in insns)).hexdigest()
            key = (lo, body_hash, tuple(unit["entries"]))
            if key in seen:                     # same bytes at the same address in another module
                stats["duplicate"] += len(unit["entries"])
                continue
            name = names[unit["members"][0]]
            lf = Lifter(name, insns, lo, hi, tables, starts, read_word, unit["entries"])
            try:
                body = lf.emit()
            except (Unsupported, KeyError, IndexError) as e:
                reasons[str(e).split(" at ")[0]] += 1
                if os.environ.get("A2LIFT_VERBOSE"):
                    print(f"  {mod} {name}: {e}")
                stats["left original"] += len(unit["entries"])
                continue
            seen[key] = True
            single = unit["entries"] == [lo]
            uname = f"lf_{mod}_{lo:08X}" if single else f"lu_{mod}_{lo:08X}"
            members = ", ".join(names[k] for k in unit["members"])
            text = [f"/* {members} ({mod}, {lo:#010x}, {len(insns)} instructions) */"]
            if single:
                text += [f"static int {uname}(CPUState *cpu)", "{", "    A2_ENTER();"]
            else:
                # code shared by several entries (hand-written assembly jumping between
                # routines): one body, entered at the address the caller jumped to
                text += [f"static int {uname}(CPUState *cpu, uint32_t entry_)", "{", "    A2_ENTER();",
                         "    switch (entry_) {"]
                text += [f"    case {hx(e)}: goto L_{e:08X};" for e in unit["entries"]]
                text += ["    default: return a2_bad_jump(cpu, entry_);", "    }"]
            for line in body:
                if line.startswith("L_"):
                    lab = int(line[2:10], 16)
                    if lab not in lf.labels_used and lab != lo:
                        continue
                text.append(line)
            if lf.falls_through():
                text.append(f"    return a2_fallthrough(cpu, {hx(hi)});  /* continues into the next function */")
            else:
                text.append("    return a2_fell_off(cpu);")
            text.append("}")
            for e in unit["entries"]:
                cname = f"lf_{mod}_{e:08X}"
                if not single:
                    text.append(f"static int {cname}(CPUState *cpu) {{ return {uname}(cpu, {hx(e)}); }}")
                ename = next((names[k] for k in unit["members"] if bodies[k][0].addr == e), f"func_{e:08X}")
                # functions rewritten by hand keep a lifted version, registered only while the
                # hand-written one is disabled (A2_DISABLE)
                entries.append({"addr": e, "mod": mod, "name": ename, "cname": cname,
                                "words": [i.word for i in insns if i.addr >= e],
                                "mem": {i.addr: i.word for i in insns}, "hand": ename in skip})
                if ename not in skip:
                    stats["lifted"] += 1
                if e not in starts:
                    stats["inner entries"] += 1
            texts[mod].append(([f"lf_{mod}_{e:08X}" for e in unit["entries"]], "\n".join(text)))
    distinguishing_checks(entries)
    by_cname = {e["cname"]: e for e in entries}
    for mod, funcs in texts.items():
        chunks = [funcs[k:k + a.per_file] for k in range(0, len(funcs), a.per_file)]
        for n, chunk in enumerate(chunks):
            with open(os.path.join(a.out, f"{mod}_{n:02d}.c"), "w") as f:
                f.write(f"/* Lifted from {mod} by tools/a2lift.py - do not edit by hand: rewrite the\n"
                        f" * function in src/game/ instead (A2_REWRITE wins over the lifted version). */\n")
                if mod != "main":
                    f.write("#define A2_LIFT_PRECISE 1   /* overlay code: the interpreter's per-instruction timing */\n")
                f.write('#include "../../a2_lift.h"\n\n')
                for _, fn in chunk:
                    f.write(fn + "\n\n")
                f.write(f"const A2LiftEntry a2_lift_{mod}_{n:02d}[] = {{\n")
                for cname in (c for cnames, _ in chunk for c in cnames if c in by_cname):
                    e = by_cname[cname]
                    g = guard_words([Insn(0, w) for w in e["words"]])
                    c = e["checks"]
                    f.write(f"    {{ {hx(e['addr'])}, \"{e['name']}\", {cname}, {len(g)}, "
                            f"{{ {', '.join(hx(w) for w in g)} }}, {len(c)}, "
                            f"{{ {', '.join(str(k) for k, _ in c) or '0'} }}, "
                            f"{{ {', '.join(hx(w) for _, w in c) or '0'} }}, {int(e['hand'])} }},\n")
                f.write("    { 0, 0, 0, 0, { 0 }, 0, { 0 }, { 0 }, 0 }\n};\n")
    # registry
    with open(os.path.join(a.out, "registry.c"), "w") as f:
        f.write("/* Registry of the lifted functions - generated by tools/a2lift.py. */\n")
        f.write('#include "../../a2_lift.h"\n\n')
        tables_list = sorted(os.path.basename(p)[:-2] for p in glob.glob(os.path.join(a.out, "*.c"))
                             if not p.endswith("registry.c"))
        for t in tables_list:
            f.write(f"extern const A2LiftEntry a2_lift_{t}[];\n")
        f.write("\nconst A2LiftEntry *const a2_lift_tables[] = {\n")
        for t in tables_list:
            f.write(f"    a2_lift_{t},\n")
        f.write("    0\n};\n")
    print(", ".join(f"{k}: {v}" for k, v in stats.items()))
    for k, v in reasons.most_common(12):
        print(f"  left original: {v:4}  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
