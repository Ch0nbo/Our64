#!/usr/bin/env bash
# Uploads the stage-2 replay app to the console's SD card over FTPiiU. It plays
# the frames the bench captured (sd:/wiiu/our64/frames) and writes
# sd:/wiiu/our64/replay-results.txt and traces/<game>.trace.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WUHB="$ROOT/replay/our64-replay.wuhb"
[ -f "$WUHB" ] || { echo "Build first: make -C replay"; exit 1; }
lftp -u anonymous: "$HOST" -e "set net:max-retries 2; set net:timeout 20; put -O /fs/vol/external01/wiiu/apps $WUHB; bye"
echo "Uploaded. Restart the Wii U Menu so \"Our64 Replay\" appears."
