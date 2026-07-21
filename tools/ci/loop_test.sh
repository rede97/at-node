#!/usr/bin/env bash
# loop_test.sh — one-command closed loop (PLAN.md M3):
#   build -> flash both boards -> run the two-board dongle loop test.
#
# Default: ISP flashing (tools/ci/isp_flash.py) — both boards are
# triggered over their own CDC via AT+ISP, NO debug-wire moves.
# Requires both boards to already run firmware that has AT+ISP
# (first-time bring-up still needs wlink: tools/ci/flash.sh).
#
#   --wlink   legacy interactive flow (prompts to move the debug wire)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CI="$ROOT/tools/ci"
MODE=isp
[ "${1:-}" = "--wlink" ] && MODE=wlink
cd "$ROOT"

port_of() {  # $1 = role (kbd|dongle) -> port or empty
    uv run python tools/ci/board_roles.py 2>/dev/null \
        | awk -v r="$1" '$2 == r { print $1; exit }'
}

"$CI/build_all.sh"

if [ "$MODE" = wlink ]; then
    read -r -p "Attach WCH-Link to the KBD board. [Enter] " _
    "$CI/flash.sh" "$CI/out/kbd.hex" kbd
    read -r -p "Move WCH-Link to the DONGLE board. [Enter] " _
    "$CI/flash.sh" "$CI/out/dongle.hex" dongle
else
    KBD_PORT="$(port_of kbd)"
    DGL_PORT="$(port_of dongle)"
    [ -n "$KBD_PORT" ] && [ -n "$DGL_PORT" ] || {
        echo "need both boards with [kbd]/[dongle] tags (flash.sh first)" >&2; exit 1; }
    echo "=== ISP flash kbd ($KBD_PORT) ==="
    uv run python "$CI/isp_flash.py" "$CI/out/kbd.hex" --port "$KBD_PORT"
    sleep 3
    echo "=== ISP flash dongle ($DGL_PORT) ==="
    DGL_PORT="$(port_of dongle)"   # ports can renumber after reset
    [ -n "$DGL_PORT" ] || { echo "dongle board lost after kbd flash" >&2; exit 1; }
    uv run python "$CI/isp_flash.py" "$CI/out/dongle.hex" --port "$DGL_PORT"
fi

sleep 3
echo "=== dongle loop test ==="
uv run python tools/test_dongle_loop.py
