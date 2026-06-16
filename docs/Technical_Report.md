# RISC-V Virtual Platform — Technical Report

**Project:** CVA6-Aligned RISC-V 64-bit Virtual Prototype  
**Platform:** SystemC 2.3.3 / TLM-2.0, C++17, WSL2 / Linux  
**Status:** Linux 6.1.0 boots to BusyBox user-space shell  

---

## 1. Overview

This project is a **cycle-accurate virtual platform (VP)** for the RISC-V 64-bit ISA, implemented in SystemC/TLM-2.0. It models the microarchitecture of the **CVA6 (Ariane)** open-source application-class processor. The primary purpose is pre-silicon software validation: the VP can execute a complete Linux kernel boot sequence and provide detailed pipeline performance statistics.

The VP supports four timing tiers:

| Model | Speed | Use |
|-------|-------|-----|
| LT (Loosely-Timed) | ~100+ MIPS | Fast functional, OS bring-up |
| AT (Approximately-Timed) | ~50 MIPS | Bus/interconnect analysis |
| 2-Stage Cycle | ~10 MIPS | Basic hardware timing |
| **6-Stage Cycle (Cycle6)** | ~2–5 MIPS | **Full micro-arch simulation** |

The **Cycle6** model is the primary deliverable and the subject of this report.

---

## 2. System Architecture

### 2.1 SoC Composition

```
┌─────────────────────────────────────────────────────┐
│                     VPTop (SystemC)                  │
│                                                      │
│  ┌──────────────┐    ┌─────────┐    ┌────────────┐  │
│  │  CPURV64P6   │◄──►│ BusCtrl │◄──►│   Memory   │  │
│  │  (Cycle6)    │    │         │    │  (512 MB)  │  │
│  └──────────────┘    │         │    └────────────┘  │
│                      │         │◄──►│    CLINT   │  │
│  IRQ lines:          │         │    │  (timer)   │  │
│  timer_irq ──────────►         │◄──►│    PLIC    │  │
│  msip_irq ───────────►         │    │(ext. irq)  │  │
│  ext_irq ────────────►         │◄──►│    UART    │  │
│                      └─────────┘    │  (16550)   │  │
└─────────────────────────────────────────────────────┘
```

### 2.2 Physical Address Map

| Region | Address | Component |
|--------|---------|-----------|
| CLINT | `0x0200_0000` | Timer, MSIP |
| PLIC | `0x0C00_0000` | External interrupts |
| UART | `0x1000_0000` | Serial console (16550) |
| DRAM | `0x8000_0000` | 512 MB flat memory |
| HTIF tohost | `0x8000_1000` | riscv-tests pass/fail |

### 2.3 Boot Chain

```
fw_jump.bin (OpenSBI M-mode)
    └─► Linux 6.1.0 kernel (S-mode)
            └─► BusyBox initramfs (U-mode)
                    └─► ~ #  (shell)
```

---

## 3. Cycle6 Pipeline — Microarchitecture

### 3.1 Pipeline Stages

```
PCGen → Fetch → Decode → Issue → EX/MEM → Commit
  1        2       3       4        5         6
```

| Stage | Function |
|-------|----------|
| **PCGen** | Next-PC selection: sequential, branch prediction, or redirect |
| **Fetch** | Instruction memory access, I$ lookup, ITLB translation |
| **Decode** | Instruction decode, immediate extraction, register index read |
| **Issue** | Operand read + forwarding, scoreboard allocation, hazard detection |
| **EX/MEM** | ALU, load/store (D$, store buffer), branch resolution, FPU, MUL/DIV |
| **Commit** | In-order retirement, register writeback, store drain |

### 3.2 Hazard Handling

| Hazard Type | Mechanism |
|-------------|-----------|
| RAW (register) | Scoreboard `rd_clobber` + forwarding from completed entries |
| Load-use (D$ hit) | `load_hit_fu` defers completion 1 cycle — correct 1-cycle stall |
| Load-use (D$ miss) | `dcache_miss_fu` defers for `dcache_miss_penalty` cycles |
| Store-buffer partial overlap | Tri-state `FwdResult` (HIT/PARTIAL_OVERLAP/MISS); stall on overlap |
| Structural (MUL/DIV/FPU) | Issue stalls when FU is occupied |
| Branch misprediction | Full pipeline flush + redirect from EX stage |
| CSR RAW | Serialization stall in Issue |

### 3.3 Branch Prediction

| Structure | Size | Algorithm |
|-----------|------|-----------|
| BTB | 128 entries | Direct-mapped, full-PC tag |
| BHT | 256 entries | 2-bit saturating counter (gshare) |
| RAS | 4 entries | LIFO, call/return detection |

Mispredict penalty: **4–5 cycles** (full flush from EX stage).

### 3.4 Memory Hierarchy

| Level | Size | Associativity | Line | Miss Penalty |
|-------|------|--------------|------|-------------|
| I$ | 16 KB | 4-way LRU | 64 B | 10 cycles |
| D$ | 32 KB | 8-way LRU | 64 B | 10 cycles |
| ITLB | 32 entries | Fully associative | — | 6 cycles (3-level PTW) |
| DTLB | 32 entries | Fully associative | — | 6 cycles (3-level PTW) |
| DRAM | 512 MB | Flat array | — | (no L2 modelled) |

Sizes match CVA6 cv64a6 default configuration.

### 3.5 Multi-Cycle Functional Units

| Unit | Latency | Instructions |
|------|---------|-------------|
| ALU | 1 cycle | All integer ops |
| Load (D$ hit) | 1 cycle + 1 stall | LB/LH/LW/LD |
| Load (D$ miss) | 10 cycles | LB/LH/LW/LD |
| MUL | 2 cycles | MUL/MULH/MULHU/MULHSU/MULW |
| DIV | 64 cycles | DIV/DIVU/REM/REMU + W-variants |
| FPU | 2–14 cycles | F/D extension ops |
| CSR | 1 cycle (at commit) | CSRRW/CSRRS/CSRRC |

### 3.6 Store Buffer

Split design matching CVA6:
- **4 speculative slots** — filled at Issue, visible to forwarding
- **4 committed slots** — drained to memory at Commit
- Forwarding: newest-first scan, tri-state result (HIT / PARTIAL_OVERLAP / MISS)

### 3.7 Privilege Architecture

| Feature | Implemented |
|---------|-------------|
| M / S / U modes | Yes |
| mstatus / sstatus / mie / mip | Yes |
| MTVEC / STVEC vectoring | Yes |
| MEPC / SEPC | Yes (with MRET/SRET sepc fix) |
| ECALL / EBREAK | Yes |
| MRET / SRET | Yes |
| PMP (4 regions) | Yes |
| mstatus.UXL / SXL = 2 (RV64) | Yes (reset-value fix applied) |

### 3.8 Virtual Memory (sv39)

| Feature | Detail |
|---------|--------|
| Mode | sv39 (3-level page table, 39-bit VA) |
| ITLB | 32-entry fully-associative, global-bit aware |
| DTLB | 32-entry fully-associative |
| PTW | Hardware 3-level walk, 2 cycles/PTE access |
| sfence.vma | Full flush (including global entries) |
| satp modes accepted | 0 (bare), 8 (sv39) |

---

## 4. ISA Coverage

| Extension | Status |
|-----------|--------|
| RV64I (base integer) | Complete |
| RV64M (multiply/divide) | Complete |
| RV64A (atomics, LR/SC + AMOs) | Complete |
| RV64C (compressed 16-bit) | Complete |
| RV64F (single-precision FP) | Complete |
| RV64D (double-precision FP) | Complete |
| Zicsr (CSR instructions) | Complete |
| Zifencei (instruction fence) | Complete |

---

## 5. Bug Fixes Required to Boot Linux

24 bugs were identified and fixed during development. Key categories:

| Category | Bugs Fixed | Worst Symptom |
|----------|-----------|---------------|
| Reset / Initialisation | 4 | Timer storm from cycle 0; SP out-of-bounds |
| ISA correctness | 5 | Illegal instruction traps on common RV64 shift ops |
| Store buffer | 2 | Linux SLUB `BUG_ON` at 50M instructions |
| MMU / TLB | 2 | Instruction page faults after `free_initmem()` |
| Privilege & interrupts | 3 | WFI freeze at 131M instructions; wrong SRET address |
| Peripherals | 2 | UART ISR loop consuming 685M+ instructions |
| Pipeline microarch | 3 | IPC ~0.75 before branch predictor |
| Simulation infra | 2 | WFI not yielding; 32-bit `tohost` truncation |

Full details in `docs/BUGS_AND_ISSUES.md`.

---

## 6. Linux Boot Results

**Kernel:** Linux 6.1.0  
**User space:** BusyBox 1.35 initramfs  
**Result:** Boots to interactive `~ #` shell

```
  Architecture: RV64IMAC (CVA6 6-Stage, M+S+U, sv39 MMU)
  Final Priv:   S-mode
  Cycles:       203,316,691
  Instructions: 151,070,193
  CPI:          1.35
  IPC:          0.743
  Stalls:       9,652,020
  Forwarded:    47,294,203    (RAW resolved by forwarding)
  I$ miss rate: 0.92%         (1,537,144 misses, 13,882,194 stall cycles)
  D$ miss rate: 1.43%         (422,993 misses, 3,857,040 stall cycles)
  Load-use stalls: 29,238,255 (1-cycle penalty per D$-hit/fwd load)
  ITLB miss rt: 0.00%         (1,893 misses, 8,484 stall cycles)
  DTLB miss rt: 0.01%         (5,803 misses, 21,476 stall cycles)
  Branches:     ~29M
  Predict Rate: 63.8%
```

**Run command:**
```bash
./build_cycle6/RISCV_VP -R 64 \
  --bios boot/fw_jump.bin \
  --dtb  boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz
```

---

## 7. Comparison with Real CVA6

| Metric | This VP | Real CVA6 (published) |
|--------|---------|-----------------------|
| IPC (integer workloads) | **0.743** | 0.6 – 0.8 (up to ~1.1 on superscalar-friendly sequential code) |
| CPI | **1.35** | 1.3 – 1.7 |
| Commit ports / width | **1 (single-commit, max IPC 1.0)** | 2 (can retire up to 2 instructions/cycle) |
| Branch mispredict penalty | 4–5 cycles | 6 cycles |
| I$ size / associativity | 16 KB / 4-way | 16 KB / 4-way ✓ |
| D$ size / associativity | 32 KB / 8-way | 32 KB / 8-way ✓ |
| Load-use stall | 1 cycle ✓ | 1 cycle |
| MUL latency | 2 cycles ✓ | 2 cycles |
| sv39 MMU | Yes ✓ | Yes |
| L2 cache | **Not modelled** | 256 KB (optional) |
| DRAM latency | **Not modelled** | ~100 cycles |

The VP produces IPC within **~10% of real CVA6** for average Linux integer workloads. The two primary timing and microarchitectural calibration gaps are:
1. **Absence of L2 Cache**: All D$ misses are handled at a flat 10-cycle penalty rather than hitting a real L2/DRAM hierarchy, which would lower real-hardware IPC on memory-bound workloads.
2. **Single-commit Cap**: The real CVA6 features a dual-commit ROB (can retire 2 instructions per cycle). The VP's 6-stage model executes and commits at most 1 instruction per cycle, causing it to underestimate absolute throughput by 15-20% on highly sequential code where real CVA6 achieves an IPC of 1.1+.

---

## 8. Known Limitations

| Limitation | Impact |
|------------|--------|
| No L2 cache | D$ misses cheaper than real HW; IPC slightly optimistic on memory-bound code |
| Single-commit cap | VP commits at most 1 instruction/cycle, missing CVA6's 2-commit throughput advantage (IPC hard-capped at 1.0) |
| DIV always 64 cycles | Real CVA6 early-terminates for small operands (~20 cycles avg) |
| Branch prediction accuracy 63.8% | Real CVA6 ~80–90% on Linux with larger BHT |
| RV32 model not updated | RV32 6-stage lacks cache model, CSR_File, MMU — bare-metal only |
| No write-combining buffer | Stores drain one at a time; real HW coalesces |
| Single-core only | No cache coherence protocol needed |

---

## 9. Build Instructions

**Dependencies:** CMake ≥ 3.10, C++17, SystemC 2.3.3+, Boost headers

```bash
# Configure (first time)
mkdir build_cycle6 && cd build_cycle6
cmake .. -DRISCV_RV64=ON -DRISCV_CYCLE6=ON
cd ..

# Build
touch src/CPU_P64_6_Cycle.cpp inc/CPU_P64_6_Cycle.h
cd build_cycle6 && make -j$(nproc)
```

---

## 10. Key Files

| File | Purpose |
|------|---------|
| `src/CPU_P64_6_Cycle.cpp` | Full 6-stage pipeline implementation (~2500 lines) |
| `inc/CPU_P64_6_Cycle.h` | Pipeline state, latches, stats, FU structs |
| `inc/Scoreboard.h` | CVA6-aligned scoreboard / ROB |
| `inc/StoreBuffer.h` | Split speculative/committed store buffer |
| `inc/Cache.h` | N-way set-associative LRU cache template |
| `inc/MMU.h` | sv39 ITLB + DTLB + page table walker |
| `inc/CSR_File.h` | Full M/S/U CSR file with interrupt logic |
| `inc/CLINT.h` | Core-local interrupt controller |
| `inc/PLIC.h` | Platform-level interrupt controller |
| `inc/VPTop.h` / `src/VPTop.cpp` | SoC top-level wiring |
| `docs/BUGS_AND_ISSUES.md` | Complete bug log (24 entries) |
