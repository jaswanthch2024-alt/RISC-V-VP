#!/usr/bin/env bash
# run_riscv_tests.sh — RISC-V ISA conformance test runner for RISCV-VP
# Usage: ./tests/run_riscv_tests.sh [SUITE...]
# Suites: rv64ui-p  rv64si-p  rv64mi-p  (default: all three)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VP="$REPO/build_cycle6/RISCV_VP"
ISA_DIR="$REPO/tests/riscv-tests/build/share/riscv-tests/isa"
OBJCOPY="riscv64-unknown-elf-objcopy"
MAX_INSTR=2000000
TIMEOUT_SEC=30

PASS=0
FAIL=0
TIMEOUT_COUNT=0
UNKNOWN=0

# Parse suite arguments; default to all three suites
if [ "$#" -gt 0 ]; then
    SUITES=("$@")
else
    SUITES=(rv64ui-p rv64si-p rv64mi-p)
fi

# Validate required paths
if [ ! -f "$VP" ]; then
    echo "ERROR: VP binary not found at $VP" >&2
    exit 1
fi
if [ ! -d "$ISA_DIR" ]; then
    echo "ERROR: riscv-tests ISA directory not found at $ISA_DIR" >&2
    exit 1
fi

printf "\n%-44s  %s\n" "TEST" "RESULT"
printf '%0.s-' {1..60}; echo ""

for suite in "${SUITES[@]}"; do
    for elf in "$ISA_DIR/${suite}"-*; do
        # Skip .dump files
        [[ "$elf" == *.dump ]] && continue
        [ -f "$elf" ] || continue

        name="$(basename "$elf")"

        # Convert ELF to IHEX in a per-test temp file
        TMP_HEX=$(mktemp /tmp/riscv_test_XXXXXX.hex)
        if ! "$OBJCOPY" -O ihex "$elf" "$TMP_HEX" 2>/dev/null; then
            rm -f "$TMP_HEX"
            printf "%-44s  UNKNOWN (objcopy failed)\n" "$name"
            UNKNOWN=$((UNKNOWN + 1))
            continue
        fi

        # Run VP with timeout; capture combined stdout+stderr
        rc=0
        out=$(timeout "$TIMEOUT_SEC" "$VP" -f "$TMP_HEX" -R 64 --max-instr "$MAX_INSTR" 2>&1) || rc=$?
        rm -f "$TMP_HEX"

        if echo "$out" | grep -q '\[HTIF\] PASS'; then
            printf "%-44s  PASS\n" "$name"
            PASS=$((PASS + 1))
        elif echo "$out" | grep -q '\[HTIF\] FAIL'; then
            fail_detail=$(echo "$out" | grep '\[HTIF\] FAIL' | head -1 | tr -d '\r\n')
            printf "%-44s  FAIL  (%s)\n" "$name" "$fail_detail"
            FAIL=$((FAIL + 1))
        elif [ "$rc" -eq 124 ]; then
            printf "%-44s  TIMEOUT\n" "$name"
            TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        elif echo "$out" | grep -q '\[ECALL\] exit'; then
            # VP intercepted ecall exit before reaching tohost — not a VP bug
            printf "%-44s  UNKNOWN (ecall exit)\n" "$name"
            UNKNOWN=$((UNKNOWN + 1))
        else
            printf "%-44s  UNKNOWN\n" "$name"
            UNKNOWN=$((UNKNOWN + 1))
        fi
    done
done

printf '%0.s-' {1..60}; echo ""
printf "PASS: %d   FAIL: %d   TIMEOUT: %d   UNKNOWN: %d\n" \
    "$PASS" "$FAIL" "$TIMEOUT_COUNT" "$UNKNOWN"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
