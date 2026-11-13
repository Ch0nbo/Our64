#!/usr/bin/env python3
"""Lists the shader pairs in Our64 shader packs (<game>.pack: "O64K", a
version, the number of pairs, then each pair as its length and its .bin
file's bytes, whose bytes 8-15 are its key), one 16-digit hex key a line.
  python3 tools/pack-keys.py <pack>..."""
import struct
import sys

for path in sys.argv[1:]:
    d = open(path, "rb").read()
    if len(d) < 12 or d[:4] != b"O64K":
        sys.exit("%s: not a shader pack" % path)
    count, pos = struct.unpack(">I", d[8:12])[0], 12
    for _ in range(count):
        n = struct.unpack(">I", d[pos:pos + 4])[0]
        pos += 4
        if n < 16 or pos + n > len(d):
            sys.exit("%s: damaged" % path)
        print(d[pos + 8:pos + 16].hex())
        pos += n
