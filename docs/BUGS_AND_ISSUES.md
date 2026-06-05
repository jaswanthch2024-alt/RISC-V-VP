# RISCV-VP Bug & Issue Log

Complete record of every bug and implementation issue encountered during development of the Cycle6 6-stage CVA6-aligned pipeline and Linux boot. Organised by category. Use this as the first reference when a new simulation failure appears.

---

## How to Use This Document

Each entry has:
- **Symptom** — what you observe at the surface
- **Root Cause** — the actual code defect
- **Fix** — what was changed and where
- **Found Via** — how it was discovered
- **Tags** — for quick filtering

---

## Category 1 — Reset / Initialisation

### BUG-01: CLINT mtimecmp initialised to 0

| Field | Detail |
|-------|--------|
| **Symptom** | Infinite MTIP storm from cycle 0. Kernel never reaches main init. |
| **Root Cause** | `m_mtimecmp` default was `0`. Since `mtime=0 >= mtimecmp=0`, `timer_irq` asserts immediately and never deasserts. |
| **Fix** | `inc/CLINT.h` line 20: `m_mtimecmp(0xFFFFFFFFFFFFFFFFULL)` |
| **Found Via** | robust_fast64 test — CPU trapped in timer IRQ from first instruction |
| **Tags** | `clint` `init` `interrupt` |

---

### BUG-02: Stack pointer (SP) initialised out-of-bounds

| Field | Detail |
|-------|--------|
| **Symptom** | Bare-metal tests crash on first stack push; SP points past Memory::SIZE |
| **Root Cause** | SP was set to `0x10000000 + Memory::SIZE - 8`. The `0x10000000` offset pushed SP above the allocated memory region. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` line 20: changed to `Memory::SIZE - 8` = `0x1FFFFFF8` |
| **Found Via** | robust_fast64 stack-push fault |
| **Tags** | `init` `register` `stack` |

---

### BUG-03: GP register wrong value → misaligned store trap

| Field | Detail |
|-------|--------|
| **Symptom** | robust_fast64 faults on a `sw` with `store-address-misaligned` trap |
| **Root Cause** | `gp = 0x11000`. The test binary's global buffer starts at `gp - 0x7F3 = 0x1080D`, which is 1-byte misaligned for a 4-byte store. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp`: `gp = 0x10FF3`. Buffer then lands at `0x10800` (4-byte aligned, above .text end at `0x10722`). |
| **Found Via** | robust_fast64 misaligned exception trace |
| **Tags** | `init` `register` `alignment` |

---

### BUG-04: `mstatus.UXL` / `mstatus.SXL` initialised to 0 (not RV64)

| Field | Detail |
|-------|--------|
| **Symptom** | `rv64si-p-csr` test 18 FAIL. `csrr sstatus` returned `UXL=0` instead of `0x200000000`. Linux user-space processes could observe wrong XLEN. |
| **Root Cause** | `mstatus` was zero-initialised. Per Privileged Spec §3.1.6, `UXL[33:32]` and `SXL[35:34]` must be hardwired to `2` (RV64) at reset. Also `SSTATUS_MASK` did not include the `UXL` field so `csrr sstatus` masked it out. |
| **Fix** | `inc/CSR_File.h`: (1) Added `UXL=(3ULL<<32)` and `SXL=(3ULL<<34)` constants to `MSTATUS` namespace. (2) Changed `mstatus` init to `{(2ULL<<32)|(2ULL<<34)}`. (3) Added `MSTATUS::UXL` to `SSTATUS_MASK`. |
| **Found Via** | riscv-tests rv64si-p-csr suite; also contributed to init SIGBUS fix |
| **Tags** | `csr` `init` `privilege` `rv64` |

---

## Category 2 — ISA Instruction Correctness

### BUG-05: Missing RV64 word-ops: SLLIW, SRLIW, SRAIW (opcode 0x1B)

| Field | Detail |
|-------|--------|
| **Symptom** | Illegal instruction trap on any `slliw`/`srliw`/`sraiw` — common in compiler-generated RV64 code |
| **Root Cause** | EX-stage opcode 0x1B handler only implemented `ADDIW`. The three shift-immediate word ops were absent. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` EX handler for opcode `0x1B`: added `SLLIW` (`funct3=1`), `SRLIW` (`funct3=5, funct7=0`), `SRAIW` (`funct3=5, funct7=0x20`). |
| **Found Via** | Compile errors when `instr` field was used in EX; confirmed by riscv-tests |
| **Tags** | `isa` `rv64` `shift` |

---

### BUG-06: Missing RV64 word-ops: SLLW, SRLW, SRAW (opcode 0x3B)

| Field | Detail |
|-------|--------|
| **Symptom** | Illegal instruction trap on `sllw`/`srlw`/`sraw` register-register variants |
| **Root Cause** | Opcode `0x3B` non-M handler only had `ADDW`/`SUBW`. The three word-shift register ops were absent. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` opcode `0x3B`: added `SLLW`, `SRLW`, `SRAW` with correct funct3/funct7 decode. |
| **Found Via** | Same compilation pass that found BUG-05 |
| **Tags** | `isa` `rv64` `shift` |

---

### BUG-07: Missing `instr` and `rs1` fields in Issue→EX latch

| Field | Detail |
|-------|--------|
| **Symptom** | Compile error: `issue_ex_reg.instr` and `issue_ex_reg.rs1` undefined in EX stage |
| **Root Cause** | The `Issue_EX_Latch` struct was missing these two fields. EX stage needed `instr` for CSR-immediate zimm extraction and illegal-instruction tval, and `rs1` for SFENCE.VMA zero-check. |
| **Fix** | Added both fields to the latch struct and populated them in `Issue_stage` dispatch. |
| **Found Via** | Compile error during EX stage extension |
| **Tags** | `pipeline` `latch` `compile` |

---

### BUG-08: C.ADDI4SPN with nzuimm=0 not treated as illegal instruction

| Field | Detail |
|-------|--------|
| **Symptom** | Encoding `0x0000` silently decoded as a no-op instead of raising illegal-instruction trap. Linux kernel relies on this trap to detect corrupt/absent instructions. |
| **Root Cause** | C-extension decoder did not check the nzuimm≠0 constraint specified by the RVC spec. |
| **Fix** | `inc/C_extension.h` C.ADDI4SPN decode: added `if (nzuimm == 0) → illegal_instruction`. |
| **Found Via** | Linux boot stall — kernel kept executing past end of valid text |
| **Tags** | `isa` `compressed` `illegal-instruction` |

---

### BUG-09: DIVUW spec violation

| Field | Detail |
|-------|--------|
| **Symptom** | `DIVUW` returned wrong result for certain 32-bit unsigned operands |
| **Root Cause** | Operands were not zero-extended to 64 bits before the division; sign-extension was applied instead, corrupting large unsigned dividends. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` DIVUW handler: `uint64_t(uint32_t(rs1)) / uint64_t(uint32_t(rs2))` with correct sign-extension of the 32-bit result to 64 bits. |
| **Found Via** | DIVUW riscv-tests case |
| **Tags** | `isa` `mul-div` `rv64` |

---

### BUG-10: No illegal-instruction trap on opcode[1:0] != 0b11 → fake NOP slide

| Field | Detail |
|-------|--------|
| **Symptom** | After bare-metal Dhrystone exits, PC escapes to unmapped memory. `b_transport` returns `0x00000000`. CPU retires these as NOPs, producing ~99k fake cycles with CPI=1.0 and near-zero stalls — completely misleading stats. |
| **Root Cause** | Issue stage had no check for the RISC-V 32-bit instruction encoding requirement `opcode[1:0] == 0b11`. Encoding `0x00000000` (opcode=0x00) was silently retired. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` Issue_stage: any instruction with `opcode[1:0] != 0b11` raises illegal-instruction trap, flushes ROB, clears scoreboard, prints stats, calls `sc_stop()`. |
| **Found Via** | Dhrystone benchmark post-exit stats looked suspiciously good |
| **Tags** | `pipeline` `illegal-instruction` `simulation` |

---

## Category 3 — Store Buffer

### BUG-11: Store buffer slot-reuse ordering (newest-first scan)

| Field | Detail |
|-------|--------|
| **Symptom** | Load forwarded an older (stale) store value when a newer store to the same address was in the speculative queue |
| **Root Cause** | Forward scan iterated stores oldest-first, returning the first hit. When a slot was reused after commit, the older slot was found before the newer one. |
| **Fix** | `inc/StoreBuffer.h`: changed forward scan to newest-first (scan spec queue from tail toward head). Younger stores take priority over older ones for the same address. |
| **Found Via** | Phase 2 store-buffer refactor during pipeline bring-up |
| **Tags** | `store-buffer` `forwarding` `ordering` |

---

### BUG-12: Store buffer partial overlap — stale data returned (SLUB BUG_ON)

| Field | Detail |
|-------|--------|
| **Symptom** | Linux SLUB allocator hit `BUG_ON` at ~50M instructions. A narrow store was in the store buffer; a wider load to an overlapping address bypassed the buffer and read stale memory. |
| **Root Cause** | `forward_load()` only handled full-address-match hits. Partial overlaps (e.g., 1-byte store followed by 4-byte load to same cache line) returned `MISS`, so the load read pre-store memory data. |
| **Fix** | `inc/StoreBuffer.h`: introduced tri-state `FwdResult` enum (`HIT` / `PARTIAL_OVERLAP` / `MISS`). `PARTIAL_OVERLAP` stalls EX via `stall_ex` until the overlapping store commits and drains from the buffer. |
| **Found Via** | Linux boot — SLUB `BUG_ON(object == fp)` at ~50M instructions |
| **Tags** | `store-buffer` `forwarding` `alignment` `linux` |

---

## Category 4 — MMU / TLB

### BUG-13: satp mode validation missing

| Field | Detail |
|-------|--------|
| **Symptom** | Writing an unsupported satp MODE value (e.g., Sv48=9, Sv57=10) silently enabled wrong address translation. |
| **Root Cause** | No validation of the `MODE` field when `csrw satp` was executed. |
| **Fix** | `inc/CSR_File.h` / `src/CPU_P64_6_Cycle.cpp`: on `csrw satp`, only accept `MODE=0` (bare) or `MODE=8` (Sv39). All other values are ignored (satp remains unchanged), per spec. |
| **Found Via** | Linux boot — kernel probes satp modes and expects non-Sv39 writes to be silently discarded |
| **Tags** | `mmu` `csr` `satp` |

---

### BUG-14: TLB `flush_all()` skipping global entries

| Field | Detail |
|-------|--------|
| **Symptom** | After `free_initmem()`, kernel text mapped with G=1 retained stale TLB entries with wrong page permissions (X=0) even after `sfence.vma x0,x0`. CPU took instruction page faults when trying to execute kernel functions at `0xffffffff8000346e`. |
| **Root Cause** | `TLB::flush_all()` had `if (!e.global) e.valid = false;` — deliberately skipping global entries. Linux maps kernel text with the Global bit set. The sfence.vma with rs1=x0, rs2=x0 is a full flush and must invalidate all entries including global ones. |
| **Fix** | `inc/TLB.h` `flush_all()`: removed the `if (!e.global)` guard. All entries unconditionally set `valid = false`. |
| **Found Via** | init SIGBUS investigation — [LD-ISSUE] diagnostic showed issue_ex_pc at kernel address |
| **Tags** | `tlb` `mmu` `sfence` `global-bit` |

---

## Category 5 — Privilege & Interrupt Logic

### BUG-15: `s_enabled` true in M-mode → STIP fires in M-mode

| Field | Detail |
|-------|--------|
| **Symptom** | CPU froze at ~131M instructions in WFI. Trap log showed recursive STIP → M-mode handler → STIP → loop. |
| **Root Cause** | `pending_interrupt()` computed `s_enabled = (SIE != 0)` without checking current privilege. In M-mode with `mstatus.SIE=1`, `s_enabled=true` allowed STIP to fire, which is prohibited by the spec — S-mode interrupts are never delivered in M-mode. |
| **Fix** | `inc/CSR_File.h` ~line 361: `s_enabled = (priv == U) || (priv == S && SIE != 0)`. M-mode never receives S-mode interrupts regardless of SIE. |
| **Found Via** | Linux boot WFI stall at 131M instructions; trap log showed STIP in M-mode |
| **Tags** | `interrupt` `privilege` `csr` `mstatus` |

---

### BUG-16: sepc corrupted after MRET/SRET — wrong interrupt return address

| Field | Detail |
|-------|--------|
| **Symptom** | After MRET transitions M→S, the next STIP fires and `cpu_process_IRQ()` sets `sepc` to the old M-mode PC (e.g. `0x800004ee` in OpenSBI). SRET then returns to that S-mode VA → instruction page fault. |
| **Root Cause** | MRET in EX sets `pc_redirect_valid=true`. But PCGen clears it in the same cycle. On the next cycle `cpu_process_IRQ()` sees `pc_redirect_valid=false` and uses `pc_current`, which still holds the M-mode return address. |
| **Fix** | `inc/CPU_P64_6_Cycle.h`: added `pending_priv_switch` flag and `pending_priv_switch_epc` field. EX stage MRET/SRET sets both. `cpu_process_IRQ()` uses `pending_priv_switch_epc` when the flag is set. `Commit_stage` clears the flag when an instruction retires. Files: `inc/CPU_P64_6_Cycle.h`, `src/CPU_P64_6_Cycle.cpp` (MRET ~1868, SRET ~1882, cpu_process_IRQ ~2221, Commit_stage ~2186). |
| **Found Via** | Linux boot — instruction page faults returning to OpenSBI address in S-mode |
| **Tags** | `interrupt` `trap` `sepc` `privilege` `mret` `sret` |

---

### BUG-17: `stval=0` for instruction page faults

| Field | Detail |
|-------|--------|
| **Symptom** | Kernel reported all instruction page faults as "NULL pointer dereference" (badaddr=0) making them impossible to distinguish by fault address. |
| **Root Cause** | `csr.take_exception(tr.fault_cause, current_pc, 0)` was passing `0` as the tval argument. Per the spec, instruction page fault must set `stval` to the faulting instruction address. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` Fetch_stage ~line 348 and ~364: changed to `csr.take_exception(tr.fault_cause, current_pc, current_pc)`. Same fix applied for `INSTR_ACCESS` faults. |
| **Found Via** | Linux boot kernel log — all instr faults showed `badaddr=0` |
| **Tags** | `trap` `stval` `page-fault` `csr` |

---

## Category 6 — Peripherals

### BUG-18: UART THRE interrupt never cleared → interrupt storm

| Field | Detail |
|-------|--------|
| **Symptom** | Linux spent 685M+ instructions trapped in `serial8250_handle_irq` UART ISR. The interrupt was never deasserted so the kernel re-entered the handler in an infinite loop. Boot was stuck at `debug_vm_pgtable`. |
| **Root Cause** | Original UART model set `m_thre_ip = true` unconditionally and had no mechanism to clear it. The kernel's UART ISR reads IIR to acknowledge the interrupt, but IIR reads did not clear `m_thre_ip`. As a result, the PLIC line stayed asserted after the ISR returned. |
| **Fix** | `inc/UART.h`: IIR read now sets `m_thre_ip = false` then calls `update_interrupts()` — deasserts the PLIC line. IER write: transitioning THRI from disabled→enabled sets `m_thre_ip = true`; disabling THRI clears it. THR write: `m_thre_ip` goes false then immediately true (TX always empty in sim). |
| **Found Via** | Linux boot log — `serial8250_handle_irq` dominated instruction trace at 685M insns |
| **Tags** | `uart` `interrupt` `plic` `linux` |

---

### BUG-19: CLINT tick rate 1µs → kernel starved between timer interrupts

| Field | Detail |
|-------|--------|
| **Symptom** | Even after fixing the UART storm, early Linux boot was slow and RCU stall warnings appeared very early. Scheduler had almost no CPU time between timer ticks. |
| **Root Cause** | `CLINT::tick()` used `wait(1, SC_US)` — one tick per microsecond of simulation time. The Linux kernel programs `mtimecmp = mtime + 1000` for a 1ms period at 1MHz. With 1µs ticks, the CLINT fired every ~1000 CPU cycles. The kernel's timer ISR takes hundreds of thousands of cycles to complete, so the CPU spent almost all time in interrupt context. |
| **Fix** | `inc/CLINT.h` `tick()`: changed to `wait(1000, SC_US)` — one tick per millisecond of simulation time. Timer now fires every ~1M CPU cycles, giving the kernel adequate time between interrupts. |
| **Found Via** | Analysis of boot log — kernel scheduler never got CPU time between ticks |
| **Tags** | `clint` `timer` `interrupt` `linux` |

---

### BUG-20: `tohost` write handler truncated to 32 bits

| Field | Detail |
|-------|--------|
| **Symptom** | riscv-tests wrote a 64-bit value to `tohost` (address `0x80001000`). The handler read only 4 bytes, silently truncating the upper 32 bits. Pass/fail detection was unreliable. |
| **Root Cause** | `b_transport` for the `tohost` region used `memcpy(..., 4)` regardless of transaction length. `riscv-tests` pass convention is `val64 == 1`; a 4-byte read of the low word happened to work for pass but could misdetect fail codes. |
| **Fix** | `src/BusCtrl.cpp` tohost handler: reads up to 8 bytes, compares full `uint64_t`. Prints `PASS` for `val64==1`, `FAIL` with exit code otherwise. |
| **Found Via** | riscv-tests reporting FAIL on some passing tests when exit code had data in upper 32 bits |
| **Tags** | `busctrl` `tohost` `riscv-tests` |

---

## Category 7 — Pipeline Microarchitecture

### BUG-21: Every taken branch flushed the pipeline (no branch prediction)

| Field | Detail |
|-------|--------|
| **Symptom** | IPC ~0.75 on benchmarks. Every conditional branch that was taken caused a full pipeline flush and 3-4 bubble cycles regardless of whether the target was predictable. |
| **Root Cause** | PCGen always used the sequential PC. No BTB/BHT — when EX resolved a taken branch, it redirected and flushed. |
| **Fix** | Added `BranchPredictor` with: 128-entry BTB, 256-entry 2-bit BHT (gshare), 4-entry RAS. PCGen now speculatively uses predicted target. EX verifies and trains predictors; only flushes on misprediction. Results: IPC improved from ~0.75 → ~0.816, 99.3% prediction accuracy (8 mispredicts / 1210 branches on robust_fast64). |
| **Found Via** | IPC analysis on Dhrystone / robust benchmarks |
| **Tags** | `pipeline` `branch-prediction` `ipc` |

---

### BUG-22: Fetch-stage flush/stall priority inverted

| Field | Detail |
|-------|--------|
| **Symptom** | In certain flush+stall coincident cycles, Fetch would process a stale instruction instead of discarding it, injecting a bad instruction into the pipeline. |
| **Root Cause** | Flush and stall conditions were evaluated in wrong priority order — stall was checked before flush, so a simultaneous flush+stall would stall instead of flushing (letting the bad instruction through). |
| **Fix** | `src/CPU_P64_6_Cycle.cpp` Fetch_stage: flush is now checked before stall. If both are active, flush wins and the instruction is discarded. |
| **Found Via** | DIVUW spec violation investigation (commit `63f562a`) |
| **Tags** | `pipeline` `fetch` `flush` `stall` |

---

### BUG-23: C-extension decoded in 6-stage model (should be non-6-stage only)

| Field | Detail |
|-------|--------|
| **Symptom** | 6-stage model executed C-extension instructions through a separate decode path, bypassing the pipeline latch structure and causing mis-synchronized issue/execute. |
| **Root Cause** | C-extension code was shared between all CPU models. The 6-stage model inherited it without isolation. |
| **Fix** | Refactored C-extension to be invoked exclusively in non-6-stage models. The 6-stage pipeline has its own compressed instruction handling integrated into the main decode path. |
| **Found Via** | Code review during pipeline bring-up |
| **Tags** | `pipeline` `compressed` `refactor` |

---

## Category 8 — Simulation Infrastructure

### BUG-24: WFI not modelled — kernel spin-waited instead of yielding simulation time

| Field | Detail |
|-------|--------|
| **Symptom** | After kernel went idle (e.g., waiting for timer), simulation consumed CPU time at maximum speed spinning on WFI with no forward progress. Simulation rate was wasted. |
| **Root Cause** | WFI was modelled as a NOP — CPU kept advancing the cycle counter without yielding to the SystemC scheduler or fast-forwarding mtime. |
| **Fix** | `src/CPU_P64_6_Cycle.cpp`: WFI sets `wfi_stall=true`. CPU calls `clint->fast_forward_to_deadline()` to jump `mtime` to `mtimecmp - 1` then yields with `sc_core::wait()`. `wfi_stall` is cleared when an interrupt fires. |
| **Found Via** | Extremely slow simulation time during Linux idle periods |
| **Tags** | `pipeline` `wfi` `simulation-performance` |

---

## Summary Table

| ID | Category | Component | Severity | Status |
|----|----------|-----------|----------|--------|
| BUG-01 | Init | CLINT | Critical | Fixed |
| BUG-02 | Init | CPU reset | Critical | Fixed |
| BUG-03 | Init | CPU reset | High | Fixed |
| BUG-04 | Init / CSR | CSR_File | High | Fixed |
| BUG-05 | ISA | EX stage | High | Fixed |
| BUG-06 | ISA | EX stage | High | Fixed |
| BUG-07 | Pipeline | Latch struct | Medium | Fixed |
| BUG-08 | ISA | C-extension | High | Fixed |
| BUG-09 | ISA | MUL/DIV | High | Fixed |
| BUG-10 | Pipeline | Issue stage | Medium | Fixed |
| BUG-11 | Store Buffer | StoreBuffer | High | Fixed |
| BUG-12 | Store Buffer | StoreBuffer | Critical | Fixed |
| BUG-13 | MMU | CSR / satp | High | Fixed |
| BUG-14 | MMU / TLB | TLB | Critical | Fixed |
| BUG-15 | Interrupt | CSR_File | Critical | Fixed |
| BUG-16 | Interrupt | CPU pipeline | Critical | Fixed |
| BUG-17 | Trap | Fetch stage | Medium | Fixed |
| BUG-18 | Peripheral | UART | Critical | Fixed |
| BUG-19 | Peripheral | CLINT | Critical | Fixed |
| BUG-20 | Infrastructure | BusCtrl | Medium | Fixed |
| BUG-21 | Microarch | PCGen / EX | Medium | Fixed |
| BUG-22 | Pipeline | Fetch stage | Medium | Fixed |
| BUG-23 | Pipeline | C-extension | Low | Fixed |
| BUG-24 | Simulation | WFI | High | Fixed |

---

## Patterns & Lessons

1. **Peripheral interrupt edges are never self-clearing** — always implement an explicit clear path (IIR read, write-to-ack, etc.) or the kernel will loop in the ISR forever.
2. **CLINT tick rate must match kernel expectation** — Linux programs `mtimecmp = mtime + 1000` expecting a 1ms period at 1MHz. The SystemC tick period must model that ratio or the kernel starves.
3. **Global TLB entries must be flushed by `sfence.vma x0, x0`** — the RISC-V spec says `rs1=x0` flushes all VA spaces including global mappings. Do not guard on the G bit.
4. **M-mode must never receive S-mode interrupts** — even when `mstatus.SIE=1`, `s_enabled` must check current privilege level.
5. **Latch structs need all fields EX uses** — when adding EX-stage logic that reads a new decoded field, add it to the latch struct immediately or the build breaks non-obviously.
6. **tohost is always 64-bit in riscv-tests** — use a full 8-byte read; the pass/fail encoding is in the full 64-bit value.
7. **WFI must yield simulation time** — without fast-forward the simulation wastes wall time spinning; with it, RCU stall warnings appear in the kernel log (harmless false positive from mtime jumps).
8. **Store buffer partial overlaps are common in Linux** — SLUB and the page allocator do narrow stores followed by wide loads routinely. Tri-state forwarding result is required.
