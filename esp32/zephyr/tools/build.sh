#!/usr/bin/env bash
# AT-Node Zephyr build/flash wrapper (nanoESP32-S3).
# Usage:
#   tools/build.sh            # build -> build_zephyr/
#   tools/build.sh flash      # build + flash (PORT env, default /dev/ttyACM0)
#   tools/build.sh pristine   # pristine rebuild
set -e

WS="${ZEPHYR_WS:-$HOME/zephyrproject}"
export PATH="$WS/.venv/bin:$PATH"
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/.local/zephyr-sdk-1.0.0}"
export ZEPHYR_BASE="$WS/zephyr"

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$APP_DIR/build_zephyr}"
BOARD="nano_esp32s3/esp32s3/procpu"
PORT="${PORT:-/dev/ttyACM0}"

case "${1:-build}" in
build)
	west build -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR"
	;;
pristine)
	west build -b "$BOARD" -p always -d "$BUILD_DIR" "$APP_DIR"
	;;
flash)
	west build -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR"
	west flash -d "$BUILD_DIR" --esp-device "$PORT"
	;;
*)
	echo "usage: $0 [build|pristine|flash]" >&2
	exit 1
	;;
esac
