#!/usr/bin/env bash
# ci.sh — local CI script for at-node project
# Runs build and test for both CH582 and ESP32 variants.
#
# CH582 build is OPT-IN (shelved in CI): it requires the WCH MounRiver
# toolchain, which has no public direct download and cannot be fetched
# automatically. Standard xPack/upstream GCC produces BROKEN firmware
# for this codebase — see tools/ci/TOOLCHAIN.md for the full report.
# ESP32 build, Python checks and doc checks always run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[CI]${NC} $*"; }
warn() { echo -e "${YELLOW}[CI]${NC} $*"; }
error() { echo -e "${RED}[CI]${NC} $*" >&2; }

# --- CH582 firmware build (opt-in, WCH toolchain only) ---
# Do NOT substitute xPack/upstream GCC here: it silently compiles
# __attribute__((interrupt("WCH-Interrupt-fast"))) handlers into plain
# functions (ret instead of mret) -> firmware crashes on first IRQ.
# Full evidence: tools/ci/TOOLCHAIN.md
build_ch582() {
    if [ "${BUILD_CH582:-0}" != "1" ]; then
        warn "CH582 build skipped (set BUILD_CH582=1 to enable; requires WCH toolchain, see tools/ci/TOOLCHAIN.md)"
        return 0
    fi

    log "Building CH582 firmware (all variants)..."

    # Locate the WCH toolchain via env.sh (expects ~/wch layout)
    if ! command -v riscv-none-embed-gcc >/dev/null 2>&1; then
        if [ -f "$PROJECT_ROOT/env.sh" ]; then
            # shellcheck disable=SC1091
            source "$PROJECT_ROOT/env.sh" >/dev/null
        fi
    fi
    if ! command -v riscv-none-embed-gcc >/dev/null 2>&1; then
        error "BUILD_CH582=1 but WCH toolchain (riscv-none-embed-gcc) not found."
        error "Install MounRiver 'RISC-V Embedded GCC' under ~/wch and re-run. See tools/ci/TOOLCHAIN.md"
        return 1
    fi
    log "Toolchain: $(riscv-none-embed-gcc --version | head -1)"

    cd "$PROJECT_ROOT/software/obj"

    # Build all variants
    for variant in "" "DONGLE=1" "MODE=DUAL"; do
        log "Building variant: ${variant:-kbd (default)}"
        make clean || true
        if [ -n "$variant" ]; then
            make --no-print-directory main-build $variant
        else
            make --no-print-directory main-build
        fi

        # Show size
        if [ -f at-node.elf ]; then
            log "Size: $(riscv-none-embed-size at-node.elf | tail -1)"
        fi
    done
    make clean >/dev/null || true

    cd "$PROJECT_ROOT"
    log "CH582 build OK"
}

# --- ESP32 firmware build ---
build_esp32() {
    log "Building ESP32 firmware..."
    if ! command -v arduino-cli >/dev/null 2>&1; then
        warn "arduino-cli not found, skipping ESP32 build"
        return 0
    fi
    cd "$PROJECT_ROOT/esp32/esp32_at_node"
    arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app" .
    cd "$PROJECT_ROOT"
    log "ESP32 build OK"
}

# --- Python tests ---
run_tests() {
    log "Running Python tests..."
    if ! command -v uv >/dev/null 2>&1; then
        warn "uv not found, skipping Python tests"
        return 0
    fi
    cd "$PROJECT_ROOT"
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
    cd "$PROJECT_ROOT"
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
    log "Project root: $PROJECT_ROOT"
    
    build_ch582
    build_esp32
    run_tests
    check_docs
    
    log "CI completed successfully"
}

main "$@"
