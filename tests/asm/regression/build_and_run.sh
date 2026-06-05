#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
VP="$REPO/build_cycle6/RISCV_VP"
GCC="riscv64-unknown-elf-gcc"
OBJCOPY="riscv64-unknown-elf-objcopy"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PASS=0
FAIL=0

if [ ! -f "$VP" ]; then
  echo "ERROR: VP not found at $VP" >&2
  exit 1
fi

for src in "$DIR"/*.S; do
  name="$(basename "${src%.S}")"
  elf="/tmp/regression_${name}.elf"
  hex="/tmp/regression_${name}.hex"

  "$GCC" -march=rv64imac -mabi=lp64 -nostdlib \
    -T "$DIR/link.ld" "$src" -o "$elf"
  "$OBJCOPY" -O ihex "$elf" "$hex"

  rc=0
  out=$(timeout 15 "$VP" -f "$hex" -R 64 --max-instr 500000 2>&1) || rc=$?

  if echo "$out" | grep -q '\[HTIF\] PASS'; then
    echo "PASS     $name"
    PASS=$((PASS+1))
  elif echo "$out" | grep -q '\[HTIF\] FAIL'; then
    detail=$(echo "$out" | grep '\[HTIF\] FAIL' | head -1)
    echo "FAIL     $name  ($detail)"
    FAIL=$((FAIL+1))
  elif [ "$rc" -eq 124 ]; then
    echo "TIMEOUT  $name"
    FAIL=$((FAIL+1))
  else
    echo "UNKNOWN  $name"
    FAIL=$((FAIL+1))
  fi

  rm -f "$elf" "$hex"
done

echo "---"
echo "PASS: $PASS  FAIL/UNKNOWN: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
