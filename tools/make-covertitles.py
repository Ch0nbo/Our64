"""Writes player/source/covertitles.c: each N64 ROM by its header CRCs (CRC1 and
CRC2, at 0x10 and 0x14 in the header) with the No-Intro name libretro-thumbnails
files its cover under, so the console can fetch covers without reading a whole
ROM for its CRC-32.

The inputs:
- mupen64plus.ini, mupen64plus-core's ROM catalogue (GPL): each ROM's MD5 with
  its header CRCs. https://github.com/mupen64plus/mupen64plus-core/blob/master/data/mupen64plus.ini
- libretro-database's No-Intro list for the N64 (clrmamepro format): each ROM's
  MD5 with its No-Intro name, the list tools/fetch-art.py downloads.
  https://github.com/libretro/libretro-database/blob/master/metadat/no-intro/Nintendo%20-%20Nintendo%2064.dat

The two are joined on the MD5. When releases with different names share header
CRCs (Ocarina of Time's USA and Japan carts), each name gets a row with the
header country bytes (0x3E) its regions use, and the console picks by the
ROM's; the first name in mupen64plus.ini's order comes first, for a country
none lists. Counts go to stderr.

    python3 -I tools/make-covertitles.py <mupen64plus.ini> <No-Intro N64 .dat> > player/source/covertitles.c
"""
import re
import sys


def read_ini(path):
    """MD5 -> its keys, in the file's order."""
    entries, cur = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        m = re.match(r"^\[([0-9A-Fa-f]{32})\]$", line)
        if m:
            cur = entries.setdefault(m.group(1).upper(), {})
            continue
        if cur is None or "=" not in line or line.startswith(";"):
            continue
        k, v = line.split("=", 1)
        cur[k.strip()] = v.strip()
    return entries


def field(entries, e, k, seen=()):
    """A key of an entry, or of the entry its RefMD5 names (as make-gamedb.py does)."""
    if k in e:
        return e[k]
    ref = e.get("RefMD5", "").upper()
    if ref in entries and ref not in seen:
        return field(entries, entries[ref], k, seen + (ref,))
    return None


def tokens(text):
    """clrmamepro tokens: quoted strings (no escapes in this format), parentheses and words."""
    for m in re.finditer(r'"([^"]*)"|([()])|([^\s()"]+)', text):
        if m.group(1) is not None:
            yield ("str", m.group(1))
        elif m.group(2) is not None:
            yield (m.group(2), m.group(2))
        else:
            yield ("word", m.group(3))


def parse(toks):
    """A block's contents as a list of (key, value) pairs; a value is a string or a nested list."""
    items = []
    for kind, v in toks:
        if kind == ")":
            return items
        if kind != "word":
            raise ValueError("unexpected %r in the DAT" % v)
        kind2, v2 = next(toks)
        items.append((v, parse(toks) if kind2 == "(" else v2))
    return items


def read_dat(path):
    """MD5 -> the game's name."""
    toks = tokens(open(path, encoding="utf-8").read())
    names = {}
    for key, block in parse(toks):
        if key != "game" or not isinstance(block, list):
            continue
        game = next((v for k, v in block if k == "name" and isinstance(v, str)), None)
        for k, rom in block:
            if k != "rom" or not isinstance(rom, list) or game is None:
                continue
            md5 = next((v for kk, v in rom if kk == "md5" and isinstance(v, str)), None)
            if md5:
                names.setdefault(md5.upper(), game)
    return names


# No-Intro regions -> the N64 header's country bytes (0x3E)
COUNTRIES = {
    "Japan": "J", "USA": "E", "Europe": "PXY", "Germany": "D", "France": "F", "Italy": "I",
    "Spain": "S", "Australia": "U", "Netherlands": "H", "Sweden": "W", "Scandinavia": "W",
    "Brazil": "B", "Canada": "N", "China": "C", "Korea": "K", "Asia": "A",
}
COUNTRIES["World"] = "".join(sorted(set("".join(COUNTRIES.values()))))


def countries(name):
    """The country bytes of a No-Intro name's region list ("(USA, Europe)" -> "EPXY"); "" when it has none."""
    for group in re.findall(r"\(([^()]*)\)", name):
        regions = [r.strip() for r in group.split(",")]
        if all(r in COUNTRIES for r in regions):
            return "".join(sorted(set("".join(COUNTRIES[r] for r in regions))))
    return ""


def c_string(s):
    """s as a C string literal: UTF-8 bytes past ASCII as octal (hex escapes run on into a following digit)."""
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if c in '"\\':
            out.append("\\" + c)
        elif c == "?":
            out.append("\\?")  # never part of a trigraph
        elif 0x20 <= b < 0x7F:
            out.append(c)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    entries = read_ini(sys.argv[1])
    names = read_dat(sys.argv[2])
    rows, unmatched = {}, 0
    for md5, e in entries.items():
        crc = field(entries, e, "CRC")
        if not crc:
            continue
        name = names.get(md5)
        if name is None:
            unmatched += 1
            continue
        a, b = crc.split()
        key = (int(a, 16), int(b, 16))
        if name not in rows.setdefault(key, []):
            rows[key].append(name)
    shared = [k for k in rows if len(rows[k]) > 1]
    for key in shared:
        for name in rows[key]:
            if not countries(name):
                print("// %08X %08X: no region in %s" % (key[0], key[1], name), file=sys.stderr)
        print("// %08X %08X: %s" % (key[0], key[1], "; ".join("%s [%s]" % (n, countries(n)) for n in rows[key])),
              file=sys.stderr)
    print("/* Generated by tools/make-covertitles.py from mupen64plus.ini (mupen64plus-core) and")
    print(" * libretro-database's No-Intro list: header CRCs -> the No-Intro name")
    print(" * libretro-thumbnails files covers under. A pair two releases share has a row")
    print(" * per name, with the header country bytes its regions use; else 0, any country. */")
    print('#include "covertitles.h"')
    print("static const covertitle g_titles[] = {")
    count = 0
    for (a, b) in sorted(rows):
        for name in rows[(a, b)]:
            c = c_string(countries(name)) if len(rows[(a, b)]) > 1 else "0"
            print("\t{ 0x%08Xu, 0x%08Xu, %s, %s }," % (a, b, c_string(name), c))
            count += 1
    print("};")
    print("const covertitle *const covertitles = g_titles;")
    print("const unsigned covertitles_count = sizeof(g_titles) / sizeof(g_titles[0]);")
    print("// %d rows for %d CRC pairs; %d pairs shared by releases with different names; "
          "%d mupen64plus.ini entries not in the No-Intro list" % (count, len(rows), len(shared), unmatched),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
