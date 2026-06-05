#!/bin/bash
# Build robust_fast_unified.c for both RV32 and RV64
# Requires: RISC-V toolchain (riscv32/64-unknown-elf-gcc)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_FILE="$PROJECT_ROOT/tests/full_system/robust_fast_unified.c"
HEX_DIR="$PROJECT_ROOT/tests/hex"

mkdir -p "$HEX_DIR"

echo "========================================"
echo "  Building Robust Fast Test (Unified)"
echo "========================================"

# Build RV32 version
echo ""
echo "--- Building RV32 version ---"
if command -v riscv32-unknown-elf-gcc &> /dev/null; then
    riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib \
        -D__RV32__ -Wl,--entry=main -Wl,--gc-sections \
        "$SRC_FILE" -o "$PROJECT_ROOT/robust_fast32.elf"
    
    riscv32-unknown-elf-objcopy -O ihex "$PROJECT_ROOT/robust_fast32.elf" "$HEX_DIR/robust_fast32.hex"
    echo "[OK] Created: tests/hex/robust_fast32.hex"
else
    echo "[SKIP] riscv32-unknown-elf-gcc not found"
fi

# Build RV64 version
echo ""
echo "--- Building RV64 version ---"
if command -v riscv64-unknown-elf-gcc &> /dev/null; then
    riscv64-unknown-elf-gcc -march=rv64imac -mabi=lp64 -O2 -nostdlib \
        -D__RV64__ -Wl,--entry=main -Wl,--gc-sections \
        "$SRC_FILE" -o "$PROJECT_ROOT/robust_fast64.elf"
    
    riscv64-unknown-elf-objcopy -O ihex "$PROJECT_ROOT/robust_fast64.elf" "$HEX_DIR/robust_fast64.hex"
    echo "[OK] Created: tests/hex/robust_fast64.hex"
else
    echo "[SKIP] riscv64-unknown-elf-gcc not found"
fi

echo ""
echo "========================================"
echo "  Build Complete!"
echo "========================================"
echo ""
echo "Run tests with:"
echo "  ./build_cycle6/RISCV_VP -f tests/hex/robust_fast32.hex -R 32"
echo "  ./build_cycle6/RISCV_VP -f tests/hex/robust_fast64.hex -R 64"
