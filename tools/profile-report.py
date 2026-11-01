#!/usr/bin/env python3
"""Our64: report a sampling profile from the console replay app.

Usage: tools/profile-report.py <app.elf> <game.prof>... [--top N] [--exclude NAME]...

Each .prof holds (PC, LR) pairs sampled every 100 us while the decoder ran
(see replay/source/profile.cpp). Samples are mapped to functions with
powerpc-eabi-nm. For each function: self samples, and for leaf helpers
(memcpy, CRC, ...) the caller that LR points at. --exclude drops samples
whose PC or LR function name contains NAME (e.g. the harness's Host::apply).
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
        if len(parts) == 3 and parts[1] in "TtWw" and int(parts[0], 16) < 0x10000000:  # app text, not import stubs
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def main():
    args = sys.argv[1:]
    top = 30
    excludes = []
    while "--top" in args or "--exclude" in args:
        flag = "--top" if "--top" in args else "--exclude"
        i = args.index(flag)
        if flag == "--top":
            top = int(args[i + 1])
        else:
            excludes.append(args[i + 1])
        del args[i:i + 2]
    elf, profs = args[0], args[1:]
    addrs, names = load_symbols(elf)
    elf_pdl = addrs[names.index("ProcessDList")]
    text_end = addrs[-1] + 0x10000

    def name_of(pc, offset):
        a = pc - offset
        if a < addrs[0] or a > text_end:
            return "<outside app: OS or kernel>"
        i = bisect.bisect_right(addrs, a) - 1
        n = names[i]
        return n[:90]

    total_self = collections.Counter()
    for prof in profs:
        data = open(prof, "rb").read()
        magic, count, run_pdl, _ = struct.unpack(">4I", data[:16])
        if magic != 0x4F363450:
            sys.exit(f"{prof}: not a profile")
        offset = run_pdl - elf_pdl
        self_c = collections.Counter()
        callers = collections.defaultdict(collections.Counter)
        kept = 0
        for k in range(count):
            pc, lr = struct.unpack_from(">2I", data, 16 + 8 * k)
            n = name_of(pc, offset)
            if excludes and any(e in n or e in name_of(lr, offset) for e in excludes):
                continue
            kept += 1
            self_c[n] += 1
            callers[n][name_of(lr, offset)] += 1
        total_self.update(self_c)
        print(f"== {os.path.basename(prof)}: {kept} of {count} samples (load offset {offset:#x})")
        for n, c in self_c.most_common(top):
            line = f"{100.0 * c / kept:5.1f}%  {n}"
            caller, cc = callers[n].most_common(1)[0]
            if caller != n:
                line += f"   <- {caller} ({100.0 * cc / c:.0f}%)"
            print(line)
        print()
    if len(profs) > 1:
        all_count = sum(total_self.values())
        print(f"== all games: {all_count} samples")
        for n, c in total_self.most_common(top):
            print(f"{100.0 * c / all_count:5.1f}%  {n}")


if __name__ == "__main__":
    main()
