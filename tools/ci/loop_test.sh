#!/usr/bin/env bash
# loop_test.sh — one-command closed loop (PLAN.md M3):
#   build both variants -> flash kbd board -> flash dongle board ->
#   run the two-board dongle loop test.
#
# Single WCH-Link rig: the script pauses for the debug wire to be moved
# between boards. -y skips the pauses (wire/boards already in place —
# only useful when flashing is not needed or done externally).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CI="$ROOT/tools/ci"
YES=0
[ "${1:-}" = "-y" ] && YES=1

pause() {  # $1=message
    if [ "$YES" = 0 ]; then
        read -r -p "$1 [Enter] " _
    else
        echo "$1"
    fi
}

"$CI/build_all.sh"

pause "Attach WCH-Link to the KBD board."
"$CI/flash.sh" "$CI/out/kbd.hex" kbd

pause "Move WCH-Link to the DONGLE board."
"$CI/flash.sh" "$CI/out/dongle.hex" dongle

echo "=== dongle loop test ==="
cd "$ROOT"
uv run python tools/test_dongle_loop.py
