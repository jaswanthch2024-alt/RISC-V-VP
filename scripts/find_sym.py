#!/usr/bin/env python3
import sys

syms = []
for l in open("/tmp/vmlinux.syms"):
    parts = l.strip().split()
    if len(parts) == 3:
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()

target = 0xffffffff8071c540
best_sym = None
for val, name in syms:
    if val <= target:
        best_sym = (val, name)

if best_sym:
    print(f"Closest symbol: {best_sym[1]} @ {hex(best_sym[0])} (diff: {hex(target - best_sym[0])})")
else:
    print("No symbol found")
