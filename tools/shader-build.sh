#!/usr/bin/env bash
# The console's shader build (sd:/wiiu/our64/build-shaders, main.c
# build_shaders()) over FTPiiU: Our64 opens each game twice without running
# it, compiles the shaders its keys need and writes its pack. Needed for a
# release whenever a game's shader text changed.
#   tools/shader-build.sh start             every game, clearing the last build's progress
#   tools/shader-build.sh only <pattern>... the games whose file names match a pattern (grep -iE),
#                                           marking the rest done from the fullest sweep results
#   tools/shader-build.sh fetch             shader-build-results.txt and player-log.txt into
#                                           results/shader-build-<date>, CHECK lines listed
#   tools/shader-build.sh stop              switch it off so Our64 plays normally again
# Close Our64 on the console first; start it again from the Wii U Menu to run or go on.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CARD=/fs/vol/external01/wiiu/our64
ftp() { lftp -u anonymous: "$HOST" -e "set cache:enable no; set net:max-retries 2; set net:timeout 20; $1; bye"; }
roms() { sed -E -n 's/^(.*\.(z64|n64|v64))[ :].*/\1/p' "$1"; }
case "${1:-}" in
start)
	tmp=$(mktemp)
	ftp "rm -f $CARD/shader-build-done $CARD/shader-build-current $CARD/shader-build-results.txt; put $tmp -o $CARD/build-shaders" || true
	rm -f "$tmp"
	echo "Shader build on for every game. Start Our64 from the Wii U Menu; it stops at SHADERS BUILT."
	;;
only)
	[ $# -ge 2 ] || { sed -n '2,12p' "$0"; exit 1; }
	shift
	last=""; most=0
	for f in "$ROOT"/results/sweep-*/sweep-results*.txt; do
		n=$(roms "$f" | wc -l)
		[ "$n" -gt "$most" ] && { most=$n; last=$f; }
	done
	[ -n "$last" ] || { echo "No sweep results under results/ to take the list of games from"; exit 1; }
	pat=$(printf '%s|' "$@"); pat=${pat%|}
	tmp=$(mktemp); tmp2=$(mktemp)
	roms "$last" | grep -viE "$pat" > "$tmp" || true
	echo "Building:"; roms "$last" | grep -iE "$pat" | sed 's/^/  /'
	ftp "rm -f $CARD/shader-build-current $CARD/shader-build-results.txt; put $tmp -o $CARD/shader-build-done; put $tmp2 -o $CARD/build-shaders" || true
	echo "Shader build on for $(( most - $(wc -l < "$tmp") )) games (and any game not in $last). Start Our64 from the Wii U Menu."
	rm -f "$tmp" "$tmp2"
	;;
fetch)
	out="$ROOT/results/shader-build-$(date +%F)"; mkdir -p "$out"
	ftp "set xfer:clobber on; get -O $out $CARD/shader-build-results.txt; get -O $out $CARD/player-log.txt"
	cat "$out/shader-build-results.txt"
	echo "$(grep -c 'CHECK' "$out/shader-build-results.txt") to check"
	;;
stop)
	ftp "rm -f $CARD/build-shaders $CARD/shader-build-done $CARD/shader-build-current"
	echo "Shader build off."
	;;
*)	sed -n '2,12p' "$0"; exit 1 ;;
esac
