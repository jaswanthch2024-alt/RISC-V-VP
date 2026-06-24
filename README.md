# RISC-V Virtual Platform (RISCV-VP)

A cycle-accurate, CVA6-aligned RISC-V 64-bit virtual platform implemented in SystemC/TLM-2.0.
Boots **Linux 6.1.0** to a BusyBox user-space shell on the 6-stage pipeline model.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![SystemC](https://img.shields.io/badge/SystemC-2.3.3+-green.svg)](https://www.accellera.org/downloads/standards/systemc)

---

## What This Is

RISCV-VP is a pre-silicon software validation platform. It models the microarchitecture of the
**CVA6 (Ariane)** open-source application-class RISC-V processor at the cycle level, allowing
software engineers to run real workloads (including a full Linux kernel) and obtain detailed
pipeline performance statistics before silicon is available.

---

## Quick Start — Linux Boot

```bash
# Build
mkdir -p build_cycle6 && cd build_cycle6
cmake .. -DRISCV_RV64=ON -DRISCV_CYCLE6=ON && make -j$(nproc)
cd ..

# Run Linux
./build_cycle6/RISCV_VP -R 64 \
  --bios  boot/fw_jump.bin \
  --dtb   boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz
```

Expected output (reaches `~ #` BusyBox shell in ~200M cycles):

```
...
[    0.000000] Linux version 6.1.0 ...
[    0.486000] Run /init as init process
RISCV-VP: Starting shell...
~ #
```

---

## Simulation Timing Tiers

| Model | Flag | Speed | Use Case |
|-------|------|-------|----------|
| LT (Loosely-Timed) | `-R 64` (default) | ~100 MIPS | Fast functional, software debug |
| AT (Approximately-Timed) | `--model at` | ~50 MIPS | Bus/interconnect latency analysis |
| 2-Stage Cycle | `--model cycle2` | ~10 MIPS | Basic hardware timing |
| **6-Stage Cycle (Cycle6)** | `-R 64` + Cycle6 build | ~2–5 MIPS | **Full micro-arch simulation** |

---

## System Architecture

```
┌──────────────────────────────────────────────────────────┐
│                        VPTop                             │
│                                                          │
│  ┌─────────────────┐    ┌──────────┐    ┌────────────┐  │
│  │  CPURV64P6_Cycle│◄──►│ BusCtrl  │◄──►│  Memory    │  │
│  │  (6-stage CVA6) │    │          │    │  (512 MB)  │  │
│  └─────────────────┘    │          │◄──►│  CLINT     │  │
│   ▲  ▲  ▲               │          │    │  PLIC      │  │
│   │  │  │ IRQ lines      │          │◄──►│  UART      │  │
│  timer msip ext          └──────────┘    └────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### Physical Address Map

| Address | Peripheral |
|---------|-----------|
| `0x0200_0000` | CLINT (timer + MSIP) |
| `0x0C00_0000` | PLIC (external interrupts) |
| `0x1000_0000` | UART 16550 (serial console) |
| `0x8000_0000` | DRAM — 512 MB (OpenSBI + DTB + kernel + initrd) |
| `0x8000_1000` | HTIF tohost (riscv-tests pass/fail) |

### Boot Chain

```
fw_jump.bin  →  Linux 6.1.0  →  BusyBox initramfs  →  ~ #
 (OpenSBI)      (S-mode)          (U-mode)
```

---

## Cycle6 Pipeline — Microarchitecture Detail

### Pipeline Stages

```
PCGen ──► Fetch ──► Decode ──► Issue ──► EX/MEM ──► Commit
  1          2         3          4          5           6
```

| Stage | Responsibility |
|-------|---------------|
| PCGen | Next-PC: sequential / branch prediction / exception redirect |
| Fetch | Instruction memory read, I-cache lookup, ITLB translation |
| Decode | Instruction decode, immediate extraction, register index identification |
| Issue | Operand read + forwarding, scoreboard allocation, hazard check |
| EX/MEM | ALU, branch resolution, load/store (D-cache + store buffer), MUL/DIV/FPU |
| Commit | In-order retirement, architectural register writeback, store drain to memory |

### Hazard Handling

| Hazard | Mechanism |
|--------|-----------|
| RAW (data) | Scoreboard `rd_clobber` + same-cycle forwarding from completed entries |
| Load-use (D$ hit) | `load_hit_fu` defers result 1 cycle — correct 1-cycle stall penalty |
| Load-use (D$ miss) | `dcache_miss_fu` defers result for `dcache_miss_penalty` (default 10) cycles |
| Store-buffer partial overlap | Tri-state forward result: HIT / PARTIAL_OVERLAP / MISS; stalls on overlap |
| Structural (MUL/DIV/FPU) | Issue stalls new instruction while FU is occupied |
| Branch misprediction | Full pipeline flush + redirect from EX stage |
| CSR RAW | Serialisation stall in Issue until all older instructions commit |
| SYSTEM (ECALL/EBREAK) | Issue stalls until all multi-cycle FUs are idle |

### Branch Predictor

| Structure | Configuration | Algorithm |
|-----------|--------------|-----------|
| BTB | 128 entries, direct-mapped | Full-PC tag, stores predicted target |
| BHT | 256 entries | 2-bit saturating counter (gshare) |
| RAS | 4 entries | LIFO, call/return auto-detection |

Mispredict penalty: **4–5 cycles** (full flush from EX stage).  
Linux boot prediction accuracy: **63.7%** (kernel branches are difficult to predict).

### Memory Hierarchy

| Level | Size | Config | Miss Penalty |
|-------|------|--------|-------------|
| I-cache | 16 KB | 4-way, 64 B lines, LRU | 10 cycles |
| D-cache | 32 KB | 8-way, 64 B lines, LRU | 10 cycles |
| ITLB | 32 entries | Fully associative | 6 cycles (3-level PTW) |
| DTLB | 32 entries | Fully associative | 6 cycles (3-level PTW) |
| DRAM | 512 MB | Flat array | — (no L2 modelled) |

Cache sizes match the CVA6 cv64a6 default configuration.

### Functional Unit Latencies

| Unit | Latency | Instructions |
|------|---------|-------------|
| ALU | 1 cycle | All integer arithmetic/logic |
| Load (D$ hit) | 1 cycle + 1 stall | LB / LH / LW / LD |
| Load (D$ miss) | 10 cycles | LB / LH / LW / LD |
| Store | 0 (buffered) | SB / SH / SW / SD |
| MUL | 2 cycles | MUL / MULH / MULHU / MULHSU / MULW |
| DIV | 64 cycles | DIV / DIVU / REM / REMU + W-variants |
| FPU | 2–14 cycles | F / D extension operations |
| CSR | 1 cycle (at commit) | CSRRW / CSRRS / CSRRC / CSRRWI / … |

### Store Buffer

Split speculative/committed design (matching CVA6):

- **4 speculative slots** — allocated at Issue, visible to forwarding logic
- **4 committed slots** — drained to memory at Commit in-order
- **Forwarding**: newest-first scan with tri-state result (HIT / PARTIAL_OVERLAP / MISS)
- Partial overlap stalls the load until the overlapping store drains

### Privilege Architecture

| Feature | Status |
|---------|--------|
| M / S / U privilege modes | Implemented |
| mstatus / sstatus (UXL/SXL = 2 at reset) | Implemented |
| mie / mip / sie / sip | Implemented |
| MTVEC / STVEC vectored trap delivery | Implemented |
| MEPC / SEPC (correct value after MRET/SRET) | Implemented |
| ECALL / EBREAK / MRET / SRET / WFI | Implemented |
| PMP (4 regions) | Implemented |
| Hardware performance counters (mcycle, minstret) | Implemented |

### Virtual Memory — sv39

| Feature | Detail |
|---------|--------|
| Address translation mode | sv39 (39-bit VA, 3-level page table) |
| ITLB | 32-entry fully associative, global-bit aware |
| DTLB | 32-entry fully associative |
| Page table walker | Hardware 3-level walk, 2 cycles per PTE read |
| sfence.vma | Full TLB flush including global entries (rs1=x0, rs2=x0) |
| satp modes accepted | 0 (bare), 8 (sv39) — others silently ignored per spec |

---

## ISA Coverage

| Extension | Description | Status |
|-----------|-------------|--------|
| RV64I | Base integer | Complete |
| RV64M | Multiply / divide | Complete |
| RV64A | Atomics (LR/SC, AMOs) | Complete |
| RV64C | Compressed 16-bit instructions | Complete |
| RV64F | Single-precision floating point | Complete |
| RV64D | Double-precision floating point | Complete |
| Zicsr | CSR instructions | Complete |
| Zifencei | Instruction-fetch fence | Complete |

---

## Benchmark Results

All benchmarks run on the 6-stage Cycle6 model at 100 MHz (10 ns/cycle).  
Co-sim = CVA6 Verilated RTL (`cv64a6_imafdc_sv39`, `NrCommitPorts=2`, matchlib NoC + DRAM).

### VP — Bare-Metal Benchmarks

| Metric | long\_test3 | robust\_stress | cam\_bench |
|--------|------------|----------------|-----------|
| Instructions | 1,900,393 | 8,881,052 | 5,732 |
| Cycles | 2,138,000 | 10,859,000 | 6,000 |
| IPC | **0.889** | **0.818** | **0.955** |
| CPI | 1.13 | 1.22 | 1.05 |
| Stalls | 237,553 | 657,559 | 57 |
| Forwarded (RAW) | 475,096 | 2,631,560 | 1,994 |
| MUL busy cycles | 237,544 | 657,559 | 0 |
| DIV busy cycles | 0 | 0 | 0 |
| I$ miss rate | 0.00% | 0.00% | 0.07% |
| D$ miss rate | 0.00% | 0.00% | 2.02% |
| Load-use stalls | 712,633 | 0 | 388 |
| Branches | 237,544 | 1,316,431 | 1,458 |
| Mispredicts | 7 | 330,086 | 44 |
| Branch predict rate | **100.0%** | **74.9%** | **97.0%** |
| Dual commits | 237,544 | 0 | 403 |
| Dual commit rate | **11.1%** | **0.0%** | **6.7%** |
| IPS | 1.80 MIPS | 2.06 MIPS | 1.61 MIPS |

### Co-Sim — Bare-Metal Benchmarks (CVA6 RTL, 100 MHz)

> Co-sim exposes only `minstret` and simulation time — per-stage pipeline counters are not observable without RTL instrumentation.

| Metric | long\_test3 | robust\_stress | cam\_bench |
|--------|------------|----------------|-----------|
| Instructions (minstret − 15 bootrom) | 1,900,013 | 8,880,514 | 5,418 |
| Sim time | 41,004,280 ns | 192,644,530 ns | 71,830 ns |
| Cycles (sim time ÷ 10 ns) | 4,100,428 | 19,264,453 | 7,183 |
| IPC | **0.463** | **0.461** | **0.756** |
| CPI | 2.16 | 2.17 | 1.32 |

The co-sim IPC is approximately half the VP IPC for compute-bound benchmarks. The primary cause is real DRAM latency through the matchlib NoC interconnect: every D$ miss escalates to a multi-cycle bus transaction, whereas the VP uses a flat 10-cycle penalty. The co-sim also commits through the CVA6 RTL dual-commit path with realistic structural hazards.

### Dual Commit Behaviour

The VP commit stage implements a dual-commit loop (matching `NrCommitPorts=2` in CVA6) that can retire up to 2 instructions per cycle when both the current and next scoreboard entries are ready simultaneously.

| Benchmark | Dual commit cycles | Dual commit rate | Notes |
|-----------|-------------------|-----------------|-------|
| long\_test3 | 237,544 | **11.1%** | Independent arithmetic pairs fire frequently |
| robust\_stress | 0 | **0.0%** | Every loop iteration ends with a MUL stall — only one entry is ever ready at retirement |
| cam\_bench | 403 | **6.7%** | Short sequences of independent loads/stores pair up |

---

## Linux Boot Performance

Measured at the point the BusyBox shell prompt (`~ #`) appears — boot-only, not including idle shell time.

| Metric | Value |
|--------|-------|
| Instructions | 139,114,192 |
| Cycles | 190,270,446 |
| IPC | **0.731** |
| CPI | 1.37 |
| Wall time | ~92 s |
| Sim time | 1,902,705,170 ns |
| IPS | 1.52 MIPS |
| M-mode instrs (OpenSBI) | 4,397,916 |
| S-mode instrs (kernel) | 134,520,769 |
| U-mode instrs (init/BusyBox) | 195,507 |
| Stalls | 10,478,093 |
| Forwarded (RAW) | 34,305,614 |
| MUL busy cycles | 85,531 |
| DIV busy cycles | 1,229,701 |
| I$ accesses | 154,052,882 |
| I$ miss rate | 0.97% (1,496,845 misses) |
| D$ accesses | 26,707,090 |
| D$ miss rate | 2.30% (613,551 misses) |
| Load-use stalls | 26,100,905 |
| ITLB miss rate | 0.00% (1,543 misses) |
| DTLB miss rate | 0.01% (4,741 misses, 6,284 PTW walks) |
| Branches | 18,893,343 |
| Mispredicts | 6,850,941 |
| Branch predict rate | **63.7%** |
| Flushes | 6,850,941 |

Linux boot IPC (0.731) is lower than bare-metal benchmarks (0.818–0.955) because:
- Branch prediction drops to 63.7% — kernel control flow is highly data-dependent (scheduler, interrupt routing, page-table walks)
- I$ miss rate rises to 0.97% — the kernel touches many distinct code paths during boot
- D$ miss rate rises to 2.30% — kernel data structures are large and sparse
- TLB misses appear (absent in bare-metal M-mode) — MMU adds PTW stall cycles

---

## Comparison with Real CVA6

| Metric | This VP | Real CVA6 |
|--------|---------|-----------|
| IPC (Linux integer) | **0.731** | 0.6 – 0.8 (up to ~1.1 on superscalar-friendly sequential code) |
| CPI | **1.37** | 1.3 – 1.7 |
| Pipeline depth | 6 stages | 6 stages |
| Issue width | 1 (in-order) | 1 (in-order) |
| Commit ports / width | 2 (dual-commit loop, but average IPC capped at 1.0 by frontend) | 2 (can retire up to 2 instructions/cycle) |
| I-cache | 16 KB / 4-way / 64 B | 16 KB / 4-way / 64 B ✓ |
| D-cache | 32 KB / 8-way / 64 B | 32 KB / 8-way / 64 B ✓ |
| Load-use stall | 1 cycle ✓ | 1 cycle |
| MUL latency | 2 cycles ✓ | 2 cycles |
| Branch mispredict penalty | 4–5 cycles | 6 cycles |
| sv39 MMU | Yes ✓ | Yes |
| L2 cache | Not modelled | 256 KB unified (optional) |
| DRAM latency | Not modelled (all misses = 10 cyc) | ~100 cycles |

The VP is **within ~10% of real CVA6 IPC** for average Linux workloads. However, there are two primary timing and microarchitectural calibration gaps:
1. **Absence of L2 Cache**: Real hardware would show lower IPC on memory-bound workloads as D$ misses escalate to L2/DRAM rather than resolving with a flat 10-cycle penalty.
2. **Single-Issue Frontend Bottleneck**: Although the commit stage implements a dual-commit loop (retiring up to 2 instructions/cycle to clear backlogs when out-of-order execution finishes behind a stalled instruction), the average IPC is mathematically capped at 1.0. This is because the frontend (Fetch, Decode, Issue) is strictly single-issue (1 instruction/cycle). In contrast, the real CVA6 has a decoupled frontend, instruction queue, and out-of-order execution pipelines that allow superscalar retirement and IPC > 1.0 (typically ~1.1) for sequential, warm-cache code.

---

## Known Limitations

| Limitation | Notes |
|------------|-------|
| No L2 cache | D$ misses are cheaper than real HW; IPC slightly optimistic on memory-bound workloads |
| Single-issue bottleneck | Average IPC is capped at 1.0. While the commit stage has 2 ports to clear out-of-order backlogs, the strictly single-issue frontend (Fetch/Decode/Issue) cannot feed the pipeline fast enough to match CVA6's 1.1+ IPC on sequential code. |
| DIV always 64 cycles | Real CVA6 terminates early for small operands (~20 cycles average) |
| Branch prediction accuracy 63.7% on Linux | Real CVA6 ~80–90% with larger BHT; our gshare with 256 entries under-predicts kernel indirect branches |
| RV32 6-stage model is not updated | Lacks cache model, CSR_File, MMU — suitable for bare-metal RV32 only |
| No write-combining | Stores drain one at a time; real HW coalesces adjacent stores |
| Single-core | No cache coherence protocol needed or modelled |

---

## Build Instructions

### Dependencies

- CMake ≥ 3.10
- C++17 compiler (GCC 9+, Clang 10+)
- SystemC 2.3.3+
- Boost headers

### Build

```bash
mkdir -p build_cycle6 && cd build_cycle6
cmake .. -DRISCV_RV64=ON -DRISCV_CYCLE6=ON
make -j$(nproc)
cd ..
```

### Rebuild after source changes

```bash
cd build_cycle6 && make -j$(nproc)
```

---

## Running

### Linux boot

```bash
./build_cycle6/RISCV_VP -R 64 \
  --bios  boot/fw_jump.bin \
  --dtb   boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz
```

### Linux boot with stats at a fixed instruction count

```bash
./build_cycle6/RISCV_VP -R 64 \
  --bios  boot/fw_jump.bin \
  --dtb   boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz \
  --max-instr 151070189
```

### Bare-metal hex program (RV64)

```bash
./build_cycle6/RISCV_VP -R 64 -f program.hex
```

### Bare-metal hex program (RV32)

```bash
./build_cycle6/RISCV_VP -R 32 -f program.hex
```

---

## Project Structure

```
RISCV-VP/
├── src/
│   ├── CPU_P64_6_Cycle.cpp   # 6-stage CVA6-aligned pipeline (~2500 lines)
│   ├── CPU_P32_6_Cycle.cpp   # 6-stage RV32 pipeline (bare-metal only)
│   ├── VPTop.cpp             # SoC top-level wiring
│   ├── VPMain.cpp            # Main entry point, CLI parsing
│   ├── BusCtrl.cpp           # TLM bus + address decode
│   ├── Memory.cpp            # 512 MB flat memory model
│   └── BASE_ISA.cpp          # Shared ISA execution handlers
├── inc/
│   ├── CPU_P64_6_Cycle.h     # Pipeline state, latches, stats structs
│   ├── Scoreboard.h          # CVA6-aligned scoreboard / ROB (32 entries)
│   ├── StoreBuffer.h         # Split speculative/committed store buffer
│   ├── Cache.h               # N-way set-associative LRU cache template
│   ├── MMU.h                 # sv39 ITLB + DTLB + page table walker
│   ├── CSR_File.h            # Full M/S/U CSR file + interrupt logic
│   ├── CLINT.h               # Core-local interrupt controller
│   ├── PLIC.h                # Platform-level interrupt controller
│   ├── UART.h                # 16550 UART peripheral
│   ├── TLB.h                 # TLB entry and flush logic
│   └── VPTop.h               # SoC composition
├── boot/
│   ├── fw_jump.bin           # OpenSBI M-mode firmware
│   ├── vp.dtb                # Device tree blob for this VP
│   ├── Image                 # Linux 6.1.0 kernel image
│   └── initramfs.cpio.gz     # BusyBox root filesystem
├── docs/
│   ├── Technical_Report.md   # Supervisor-facing technical summary
│   └── BUGS_AND_ISSUES.md    # Complete bug log (24 entries, all fixed)
└── dts/
    └── riscv_vp.dts          # Device tree source
```

---

## Bug History

26 bugs were identified and fixed during development to achieve Linux boot.
Full details with root cause, fix location, and discovery method in [`docs/BUGS_AND_ISSUES.md`](docs/BUGS_AND_ISSUES.md).

| Category | Bugs | Worst Symptom Before Fix |
|----------|------|--------------------------|
| Reset / Initialisation | 4 | Timer interrupt storm from cycle 0 |
| ISA correctness | 5 | Illegal instruction on common RV64 shift ops |
| Store buffer | 2 | Linux SLUB `BUG_ON` at 50M instructions |
| MMU / TLB | 2 | Instruction page faults after `free_initmem()` |
| Privilege & interrupts | 4 | WFI freeze at 131M instructions; hang in early delay loops |
| Peripherals | 2 | UART ISR loop consuming 685M+ instructions |
| Pipeline microarchitecture | 3 | IPC ~0.75 without branch predictor |
| Simulation infrastructure | 4 | WFI not yielding simulation time; 32-bit `tohost` truncation; no load-use stall; setsid not found shell panic |

---

## Acknowledgements

Built on top of the [RISC-V TLM Simulator](https://github.com/mariusmm/RISC-V-TLM)
originally developed by **Màrius Montón**. The TLM-2.0 infrastructure, memory model,
and base ISA handlers originate from that work.

Extensions added in this project:
- Complete CVA6-aligned 6-stage cycle-accurate pipeline
- sv39 MMU with ITLB, DTLB, and hardware page table walker
- Full M/S/U privilege architecture and CSR file
- L1 I/D cache model with LRU replacement
- Load-use hazard detection (1-cycle stall)
- CLINT, PLIC, UART peripherals wired for Linux
- Store buffer with partial-overlap stall
- Branch predictor (BTB + BHT + RAS)
- Linux 6.1.0 boot support

## License

GNU General Public License v3.0 — see `LICENSE` for details.
