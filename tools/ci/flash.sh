#!/usr/bin/env bash
# flash.sh <hex> [expected-role] — flash the board on the WCH-Link debug
# wire, then verify by AT+VER role tag.
#
#   <hex>           firmware image (e.g. tools/ci/out/kbd.hex)
#   [expected-role] kbd | dongle — post-flash check, exit 1 on mismatch
#
# NOTE: wlink flashes whichever board the debug wire is physically
# attached to — there is no software board selection. Two-board rigs
# must move the wire (loop_test.sh prompts for this).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HEX="${1:?usage: flash.sh <hex> [expected-role]}"
ROLE="${2:-}"

[ -f "$HEX" ] || { echo "no such hex: $HEX" >&2; exit 1; }
command -v wlink >/dev/null || { echo "wlink not found (see SKILL-Linux.md §1.1)" >&2; exit 1; }

echo "=== wlink status ==="
wlink status

echo "=== flash $HEX ==="
wlink flash "$HEX"

echo "=== post-flash role check ==="
cd "$ROOT"
if [ -n "$ROLE" ]; then
    uv run python tools/ci/board_roles.py --wait 10 --require "$ROLE"
else
    uv run python tools/ci/board_roles.py --wait 10
fi
