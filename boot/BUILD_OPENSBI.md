# Building OpenSBI for RISCV-VP

## Memory layout

| Binary        | Physical address | mem[] offset |
|---------------|-----------------|-------------|
| `fw_jump.bin` | `0x80000000`    | `0x00000000` |
| `vp.dtb`      | `0x80100000`    | `0x00100000` |
| Linux `Image` | `0x80200000`    | `0x00200000` |

CPU reset state: PC=0x80000000, a0=0 (hart ID), a1=0x80100000 (DTB ptr)

## Step 1: Build the DTB

```bash
dtc -I dts -O dtb -o boot/vp.dtb boot/vp.dts
```

Requires `device-tree-compiler`:
```bash
sudo apt install device-tree-compiler
```

## Step 2: Build OpenSBI

```bash
sudo apt install gcc-riscv64-unknown-elf   # or gcc-riscv64-linux-gnu
git clone https://github.com/riscv-software-src/opensbi.git
cd opensbi
make PLATFORM=generic \
     CROSS_COMPILE=riscv64-unknown-elf- \
     FW_JUMP=y \
     FW_JUMP_ADDR=0x80200000 \
     FW_JUMP_FDT_ADDR=0x80100000 \
     -j$(nproc)
```

Output: `build/platform/generic/firmware/fw_jump.bin`

Copy to project:
```bash
cp build/platform/generic/firmware/fw_jump.bin /path/to/RISCV-VP/boot/fw_jump.bin
```

## Step 3: Build a Linux kernel + initramfs

Using Buildroot (simplest):
```bash
git clone https://github.com/buildroot/buildroot.git
cd buildroot
make qemu_riscv64_virt_defconfig
make -j$(nproc)
```

Output: `output/images/Image` (flat kernel binary)

Copy to project:
```bash
cp output/images/Image /path/to/RISCV-VP/boot/Image
```

> Note: The Buildroot `qemu_riscv64_virt_defconfig` uses a different UART (SiFive).
> After running `make menuconfig`, change:
>   System configuration → Run a getty after boot → /dev/ttyS0
>   Kernel → Linux Kernel → Custom config → add CONFIG_SERIAL_8250=y, CONFIG_SERIAL_8250_CONSOLE=y

## Step 4: Run the VP

```bash
cd build_cycle6
./RISCV_VP -R 64 \
  --bios ../boot/fw_jump.bin \
  --dtb  ../boot/vp.dtb \
  --kernel ../boot/Image
```

## Troubleshooting

- **No output**: UART at 0x10000000 may need `reg-io-width = <4>` instead of `<1>` depending on the UART peripheral implementation. Check `inc/UART.h`.
- **Kernel panic — no init**: Initramfs is missing. Rebuild Buildroot or use `CONFIG_INITRAMFS_SOURCE`.
- **OpenSBI boot hangs**: Check that `FW_JUMP_ADDR` matches the kernel load address (0x80200000) and `FW_JUMP_FDT_ADDR` matches the DTB address (0x80100000).
