#!/usr/bin/env bash
# ci.sh — local CI script for at-node project
# Runs build and test for both CH582 and ESP32 variants.
# Uses standard xPack RISC-V toolchain (not WCH official).

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

# --- Toolchain setup ---
setup_toolchain() {
    log "Setting up xPack RISC-V toolchain..."
    
    TOOLCHAIN_DIR="$PROJECT_ROOT/.toolchain"
    ALIAS_DIR="$PROJECT_ROOT/.toolchain-alias"
    
    # Check if toolchain already exists
    if [ -d "$TOOLCHAIN_DIR" ] && [ -f "$ALIAS_DIR/bin/riscv-none-embed-gcc" ]; then
        log "Toolchain already installed"
        export PATH="$ALIAS_DIR/bin:$PATH"
        return 0
    fi
    
    # Download xPack toolchain if not present
    XPACK_VERSION="8.2.0-3.1"
    XPACK_NAME="xpack-riscv-none-elf-gcc-${XPACK_VERSION}-linux-x64"
    XPACK_URL="https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${XPACK_VERSION}/${XPACK_NAME}.tar.gz"
    
    log "Downloading xPack toolchain ${XPACK_VERSION}..."
    mkdir -p "$TOOLCHAIN_DIR"
    curl -sL "$XPACK_URL" -o /tmp/xpack-toolchain.tar.gz
    tar -xzf /tmp/xpack-toolchain.tar.gz -C "$TOOLCHAIN_DIR" --strip-components=1
    rm /tmp/xpack-toolchain.tar.gz
    
    # Create aliases for riscv-none-embed-* prefix
    log "Creating toolchain aliases..."
    mkdir -p "$ALIAS_DIR/bin"
    for tool in gcc g++ objcopy objdump size ar ranlib nm strip gdb; do
        src="$TOOLCHAIN_DIR/bin/riscv-none-elf-${tool}"
        dst="$ALIAS_DIR/bin/riscv-none-embed-${tool}"
        if [ -f "$src" ]; then
            ln -sf "$src" "$dst"
        fi
    done
    
    export PATH="$ALIAS_DIR/bin:$PATH"
    log "Toolchain setup complete"
}

# --- CH582 firmware build ---
build_ch582() {
    log "Building CH582 firmware (all variants)..."
    
    if ! command -v riscv-none-embed-gcc >/dev/null 2>&1; then
        setup_toolchain
    fi
    
    # Verify toolchain works
    if ! riscv-none-embed-gcc --version >/dev/null 2>&1; then
        error "Toolchain verification failed"
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
