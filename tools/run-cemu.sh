#!/usr/bin/env bash
# Runs a .wuhb in the Cemu flatpak until a results file on Cemu's SD card says
# "Done", or until the time limit, then closes Cemu (a plain timeout on
# `flatpak run` leaves Cemu itself running).
# Usage: tools/run-cemu.sh <app.wuhb> <results file under sd:/wiiu/our64> [seconds]
set -uo pipefail
WUHB="$(realpath "$1")"
RESULTS="$HOME/.var/app/info.cemu.Cemu/data/Cemu/sdcard/wiiu/our64/$2"
LIMIT="${3:-300}"
SETTINGS="$HOME/.var/app/info.cemu.Cemu/config/Cemu/settings.xml"

# Cemu rewrites its settings on exit; keep OSReport logging on and the update check off.
sed -i 's|<logflag>[0-9]*</logflag>|<logflag>2305843009213825028</logflag>|; s|<check_update>true</check_update>|<check_update>false</check_update>|' "$SETTINGS"
rm -f "$RESULTS"

flatpak run --filesystem="$(dirname "$WUHB")" info.cemu.Cemu -g "$WUHB" >/dev/null 2>&1 &
end=$((SECONDS + LIMIT))
until grep -qs "Done. Press HOME" "$RESULTS" || [ $SECONDS -ge $end ]; do sleep 2; done
grep -qs "Done. Press HOME" "$RESULTS" && status=done || status=timeout

pkill -f "Cemu_relwithdebinfo -g $WUHB"
sleep 2
pkill -9 -f "Cemu_relwithdebinfo -g $WUHB"
echo "cemu: $status"
[ -f "$RESULTS" ] && cat "$RESULTS"
