#!/usr/bin/env python3
"""Our64: report a CPU core profile from the player (sd:/wiiu/our64/profile-cpu).

Usage: tools/profile-cpu.py <our64.elf> <player-cpu.prof> [--top N] [--code N]

The .prof holds samples taken every 250 us on the CPU core (PC, LR, the thread,
and for the CPU thread in system code the first return address on its stack
outside the system), then every
compiled function: its MIPS start, the generated code of each MIPS instruction
that starts a mapping, and the MIPS words (see player/source/cpuprof.c).
Samples in generated code count for the N64 function they run; samples in the
app count for its function, and leaf helpers also for the N64 function the
generated code called them from (LR). System code is listed by which thread
ran it and, for the CPU thread, which app or N64 code called it. --code N disassembles the N hottest
N64 functions with samples per instruction (llvm-mc).
"""
import bisect
import collections
import os
import struct
import subprocess
import sys

NM = os.path.join(os.environ.get("DEVKITPPC", "/opt/devkitpro/devkitPPC"), "bin", "powerpc-eabi-nm")


def load_symbols(elf):
    out = subprocess.run([NM, "-C", "-n", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "TtWw" and int(parts[0], 16) < 0x10000000:
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def load_prof(path):
    data = open(path, "rb").read()
    magic, n, jit, jit_size, anchor, thread = struct.unpack_from(">6I", data, 0)
    if magic != 0x4F363444:
        sys.exit(f"{path}: not a CPU profile (version 2)")
    samples = list(struct.iter_unpack(">4I", data[24:24 + n * 16]))
    funcs = []  # (code, start, [(ppc, index)], mips words)
    off = 24 + n * 16
    while True:
        (start,) = struct.unpack_from(">I", data, off)
        if start == 0xFFFFFFFF:
            break
        count, code = struct.unpack_from(">2I", data, off + 4)
        off += 12
        addrs = struct.unpack_from(f">{count}I", data, off)
        off += 4 * count
        words = struct.unpack_from(f">{count}I", data, off)
        off += 4 * count
        marks = sorted((a, i) for i, a in enumerate(addrs) if a)
        funcs.append((code, start, marks, words))
    funcs.sort()
    return samples, jit, jit_size, anchor, thread, funcs


def disasm(words):
    text = " ".join(f"0x{w >> 24 & 255:02x} 0x{w >> 16 & 255:02x} 0x{w >> 8 & 255:02x} 0x{w & 255:02x}" for w in words)
    out = subprocess.run(["llvm-mc", "--disassemble", "-triple=mips64", "-mcpu=mips3"], input=text,
                         capture_output=True, text=True).stdout
    lines = [l.strip().replace("\t", " ") for l in out.splitlines() if l.startswith("\t")]
    return lines if len(lines) == len(words) else [f".word 0x{w:08X}" for w in words]


def main():
    args = sys.argv[1:]
    top, code_n = 30, 3
    for flag in ("--top", "--code"):
        if flag in args:
            i = args.index(flag)
            if flag == "--top":
                top = int(args[i + 1])
            else:
                code_n = int(args[i + 1])
            del args[i:i + 2]
    elf, prof = args
    addrs, names = load_symbols(elf)
    text_end = addrs[-1] + 0x10000
    samples, jit, jit_size, anchor, cpu_thread, funcs = load_prof(prof)
    load = anchor - addrs[names.index("cpuprof_save")]  # the app runs moved from its link address
    codes = [f[0] for f in funcs]

    def app_name(pc):
        pc -= load
        if pc < addrs[0] or pc > text_end:
            return None
        return names[bisect.bisect_right(addrs, pc) - 1][:80]

    def is_system(pc):
        return 0x01000000 <= pc < 0x01800000

    def n64_of(pc):
        """(function index, MIPS index) for a PC in generated code, else None."""
        if not jit - 0x10000 <= pc < jit + jit_size:
            return None
        k = bisect.bisect_right(codes, pc) - 1
        if k < 0:
            return ("stubs", None)
        marks = funcs[k][2]
        j = bisect.bisect_right(marks, (pc, 1 << 30)) - 1
        return (k, marks[j][1] if j >= 0 else 0)

    total = len(samples)
    kinds = collections.Counter()
    n64_self = collections.Counter()      # N64 function: samples in its generated code
    n64_all = collections.Counter()       # ... plus helpers it called
    n64_instr = collections.Counter()     # (function, MIPS index)
    helpers = collections.Counter()       # app function: self samples
    helper_from = collections.Counter()   # (app function, N64 function) for helpers called from generated code
    system = collections.Counter()        # (thread, PC, caller) for system code
    for pc, lr, thread, caller in samples:
        where = n64_of(pc)
        if where is not None:
            if where[0] == "stubs":
                kinds["generated code: call stubs"] += 1
                continue
            kinds["generated code"] += 1
            n64_self[where[0]] += 1
            n64_all[where[0]] += 1
            n64_instr[where] += 1
            continue
        name = app_name(pc)
        if name is None:
            other = thread != cpu_thread
            kinds["system code, another thread" if other else "system code, CPU thread"] += 1
            if other:
                system[("another thread 0x%08X" % thread, pc, "")] += 1
            else:
                who = app_name(caller) if caller else None
                if who is None and caller:
                    w = n64_of(caller)
                    who = "generated code" if w is None or w[0] == "stubs" else f"N64 0x{funcs[w[0]][1]:08X}"
                system[("CPU thread", pc, who or "?")] += 1
            continue
        kinds["app code"] += 1
        helpers[name] += 1
        caller = n64_of(lr)
        if caller is not None and caller[0] != "stubs":
            helper_from[(name, caller[0])] += 1
            n64_all[caller[0]] += 1

    def fname(k):
        return f"0x{funcs[k][1]:08X}"

    def pct(v):
        return f"{100.0 * v / total:5.1f}%"

    print(f"{total} samples ({total / 4000:.1f} s at 250 us), {len(funcs)} compiled functions")
    for k, v in kinds.most_common():
        print(f"  {pct(v)}  {k}")
    print("\nApp functions (self):")
    for name, v in helpers.most_common(top):
        print(f"  {pct(v)}  {name}")
    print("\nSystem code (thread, PC, called from):")
    for (t, pc, who), v in system.most_common(top):
        print(f"  {pct(v)}  {t}  0x{pc:08X}  {who}")
    print("\nHelpers called from generated code, by N64 function:")
    for (name, k), v in helper_from.most_common(top):
        print(f"  {pct(v)}  {name}  <- N64 {fname(k)}")
    print("\nN64 functions (generated code, then with the helpers they call):")
    for k, v in n64_all.most_common(top):
        f = funcs[k]
        print(f"  {pct(n64_self[k])} {pct(v)}  N64 {fname(k)}-0x{f[1] + 4 * len(f[3]):08X} ({len(f[3])} instructions)")
    for k, _ in n64_self.most_common(code_n):
        f = funcs[k]
        lines = disasm(f[3])
        print(f"\nN64 {fname(k)}: {pct(n64_self[k])} of samples in its generated code")
        for i, line in enumerate(lines):
            v = n64_instr[(k, i)]
            print(f"  {v:6d}  {f[1] + 4 * i:08X}  {line}")


if __name__ == "__main__":
    main()
