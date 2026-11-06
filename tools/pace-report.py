#!/usr/bin/env python3
"""Reads sd:/wiiu/our64/pace.bin (recorded when sd:/wiiu/our64/pace exists,
or after the dev build's L + R + minus): the frame check of video.cpp, with
a cause for every VI the TV skipped or repeated.
  tools/pace-report.py pace.bin [--events N]   (N: list the first N faults)

The N64 shows VI n's frame buffer for one VI. k vsyncs after one flip, the
TV should show the VI k later. Entries (big-endian >QQIIII t, x, kind, a, b, c):
  M  an emulated VI: t its time on the CPU core, a its number, b its origin,
     c its slot (the vsync it is due at; PAL on a 59.94 Hz TV: 5 VIs in 6; 0 in old files)
  P  a present: x the VI's time, a the VI, b its origin, c fresh
  S  a swap: x the vsync it was meant for, a the VI, b its origin, c swaps after
  F  a TV flip: t its time, a flips, b the VI it showed (0 unknown)
  T  a repeat's swap left out on a still screen (TV clock): a the VI
  R  the frame check started over"""
import collections
import struct
import sys

TPS = 62156250.0              # OSTime ticks a second
PERIOD = TPS * 0.016683333    # ticks between TV vsyncs


def ms(ticks):
    return ticks * 1000.0 / TPS


def main():
    path = sys.argv[1]
    show_n = int(sys.argv[sys.argv.index("--events") + 1]) if "--events" in sys.argv else 10
    data = open(path, "rb").read()
    events = [struct.unpack(">QQIIII", data[o:o + 32]) for o in range(0, len(data) - 31, 32)]
    kinds = collections.Counter(chr(e[2]) for e in events)
    print("events:", dict(kinds))
    marks, presents, swaps, flips, slots = {}, {}, {}, [], {}
    for t, x, kind, a, b, c in events:
        k = chr(kind)
        if k == "M":
            marks[a] = (t, b)
            slots[a] = c
        elif k == "P":
            presents.setdefault(a, (t, c))
        elif k == "S":
            swaps.setdefault(a, t)
        elif k == "F":
            flips.append((t, b, c))
        elif k == "R":
            flips.append((t, 0, 0))  # the frame check started over: nothing measured across it
        elif k == "T":
            flips.append((t, -1, 0))  # a still screen's swap left out: the next skip is not seen
    if not marks:
        print("no VIs recorded (an old pace.bin?)")
        return
    first, last = min(marks), max(marks)
    vis = last - first + 1
    starts = [v for v in range(first + 1, last + 1) if v in marks and v - 1 in marks and marks[v][1] != marks[v - 1][1]]
    span_s = ms(marks[last][0] - marks[first][0]) / 1000
    print("N64 VIs %d over %.1f s (%.2f VI/s), game frames %d (%.1f a second)" %
          (vis, span_s, vis / span_s if span_s else 0, len(starts), len(starts) / span_s if span_s else 0))
    print("VIs never presented (the graphics core still had one): %d" %
          sum(1 for v in range(first, last + 1) if v not in presents))
    print("VIs presented, never swapped: %d (fresh %d)" %
          (sum(1 for v in presents if v not in swaps),
           sum(1 for v, (t, fresh) in presents.items() if v not in swaps and fresh)))

    skipped = repeated = never = 0
    causes = collections.Counter()
    faults = []
    latency = []
    prev = None
    excused = 0
    pslot = None
    for t, vi, fslot in flips:
        if vi == -1:
            excused += 1
            continue
        if vi == 0 or vi not in marks:
            excused = 0
            prev = None
            pslot = None
            continue
        if prev is None or prev[1] != vi:  # a VI's first flip (one due two vsyncs is swapped twice)
            latency.append(ms(t - marks[vi][0]))
        if prev is not None:
            pt, pvi = prev
            vsyncs = int(round((t - pt) / PERIOD))
            # the swap's own slot (a VI due two vsyncs is swapped twice), else its VI's
            due_vsyncs = (fslot - pslot) if fslot and pslot else (slots[vi] - slots[pvi] if slots.get(vi) and slots.get(pvi) else vi - pvi)
            d = due_vsyncs - vsyncs
            while d > 0 and excused:
                d -= 1
                excused -= 1
            if d:
                # What kept the VI after the last one shown from its vsync: it was due
                # one vsync after the last flip, at the latency the last flip had.
                # this flip's VI came late: it was due (vi - pvi) vsyncs after the last flip
                nxt = vi
                due = pt + PERIOD * due_vsyncs
                if d > 0:
                    skipped += d
                    why = []
                    for v in range(max(pvi + 1, vi - d), vi):  # the VIs due in the vsyncs passed
                        if v not in presents:
                            why.append("never presented")
                        elif v not in swaps:
                            why.append("replaced in the queue")
                        else:
                            why.append("swapped, flip passed")
                    cause = "skip: " + (max(set(why), key=why.count) if why else "behind, caught up")
                else:
                    repeated += -d
                    if nxt not in marks:
                        cause = "repeat: unknown"
                    elif marks[nxt][0] > due:
                        cause = "repeat: CPU core late (emulated after its vsync)"
                    elif nxt in presents and presents[nxt][0] > due:
                        cause = "repeat: graphics core late (presented after its vsync)"
                    elif nxt in swaps and swaps[nxt] > due:
                        cause = "repeat: swapped after its vsync (queue)"
                    elif nxt in swaps:
                        cause = "repeat: GPU flipped late (swap before the vsync)"
                    else:
                        cause = "repeat: never swapped"
                causes[cause] += abs(d)
                faults.append((ms(t - flips[0][0]), pvi, vi, vsyncs, d, cause))
            span = vi - pvi
            if 1 < span < 512:
                n = sum(1 for v in range(pvi + 1, vi + 1) if v in marks and v - 1 in marks and marks[v][1] != marks[v - 1][1])
                if n > 1:
                    never += n - 1
        prev = (t, vi)
        pslot = fslot
        excused = 0
    print("TV flips %d; VIs skipped %d, repeated %d; game frames never shown %d" % (sum(1 for f in flips if f[1] > 0), skipped, repeated, never))
    if latency:
        latency.sort()
        print("VI to TV: median %.1f ms, 5%% %.1f, 95%% %.1f, most %.1f" %
              (latency[len(latency) // 2], latency[len(latency) // 20], latency[len(latency) * 19 // 20], latency[-1]))
    print("causes (VIs):")
    for cause, n in causes.most_common():
        print("  %5d  %s" % (n, cause))
    print("first faults (ms from the first flip, last VI shown, this VI, vsyncs between, off by, cause):")
    for f in faults[:show_n]:
        print("  %9.1f  %6d -> %6d  %d vsyncs  %+d  %s" % f)


main()
