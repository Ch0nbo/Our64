#!/usr/bin/env bash
# Uploads the player app (Our64) to the console's SD card over FTPiiU. Exit
# the app on the console first: never overwrite a .wuhb while it runs.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WUHB="$ROOT/player/our64.wuhb"
[ -f "$WUHB" ] || { echo "Build first: make -C player"; exit 1; }
INI="${GLIDEN64:-$ROOT/third_party/gliden64}/ini/GLideN64.custom.ini"  # GLideN64's per-game settings (GLIDEN64= for a worktree)
lftp -u anonymous: "$HOST" -e "set net:max-retries 2; set net:timeout 20; put -O /fs/vol/external01/wiiu/apps $WUHB; put -O /fs/vol/external01/wiiu/our64 $INI; bye"
# FTPiiU can report success when the console keeps the old file (Our64 still
# open): the size on the card must match.
want=$(stat -c %s "$WUHB")
got=$(lftp -u anonymous: "$HOST" -e "set cache:enable no; set net:timeout 20; cls -s --block-size=1 /fs/vol/external01/wiiu/apps/our64.wuhb; bye" | awk '{print $1}')
if [ "$got" != "$want" ]; then
	echo "NOT UPLOADED: the card has $got bytes, the build is $want. Close Our64 on the console and run this again."
	exit 1
fi
echo "Uploaded sd:/wiiu/apps/our64.wuhb ($want bytes, checked) and sd:/wiiu/our64/GLideN64.custom.ini."
