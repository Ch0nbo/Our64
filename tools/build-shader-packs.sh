#!/usr/bin/env bash
# Builds every game's shader pack on the PC, so no game reads its shaders
# file by file on the console (0.25 s or more each on 2026-10-22: Majora's
# Mask took 83 s to start), and writes shaders/index.txt, the list of cached
# pairs, so a pair not in the cache costs no file open. Copies the console's
# shader cache over FTPiiU, makes each <game>.pack from its .keys file with
# tools/shader-dump --pack, taking only pairs the console compiled (the PC's
# CafeGLSL makes different code from the same sources, untested on the
# console), then puts the packs and the index back. Our64 must be closed.
#   tools/build-shader-packs.sh [<local copy dir>]
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD=/fs/vol/external01/wiiu/our64/shaders
COPY="$(realpath -m "${1:-$ROOT/results/shader-cache-card}")"
DUMP="$ROOT/tools/shader-dump/shader-dump"
[ -x "$DUMP" ] || { echo "Build tools/shader-dump first (make -C tools/shader-dump CAFEGLSL=...)"; exit 1; }
mkdir -p "$COPY"
lftp -u anonymous: "$HOST" -e "set net:max-retries 3; set net:timeout 30; mirror --only-newer $SD $COPY; bye"
chmod -R u+rwX "$COPY"
for keys in "$COPY"/games/*.keys; do
	game="$(sed -n '1s/^# Our64: combiner keys of \(.*\), loaded before the game starts$/\1/p' "$keys")"
	[ -n "$game" ] || { echo "$keys: no game name in its first line, skipped"; continue; }
	OUR64_PACK_NO_COMPILE=1 OUR64_ROM="$game" "$DUMP" --pack "$COPY" "${keys%.keys}.pack" "$keys" | sed 's#.*/##'
done
find "$COPY" -mindepth 2 -name '*.bin' -printf '%f\n' | sed 's/\.bin$//' | sort > "$COPY/index.txt"
echo "index.txt: $(wc -l < "$COPY/index.txt") pairs"
cmds="$(mktemp)"; trap 'rm -f "$cmds"' EXIT
{
	echo "set net:max-retries 3; set net:timeout 30; set cmd:fail-exit no"
	echo "open -u anonymous: $HOST"
	for p in "$COPY"/games/*.pack; do echo "put -O $SD/games \"$p\""; done
	echo "put -O $SD \"$COPY/index.txt\""
	echo "bye"
} > "$cmds"
lftp -f "$cmds"
echo "Uploaded $(ls "$COPY"/games/*.pack | wc -l) packs and index.txt"
