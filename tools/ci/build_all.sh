#!/usr/bin/env bash
# build_all.sh — build both firmware variants from scratch.
#
#   kbd     (default)   BLE HID keyboard (Peripheral)
#   dongle  (DONGLE=1)  BLE HID receiver (Central)
#   dual    (MODE=DUAL) both roles, AT+ROLE runtime switch
#
# Outputs: tools/ci/out/{kbd,dongle,dual}.{hex,elf,map}
# Ends with `make clean` so a later manual `make` in software/obj can
# never silently link stale objects of the wrong variant (make does
# not track flag changes).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/env.sh" >/dev/null

OBJ="$ROOT/software/obj"
OUT="$ROOT/tools/ci/out"
mkdir -p "$OUT"

build_variant() {  # $1=name, $2=extra make args (may be empty)
    echo "=== build $1 ==="
    make -C "$OBJ" --no-print-directory clean >/dev/null
    # shellcheck disable=SC2086
    make -C "$OBJ" --no-print-directory main-build $2
    cp "$OBJ/at-node.hex" "$OUT/$1.hex"
    cp "$OBJ/at-node.elf" "$OUT/$1.elf"
    cp "$OBJ/at-node.map" "$OUT/$1.map"
}

build_variant kbd ""
build_variant dongle "DONGLE=1"
build_variant dual "MODE=DUAL"

make -C "$OBJ" --no-print-directory clean >/dev/null
echo "=== done: $OUT/{kbd,dongle,dual}.hex (obj tree cleaned) ==="
