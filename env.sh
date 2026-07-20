#!/usr/bin/env bash
# at-node Linux dev environment.
# Usage:  source env.sh
#
# Adds the WCH RISC-V toolchain and OpenOCD (both under ~/wch) to PATH.
# Safe to source multiple times; paths are only prepended if missing.

WCH_ROOT="${WCH_ROOT:-$HOME/wch}"

# Classic MounRiver GCC 8.2 — provides riscv-none-embed-gcc, the exact
# prefix used by software/obj/makefile. (GCC12/GCC15 use different
# prefixes — riscv-wch-elf / riscv32-wch-elf — and are NOT picked up.)
WCH_GCC="$WCH_ROOT/Toolchain/RISC-V Embedded GCC/bin"
WCH_OPENOCD="$WCH_ROOT/OpenOCD/OpenOCD/bin"

for d in "$WCH_GCC" "$WCH_OPENOCD"; do
    if [ -d "$d" ] && [[ ":$PATH:" != *":$d:"* ]]; then
        PATH="$d:$PATH"
    fi
done
export PATH

if command -v riscv-none-embed-gcc >/dev/null 2>&1; then
    echo "at-node env OK: $(riscv-none-embed-gcc --version | head -1)"
else
    echo "at-node env ERROR: riscv-none-embed-gcc not found (check WCH_ROOT=$WCH_ROOT)" >&2
    return 1 2>/dev/null || exit 1
fi
