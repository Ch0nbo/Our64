#!/usr/bin/env python3
"""Reads sd:/wiiu/our64/pace.bin (the dev build's flip record, started by the
GamePad's L + R + minus): how long each new game frame stayed on the TV
against how many VIs the game kept it, and how the flips were spaced.
  tools/pace-report.py pace.bin"""
import collections
import struct
import sys

TPS = 62156250.0          # OSTime ticks a second
PERIOD = 1000 / 59.94     # ms between TV vsyncs

data = open(sys.argv[1], "rb").read()
events = [struct.unpack(">QQIIII", data[o:o + 32]) for o in range(0, len(data) - 31, 32)]
shows, flips = [], {}
for t, x, kind, a, b, c in events:
    if kind == ord("S"):
        shows.append((t, a, b, c))      # time, VI, origin, swaps after
    elif kind == ord("F"):
        flips[a] = t                    # flip count -> its time
print("events:", dict(collections.Counter(chr(e[2]) for e in events)))

new = []   # (VI, flip time) of each new game frame whose flip time is known
last_origin = None
for t, vi, origin, swaps in shows:
    if origin != last_origin and swaps in flips:
        new.append((vi, flips[swaps]))
    last_origin = origin
holds = collections.Counter()
for (vi0, f0), (vi1, f1) in zip(new, new[1:]):
    holds[(vi1 - vi0, round((f1 - f0) * 1000 / TPS / PERIOD))] += 1
total = sum(holds.values())
wrong = sum(n for (v, h), n in holds.items() if v != h)
print("game frames %d, held a wrong number of vsyncs: %d (%.0f%%)" % (total, wrong, 100 * wrong / max(total, 1)))
print("(VIs, vsyncs held): count")
for (v, h), n in holds.most_common(10):
    print("  (%d, %d): %d" % (v, h, n))
ft = sorted(flips.items())
spacing = collections.Counter(round((t1 - t0) * 1000 / TPS / PERIOD) for (n0, t0), (n1, t1) in zip(ft, ft[1:]) if n1 - n0 == 1)
print("vsyncs between consecutive flips:", sorted(spacing.items()))
