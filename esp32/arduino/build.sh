#!/usr/bin/env bash
# build.sh - shared build/upload engine for the esp32/arduino sketch (Linux)
#
# Agents: do NOT call this directly. Use the board-specific wrappers:
#   ./build-c3.sh    [-p /dev/ttyACMx] [-v VARIANT]   (ESP32-C3 SuperMini)
#   ./build-esp32.sh [-p /dev/ttyUSBx] [-v VARIANT]   (standard ESP32)
#
# Prerequisites:
#   - arduino-cli on PATH (core: esp32:esp32 >= 3.3.5, libs: NimBLE-Arduino,
#     PubSubClient). One-time setup:
#       arduino-cli config init
#       arduino-cli config add board_manager.additional_urls \
#         https://espressif.github.io/arduino-esp32/package_esp32_index.json
#       arduino-cli core update-index && arduino-cli core install esp32:esp32
#       arduino-cli lib install "NimBLE-Arduino" "PubSubClient"
#
# Variants (feature macros in features.h — unified model: core = BLE/LED/I2C,
# comm = HTTP/MQTT/RATHOLE; LED via ATNODE_LED=0|1|2, board profile via
# ATNODE_BOARD, both default per compile target):
#   full    - everything on (default)
#   remoter - FEATURE_RATHOLE=0 (Remoter: IR + BLE HID + MQTT + HTTP + I2C,
#             no rathole tunnel -> saves heap for BLE/MQTT)
#   base    - FEATURE_RATHOLE=0 FEATURE_HTTP=0 (production keyboard: BLE+MQTT+I2C,
#             no tunnel, no LAN HTTP control plane -> serial-only config)
#   rathole - FEATURE_BLE=0 FEATURE_MQTT=0 FEATURE_I2C=0 (Rathole: tunnel test unit;
#             I2C off makes ATNODE_LED default to breath on the C3 GPIO8 LED)
#
# Board notes:
#   c3    - ESP32-C3 SuperMini. fqbn MUST carry CDCOnBoot=cdc; any other flash
#           path routes Serial to UART0 pads (native-USB COM shows only the ROM
#           boot log, AT dead, WiFi/HTTP fine). Symptom -> reflash, do not debug.
#   esp32 - standard ESP32 (no native USB; Serial = UART0 via onboard USB-UART
#           bridge, CDCOnBoot not applicable).

set -euo pipefail

usage() {
    echo "usage: $0 <c3|esp32> [-p PORT] [-v full|remoter|base|rathole]" >&2
    exit 2
}

BOARD="" PORT="" VARIANT="full"
while [ $# -gt 0 ]; do
    case "$1" in
        c3|esp32)   BOARD="$1"; shift ;;
        -p|--port)  PORT="$2"; shift 2 ;;
        -v|--variant) VARIANT="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$BOARD" ] || usage

case "$BOARD" in
    c3)    FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app" ;;
    esp32) FQBN="esp32:esp32:esp32:PartitionScheme=huge_app" ;;
esac

case "$VARIANT" in
    full)    DEFS="" ;;
    remoter) DEFS="-DFEATURE_RATHOLE=0" ;;
    base)    DEFS="-DFEATURE_RATHOLE=0 -DFEATURE_HTTP=0" ;;
    rathole) DEFS="-DFEATURE_BLE=0 -DFEATURE_MQTT=0 -DFEATURE_I2C=0" ;;
    *) usage ;;
esac

command -v arduino-cli >/dev/null || {
    echo "arduino-cli not found on PATH" >&2; exit 1; }

echo "Compiling esp32/arduino [$BOARD/$VARIANT] ..."
if [ -n "$DEFS" ]; then
    arduino-cli compile --fqbn "$FQBN" \
        --build-property "compiler.cpp.extra_flags=$DEFS" .
else
    arduino-cli compile --fqbn "$FQBN" .
fi

if [ -n "$PORT" ]; then
    echo "Uploading to $PORT ..."
    arduino-cli upload --fqbn "$FQBN" -p "$PORT" .
else
    echo "No -p given; skipping upload."
fi
