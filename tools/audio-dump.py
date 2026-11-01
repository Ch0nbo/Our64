#!/usr/bin/env python3
"""Reads sd:/wiiu/our64/audio-dump.bin (the player's audio-dump switch): the
pieces the game gave the AI. Prints one line per piece (VI, Count since the
last, address, length, whether the previous piece's bytes changed after the
AI took them) and writes the samples as s16 stereo to <out>.raw (big-endian
in the file, written little-endian for ffmpeg/sox).
  tools/audio-dump.py audio-dump.bin [out]"""
import struct
import sys

data = open(sys.argv[1], "rb").read()
out = sys.argv[2] if len(sys.argv) > 2 else None
pieces, off = [], 0
while off + 32 <= len(data):
    count, vi, addr, length, prev_sum, at, t_hi, t_lo = struct.unpack(">8I", data[off:off + 32])
    pieces.append((count, vi, addr, length, prev_sum, (t_hi << 32) | t_lo, data[off + 32:off + 32 + length]))
    off += 32 + length


def sum_of(b):
    s = 0
    for (w,) in struct.iter_unpack(">I", b[:len(b) // 4 * 4]):
        s = (s * 31 + w) & 0xFFFFFFFF
    return s


changed = 0
pcm = bytearray()
for i, (count, vi, addr, length, prev_sum, t, b) in enumerate(pieces):
    late = i > 0 and pieces[i - 1][3] and prev_sum != sum_of(pieces[i - 1][6])
    changed += late
    gap = (count - pieces[i - 1][0]) & 0xFFFFFFFF if i else 0
    print("VI %6u  +%9u Count  %06x len %5u%s" % (vi, gap, addr, length, "  previous piece changed after the AI took it" if late else ""))
    for j in range(0, len(b) - 1, 2):
        pcm += b[j + 1:j + 2] + b[j:j + 1]
print("%d pieces, %d changed after the AI took them" % (len(pieces), changed), file=sys.stderr)
if out:
    open(out + ".raw", "wb").write(pcm)
