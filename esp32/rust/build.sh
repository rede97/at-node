#!/usr/bin/env bash
# build.sh - variant build/flash engine for the esp32/rust (S3) firmware.
#
# Mirror of esp32/arduino/build.sh semantics: one variant name selects a
# compile-time feature set (cargo features, matrix documented in
# Cargo.toml [features]):
#
#   full    - everything except kbd-ble (R6): LED + rathole + MQTT + HTTP
#             + HWS + USB HID keyboard + SSDP discovery
#   remoter - full minus rathole (saves RAM for BLE/MQTT)
#   base    - full minus rathole and http/ssdp (production keyboard: MQTT +
#             HWS + USB HID, serial-only configuration)
#   rathole - tunnel test unit: LED + HTTP + rathole + SSDP (no kbd/mqtt/hws)
#   ble     - R6 bring-up unit: kbd-ble + LED only, WiFi OFF (frees ~46 KB
#             of internal heap for the BLE controller)
#
# Usage:
#   ./build.sh [full|remoter|base|rathole|ble] [--flash [/dev/ttyACMx]]
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
    remoter) FLAGS="--no-default-features --features led-color,mqtt,http,hws,kbd-usb,ssdp" ;;
    base)    FLAGS="--no-default-features --features led-color,mqtt,hws,kbd-usb" ;;
    rathole) FLAGS="--no-default-features --features led-color,http,rathole,ssdp" ;;
    ble)     FLAGS="--no-default-features --features led-color,kbd-ble" ;;
    *) echo "usage: $0 [full|remoter|base|rathole|ble] [--flash [PORT]]" >&2; exit 2 ;;
esac

# shellcheck disable=SC2086
cargo build --release $FLAGS

ELF=target/xtensa-esp32s3-none-elf/release/atnode-s3
if [ -n "$FLASH_PORT" ]; then
    espflash flash --chip esp32s3 --port "$FLASH_PORT" "$ELF"
else
    echo "built: $ELF (no --flash given)"
fi
