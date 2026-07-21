#!/usr/bin/env bash
# loop_test.sh — one-command closed loop (PLAN.md M3):
#   build -> flash both boards -> run the two-board dongle loop test.
#
# Default flow matches the standard rig wiring (debug wire lives on the
# DONGLE board): kbd is flashed over ISP (AT+ISP, wireless), dongle via
# wlink — because the dongle board's ISP USB handshake proved flaky
# (2026-07-21: kbd ISP succeeds every time, dongle ISP never lands;
# suspected marginal USB connection, wlink is the reliable path there).
#
#   --isp     both boards via ISP (needs AT+ISP firmware on both)
#   --wlink   legacy interactive flow (prompts to move the debug wire)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CI="$ROOT/tools/ci"
MODE=mixed
[ "${1:-}" = "--isp" ]   && MODE=isp
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
    [ -n "$KBD_PORT" ] || { echo "no [kbd] board found" >&2; exit 1; }
    echo "=== ISP flash kbd ($KBD_PORT) ==="
    uv run python "$CI/isp_flash.py" "$CI/out/kbd.hex" --port "$KBD_PORT"
    sleep 3
    if [ "$MODE" = isp ]; then
        DGL_PORT="$(port_of dongle)"   # ports can renumber after reset
        [ -n "$DGL_PORT" ] || { echo "no [dongle] board found" >&2; exit 1; }
        echo "=== ISP flash dongle ($DGL_PORT) ==="
        uv run python "$CI/isp_flash.py" "$CI/out/dongle.hex" --port "$DGL_PORT"
    else
        echo "=== wlink flash dongle (debug wire) ==="
        "$CI/flash.sh" "$CI/out/dongle.hex" dongle
    fi
fi

sleep 3
echo "=== dongle loop test ==="
uv run python tools/test_dongle_loop.py
