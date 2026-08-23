#!/usr/bin/env bash
# build-esp32.sh - classic ESP32 wrapper (see build.sh header for setup)
set -euo pipefail
exec "$(dirname "$0")/build.sh" esp32 "$@"
