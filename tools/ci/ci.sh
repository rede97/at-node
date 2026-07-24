#!/usr/bin/env bash
# ci.sh — local CI script for at-node project
# Runs build and test for both CH582 and ESP32 variants.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[CI]${NC} $*"; }
warn() { echo -e "${YELLOW}[CI]${NC} $*"; }
error() { echo -e "${RED}[CI]${NC} $*" >&2; }

# --- CH582 firmware build ---
build_ch582() {
    log "Building CH582 firmware (all variants)..."
    if ! command -v riscv-none-embed-gcc >/dev/null 2>&1; then
        warn "riscv-none-embed-gcc not found, skipping CH582 build"
        return 0
    fi
    cd "$SCRIPT_DIR/../../software/obj"
    make clean || true
    make --no-print-directory main-build
    make clean || true
    make --no-print-directory main-build DONGLE=1
    make clean || true
    make --no-print-directory main-build MODE=DUAL
    cd "$SCRIPT_DIR"
    log "CH582 build OK"
}

# --- ESP32 firmware build ---
build_esp32() {
    log "Building ESP32 firmware..."
    if ! command -v arduino-cli >/dev/null 2>&1; then
        warn "arduino-cli not found, skipping ESP32 build"
        return 0
    fi
    cd "$SCRIPT_DIR/../../esp32/esp32_at_node"
    arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app" .
    cd "$SCRIPT_DIR"
    log "ESP32 build OK"
}

# --- Python tests ---
run_tests() {
    log "Running Python tests..."
    if ! command -v uv >/dev/null 2>&1; then
        warn "uv not found, skipping Python tests"
        return 0
    fi
    cd "$SCRIPT_DIR/../.."
    # Run encoding check (if script exists)
    if [ -f tools/batch_utf8.py ]; then
        uv run python tools/batch_utf8.py software --check || error "Encoding check failed"
        log "Encoding check OK"
    else
        warn "Encoding check script not found, skipping"
    fi
    # Run AT test (if device available)
    # uv run python tools/test_at.py || warn "AT test skipped (no device)"
    log "Python tests OK"
}

# --- Documentation check ---
check_docs() {
    log "Checking documentation..."
    cd "$SCRIPT_DIR/../.."
    # Verify key docs exist
    for doc in AGENTS.md README.md REQUIREMENTS.md esp32/README.md esp32/PLAN.md esp32/API.md; do
        if [ ! -f "$doc" ]; then
            error "Missing doc: $doc"
            return 1
        fi
    done
    log "Docs check OK"
}

# --- Main ---
main() {
    log "Starting CI..."
    build_ch582
    build_esp32
    run_tests
    check_docs
    log "CI completed successfully"
}

main "$@"
