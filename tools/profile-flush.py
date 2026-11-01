#!/usr/bin/env python3
"""Our64: how much CPU core time goes to register flushes and reloads.

Usage: tools/profile-flush.py <player-cpu.prof>

The profile must come from the flush-measurement build ("Our64 Flush Test",
player built with NOT64=<not64 count-interp branch> NOT64_EXTRA=-DOUR64_PROFILE_CODE),
whose player-cpu.prof also holds every compiled function's PowerPC code (see
player/source/cpuprof.c). Generated code keeps the N64 registers in reg[],
reached as stw/lwz rN, offset(r13): a store there is a register written back
(flushRegisters), a load one read again after it was dropped
(invalidateRegisters, or a first use). Each sample in generated code is
classed by the PowerPC instruction it landed on and by the MIPS instruction
that code came from.

What the lines mean:
  reg[] stores / loads: samples on those instructions, as a share of all CPU
    core samples (the CPU thread's busy time is the rest of the line "busy").
  in loads/stores: the MIPS instruction is a load or store; Not64 flushes
    every mapped register before one whose address might not be plain RDRAM.
  loads after a memory op: reg[] loads within 4 MIPS instructions after a
    load or store, registers read again because the memory op dropped them.
  static: what the compiled code holds, whether it runs or not.
Keeping registers mapped across memory accesses could remove at most the
"in loads/stores" stores plus the "after a memory op" loads; the slow path's C
call still needs a flush, so the real gain is lower.
"""
import bisect
import collections
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    magic, n, jit, jit_size, anchor, thread = struct.unpack_from(">6I", d, 0)
    if magic != 0x4F363444:
        sys.exit(f"{path}: not a CPU profile (version 2)")
    samples = list(struct.iter_unpack(">4I", d[24:24 + n * 16]))
    off = 24 + n * 16
    funcs = {}  # code address -> (MIPS start, marks [(ppc, index)], MIPS words)
    while True:
        (start,) = struct.unpack_from(">I", d, off)
        if start == 0xFFFFFFFF:
            off += 4
            break
        count, code = struct.unpack_from(">2I", d, off + 4)
        off += 12
        addrs = struct.unpack_from(f">{count}I", d, off)
        off += 4 * count
        words = struct.unpack_from(f">{count}I", d, off)
        off += 4 * count
        funcs[code] = (start, sorted((a, i) for i, a in enumerate(addrs) if a), words)
    if off + 8 > len(d) or struct.unpack_from(">I", d, off)[0] != 0x434F4445:
        sys.exit(f"{path}: no generated code in it (not the Our64 Flush Test build)")
    regoff = struct.unpack_from(">i", d, off + 4)[0]
    off += 8
    code = []  # (address, size, words)
    while True:
        (addr,) = struct.unpack_from(">I", d, off)
        if addr == 0xFFFFFFFF:
            break
        size = struct.unpack_from(">I", d, off + 4)[0]
        off += 8
        words = struct.unpack_from(f">{size // 4}I", d, off)
        off += size & ~3
        code.append((addr, size, words))
    code.sort()
    return samples, jit, jit_size, funcs, regoff, code


def is_mem(mips):
    op = mips >> 26
    return 0x20 <= op <= 0x3F and op != 0x2F  # loads and stores, not CACHE


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    samples, jit, jit_size, funcs, regoff, code = load(sys.argv[1])
    starts = [c[0] for c in code]

    def reg_access(w):
        op, ra, disp = w >> 26, (w >> 16) & 31, w & 0xFFFF
        disp = disp - 0x10000 if disp & 0x8000 else disp
        if ra != 13 or op not in (32, 36):
            return None
        if regoff <= disp < regoff + 34 * 8:
            return "store" if op == 36 else "load"
        return "other state"

    def mips_index(f, pc):
        j = bisect.bisect_right(f[1], (pc, 1 << 30)) - 1
        return f[1][j][1] if j >= 0 else None

    total = len(samples)
    k = collections.Counter()
    for pc, lr, thread, caller in samples:
        if not jit - 0x10000 <= pc < jit + jit_size:
            continue
        k["generated"] += 1
        c = bisect.bisect_right(starts, pc) - 1
        if c < 0 or pc >= code[c][0] + code[c][1]:
            k["unmapped"] += 1
            continue
        w = code[c][2][(pc - code[c][0]) // 4]
        kind = reg_access(w)
        f = funcs.get(code[c][0])
        i = mips_index(f, pc) if f else None
        mem = i is not None and is_mem(f[2][i])
        after = i is not None and any(is_mem(f[2][j]) for j in range(max(0, i - 4), i))
        k["mapped"] += 1
        if kind:
            k[kind] += 1
            if kind == "store" and mem:
                k["store in mem"] += 1
            if kind == "load" and (mem or after):
                k["load near mem"] += 1
        if mem:
            k["in mem"] += 1

    # static: reg[] stores emitted inside each load/store's code
    sites = stores = all_instr = reg_instr = 0
    for addr, size, words in code:
        all_instr += len(words)
        reg_instr += sum(1 for w in words if reg_access(w) in ("store", "load"))
        f = funcs.get(addr)
        if not f:
            continue
        marks = f[1]
        for m, (ppc, i) in enumerate(marks):
            if not is_mem(f[2][i]):
                continue
            end = marks[m + 1][0] if m + 1 < len(marks) else addr + size
            sites += 1
            for a in range(ppc, min(end, addr + size), 4):
                if reg_access(words[(a - addr) // 4]) == "store":
                    stores += 1

    pct = lambda v: f"{100.0 * v / total:5.1f}%"
    print(f"{total} samples; generated code {pct(k['generated'])} ({k['mapped']} on dumped code, {k['unmapped']} on code freed since)")
    print(f"  reg[] stores {pct(k['store'])} (in loads/stores {pct(k['store in mem'])})")
    print(f"  reg[] loads  {pct(k['load'])} (in or after loads/stores {pct(k['load near mem'])})")
    print(f"  other r13 state {pct(k['other state'])}; all code of loads/stores {pct(k['in mem'])}")
    print(f"  at most removable by keeping registers mapped: {pct(k['store in mem'] + k['load near mem'])}")
    print(f"static: {all_instr} instructions, {100.0 * reg_instr / max(all_instr, 1):.1f}% reg[] loads/stores; "
          f"{sites} loads/stores compiled, {stores / max(sites, 1):.2f} reg[] stores each")


if __name__ == "__main__":
    main()
