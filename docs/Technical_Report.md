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
PCGen → Fetch →[instr queue]→ Decode → Issue → EX/MEM → Commit
  1        2        (4 deep)      3        4        5         6
```

The frontend is **decoupled** from the backend by a 4-entry instruction queue
(`fetch_queue`, matching CVA6's instruction-queue depth). Fetch continues to run
ahead while the backend stalls; PCGen and Fetch stall only when the queue fills.

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
| Shift → arithmetic RAW | 1-cycle bubble in Issue (see 3.2.1) |
| Branch misprediction | Full pipeline flush + redirect from EX stage |
| CSR RAW | Serialization stall in Issue |

#### 3.2.1 Back-to-back shift→adder dependency bubble

CVA6 cannot forward the result of a **shift** (`SLL/SRL/SRA` and their `I`/`W`
variants) to the immediately following consumer without a one-cycle bubble, unless
that consumer is a fast logical op (`AND/OR/XOR`). The VP reproduces this in
`Issue_stage`: if the instruction currently in EX is a shift and the instruction in
Issue reads its `rd` as an integer source, Issue stalls one cycle.

This is worth **~0.19 cycles per instruction** on dense unrolled ALU code and was
the single largest remaining integer-timing error. It was isolated with a
discriminating experiment: two branch-free, identically-sized unrolled kernels, one
with back-to-back dependencies (`int_nobranch`) and one with the same ops but
dependencies spaced 8 apart (`int_indep`). CVA6's CPI fell from **1.200 to 1.002**
between them — proving a dependency bubble rather than a fetch-bandwidth limit, and
pinning the cost at exactly **1.00 cycle per dependent pair**. See `BUGS.md` §B4.

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
| I$ | 16 KB | 4-way LRU | 64 B | 107 cycles |
| D$ | 32 KB | 8-way LRU | 64 B | 107 cycles |
| ITLB | 32 entries | Fully associative | — | 6 cycles (3-level PTW) |
| DTLB | 32 entries | Fully associative | — | 6 cycles (3-level PTW) |
| DRAM | 512 MB | Flat array | — | (no L2 modelled) |

Sizes match CVA6 cv64a6 default configuration.

### 3.5 Multi-Cycle Functional Units

| Unit | Latency | Instructions |
|------|---------|-------------|
| ALU | 1 cycle | All integer ops |
| Load (D$ hit) | 1 cycle + 1 stall | LB/LH/LW/LD |
| Load (D$ miss) | 107 cycles | LB/LH/LW/LD |
| MUL | 2 cycles | MUL/MULH/MULHU/MULHSU/MULW |
| DIV | Operand-dependent, ~2–66 cyc (64-bit) / ~2–34 cyc (32-bit) | DIV/DIVU/REM/REMU + W-variants; early-terminating serial divider |
| FPU add/mul/cvt | 2–5 cycles, **pipelined** (throughput 1/cycle) | FADD/FSUB/FMUL/FMADD/FCVT/FSGNJ/FCMP |
| FPU div/sqrt | ~10–22 cycles, **non-pipelined** (blocking) | FDIV/FSQRT |
| CSR | 1 cycle (at commit) | CSRRW/CSRRS/CSRRC |
| Taken branch | +1 fetch bubble | correctly-predicted taken branch/jump (BTB redirect) |

#### Divider — operand-value-dependent early termination

CVA6 uses a bit-serial divider that terminates early: its latency scales with the
magnitude of the operands, not just the instruction width. The VP models this by
deriving the cycle count from the leading-zero counts (LZC) of the *magnitudes* of
the dividend and divisor:

```
div_shift = LZC(divisor) − LZC(dividend)      // ≈ number of significant quotient bits
cycles    = div_shift + 6                      // + fixed serdiv setup/finish overhead
```

with 1-cycle fast paths for divide-by-zero, divide-by-(−1), and zero-quotient
(`divisor > dividend`) cases, and clamping to the 66/34-cycle worst case. Small
operands finish early exactly as on hardware. On a register-only 64-bit div/rem
stress benchmark this tracks the CVA6 RTL co-simulation to **within 0.6%**
(47 vs ~47 cycles/divide average), versus **+31%** for the previous flat 66-cycle
model.

#### Taken-branch redirect bubble

A correctly-predicted **taken** branch or jump costs one fetch cycle on CVA6: the
frontend needs a cycle to steer fetch to the BTB-predicted target. PCGen models
this with a one-cycle bubble injected after every predicted-taken control transfer
(`taken_redirect_bubble`, cleared on flush so it never double-counts a
misprediction). Not-taken branches are free. This closed the integer-ALU gap on a
tight register-only loop from **+9%** (VP IPC 1.000 vs CVA6 0.917) to **−0.1%**,
and — being a *frontend* cost — is correctly hidden behind backend stalls on
load-/divide-/FP-bound code (no effect there).

#### Pipelined FPU (FPnew-aligned)

CVA6's FPnew has a **pipelined** ADDMUL block (throughput 1 op/cycle, latency
2–5) and a separate **iterative, non-pipelined** DIVSQRT block. The VP mirrors
this:

- **Add/mul/convert** (`fadd`, `fsub`, `fmul`, `fmadd`, `fcvt`, `fsgnj`, `fcmp`)
  execute in an **8-slot pipelined unit** (`fpu_pipe[]`): a new FP op issues every
  cycle; only *dependent* consumers stall (via the scoreboard). Double-precision
  add/mul latency is **4** cycles.
- **Divide/square-root** (`fdiv`, `fsqrt`) run in a **blocking** unit
  (`fpu_divsqrt_fu`): iterative, one at a time, ~10–22 cycles.

This replaced the previous single blocking FPU (which serialised *all* FP ops).
On register-only FP benchmarks it tracks CVA6 to **+3.6%** (add/mul, `fp_bench`
IPC 0.518 vs 0.500) and **+4.8%** (add/mul/div/sqrt, `fp_stress` 0.152 vs 0.145),
with bit-exact numerical results — versus **−21%** for the old blocking model.
(Pipelining div/sqrt as well is *wrong* — it over-speeds `fp_stress` to 0.250;
the ADDMUL/DIVSQRT split is essential.)

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
| Commit ports / width | **2 (dual-commit loop, but average IPC capped at 1.0 by frontend)** | 2 (can retire up to 2 instructions/cycle) |
| Branch mispredict penalty | 4–5 cycles | 6 cycles |
| I$ size / associativity | 16 KB / 4-way | 16 KB / 4-way ✓ |
| D$ size / associativity | 32 KB / 8-way | 32 KB / 8-way ✓ |
| Load-use stall | 1 cycle ✓ | 1 cycle |
| MUL latency | 2 cycles ✓ | 2 cycles |
| sv39 MMU | Yes ✓ | Yes |
| L2 cache | **Not modelled** | 256 KB (optional) |
| DRAM latency | Flat 107-cycle miss penalty | ~100 cycles |

> **Note on the co-sim's memory.** The Verilated CVA6 harness has **no main-memory
> timing model**: the instance named `dram` (`cva6_matchlib_system.h:41`) is a
> matchlib testbench `SlaveFromFile`, backed by a `std::map` with zero wait states.
> It is neither DRAM nor a real SRAM macro — it is an idealized zero-latency stub.
> (The `tc_sram_wrapper` modules are CVA6's internal L1 cache tag/data arrays, not
> memory.) All VP-vs-CVA6 agreement on memory-touching benchmarks is therefore
> agreement **with the harness**, not a prediction of silicon behaviour.

**Per-unit validation (register-only microbenchmarks, both bit-exact vs CVA6 — see `BUGS.md`):**

| Unit | Benchmark | CVA6 IPC | VP IPC | Error |
|------|-----------|---------:|-------:|------:|
| Integer ALU (looped) | int_alu | 0.917 | 0.916 | −0.0% |
| Integer ALU (dependency chain, branch-free) | int_nobranch | 0.833 | 0.831 | −0.2% |
| Integer ALU (independent ops, branch-free) | int_indep | 0.998 | 0.996 | −0.2% |
| Integer divide | div_stress | 0.165 | 0.166 | +0.3% |
| FP add/mul | fp_bench | 0.500 | 0.518 | +3.6% |
| FP add/mul/div/sqrt | fp_stress | 0.145 | 0.153 | +5.1% |
| Load/store + MUL | long_test3 | 0.727 | 0.727 | −0.0% |
| Branch mispredict recovery | robust_stress | 0.685 | 0.704 | **+2.8%** |

After calibrating the divider (operand-value early termination), the taken-branch
bubble, the pipelined FPU, the shift→adder dependency bubble, and the memory
penalties, **five of the eight benchmarks agree with the RTL to within 0.3%**, and
the worst case is +5.1%. Two gaps remain:

1. **Memory latency is calibrated to the co-sim, not to silicon.**
   `load_hit_penalty = 1` and `store_write_penalty = 0` reproduce the co-sim
   exactly (long_test3 matches to 3 decimal places) because the co-sim's memory is
   a zero-wait-state `SlaveFromFile`. They are **optimistic for a real SoC** with
   DRAM behind AXI, and must be re-derived if a timed memory model is introduced.
   See `BUGS.md` §B5.
2. **Branch-mispredict flush penalty is one cycle too cheap.** On robust_stress the
   VP runs *faster* than the RTL (0.704 vs 0.685, **+2.8%**). The HPM counters
   isolate the cause: mispredict **counts** agree (VP 125,500 vs CVA6 125,498), so
   the predictor's accuracy is right, while the 125,221-cycle shortfall divided by
   125,498 mispredicts gives **0.998 cycles per mispredict** — exactly one missing
   bubble on the flush path. This matches the table above independently: the VP's
   mispredict penalty is 4–5 cycles where CVA6's is 6. Fix is the same shape as the
   taken-branch bubble (§3.5). See `BUGS.md` §B6. **Not yet implemented.**

   *(An earlier revision of this report blamed a "lockstep frontend". That was wrong
   twice over — the VP does model CVA6's 4-entry instruction queue (§3.1), and a
   missing queue would make the VP pessimistic, not optimistic. Retracted.)*

---

## 8. Known Limitations

| Limitation | Impact |
|------------|--------|
| No L2 cache | D$ misses cheaper than real HW; IPC slightly optimistic on memory-bound code |
| Single-issue backend | Average IPC is capped at 1.0. The commit stage has 2 ports to clear backlogs, and the frontend is decoupled via a 4-entry instruction queue (§3.1), but Issue retires at most one instruction per cycle. |
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
