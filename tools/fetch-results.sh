#!/usr/bin/env bash
# Downloads a results file (default probe-results.txt) from sd:/wiiu/our64 over FTPiiU.
set -euo pipefail
HOST="${WIIU_HOST:-192.168.0.110}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/results"
lftp -u anonymous: "$HOST" -e "set cache:enable no; set net:max-retries 2; set net:timeout 10; set xfer:clobber on; get -O $ROOT/results /fs/vol/external01/wiiu/our64/${1:-probe-results.txt}; bye"
cat "$ROOT/results/${1:-probe-results.txt}"
