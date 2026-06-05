#!/usr/bin/env python3
"""Dump kernel instructions at a specific PC from the flat Image file."""
import struct, sys

IMAGE = "/mnt/c/Users/jaswa/RISCV-VP/boot/Image"
# Kernel Image file: 64-byte header, then code starting at virtual 0xffffffff80200000
# Physical load address: 0x80200000
# The Image file maps: file[64 + N] → PA 0x80200000 + N → VA 0xffffffff80200000 + N
#
# Target PC: 0xffffffff8016957a
# VA offset from 0xffffffff80200000: 0x8016957a - 0x80200000 = -0x96a86
# So the target is BEFORE the load address in VA space.
# 
# Wait, that means the kernel code at 0x8016957a is mapped differently.
# Looking at the satp page tables: kernel text is at VA 0xffffffff80000000+,
# kernel is linked to start at 0xffffffff80200000 (load address).
# So VA_base = 0xffffffff80200000 is where the Image file starts.
# PC 0xffffffff8016957a → file offset = 64 + (0x8016957a - 0x80200000)
#                                      = 64 + (-0x96a86) — negative means before load address!
#
# Hmm. The kernel must have been linked to a different base. 
# Perhaps the load address is 0x80000000, not 0x80200000?
# Image loads at physical 0x80200000, but the kernel might be linked at 0xffffffff80000000 VA.
# Then: file_off = 64 + (PC - 0xffffffff80000000) = 64 + 0x8016957a - 0x80000000 = 64 + 0x16957a
LINK_BASE_VA = 0xffffffff80000000
LOAD_PA      = 0x80200000
target_pc = 0xffffffff8016957a

file_off = 64 + (target_pc - LINK_BASE_VA)
print(f"Target PC = {hex(target_pc)}")
print(f"File offset = {hex(file_off)} ({file_off})")

with open(IMAGE, "rb") as f:
    f.seek(file_off)
    data = f.read(64)

print("Instructions:")
for i in range(0, 64, 4):
    word = struct.unpack("<I", data[i:i+4])[0]
    print(f"  {hex(target_pc + i)}: 0x{word:08x}")

# Also check the slab write surrounding instructions (around 0x8016957a)
# and the area around 0x8016bd06 (write to 0x9f85bc88)
for check_pc in [0xffffffff8016957a, 0xffffffff8016bd06]:
    file_off2 = 64 + (check_pc - LINK_BASE_VA) - 32
    with open(IMAGE, "rb") as f:
        f.seek(file_off2)
        d2 = f.read(80)
    print(f"\nContext at {hex(check_pc)} (-32 bytes):")
    for i in range(0, 80, 4):
        word = struct.unpack("<I", d2[i:i+4])[0]
        marker = " <<<<" if (check_pc + i - 32) == check_pc else ""
        print(f"  {hex(check_pc - 32 + i)}: 0x{word:08x}{marker}")
