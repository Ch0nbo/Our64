#!/usr/bin/env bash
# The 217-game sweep (sd:/wiiu/our64/sweep, main.c sweep()) over FTPiiU.
#   tools/sweep.sh start [seconds]  switch it on (default 30 s a game), clearing the last sweep's progress
#   tools/sweep.sh only <seconds> <pattern>...  the same for the games whose file names match a pattern
#                                   (grep -E), marking the rest done from the last fetched results
#   tools/sweep.sh fetch            sweep-results.txt and player-log.txt into results/sweep-<date>, CHECK lines listed
#   tools/sweep.sh stop             switch it off so Our64 plays normally again
# Close Our64 on the console first; start it again from the Wii U Menu to run or go on.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CARD=/fs/vol/external01/wiiu/our64
ftp() { lftp -u anonymous: "$HOST" -e "set cache:enable no; set net:max-retries 2; set net:timeout 20; $1; bye"; }
case "${1:-}" in
start)
	tmp=$(mktemp); echo "${2:-30}" > "$tmp"
	ftp "rm -f $CARD/sweep-done $CARD/sweep-current $CARD/sweep-results.txt; rm -rf $CARD/sweep-data; put $tmp -o $CARD/sweep" || true
	rm -f "$tmp"
	echo "Sweep on (${2:-30} s a game). Start Our64 from the Wii U Menu; it plays every game, then stops."
	;;
only)
	[ $# -ge 3 ] || { sed -n '2,8p' "$0"; exit 1; }
	secs=$2; shift 2
	last=$(ls -d "$ROOT"/results/sweep-* | tail -1)/sweep-results.txt
	pat=$(printf '%s|' "$@"); pat=${pat%|}
	tmp=$(mktemp); tmp2=$(mktemp)
	grep -E '^.*\.(z64|n64|v64)[ :]' "$last" | sed -E 's/^(.*\.(z64|n64|v64))[ :].*/\1/' | grep -vE "$pat" > "$tmp" || true
	echo "$secs" > "$tmp2"
	ftp "rm -f $CARD/sweep-current $CARD/sweep-results.txt; rm -rf $CARD/sweep-data; put $tmp -o $CARD/sweep-done; put $tmp2 -o $CARD/sweep" || true
	echo "Sweep on ($secs s a game) for $(( $(grep -cE '\.(z64|n64|v64)[ :]' "$last") - $(wc -l < "$tmp") )) games. Start Our64 from the Wii U Menu."
	rm -f "$tmp" "$tmp2"
	;;
fetch)
	out="$ROOT/results/sweep-$(date +%F)"; mkdir -p "$out"
	ftp "set xfer:clobber on; get -O $out $CARD/sweep-results.txt; get -O $out $CARD/player-log.txt"
	echo "$(grep -vc '^Our64 sweep\|^Done' "$out/sweep-results.txt") games, $(grep -c 'CHECK' "$out/sweep-results.txt") to check:"
	grep 'CHECK' "$out/sweep-results.txt" || true
	;;
stop)
	ftp "rm -f $CARD/sweep"
	echo "Sweep off."
	;;
*)	sed -n '2,8p' "$0"; exit 1 ;;
esac
