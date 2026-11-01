#!/usr/bin/env bash
# Screenshots one frame of the GX2 replay in Cemu. Cemu never writes GPU
# results back to emulated memory, so the replay's saved images are black
# there; sd:/wiiu/our64/gx2-hold.txt makes the replay hold the frame on the TV
# instead, and this grabs Cemu's window.
# Shaders: Cemu cannot compile them. With sd:/wiiu/our64/gx2-shader-sources
# present, missing ones are saved there; this builds them into the cache with
# tools/shader-dump --cache and runs the replay again.
# Usage: tools/cemu-shot.sh <frame name> <out.png>   (frames in Cemu's sd:/wiiu/our64/frames)
# CEMU_WAIT=<s> waits that long on the held frame before the shot (default 3),
# for Cemu's own shader compiles to finish.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="$HOME/.var/app/info.cemu.Cemu/data/Cemu/sdcard/wiiu/our64"
WUHB="$ROOT/replay/our64-replay-gx2.wuhb"
FRAME="$1"
OUT="$(realpath -m "$2")"
mkdir -p "$SD/gx2-shader-sources"
touch "$SD/gx2-shader-cache"
WAIT="${CEMU_WAIT:-3}"
echo "$FRAME $((WAIT + 5))" > "$SD/gx2-hold.txt"

for attempt in 1 2 3 4; do
	rm -f "$SD/gx2-shader-sources/"* "$SD/replay-results.txt"
	flatpak run --filesystem="$(dirname "$WUHB")" info.cemu.Cemu -g "$WUHB" >/dev/null 2>&1 &
	end=$((SECONDS + 240))
	shot=no
	until [ $SECONDS -ge $end ]; do
		if grep -qs "$FRAME: held on screen" "$SD/replay-results.txt"; then
			sleep 3
			win=$(xdotool search --name "Cemu" 2>/dev/null | tail -1)
			[ -n "$win" ] && import -window "$win" "$OUT" && shot=yes
			break
		fi
		grep -qs "Done. Press HOME" "$SD/replay-results.txt" && break
		sleep 1
	done
	pkill -f "Cemu_relwithdebinfo -g $WUHB"; sleep 2; pkill -9 -f "Cemu_relwithdebinfo -g $WUHB"
	missing=$(ls "$SD/gx2-shader-sources" | wc -l)
	if [ "$missing" -gt 0 ]; then
		echo "attempt $attempt: $missing shader files missing from the cache; building them"
		"$ROOT/tools/shader-dump/shader-dump" --cache "$SD/gx2-shader-sources" "$SD/shaders" || exit 1
		continue
	fi
	[ $shot = yes ] && { echo "saved $OUT"; exit 0; }
	echo "frame $FRAME never held (not in sd:/wiiu/our64/frames?)"; exit 1
done
echo "shaders still missing after $attempt runs"; exit 1
