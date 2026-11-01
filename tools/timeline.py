#!/usr/bin/env python3
"""Reads sd:/wiiu/our64/timeline.bin (the player's timeline switch): one line
per event, with milliseconds since the first and Count since the last VI.
Kinds: V VI (origin, width, status, delay), T task start, F task finished,
Y yield, S SP status write (written, status, spinning, tasks in flight),
I interrupt, A AI length write, W audio wait (us, frames, queued, limit),
E audio list end, G GB Tower.
  tools/timeline.py timeline.bin [ticks per second] [first VI] [last VI]"""
import struct
import sys

path = sys.argv[1]
tps = float(sys.argv[2]) if len(sys.argv) > 2 else 62156250.0
lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
hi = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 32
data = open(path, "rb").read()
size = 40
t0 = None
vi_count = 0
for off in range(0, len(data) - size + 1, size):
    t, count, vi, kind, a, b, c, d, _ = struct.unpack(">QIIIIIIII", data[off:off + size])
    if t0 is None:
        t0 = t
    if kind == ord("V"):
        vi_count = count
    if not lo <= vi <= hi:
        continue
    print("%10.3f ms  VI %6u  +%8u  %s  %08x %08x %08x %08x" % (
        (t - t0) * 1000.0 / tps, vi, (count - vi_count) & 0xFFFFFFFF, chr(kind), a, b, c, d))
