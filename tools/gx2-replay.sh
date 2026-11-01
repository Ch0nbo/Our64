#!/usr/bin/env bash
# Runs the GX2 replay (Our64 GX2 Replay) on the console with debug variants,
# then collects what it drew. Two steps, with Zach starting the app between:
#
#   tools/gx2-replay.sh setup <games file> <variants file> <frame.o64f>...
#       Uploads the app, the frames (into sd:/wiiu/our64/frames; a game is the
#       file name before the last '-'; no spaces in the paths), the games file as
#       gx2-games.txt (one "<game>" or "<game> last=<n>" per line) and the
#       variants file as gx2-debug.txt (one variant per line, see
#       replay/source/main.cpp), and deletes those games' old images in
#       sd:/wiiu/our64/gx2. Then start "Our64 GX2 Replay" and wait for
#       "Done. Press HOME" (HOME to leave).
#   tools/gx2-replay.sh fetch <games file> <out dir>
#       Downloads those games' images, replay-results.txt and gx2-log.txt into
#       <out dir>, then deletes the images, gx2-debug.txt and gx2-games.txt
#       from the card: the player reads gx2-debug.txt too, and kept images
#       grew to 930 MB on a card whose writes slow down when nearly full.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD=/fs/vol/external01/wiiu/our64
FTP=(lftp -u anonymous: "$HOST")
step="${1:-}"; shift || true

case "$step" in
setup)
	games="$1" variants="$2"; shift 2
	WUHB="$ROOT/replay/our64-replay-gx2.wuhb"
	[ -f "$WUHB" ] || { echo "Build first: make -C replay GFX=gx2"; exit 1; }
	tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
	cp "$games" "$tmp/gx2-games.txt"
	cp "$variants" "$tmp/gx2-debug.txt"
	puts="" mrms=""
	for f in "$@"; do puts+="put -O $SD/frames $f; "; done
	for g in $(awk '{print $1}' "$games"); do mrms+="mrm $SD/gx2/$g-*; "; done
	"${FTP[@]}" -e "set net:max-retries 2; set net:timeout 30; set cmd:fail-exit no; \
		put -O /fs/vol/external01/wiiu/apps $WUHB; mkdir -p $SD/frames; mkdir -p $SD/gx2; $puts \
		put -O $SD $tmp/gx2-games.txt; put -O $SD $tmp/gx2-debug.txt; $mrms bye"
	echo "Set up: start \"Our64 GX2 Replay\" (restart the Wii U Menu if it is new), wait for \"Done\", then HOME."
	;;
fetch)
	games="$1" out="$2"
	mkdir -p "$out"
	mgets=""
	mrms=""
	for g in $(awk '{print $1}' "$games"); do mgets+="mget -O $out $SD/gx2/$g-*; "; mrms+="mrm $SD/gx2/$g-*; "; done
	"${FTP[@]}" -e "set cache:enable no; set net:max-retries 2; set net:timeout 30; set xfer:clobber on; set cmd:fail-exit no; \
		$mgets get -O $out $SD/replay-results.txt; get -O $out $SD/gx2-log.txt; \
		rm $SD/gx2-debug.txt; rm $SD/gx2-games.txt; bye"
	n=$(ls "$out" | grep -c '\.ppm$' || true)
	[ "$n" -gt 0 ] && "${FTP[@]}" -e "set net:max-retries 2; set net:timeout 30; set cmd:fail-exit no; $mrms bye" >/dev/null 2>&1
	echo "$n images in $out; they, gx2-debug.txt and gx2-games.txt removed from the card."
	;;
*)
	sed -n '2,16p' "$0"; exit 2
	;;
esac
