# RISC-V VP — Bugs Found & Timing-Model Calibration (CVA6 co-simulation validation)

This document records every functional bug and timing-model calibration gap found
while validating the 6-stage cycle-accurate VP (`CPU_P64_6_Cycle`) against the
**CVA6 RTL co-simulation** (`cv64a6_imafdc_sv39`, Verilated + matchlib/SystemC).

**Method.** Each functional unit was isolated with a **register-only, single-
predictable-branch bare-metal microbenchmark** (no memory, no random branches, no
cold-start), run on both the VP and the CVA6 co-sim with the *same binary*.
Every benchmark emits its HPM counters and the **bit-exact result word**, so both
numerical correctness and cycle/IPC accuracy are checked. Benchmarks:
`int_alu_bench` (ALU), `fp_bench` (FP add/mul), `fp_stress` (FP add/mul/div/sqrt),
`div_stress` (integer div/rem), plus `robust_stress` (branches), `long_test3`
(loads), `cam_bench`.

---

## A. Functional (correctness) bugs

### A1. FP load/store immediate decoded as 0 — **FPU produced garbage** *(FIXED)*
- **Symptom:** every floating-point program produced wrong results (e.g. `2.0+3.0`
  returned `1.56e-286`). Short FP loops also failed to terminate (control flow
  derailed by corrupted FP-compare operands).
- **Root cause:** the standard-decode immediate switch handled the integer load
  `0x03` and store `0x23`, but **not the FP load `0x07` or FP store `0x27`**, so
  `fld/fsd` fell to `default: imm = 0`. `fld fa5, 72(a5)` computed address
  `a5 + 0` → read the **wrong memory** → garbage operands. (Offset-0 loads worked
  by luck, which is why the bug hid for a long time.)
- **Fix:** add `case 0x07` to the I-type immediate group and `case 0x27` to the
  S-type group in `PCGen/decode` (`CPU_P64_6_Cycle.cpp`), plus `0x27` to the
  no-`rd` group and `0x07` to the rs2-mask group. Commit *"Decode FP load and
  store instructions correctly in standard decode stage."*
- **Validation:** `2.0+3.0 → 0x4014000000000000` (5.0); `fp_stress` result now
  bit-exact vs CVA6 (`0x3ff7c6176c452d17`). Everything else in the FP datapath
  (F/D decode dispatch, `f_regs` sharing, commit FP/int routing) was already
  correct.

### Note: numerical correctness of all units
After A1, **every** benchmark's result word matches the CVA6 co-sim **bit-for-bit**
(int `0x3fb776e12465e67d`, fp_bench `0x4184463b0cf87061`, fp_stress
`0x3ff7c6176c452d17`, div `0x5f6dd0b692933335`). The FP path uses native host
IEEE-754 (`std::fma`, rounding via `fesetround`); it matches FPnew for normal
values (NaN-boxing / signalling-NaN / subnormal edge cases are not separately
verified).

---

## B. Timing-model calibration gaps (each measured, then modelled)

### B1. Divider: flat 66-cycle → **operand-value early termination** *(FIXED)*
- **Symptom:** `div_stress` cycles **+31%** vs CVA6 (VP IPC 0.126 vs 0.165).
- **Root cause:** every 64-bit divide was charged a flat **66 cycles**; CVA6's
  serial divider terminates early (latency ∝ significant quotient bits), averaging
  ~47 cycles on this workload.
- **Fix:** data-dependent latency `div_shift = LZC(divisor) − LZC(dividend);
  cycles = div_shift + 6` with 1-cycle fast paths for ÷0 / ÷(−1) / zero-quotient,
  clamped to 66/34. (`+6` tuned to CVA6's ~47-cycle average.)
- **Result:** cycle error **+31% → −0.6%** (VP 1,123,900 vs CVA6 1,130,126, IPC
  0.166 vs 0.165), result bit-exact.

### B2. Taken-branch: zero-cost redirect → **1-cycle fetch bubble** *(FIXED)*
- **Symptom:** `int_alu` ran at a *perfect* IPC 1.000 while CVA6 was 0.917 (**+9%**).
- **Root cause:** a correctly-predicted **taken** branch set `next_pc = target`
  with **no cost**; CVA6 pays a 1-cycle BTB redirect bubble (~1 cyc per taken
  branch → ~200K extra cycles on a 200K-iteration loop).
- **Fix:** `taken_redirect_bubble` flag — PCGen injects one fetch bubble after each
  predicted-taken branch/jump (cleared on flush so it never double-counts
  mispredicts).
- **Result:** `int_alu` **1.000 → 0.916** (CVA6 0.917, −0.1%). Correctly a
  *no-op* on load-/div-/FP-bound code (the bubble hides behind backend stalls).

### B3. FPU: single blocking unit → **pipelined add/mul, blocking div/sqrt** *(FIXED)*
- **Symptom:** `fp_bench` (add/mul) **−21%** vs CVA6 (IPC 0.394 vs 0.500).
- **Root cause:** the FPU was one blocking unit — every FP op held it for its full
  latency, so *independent* FP ops could not overlap. CVA6's FPnew ADDMUL block is
  **pipelined** (throughput 1/cycle); only its DIVSQRT block is iterative/blocking.
- **Fix:**
  1. Replace the single `fpu_fu` with an 8-slot **pipelined** unit `fpu_pipe[]`
     (throughput 1; Issue stalls only when all slots busy; dependent consumers
     stall via the scoreboard).
  2. Route **fdiv/fsqrt** (latency > 5) to a dedicated **non-pipelined blocking**
     unit `fpu_divsqrt_fu` (a 2nd div/sqrt waits for the first) — mirrors FPnew.
  3. Raise ADDMUL latency 3 → **4** (CVA6 FPnew double-precision add/mul depth).
- **Result:** `fp_bench` **0.394 → 0.518** (CVA6 0.500, +3.6%); `fp_stress`
  (add/mul/div/sqrt) **0.152** (CVA6 0.145, +4.8%); results bit-exact.
- **Pitfall recorded:** the first cut pipelined *everything*, which wrongly sped up
  div/sqrt (`fp_stress` 0.250 vs 0.145). Splitting ADDMUL (pipelined) from DIVSQRT
  (blocking) is essential.

### B4. Load-use stall over-charge *(IDENTIFIED — calibration, not yet modelled)*
- **Symptom:** `long_test3` (load-heavy `-O1`) cycles **+100%** / IPC **−50%**
  (VP 0.364 vs CVA6 0.727).
- **Root cause:** `load_hit_penalty = 5` is charged as a **structural stall on
  every load** (arms `load_hit_fu`; Issue stalls while busy), regardless of whether
  the next instruction uses the result. Real CVA6 only stalls a *dependent*
  consumer and hides load latency behind independent work. Confirmed by the stall
  breakdown: 1.5M load-use stalls vs CVA6's ~199K total bubbles.
- **Proposed fix (same shape as B3):** model load latency as a **scoreboard
  dependency** (stall only the dependent consumer) instead of a blanket Issue
  stall, and/or lower `load_hit_penalty` to ~1–2.

---

## C. Measurement caveats discovered

- **Instruction-count difference (VP vs CVA6):** a fixed **~18–19 instruction**
  offset — CVA6 reports the `minstret` value read *inside* the program (before the
  halter writes); the VP counts to its actual halt (after the readout epilogue).
  Not an execution difference. Earlier scattered deltas (+14/+46/+125) were
  `--max-instr` overshoot; adding an **HTIF halt** to every benchmark makes the
  offset a constant ~18–19.
- **HPM "access" counters are not directly comparable.** CVA6 I$/D$ *access* events
  count fetch/port *activity* (speculative, multi-port OR), so they exceed retired
  instructions and differ from the VP's exact software counters. Compare **miss
  counts** and **loads/stores**, not the "access" denominators.
- **CVA6 "dram" is a behavioural `SlaveFromFile` (a `std::map`), zero-wait-state /
  SRAM-speed — no DRAM latency modelled.** So co-sim cache misses are cheap; the VP
  instead uses a flat 107-cycle miss penalty.
- **Branch counts differ by definition** (CVA6 counts jumps/calls/returns as
  branches; the VP does not) — compare mispredict *rate*, not raw counts.

---

## D. Accuracy summary (register-only microbenchmarks, both bit-exact)

| Unit tested | Benchmark | CVA6 IPC | VP IPC | Error | Status |
|---|---|---:|---:|---:|---|
| Integer ALU | int_alu | 0.917 | 0.916 | −0.1% | ✅ B2 |
| Integer divide | div_stress | 0.165 | 0.166 | +0.6% | ✅ B1 |
| FP add/mul | fp_bench | 0.500 | 0.518 | +3.6% | ✅ B3 |
| FP add/mul/div/sqrt | fp_stress | 0.145 | 0.152 | +4.8% | ✅ B3 |

### Remaining known gaps
| Workload | CVA6 | VP | Error | Cause |
|---|---:|---:|---:|---|
| Load-heavy (long_test3 -O1) | 0.727 | 0.364 | −50% | load-use over-charge (B4, open) |
| Branch-heavy (robust_stress) | 0.685 | 0.793 | +16% | single-issue frontend cap |
| Tiny (cam_bench) | 0.682 | 0.195 | −71% | too small — cold-start dominated (not a valid timing measurement) |
