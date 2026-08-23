#!/usr/bin/env bash
# build.sh - variant build/flash engine for the esp32/rust (S3) firmware.
#
# Mirror of esp32/arduino/build.sh semantics: one variant name selects a
# compile-time feature set (cargo features, matrix documented in
# Cargo.toml [features]):
#
#   full    - everything except kbd-ble (R6): LED + rathole + MQTT + HTTP
#             + HWS + USB HID keyboard
#   remoter - full minus rathole (saves RAM for BLE/MQTT)
#   base    - full minus rathole and http (production keyboard: MQTT +
#             HWS + USB HID, serial-only configuration)
#   rathole - tunnel test unit: LED + HTTP + rathole only (no kbd/mqtt/hws)
#
# Usage:
#   ./build.sh [full|remoter|base|rathole] [--flash [/dev/ttyACMx]]
#
# Prerequisites: espup toolchain (source ~/export-esp.sh), espflash.

set -euo pipefail
cd "$(dirname "$0")"

VARIANT="${1:-full}"
FLASH_PORT=""
if [ "${2:-}" = "--flash" ]; then
    FLASH_PORT="${3:-/dev/ttyACM0}"
fi

case "$VARIANT" in
    full)    FLAGS="" ;;
    remoter) FLAGS="--no-default-features --features led-color,mqtt,http,hws,kbd-usb" ;;
    base)    FLAGS="--no-default-features --features led-color,mqtt,hws,kbd-usb" ;;
    rathole) FLAGS="--no-default-features --features led-color,http,rathole" ;;
    *) echo "usage: $0 [full|remoter|base|rathole] [--flash [PORT]]" >&2; exit 2 ;;
esac

# shellcheck disable=SC2086
cargo build --release $FLAGS

ELF=target/xtensa-esp32s3-none-elf/release/atnode-s3
if [ -n "$FLASH_PORT" ]; then
    espflash flash --chip esp32s3 --port "$FLASH_PORT" "$ELF"
else
    echo "built: $ELF (no --flash given)"
fi
