#!/usr/bin/env bash
# Uploads the stage-1 benchmark, and any ROMs given as arguments, to the
# console's SD card over FTPiiU (Wii U mode only). ROMs go to
# sd:/wiiu/our64/roms/ under the name given after a colon, or their own name:
#   tools/deploy-bench.sh "/path/Super Mario 64.z64:sm64.z64"
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WUHB="$ROOT/bench/our64-bench.wuhb"
[ -f "$WUHB" ] || { echo "Build first: make -C bench"; exit 1; }
CMDS="set net:max-retries 2; set net:timeout 20; mkdir -p /fs/vol/external01/wiiu/our64/roms; put -O /fs/vol/external01/wiiu/apps $WUHB;"
for arg in "$@"; do
	src="${arg%%:*}"; name="${arg##*:}"; [ "$src" = "$arg" ] && name="$(basename "$src")"
	[ -f "$src" ] || { echo "No such ROM: $src"; exit 1; }
	CMDS="$CMDS put \"$src\" -o \"/fs/vol/external01/wiiu/our64/roms/$name\";"
done
lftp -u anonymous: "$HOST" -e "$CMDS bye"
echo "Uploaded. Restart the Wii U Menu so \"Our64 CPU Bench\" appears."
