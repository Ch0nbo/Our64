#!/usr/bin/env bash
# Builds the release zip, release/Our64-<version>.zip, laid out to be
# unpacked onto the root of the SD card:
#   wiiu/apps/our64.wuhb             the public build (make RELEASE=1)
#   wiiu/our64/GLideN64.custom.ini   per-game graphics settings
#   wiiu/our64/shaders/              pre-built shaders for every game
#   wiiu/our64/roms/                 empty: the games go here
# and beside it, in release/Our64-<version>/, the files an installed Our64
# takes by itself (Settings > Update); they go on the GitHub release with the
# zip, under these names:
#   our64.wuhb            the same app
#   GLideN64.custom.ini   the same settings
#   shaders.zip           games/<game>.pack and .keys of the same shaders
#                         (taken only with Include shaders on)
# The zip's wiiu/our64/shaders/release.txt holds shaders.zip's SHA-256: a card
# set up from the zip already has those shaders, and the updater says so.
# Usage: tools/make-release.sh [shaders-dir]
#   shaders-dir: a copy of a card's sd:/wiiu/our64/shaders after the shader
#   build (default results/release-shaders).
#        tools/make-release.sh --fetch
#   copies the card's shaders/ into results/release-shaders over FTPiiU.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHADERS="$ROOT/results/release-shaders"

if [ "${1:-}" = --fetch ]; then
	mkdir -p "$SHADERS"
	lftp -u anonymous: "$HOST" -e "set net:max-retries 2; set net:timeout 20; mirror --delete --no-perms --parallel=4 /fs/vol/external01/wiiu/our64/shaders $SHADERS; bye"
	echo "$(find "$SHADERS" -type f | wc -l) files in $SHADERS"
	exit 0
fi
SHADERS="${1:-$SHADERS}"
[ -f "$SHADERS/index.txt" ] && [ -d "$SHADERS/games" ] || {
	echo "No shader cache in $SHADERS (needs index.txt and games/): run tools/make-release.sh --fetch"; exit 1; }

VERSION=$(sed -n 's/^#define OUR64_VERSION "\(.*\)"/\1/p' "$ROOT/player/source/version.h")
make -C "$ROOT/player" RELEASE=1 -j"$(nproc)" > /dev/null

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/wiiu/apps" "$STAGE/wiiu/our64/roms"
cp "$ROOT/player/release/our64.wuhb" "$STAGE/wiiu/apps/"
cp "$ROOT/third_party/gliden64/ini/GLideN64.custom.ini" "$STAGE/wiiu/our64/"
cp -r "$SHADERS" "$STAGE/wiiu/our64/shaders"
chmod -R u=rwX,go=rX "$STAGE/wiiu" # the card's own modes would unpack as unreadable folders

# Only the games in tools/release-games.txt: a card also holds the shaders of
# whatever else was played on it (hacks, homebrew). Their .keys and .pack are
# left out, and so are the single files only their packs hold; index.txt is
# written again from the files that stay.
G="$STAGE/wiiu/our64/shaders"
left=0; : > "$STAGE/out.txt"
for f in "$G"/games/*; do
	name=$(basename "$f"); name=${name%.*}
	grep -qxF -- "$name" "$ROOT/tools/release-games.txt" && continue
	case "$f" in *.pack) python3 "$ROOT/tools/pack-keys.py" "$f" >> "$STAGE/out.txt"; echo "Left out: $name"; left=$((left + 1));; esac
	rm -f "$f"
done
python3 "$ROOT/tools/pack-keys.py" "$G"/games/*.pack | sort -u > "$STAGE/kept.txt"
sort -u "$STAGE/out.txt" | comm -23 - "$STAGE/kept.txt" | while read -r k; do rm -f "$G/${k:0:2}/$k.bin"; done
find "$G" -mindepth 2 -name '*.bin' -printf '%f\n' | sed 's/\.bin$//' | sort > "$G/index.txt"
rm -f "$STAGE/out.txt" "$STAGE/kept.txt"
echo "$left games' shaders left out; $(ls "$G"/games/*.pack | wc -l) packs, $(wc -l < "$G/index.txt") single files"

# The updater's files. shaders.zip first: its SHA-256 goes into the full zip.
PARTS="$ROOT/release/Our64-$VERSION"
rm -rf "$PARTS"
mkdir -p "$PARTS"
cp "$STAGE/wiiu/apps/our64.wuhb" "$STAGE/wiiu/our64/GLideN64.custom.ini" "$PARTS/"
(cd "$G" && zip -q -X "$PARTS/shaders.zip" games/*.pack games/*.keys)
sha256sum "$PARTS/shaders.zip" | cut -d' ' -f1 > "$G/release.txt"
echo "$PARTS: our64.wuhb, GLideN64.custom.ini, shaders.zip ($(du -h "$PARTS/shaders.zip" | cut -f1))"

OUT="$ROOT/release/Our64-$VERSION.zip"
mkdir -p "$ROOT/release"
rm -f "$OUT"
(cd "$STAGE" && zip -qr -X "$OUT" wiiu)
echo "$OUT: $(du -h "$OUT" | cut -f1), $(find "$G" -type f | wc -l) shader files"
