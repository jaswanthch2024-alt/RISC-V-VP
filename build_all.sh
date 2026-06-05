#!/bin/bash
set -e

echo "========================================"
echo "Building AT Model..."
echo "========================================"
rm -rf build_at
cmake -S . -B build_at -DTIMING_MODEL=AT
cmake --build build_at -j$(nproc)

echo ""
echo "========================================"
echo "Building Cycle-2 Model..."
echo "========================================"
rm -rf build_cycle2
cmake -S . -B build_cycle2 -DTIMING_MODEL=CYCLE
cmake --build build_cycle2 -j$(nproc)

echo ""
echo "========================================"
echo "Building Cycle-6 Model..."
echo "========================================"
rm -rf build_cycle6
cmake -S . -B build_cycle6 -DTIMING_MODEL=CYCLE6
cmake --build build_cycle6 -j$(nproc)

echo ""
echo "========================================"
echo "All builds complete successfully!"
echo "Executables are in:"
echo "  - build_at/riscv_vp"
echo "  - build_cycle2/riscv_vp"
echo "  - build_cycle6/riscv_vp"
echo "========================================"
