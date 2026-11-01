"""Box art for Our64's game list, made on a computer (the console has no network code).

Each ROM is matched by the CRC-32 of its contents (in .z64 byte order) against
the No-Intro list in libretro-database; the matching name's cover comes from
libretro-thumbnails. Covers keep their own size (at most COVER_H tall and COVER_W wide) and are written as
<out>/<game id>.png, where the game id is the one Our64's saves use (header
name + CRC1), so renaming a ROM keeps its cover.

Run through tools/fetch-art.sh.
"""
import argparse
import io
import os
import re
import struct
import sys
import urllib.parse
import urllib.request
import zlib

DAT_URL = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/Nintendo%20-%20Nintendo%2064.dat"
ART_URL = "https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%2064/Named_Boxarts/{}.png"
COVER_H = 357  # libretro-thumbnails' N64 height: the selected cover is drawn about 330 tall (2026-10-30)
COVER_W = 640  # the console's art slots (library_wiiu.c ART_W)
ROM_EXT = (".z64", ".n64", ".v64")


def to_z64(data):
    """The ROM in big-endian (.z64) byte order, or None when it is not an N64 ROM."""
    magic = struct.unpack(">I", data[:4])[0]
    if magic == 0x80371240:
        return data
    if magic == 0x37804012:  # .v64: bytes swapped in pairs
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if magic == 0x40123780:  # .n64: little-endian words
        b = bytearray(len(data))
        for i in range(4):
            b[i::4] = data[3 - i::4]
        return bytes(b)
    return None


def game_id(z64):
    """saves_game_id in player/source/saves.c: header name + CRC1."""
    name = z64[32:52]
    clean = []
    for c in name:
        if c == 0:
            break
        clean.append("_" if c < 0x20 or c >= 0x7F or chr(c) in '\\/:*?"<>|' else chr(c))
    s = "".join(clean).rstrip(" .").lstrip(" ")
    crc1 = struct.unpack(">I", z64[16:20])[0]
    return "%s-%08X" % (s or "UNKNOWN", crc1)


def load_dat(cache):
    path = os.path.join(cache, "n64.dat")
    if not os.path.exists(path):
        print("Downloading the No-Intro list from libretro-database...")
        with urllib.request.urlopen(DAT_URL, timeout=60) as r, open(path, "wb") as f:
            f.write(r.read())
    names = {}
    game = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r'\s*name "(.*)"\s*$', line)
        if m and game is None:
            game = m.group(1)
            continue
        m = re.search(r'\bcrc ([0-9A-Fa-f]{8})\b', line)
        if m and game is not None:
            names[m.group(1).upper()] = game
        if line.strip() == ")":
            game = None
    return names


def thumbnail_name(name):
    # libretro-thumbnails replaces these characters in file names
    return re.sub(r'[&*/:`<>?\\|"]', "_", name)


def fetch_cover(names):
    """The first of these names that has a cover online."""
    for name in names:
        url = ART_URL.format(urllib.parse.quote(thumbnail_name(name)))
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                return name, r.read()
        except Exception as e:  # a missing cover is normal: the console draws a poster card
            error = e
    print("  no cover online (%s)" % error)
    return None, None


def scale(png):
    from PIL import Image
    im = Image.open(io.BytesIO(png)).convert("RGB")
    # never scaled up: a smaller cover stays as it is, a bigger one shrinks to fit
    k = min(1.0, COVER_H / im.height, COVER_W / im.width)
    if k < 1:
        im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, "PNG", optimize=True)
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("roms", help="folder with the same ROM files as sd:/wiiu/our64/roms")
    ap.add_argument("out", help="folder for the covers")
    ap.add_argument("cache", help="folder for the downloaded game list")
    ap.add_argument("--force", action="store_true", help="fetch covers that are already there again")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.cache, exist_ok=True)
    names = load_dat(a.cache)
    found = missing = skipped = 0
    for file in sorted(os.listdir(a.roms)):
        if not file.lower().endswith(ROM_EXT):
            continue
        print(file)
        data = open(os.path.join(a.roms, file), "rb").read()
        z64 = to_z64(data)
        if z64 is None or len(z64) < 0x101000:
            print("  not an N64 ROM: skipped")
            missing += 1
            continue
        gid = game_id(z64)
        dest = os.path.join(a.out, gid + ".png")
        if os.path.exists(dest) and not a.force:
            print("  already there: %s.png" % gid)
            skipped += 1
            continue
        crc = "%08X" % (zlib.crc32(z64) & 0xFFFFFFFF)
        name = names.get(crc)
        if name is None:
            print("  CRC %s is not in the No-Intro list (a hack or a bad dump?): no cover" % crc)
            missing += 1
            continue
        # libretro-thumbnails sometimes files a cover under an older No-Intro name,
        # which is often what the ROM file is still called
        stem = os.path.splitext(file)[0]
        name, png = fetch_cover([name] + ([stem] if stem != name else []))
        if png is None:
            missing += 1
            continue
        open(dest, "wb").write(scale(png))
        print("  %s -> %s.png" % (name, gid))
        found += 1
    print("%d covers written, %d already there, %d without a cover (the console draws a poster card for those)"
          % (found, skipped, missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
