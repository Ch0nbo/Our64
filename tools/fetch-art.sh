#!/usr/bin/env bash
# Box art for the game list. Matches each ROM in a folder on this computer by
# checksum against libretro's lists, writes the covers to results/art, and with
# --upload copies them to sd:/wiiu/our64/art over FTPiiU.
#   tools/fetch-art.sh <rom folder> [--upload] [--force]
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROMS="${1:?usage: tools/fetch-art.sh <rom folder> [--upload] [--force]}"
shift
UPLOAD=0
FORCE=()
for a in "$@"; do
	case "$a" in
	--upload) UPLOAD=1 ;;
	--force) FORCE=(--force) ;;
	*) echo "unknown option $a"; exit 1 ;;
	esac
done
OUT="$ROOT/results/art"
python3 -I "$ROOT/tools/fetch-art.py" "$ROMS" "$OUT" "$ROOT/results/art-cache" "${FORCE[@]}"
if [ "$UPLOAD" = 1 ]; then
	lftp -u anonymous: "$HOST" -e "set net:max-retries 2; set net:timeout 20; mkdir -p /fs/vol/external01/wiiu/our64/art; mirror -R --only-newer $OUT /fs/vol/external01/wiiu/our64/art; bye"
	echo "Covers copied to sd:/wiiu/our64/art."
fi
