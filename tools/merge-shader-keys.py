"""Our64 pre-built shaders: merges per-game key files (the console's
shaders/games/*.keys, the PC harvest's results/shader-keys/*.keys) into one
.keys file per game: the union of its keys, sorted, under the first header
line found, as gx2_ShaderCache.cpp writes them. Files pair up by name, which
is the game's RSP.romname made safe (gamePath). Keys only: the console
compiles them in each game's own setup (build-shaders).
  python3 tools/merge-shader-keys.py <out dir> <keys dir>..."""
import os
import re
import sys

out_dir, sources = sys.argv[1], sys.argv[2:]
games = {}
for d in sources:
    for f in sorted(os.listdir(d)):
        if not f.endswith(".keys"):
            continue
        header, keys = games.get(f, (None, set()))
        with open(os.path.join(d, f), errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("#"):
                    header = header or line
                elif re.fullmatch(r"0x[0-9a-fA-F]{1,16}", line):
                    keys.add(int(line, 16))
        games[f] = (header, keys)
os.makedirs(out_dir, exist_ok=True)
for f, (header, keys) in sorted(games.items()):
    with open(os.path.join(out_dir, f), "w") as fh:
        fh.write((header or "# Our64: combiner keys of %s, loaded before the game starts" % f[:-5]) + "\n")
        for k in sorted(keys):
            fh.write("0x%016x\n" % k)
print("%d games, %d keys" % (len(games), sum(len(k) for _, k in games.values())))
