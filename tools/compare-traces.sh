#!/usr/bin/env bash
# Compares the null backend's input traces from the PC build (little-endian,
# tools/replay-pc/replay64 with REPLAY_TRACE) and the console replay app
# (big-endian, sd:/wiiu/our64/traces). Prints the first differing lines.
# Usage: tools/compare-traces.sh <pc.trace> <console.trace> [lines of context]
set -uo pipefail
n="${3:-6}"
d=$(diff "$1" "$2")
if [ -z "$d" ]; then echo "identical ($(wc -l < "$1") lines)"; exit 0; fi
echo "$(grep -c '^<' <<<"$d") of $(wc -l < "$1") lines differ; first difference:"
diff --unchanged-line-format='  %L' --old-line-format='- %L' --new-line-format='+ %L' "$1" "$2" \
	| grep -n -m1 -B"$n" -A"$((n * 2))" '^[-+] ' | head -$((n * 3 + 2))
