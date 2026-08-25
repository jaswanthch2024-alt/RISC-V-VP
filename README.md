# RISC-V Virtual Platform (RISCV-VP)

A cycle-accurate, CVA6-aligned RISC-V 64-bit virtual platform implemented in SystemC/TLM-2.0.
Boots **Linux 6.1.0** to a BusyBox user-space shell on the 6-stage pipeline model, and includes
an optional real-AXI memory-contention model validated against CVA6 RTL co-simulation.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![SystemC](https://img.shields.io/badge/SystemC-3.0.2-green.svg)](https://www.accellera.org/downloads/standards/systemc)

---

## What This Is

RISCV-VP is a pre-silicon software validation platform. It models the microarchitecture of the
**CVA6 (Ariane)** open-source application-class RISC-V processor at the cycle level, allowing
software engineers to run real workloads (including a full Linux kernel) and obtain detailed
pipeline performance statistics before silicon is available. Every timing constant in the
pipeline — cache geometry, replacement policy, predictor sizes, miss latencies — is calibrated
against direct CVA6 RTL co-simulation, not assumed; see [`docs/BUGS.md`](docs/BUGS.md) for the
full calibration history.

**SystemC is fully vendored in this repo** (`systemc/`, ~47 MB) — a fresh clone needs nothing
external beyond a C++17 compiler and CMake to build the default model.

---

## Quick Start — Build & Test on Your Own Desktop

```bash
git clone <this-repo-url> riscv-vp
cd riscv-vp

# Build the default cycle-accurate model
mkdir -p build_cycle6 && cd build_cycle6
cmake .. -DTIMING_MODEL=CYCLE6 -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# Run a bare-metal test binary (fast, seconds)
export LD_LIBRARY_PATH=build_cycle6:build_cycle6/systemc/src:build_cycle6/spdlog
./build_cycle6/RISCV_VP -R 64 -f tests/hex/robust_fast64.hex --max-instr 3000000

# Boot Linux to a shell (slower, ~3-4 minutes wall-clock on a modern desktop)
./build_cycle6/RISCV_VP -R 64 \
  --bios  boot/fw_jump.bin \
  --dtb   boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz
```

Expected microbenchmark output ends with a `printStats()` block (`Cycles:`, `IPC:`, cache miss
rates, etc.). Expected Linux boot output reaches:

```
...
[    0.000000] Linux version 6.1.0 ...
Run /init as init process
RISCV-VP: Starting interactive shell...
~ #
```

### Dependencies

| Requirement | Notes |
|---|---|
| CMake ≥ 3.10 | |
| C++17 compiler | GCC 9+ or Clang 10+ tested; this project was verified with GCC 13 |
| Nothing else for the default build | SystemC and spdlog are vendored in-repo |

If a system-installed SystemC is found by `find_package(SystemC)` it is preferred; otherwise
CMake automatically falls back to the vendored `systemc/` subdirectory
(`-DUSE_LOCAL_SYSTEMC=ON`, the default) and builds it as part of the same `make` invocation —
no separate SystemC install step is required.

### Troubleshooting: `No CMAKE_C_COMPILER could be found`

On RHEL/AlmaLinux/CentOS-family systems (including the `almalinux`-based dev containers this
project has been built and tested in), there is often **no default `gcc`/`g++` on `PATH`** —
only an opt-in GCC Toolset (e.g. `gcc-toolset-13`) that needs activating first:

```bash
export PATH=/opt/rh/gcc-toolset-13/root/usr/bin:$PATH   # adjust version to what's installed
cmake .. -DTIMING_MODEL=CYCLE6 -DCMAKE_BUILD_TYPE=Release
```

(Verified: a genuinely fresh clone in this environment fails with "No CMAKE_C_COMPILER could be
found" until this `PATH` export is done — Ubuntu/Debian/Fedora desktops with a default system
`gcc` package installed do not need this step.)

---

## Simulation Timing Tiers

| Model | `TIMING_MODEL` | Speed | Use Case |
|-------|------|-------|----------|
| LT (Loosely-Timed) | `LT` (default) | ~100 MIPS | Fast functional, software debug |
| AT (Approximately-Timed) | `AT` | ~50 MIPS | Bus/interconnect latency analysis |
| 2-Stage Cycle | `CYCLE` | ~10 MIPS | Basic hardware timing |
| **6-Stage Cycle (Cycle6)** | `CYCLE6` | ~2–5 MIPS | **Full micro-arch simulation — the model this README describes** |
| **6-Stage Cycle + real AXI contention** | `CYCLE6_AXI` | slower (Connections/matchlib) | Microbenchmark-only; models real I$/D$ AXI port contention, see below |

---

## System Architecture

```
┌──────────────────────────────────────────────────────────┐
│                          VPTop                           │
│                                                          │
│  ┌─────────────────┐     ┌──────────┐     ┌────────────┐ │
│  │ CPURV64P6_Cycle │<===>│  BusCtrl │<===>│ Memory     │ │
│  │ (6-stage CVA6)  │     │          │     │ (512 MB)   │ │
│  └─────────────────┘     │          │<===>│ CLINT      │ │
│   ^  ^  ^                │          │     │ PLIC       │ │
│   │  │  │ IRQ lines      │          │<===>│ UART       │ │
│  timer msip ext          └──────────┘     └────────────┘ │
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
| Fetch | Instruction memory read, I-cache lookup, ITLB translation, 4-entry decoupling FIFO |
| Decode | Instruction decode, immediate extraction, register index identification |
| Issue | Operand read + forwarding, scoreboard allocation, hazard check |
| EX/MEM | ALU, branch resolution, load/store (D-cache + store buffer), MUL/DIV/FPU |
| Commit | In-order retirement, architectural register writeback, store drain to memory |

### Hazard Handling

| Hazard | Mechanism |
|--------|-----------|
| RAW (data) | Scoreboard `rd_clobber` + same-cycle forwarding from completed entries |
| Load-use (D$ hit) | `load_hit_fu` defers result 1 cycle — correct 1-cycle stall penalty |
| Load-use (D$ miss) | `dcache_miss_fu` defers result for `dcache_miss_penalty` (10) cycles |
| Load→address dependency (gather / pointer-chase) | A load result feeding a subsequent load/store address costs an extra `load_addr_penalty` (2) cycles — matches CVA6's route-to-address-generation latency, verified to 0.006% on an isolated pointer-chase benchmark; see `docs/BUGS.md` §B9 |
| Store-buffer partial overlap | Tri-state forward result: HIT / PARTIAL_OVERLAP / MISS; stalls on overlap |
| Structural (MUL/DIV/FPU) | Issue stalls new instruction while FU is occupied |
| Branch misprediction | Full pipeline flush + redirect from EX stage |
| CSR RAW | Serialisation stall in Issue until all older instructions commit |
| SYSTEM (ECALL/EBREAK) | Issue stalls until all multi-cycle FUs are idle |

### Branch Predictor

RTL-matched configuration (`docs/BUGS.md`, commit that fixed the earlier oversized default):

| Structure | Configuration | Algorithm |
|-----------|--------------|-----------|
| BTB | **32 entries**, direct-mapped | Full-PC tag, stores predicted target |
| BHT | **128 entries** | 2-bit saturating counter (gshare) |
| RAS | **2 entries** | LIFO, call/return auto-detection |

Boots Linux with these RTL-matched structures (137,936,875 instr / 230,924,306 cycles / **IPC
0.597**, verified bit-exact against the paper-documented figure).

### Memory Hierarchy

| Level | Size | Config | Replacement | Miss Penalty |
|-------|------|--------|-------------|-------------|
| I-cache | 16 KB | 4-way, **16 B lines**, 256 sets | **LFSR pseudo-random** | **7 cycles** |
| D-cache | 32 KB | 8-way, **16 B lines**, 256 sets | **LFSR pseudo-random** | 10 cycles |
| ITLB | **16 entries** | Fully associative | — | 2 cycles/PTW level |
| DTLB | **16 entries** | Fully associative | — | 2 cycles/PTW level |
| DRAM | 512 MB | Flat array | — | — (no L2 modelled) |

Cache geometry, replacement policy, and I$ miss latency are all directly matched/re-derived
against CVA6 RTL — see `docs/BUGS.md` §B8 (LRU→LFSR replacement fix, was over-missing by ~22%)
and §B10 (I$ miss penalty re-derived from 10→7 cycles via an isolated single-cache benchmark,
matched to 0.046% error). **D$ miss penalty (10 cycles) has not yet been independently
re-derived the same way — a known open item, likely similarly over-stated.**

`Cache.h` currently implements LFSR only (LRU was fully removed when the policy was corrected,
not kept as a runtime option). A switchable LRU/LFSR flag for A/B testing was scoped but is
**not implemented**.

### Functional Unit Latencies

| Unit | Latency | Instructions |
|------|---------|-------------|
| ALU | 1 cycle | All integer arithmetic/logic |
| Load (D$ hit) | 1 cycle + 1 stall | LB / LH / LW / LD |
| Load (D$ miss) | 10 cycles | LB / LH / LW / LD |
| Load→address dependency | +2 cycles (on top of the above) | Only when the loaded value feeds a subsequent load/store address (gather patterns) |
| Store | 0 (buffered) | SB / SH / SW / SD — verified against RTL to be genuinely free (CVA6's write-through buffer absorbs stores), see `docs/BUGS.md` §B7 |
| MUL | 2 cycles | MUL / MULH / MULHU / MULHSU / MULW |
| DIV | Operand-dependent, ~1–70 cyc | DIV / DIVU / REM / REMU + W-variants; early-terminating serial divider (latency ∝ significant quotient bits, matches CVA6 to within ~0.6%) |
| FPU | 2–14 cycles | F / D extension operations |
| CSR | 1 cycle (at commit) | CSRRW / CSRRS / CSRRC / CSRRWI / … |

### Store Buffer

Split speculative/committed design (matching CVA6):

- **4 speculative slots** — allocated at Issue, visible to forwarding logic
- **4 committed slots** — drained to memory at Commit in-order
- **Forwarding**: newest-first scan with tri-state result (HIT / PARTIAL_OVERLAP / MISS)
- Partial overlap stalls the load until the overlapping store drains
- Drain cost is zero cycles, independently confirmed against RTL (a store-flood benchmark shows
  CVA6's own write buffer makes stores effectively free too — see `docs/BUGS.md` §B7)

### D$ Miss Buffering

The D-cache miss unit supports **2 concurrent outstanding misses** (matching CVA6's
`NrLoadBufEntries=2`) in the default `CYCLE6` build — a second independent load can have its
miss latency overlap the first instead of queuing behind it. This is disabled (falls back to 1
slot) under the `CYCLE6_AXI` build, where the matchlib arbiter models a single D$ AXI master by
design.

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
| ITLB | 16-entry fully associative, global-bit aware |
| DTLB | 16-entry fully associative |
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

## AXI Contention Model (`CYCLE6_AXI` / `CYCLE6_AXI128`) — optional, microbenchmark-only

A separate, opt-in build mode that models real AXI-level contention between the I$ and D$ AXI
manager ports using an actual **NVIDIA matchlib `AxiArbiter`** (not a hand-rolled stand-in) —
vendored from the public [Stuart-Swan Matchlib-Examples-Kit](https://github.com/Stuart-Swan/Matchlib-Examples-Kit-For-Accellera-Synthesis-WG)
into `third_party/matchlib_kit/` (13 MB, trimmed, license files retained), which builds against
this project's own SystemC **3.0.2** — no Catapult HLS license required.

### What it models

- I$ and D$ modelled as **two independent AXI masters** contending for one arbitrated memory
  port, matching CVA6's real single AXI manager port.
- **Genuine 2-beat AXI INCR bursts** per cache-line refill (64-bit AXI data width, 16-byte
  cache line ⇒ 2 beats) — re-derived directly from CVA6's own RTL formula
  (`AxiRdBlenIcache/Dcache = LINE_WIDTH/AxiDataWidth - 1 = 128/64 - 1 = 1`, i.e. 2 beats), not
  a single-beat approximation.
- The AXI channel carries the CPU's **real** instruction/data values (not a synthetic
  placeholder) — verified by an always-on runtime check comparing every AXI-channel value
  against the value fetched via the normal `mem_intf` path; this ran clean (zero mismatches)
  across microbenchmarks and a full Linux boot to the interactive shell, including thousands
  of DTLB-miss-combined page-table-walk cases. Note: this makes the *timing* channel
  functionally data-correct in simulation; it does not add an external AXI port anything
  outside the VP could bind to (see caveats below).
- The default `CYCLE6` build's core pipeline logic is **completely untouched** by this mode —
  verified bit-exact when `CYCLE6_AXI`/`CYCLE6_AXI128` is not selected.

### Runtime comparison switches (single build, env-var toggle — no rebuild needed)

These default to the RTL-accurate value; overriding them is for side-by-side
what-if experiments only, not an RTL-accuracy claim:

| Env var | Default | What it does |
|---|---|---|
| `VP_CACHE_POLICY=LRU` | LFSR (matches CVA6 silicon) | Switches both L1 caches to LRU replacement for comparison. LFSR wins on large sequential scans (`mem_contention_bench`-style); LRU wins on workloads with genuine temporal locality (`locality_bench`-style) — neither is universally better, see `docs/BUGS.md` §B8. |
| `VP_AXI_BEATS=N` | 2 (64-bit build) / 1 (128-bit build) | Overrides the INCR burst length per refill. Lower = fewer beats = cheaper misses, same miss *count* — isolates the cycle-cost of AXI bus width without a separate build. |
| `VP_DCACHE_SLOTS=N` (max 8) | 1 with AXI / 2 without | Number of D$ misses allowed outstanding at once (non-blocking D$ experiment, modelling what a wider MSHR like CVA6's HPDcache option would allow). Finding: raising this above the default had **no effect** on tested benchmarks — this pipeline's single-issue front end never generates more than ~2 concurrent outstanding misses regardless of slot capacity, so slot count isn't the bottleneck for non-blocking-cache-style concurrency here. |

### `CYCLE6_AXI128` — genuine 128-bit AXI data width (separate build, not a runtime flag)

Unlike the switches above, AXI data width is a **compile-time build variant**
(`-DTIMING_MODEL=CYCLE6_AXI128`), not an env var — matchlib's AXI types are C++ templates on the
data-width config, so a wider bus is a different compiled binary, not a flag on the existing one.
No CVA6 target ships a 128-bit AXI config (`AxiDataWidth=64` in every `cva6_config_pkg.sv`), so
this is a genuinely new, uncalibrated-against-RTL comparison point — useful for isolating "what
does a wider bus buy you," not for an RTL-accuracy claim.

| | 64-bit AXI (`CYCLE6_AXI`) | 128-bit AXI (`CYCLE6_AXI128`) |
|---|---:|---:|
| Beats per 16 B line refill | 2 | 1 |
| `mem_contention_bench` cycles | 4,266,611 | **3,836,657 (−10.1%)** |
| Miss count (unchanged either way) | 332,743 I$ / 166,923 D$ | 332,743 I$ / 166,923 D$ |
| Linux boot (to shell) | reaches shell, IPC 0.607, 0 data mismatches | reaches shell, IPC 0.617, 0 data mismatches |

Same miss count either way — the cycle reduction comes entirely from each existing miss
costing less to service (fewer AXI beats), not from catching more hits.

### What it deliberately does *not* model

- **Store-drain contention** — tested with a dedicated store-flood benchmark and found
  unnecessary: CVA6's write-through buffer makes stores effectively free (RTL shows ~0 D$
  misses under a store flood), matching the VP's existing free-store model. Adding contention
  here would move the VP *away* from RTL, not toward it.
- **DMA** — the co-sim reference this project validates against has no DMA peripheral and the
  CVA6 core itself has no DMA engine, so there is no RTL ground truth to validate a DMA AXI
  master against.
- **An external AXI port.** All of the above (arbiter, masters, slave) is instantiated and
  wired entirely *inside* `AxiContentionTop` — there is no `sc_port` exposed for anything
  outside the VP to bind to. Connecting this to a real external AXI subordinate/master (RTL,
  a verification IP, or another SystemC block) would need that port built first, and has never
  been tested against anything but the VP's own internal slave.

### Build & run

```bash
mkdir -p build_cycle6_axi && cd build_cycle6_axi
cmake .. -DTIMING_MODEL=CYCLE6_AXI -DCMAKE_BUILD_TYPE=Release   # or CYCLE6_AXI128 for 128-bit
make -j$(nproc)
cd ..

export LD_LIBRARY_PATH=build_cycle6_axi:build_cycle6_axi/systemc/src:build_cycle6_axi/spdlog
./build_cycle6_axi/RISCV_VP -R 64 -f tests/hex/robust_fast64.hex --max-instr 3000000
# comparison-mode examples (same binary):
VP_CACHE_POLICY=LRU ./build_cycle6_axi/RISCV_VP -R 64 -f tests/hex/robust_fast64.hex --max-instr 3000000
VP_AXI_BEATS=1 ./build_cycle6_axi/RISCV_VP -R 64 -f tests/hex/robust_fast64.hex --max-instr 3000000
VP_DCACHE_SLOTS=8 ./build_cycle6_axi/RISCV_VP -R 64 -f tests/hex/robust_fast64.hex --max-instr 3000000
```

The `-DTIMING_MODEL=CYCLE6_AXI`/`CYCLE6_AXI128` binaries run any bare-metal `.hex` the same way
as the default build. The specific stress test used to derive the results below was a dedicated
I$+D$ concurrent-thrash workload built during calibration; it is not included in this repo, but
any memory-heavy bare-metal program will exercise the same contention path.

### Verified results (I$+D$ concurrent-contention stress test)

| | CVA6 RTL | VP — No AXI | VP — With AXI (2-beat burst) |
|---|---:|---:|---:|
| Cycles | 4,334,798 | 4,111,121 | **4,266,611** |
| IPC | 0.378 | 0.399 | **0.384** |
| Error vs RTL | — | −5.2% | **−1.57%** |

Once both the underlying miss-latency calibration and the AXI subsystem's own separate
`axi_slave_latency` constant were correctly derived and kept in sync, real AXI contention
modelling with genuine burst semantics measurably improved accuracy over the no-AXI baseline.
Full investigation, including the earlier (now-resolved) finding that a naively-calibrated AXI
model made things *worse*, is documented in `docs/BUGS.md` §B7/§B10.

---

## Comparison with Real CVA6

| Metric | This VP | Real CVA6 |
|--------|---------|-----------|
| Pipeline depth | 6 stages | 6 stages |
| Issue width | 1 (in-order) | 1 (in-order) |
| I-cache | 16 KB / 4-way / 16 B / LFSR | 16 KB / 4-way / 16 B / LFSR ✓ |
| D-cache | 32 KB / 8-way / 16 B / LFSR | 32 KB / 8-way / 16 B / LFSR ✓ |
| BTB / BHT / RAS | 32 / 128 / 2 | 32 / 128 / 2 ✓ |
| ITLB / DTLB | 16 / 16 | 16 / 16 ✓ |
| Load-use stall | 1 cycle ✓ | 1 cycle |
| Load→address (gather) latency | +2 cycles ✓ | ~2 extra cycles, verified via isolated pointer-chase (0.006% error) |
| MUL latency | 2 cycles ✓ | 2 cycles |
| AXI data width (optional `CYCLE6_AXI` mode) | 64-bit, 2-beat bursts ✓ (128-bit `CYCLE6_AXI128` build also available, comparison-only) | 64-bit, 2-beat bursts (per-line refill) |
| sv39 MMU | Yes ✓ | Yes |
| L2 cache | Not modelled | 256 KB unified (optional) |
| Linux boot IPC (RTL-matched predictor/TLB config) | **0.597** | (paper-documented reference figure) |

### Known residual gaps (honestly stated, not yet closed)

1. **D$ miss latency not yet independently re-derived.** The same methodology that fixed the
   I$ penalty (isolate, sweep, compare to RTL) has not been applied to `dcache_miss_penalty`
   (still 10 cycles) — likely similarly over-stated.
2. **~5% cycle-timing residual on memory-bound code, no cache-accuracy component.** Cache miss
   *counts* now match RTL to <0.2%; the remaining gap is in pipeline *timing* around
   correctly-identified misses — the VP's frontend goes idle (`if_empty`) more often than
   RTL's even at matched miss latency, and the exact mechanism is not yet root-caused.
   Documented in `docs/BUGS.md`.
3. **No L2 cache.** D$ misses resolve directly to DRAM-equivalent latency rather than escalating
   through an L2, which real hardware with an L2 would show as a further IPC difference on
   memory-bound workloads.

#### Divider validation (vs CVA6 RTL co-simulation)

A register-only 64-bit `div`/`rem` stress benchmark, bit-exact result agreement:

| Divider model | Cycles | IPC | avg cyc/divide | Error vs CVA6 |
|---------------|-------:|----:|---------------:|--------------:|
| CVA6 RTL (reference) | 1,130,126 | 0.165 | ~47 | — |
| VP — flat 66-cycle (old) | 1,483,431 | 0.126 | 65 | +31% |
| **VP — operand-value early-termination (new)** | **1,123,900** | **0.166** | **47** | **−0.6%** |

---

## Known Limitations

| Limitation | Notes |
|------------|-------|
| D$ miss penalty not yet re-derived | Uses the same historical 10-cycle assumption the (now-fixed) I$ penalty used to; likely over-stated by a similar margin |
| ~5% pipeline-timing residual, memory-bound code | Not a cache-accuracy issue (misses match RTL to <0.2%); root cause not yet identified — see `docs/BUGS.md` |
| No L2 cache | D$ misses resolve directly to a flat penalty rather than escalating through an L2 |
| `CYCLE6_AXI` covers only I$/D$ read contention | Store-drain and DMA contention deliberately not modelled — both evaluated and found unnecessary or unvalidatable, see the AXI section above |
| RV32 6-stage model is not updated | Lacks cache model, CSR_File, MMU — suitable for bare-metal RV32 only |
| No write-combining | Stores drain one at a time; real HW coalesces adjacent stores |
| Single-core | No cache coherence protocol needed or modelled |

---

## Build Instructions

### Dependencies

- CMake ≥ 3.10
- C++17 compiler (GCC 9+, Clang 10+; verified with GCC 13)
- Nothing else — SystemC and spdlog are vendored; matchlib (for `CYCLE6_AXI` only) is vendored too

### Build — default cycle-accurate model

```bash
mkdir -p build_cycle6 && cd build_cycle6
cmake .. -DTIMING_MODEL=CYCLE6 -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..
```

### Build — AXI contention model (optional)

```bash
mkdir -p build_cycle6_axi && cd build_cycle6_axi
cmake .. -DTIMING_MODEL=CYCLE6_AXI -DCMAKE_BUILD_TYPE=Release   # or CYCLE6_AXI128 for 128-bit AXI
make -j$(nproc)
cd ..
```

### Rebuild after source changes

```bash
cd build_cycle6 && make -j$(nproc)      # or build_cycle6_axi
```

---

## Running

Set the library path once per shell before running any binary (adjust the build dir name for
whichever target you built):

```bash
export LD_LIBRARY_PATH=build_cycle6:build_cycle6/systemc/src:build_cycle6/spdlog
```

### Linux boot

```bash
./build_cycle6/RISCV_VP -R 64 \
  --bios  boot/fw_jump.bin \
  --dtb   boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz
```

### Linux boot with a snapshot of stats exactly at the shell prompt

```bash
VP_BOOT_STATS=1 ./build_cycle6/RISCV_VP -R 64 \
  --bios  boot/fw_jump.bin \
  --dtb   boot/vp.dtb \
  --kernel boot/Image \
  --initrd boot/initramfs.cpio.gz
# ... reaches "~ #" and keeps running (idle-loops forever) — Ctrl+C once the
# "Boot Pipeline Statistics" block has printed.
```

### Bare-metal hex program (RV64)

```bash
./build_cycle6/RISCV_VP -R 64 -f tests/hex/robust_fast64.hex --max-instr 3000000
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
│   ├── CPU_P64_6_Cycle.cpp   # 6-stage CVA6-aligned pipeline
│   ├── CPU_P32_6_Cycle.cpp   # 6-stage RV32 pipeline (bare-metal only)
│   ├── VPTop.cpp             # SoC top-level wiring
│   ├── VPMain.cpp            # Main entry point, CLI parsing
│   ├── BusCtrl.cpp           # TLM bus + address decode
│   ├── Memory.cpp            # 512 MB flat memory model
│   ├── BASE_ISA.cpp          # Shared ISA execution handlers
│   └── axi/
│       └── AxiContentionTop.cpp  # Real matchlib AxiArbiter integration (CYCLE6_AXI only)
├── inc/
│   ├── CPU_P64_6_Cycle.h     # Pipeline state, latches, stats structs
│   ├── Scoreboard.h          # CVA6-aligned scoreboard / ROB (32 entries)
│   ├── StoreBuffer.h         # Split speculative/committed store buffer
│   ├── Cache.h               # N-way set-associative cache, LFSR replacement (matches CVA6)
│   ├── MMU.h                 # sv39 ITLB + DTLB + page table walker
│   ├── CSR_File.h            # Full M/S/U CSR file + interrupt logic
│   ├── CLINT.h               # Core-local interrupt controller
│   ├── PLIC.h                # Platform-level interrupt controller
│   ├── UART.h                # 16550 UART peripheral
│   ├── TLB.h                 # TLB entry and flush logic
│   ├── VPTop.h                # SoC composition
│   └── axi/
│       ├── AxiContentionTop.h    # Matchlib-free interface (PIMPL) — the CPU never sees matchlib
│       ├── AxiRefillMaster.h     # Per-source (I$/D$) AXI manager, 2-beat burst reads
│       └── MinimalAxiMemSlave.h  # Array-backed AXI subordinate, configurable latency
├── third_party/
│   └── matchlib_kit/          # Vendored matchlib (Stuart-Swan Accellera kit), CYCLE6_AXI only
├── boot/
│   ├── fw_jump.bin           # OpenSBI M-mode firmware
│   ├── vp.dtb                # Device tree blob for this VP
│   ├── Image                 # Linux 6.1.0 kernel image
│   └── initramfs.cpio.gz     # BusyBox root filesystem
├── systemc/                   # Vendored SystemC 3.0.2 (no external install needed)
├── tests/hex/                 # Bare-metal Intel-HEX test binaries (only robust_fast64.hex and
│                               #   dhrystone32.hex are tracked; the rest are local build output)
├── docs/
│   ├── BUGS.md                 # Primary, current bug/calibration log — start here
│   ├── BUGS_AND_ISSUES.md      # Earlier bug log (pre-dates the calibration work in BUGS.md)
│   ├── Technical_Report.md     # technical summary
│   └── Project_Overview.md     # Structural overview
└── dts/
    └── riscv_vp.dts            # Device tree source
```

---

## Bug & Calibration History

The primary, current record of every bug, timing-model calibration gap, and validation result
is [`docs/BUGS.md`](docs/BUGS.md) — organised as a lettered/numbered log (§A, §B1–§B10+),
each entry with symptom, root cause, fix, and the RTL co-simulation numbers used to verify it.
Highlights from the most recent calibration pass:

| § | Finding | Result |
|---|---|---|
| B7 | AXI contention modelling investigated; store-drain and DMA contention evaluated and found unnecessary/unvalidatable | See "AXI Contention Model" above |
| B8 | Cache replacement was LRU; CVA6 uses LFSR pseudo-random — LRU pathologically over-misses on scans larger than the cache | Miss counts corrected from ~22% error to <0.2% |
| B9 | Load→address (gather/pointer-chase) dependency latency was under-modelled by ~2 cycles | Isolated pointer-chase test now matches RTL to 0.006% |
| B10 | I$ miss latency was miscalibrated (10 cycles, should be 7); AXI burst mode implemented and its own latency constant re-derived to match | Memory-bound divergence reduced from up to +38.8% to as low as −1.57% (with recalibrated AXI burst mode) |

An earlier, separate 24-bug log covering the original Linux-boot bring-up (reset/init, ISA
correctness, store buffer, MMU/TLB, privilege/interrupts, peripherals) is preserved in
[`docs/BUGS_AND_ISSUES.md`](docs/BUGS_AND_ISSUES.md) for historical reference.

---

## Acknowledgements

Built on top of the [RISC-V TLM Simulator](https://github.com/mariusmm/RISC-V-TLM)
originally developed by **Màrius Montón**. The TLM-2.0 infrastructure, memory model,
and base ISA handlers originate from that work.

The optional AXI contention model uses **NVIDIA matchlib**, via the public
[Stuart-Swan Matchlib-Examples-Kit-For-Accellera-Synthesis-WG](https://github.com/Stuart-Swan/Matchlib-Examples-Kit-For-Accellera-Synthesis-WG),
vendored in `third_party/matchlib_kit/`.

Extensions added in this project:
- Complete CVA6-aligned 6-stage cycle-accurate pipeline
- sv39 MMU with ITLB, DTLB, and hardware page table walker
- Full M/S/U privilege architecture and CSR file
- L1 I/D cache model, RTL-matched geometry and LFSR replacement policy
- Load-use and load→address dependency hazard detection
- CLINT, PLIC, UART peripherals wired for Linux
- Store buffer with partial-overlap stall
- Branch predictor (BTB + BHT + RAS), RTL-matched sizing
- Linux 6.1.0 boot support
- Optional real-AXI (matchlib `AxiArbiter`) I$/D$ contention model with genuine burst semantics

## License

GNU General Public License v3.0 — see `LICENSE` for details.
