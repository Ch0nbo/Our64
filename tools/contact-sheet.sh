#!/usr/bin/env bash
# Renders captured frames through stock GLideN64 (tools/replay-pc/replay64) and
# tiles the last frame of each capture group, labelled, into one PNG.
# Usage: tools/contact-sheet.sh <out.png> <frame.o64f>...
# GLIDEN64_SO names the plugin: a stock mupen64plus GLideN64 build (.so).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SO="${GLIDEN64_SO:?set GLIDEN64_SO to a stock mupen64plus GLideN64 plugin (.so)}"
out="$1"; shift
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$ROOT/tools/replay-pc/replay64" "$SO" "$tmp" "$@" > /dev/null 2>&1 || true
tiles=()
for group in $(printf '%s\n' "$@" | xargs -n1 basename | sed 's/-[0-9]*\.o64f$//' | sort -u); do
	last=$(ls "$tmp"/"$group"-*.ppm 2>/dev/null | sort | tail -1) || true
	[ -n "$last" ] || continue
	magick "$last" -resize 256x192! -gravity south -background black -fill white -pointsize 14 \
		-splice 0x18 -annotate +0+2 "$group" "$tmp/tile-$group.png"
	tiles+=("$tmp/tile-$group.png")
done
magick montage "${tiles[@]}" -tile 4x -geometry +2+2 -background gray20 "$out"
echo "$out: ${#tiles[@]} groups"
