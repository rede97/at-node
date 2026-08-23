#!/usr/bin/env bash
# build-c3.sh - ESP32-C3 SuperMini wrapper (see build.sh header for setup)
set -euo pipefail
exec "$(dirname "$0")/build.sh" c3 "$@"
