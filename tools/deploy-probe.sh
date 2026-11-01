#!/usr/bin/env bash
# Uploads the probe to the console's SD card over FTPiiU (Wii U mode only).
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WUHB="$ROOT/probe/our64-probe.wuhb"
[ -f "$WUHB" ] || { echo "Build first: cd probe && make"; exit 1; }
lftp -u anonymous: "$HOST" -e "set net:max-retries 2; set net:timeout 10; mkdir -p /fs/vol/external01/wiiu/apps; put -O /fs/vol/external01/wiiu/apps $WUHB; bye"
echo "Uploaded to sd:/wiiu/apps/our64-probe.wuhb. Reboot the Wii U Menu (or the console) so the icon appears."
