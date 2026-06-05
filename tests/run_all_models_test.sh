#!/bin/bash
# RISC-V-TLM Robust Test Runner - All Timing Models
# Run via WSL: powershell.exe -Command "& wsl bash /mnt/c/Users/jaswa/source/repos/RISC-V-TLM/tests/run_all_models_test.sh"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEX_DIR="$REPO/tests/hex"
MAX_INSTR=2000000
TIMEOUT_SEC=30
PASS=0
FAIL=0
SKIP=0

declare -A BINS
BINS[LT]="$REPO/build_lt/RISCV_VP"
BINS[AT]="$REPO/build_at/RISCV_VP"
BINS[CYCLE]="$REPO/build_cycle_wsl/RISCV_VP"
BINS[CYCLE6]="$REPO/build_cycle6/RISCV_VP"

echo "========================================================"
echo "  RISC-V-TLM All-Models Test - Post-Fix Validation"
echo "  Date: $(date)"
echo "========================================================"

printf "%-8s  %-32s  %-6s  %-12s  %s\n" "MODEL" "TEST" "ARCH" "INSTRS" "STATUS"
printf '%0.s-' {1..78}
echo ""

run_test() {
    local model="$1"
    local hexfile="$2"
    local arch="$3"
    local desc="$4"
    local exe="${BINS[$model]}"

    if [ ! -f "$exe" ]; then
        printf "%-8s  %-32s  %-6s  %-12s  %s\n" "$model" "$desc" "RV${arch}" "N/A" "SKIP(no bin)"
        SKIP=$((SKIP+1))
        return
    fi
    if [ ! -f "$hexfile" ]; then
        printf "%-8s  %-32s  %-6s  %-12s  %s\n" "$model" "$desc" "RV${arch}" "N/A" "SKIP(no hex)"
        SKIP=$((SKIP+1))
        return
    fi

    local tmpout
    tmpout=$(mktemp)
    local rc=0
    TRACE_STDOUT=1 timeout "$TIMEOUT_SEC" "$exe" -f "$hexfile" -R "$arch" --max-instr "$MAX_INSTR" > "$tmpout" 2>&1 || rc=$?

    local instrs
    instrs=$(grep -iaoP '^\s*Instructions:\s*\d+' "$tmpout" | head -n 1 | grep -aoP '\d+')
    instrs=${instrs:-0}

    local status
    if [ "$rc" -eq 124 ]; then
        status="TIMEOUT"
        FAIL=$((FAIL+1))
    elif [ "$rc" -ne 0 ]; then
        status="FAIL(rc=$rc)"
        FAIL=$((FAIL+1))
    elif [ "$instrs" -eq 0 ] 2>/dev/null; then
        status="FAIL(0 instrs)"
        FAIL=$((FAIL+1))
    else
        status="PASS"
        PASS=$((PASS+1))
    fi

    printf "%-8s  %-32s  %-6s  %-12s  %s\n" "$model" "$desc" "RV${arch}" "$instrs" "$status"
    rm -f "$tmpout"
}

for model in LT AT CYCLE CYCLE6; do
    run_test "$model" "$HEX_DIR/dhrystone32.hex"          "32" "Dhrystone RV32"
    run_test "$model" "$HEX_DIR/dhrystone64.hex"          "64" "Dhrystone RV64"
    run_test "$model" "$HEX_DIR/math_test64.hex"          "64" "Math Test RV64"
    run_test "$model" "$HEX_DIR/robust_system_test64.hex" "64" "Robust System Test RV64"
    run_test "$model" "$HEX_DIR/robust_fast64.hex"        "64" "Robust Fast RV64"
done

printf '%0.s=' {1..78}
echo ""
printf "  PASS: %d  FAIL: %d  SKIP: %d  TOTAL: %d\n" "$PASS" "$FAIL" "$SKIP" "$((PASS+FAIL+SKIP))"
printf '%0.s=' {1..78}
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAILED ($FAIL failures)"
    exit 1
else
    echo "RESULT: ALL PASSED"
    exit 0
fi
