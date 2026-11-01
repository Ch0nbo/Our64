#!/usr/bin/env bash
# Moves the console's shader cache from the flat layout (every file in
# sd:/wiiu/our64/shaders) to the one GLideN64's GX2 backend uses since
# 2026-10-22: <key>.bin into a folder named for its first two hex digits, each
# game's .keys and .pack into shaders/games. In one folder of 1867 files any
# open took about 0.25 s. Over FTPiiU; Our64 must be closed. The player moves
# a leftover file itself when it first needs it, so a partial run is harmless.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
DIR=/fs/vol/external01/wiiu/our64/shaders
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
lftp -u anonymous: "$HOST" -e "set net:max-retries 2; set net:timeout 30; cls -1 $DIR/; bye" | sed 's#.*/##' > "$tmp/list"
grep -E '^[0-9a-f]{16}\.bin$' "$tmp/list" > "$tmp/bins" || true
grep -E '\.(keys|pack)$' "$tmp/list" > "$tmp/games" || true
echo "$(wc -l < "$tmp/bins") cache files and $(wc -l < "$tmp/games") game files to move"
{
	echo "set net:max-retries 2; set net:timeout 30; set cmd:fail-exit no"
	echo "open -u anonymous: $HOST"
	echo "cd $DIR"
	echo "mkdir -f games"
	cut -c1-2 "$tmp/bins" | sort -u | sed 's/^/mkdir -f /'
	sed -E 's#^(..)(.*)$#mv \1\2 \1/\1\2#' "$tmp/bins"
	sed 's#^\(.*\)$#mv "\1" "games/\1"#' "$tmp/games"
	echo "bye"
} > "$tmp/cmds"
lftp -f "$tmp/cmds" 2>&1 | grep -v -E "^mkdir: Access failed: 550 File exists" || true
left="$(lftp -u anonymous: "$HOST" -e "set net:max-retries 2; cls -1 $DIR/; bye" | sed 's#.*/##' | grep -c -E '\.(bin|keys|pack)$' || true)"
echo "Done; $left files left in the flat layout."
