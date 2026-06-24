#!/usr/bin/env python3
import sys

syms = []
for l in open("/tmp/vmlinux.syms"):
    parts = l.strip().split()
    if len(parts) == 3:
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()

targets = [0xffffffff80053a7a, 0xffffffff800538ba, 0xffffffff8000346e]
if len(sys.argv) > 1:
    targets = [int(x, 16) for x in sys.argv[1:]]

for target in targets:
    best_sym = None
    for val, name in syms:
        if val <= target:
            best_sym = (val, name)
    if best_sym:
        print(f"Target {hex(target)} -> Closest symbol: {best_sym[1]} @ {hex(best_sym[0])} (diff: {hex(target - best_sym[0])})")
    else:
        print(f"Target {hex(target)} -> No symbol found")

