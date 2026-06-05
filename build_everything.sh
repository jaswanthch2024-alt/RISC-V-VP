#!/bin/bash
set -e
echo "Cleaning old builds..."
rm -rf build_lt build_at build_cycle build_Cycle6
for model in LT AT CYCLE CYCLE6; do
    echo "--------------------------------------------------"
    echo "Building $model Model..."
    echo "--------------------------------------------------"
    cmake -S . -B "build_$model" -DTIMING_MODEL=$model -DCMAKE_BUILD_TYPE=Release
    cmake --build "build_$model" --target RISCV_VP -j$(nproc)
done
echo "--------------------------------------------------"
echo "All builds complete!"
