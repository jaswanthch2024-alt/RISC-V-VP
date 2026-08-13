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

### A2. FP loads bypassed the D-cache model entirely *(FIXED)*
- **Symptom:** `fp_bench` reported **CVA6 6 D$ misses / 47 accesses vs VP 0 / 0**,
  despite both running the same binary — which contains 10 `fld` instructions that
  load the FP constants (`0.9999`, `0.1`, …) from `.rodata`.
- **Root cause:** `dcache.access()` was called from **exactly one site** in the
  whole VP (`CPU_P64_6_Cycle.cpp`, integer-load path, opcode `0x03`). The FP load
  path (opcode `0x07`) reaches memory through `D_extension.h:113`
  (`mem_intf->readDataMem64()`), which never touches the cache model. So in the VP
  an `fld` was a **free** memory access: no access counted, no miss counted, and
  **no `dcache_miss_penalty` charged**. Real CVA6 has a single LSU — integer and FP
  loads share the same L1 D$.
- **Fix (two parts — the second is easy to miss):**
  1. Probe `dcache.access(pa)` on the FP-load path once the physical address is
     resolved (after MMU translation and the PMP check), and on a miss defer
     completion through `dcache_miss_fu` exactly as the integer path does. The FP
     register file is already written by `execute()`; only the *timing* is modelled.
  2. **Add an Issue-stage structural guard for opcode `0x07`.** `dcache_miss_fu` is
     a **single slot**, and the existing guard only covers `fu == LSU` with opcode
     `0x03`/`0x2F`. FP loads are classified as `FunctionalUnit::FPU`, so without a
     new guard a second FP load issues while the first miss is in flight,
     **overwrites `dcache_miss_fu`, and the first load's scoreboard entry never
     completes — the ROB head wedges and the VP hangs forever.** Part 1 alone hangs
     `fp_bench` (10 back-to-back `fld`s) on the first run.
- **Scope / deliberate omissions:** FP **stores** are still not counted — but
  neither are integer stores (both are write-through and handled by the store
  buffer), so the model stays internally consistent. Loads are what the D$ counters
  track in this VP.
- **Validation (measured, whole suite re-run):** `fp_bench` D$ accesses **0 → 10**
  (exactly the 10 `fld`s in the binary), misses **0 → 2**. Cycles
  2,510,107 → 2,510,207 (**+100**, 0.004%); `fp_stress` 5,900,516 → 5,900,619
  (**+103**). **The other six benchmarks are cycle-for-cycle identical**, and every
  result word is unchanged. IPC unmoved to three decimals.
  - VP reports 2 misses where CVA6 reports 6: the same line-size ratio as the I$
    (VP 64-byte lines vs CVA6 16-byte lines) over the same ~100 bytes of constants.
- **Impact on prior results: negligible, and that is the point.** The affected
  benchmarks were designed to be register-only, so the constants load a handful of
  times outside the hot loop. **This bug does not explain fp_bench's +3.6% error**
  (see §B3) and fixing it does not close that gap — B3's fitted FPnew latencies own
  that residual.
- **Why it matters anyway:** any FP workload that *streams* data from memory —
  i.e. most real FP code — would have had its memory latency silently dropped. The
  microbenchmarks hid the bug precisely because they were built to avoid memory.

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

### B4. Shift→adder dependency bubble missing *(FIXED)*
- **Symptom:** on a *branch-free*, fully-unrolled ALU dependency chain
  (`int_nobranch`), the VP ran at IPC **0.996** while CVA6 was **0.833** — a
  **+19.5%** error. Not explained by branches (1 mispredict), I$ misses (324, all
  cold), or memory (D$ = 0/0). ~478,000 cycles (**0.19 cyc/instr**) unaccounted.
- **Diagnosis — the discriminating experiment.** Two hypotheses fit the number
  equally well: (H1) a back-to-back dependency bubble, or (H2) a fetch-bandwidth /
  I$ line-crossing limit. A control benchmark `int_indep` was built with **identical
  straight-line structure and code size** but the dependencies **spaced 8 apart**
  (8 interleaved independent variables). Result:

  | Benchmark | deps | CVA6 CPI | VP CPI | verdict |
  |---|---|---:|---:|---|
  | `int_nobranch` | back-to-back | **1.200** | 1.00 | gap present |
  | `int_indep` | spaced out | **1.002** | 1.00 | gap gone |

  CVA6's CPI collapsed to 1.002 with the *same fetch pattern* → **H1 confirmed,
  H2 refuted.** Penalty quantified: 514,345 extra cycles ÷ 513,207 dependent pairs
  = **1.002 cycles per dependent pair — exactly one.**
- **Root cause:** in the `-O1` unrolled body, `d = d + (a << 3)` compiles to
  `slli t,a,3` immediately followed by the dependent `add d,d,t`. Real CVA6 cannot
  forward a **shift** result to the very next arithmetic/branch/store consumer
  without one bubble; the VP forwarded it for free.
- **Fix:** in `Issue_stage`, when the instruction in EX is a **shift**
  (`SLL/SRL/SRA` + I/W variants) and the instruction in Issue reads its `rd`,
  insert a 1-cycle stall — **unless** the consumer is a fast logical op
  (`AND/OR/XOR`), which CVA6 can bypass.
- **Result:** `int_nobranch` **0.996 → 0.831** (CVA6 0.833, **−0.2%**), with **no
  regression** on the controls (`int_indep` 0.996 vs 0.998; `int_alu` 0.916 vs
  0.917). Results bit-exact.
- **Why it hid for so long:** in a *small* loop GCC schedules the shift away from
  its consumer, so the penalty rarely fires — the looped `int_alu` test agreed
  (0.916 vs 0.917) partly by coincidence. Only the fully-unrolled, branch-free
  variant exposed it. **Lesson: build a branch-free control for every unit.**

### B5. Load-use stall over-charge *(FIXED)*
- **Symptom:** `long_test3` (load-heavy `-O1`) cycles **+100%** / IPC **−50%**
  (VP 0.364 vs CVA6 0.727).
- **Root cause:** two memory penalties were set to model *off-chip AXI/DRAM*
  latency, but the co-sim models **no main memory latency whatsoever** — its
  `dram` instance is a zero-wait-state behavioural `SlaveFromFile` (see §C) — and
  CVA6's L1 D$ hit latency is a single cycle:
  - `load_hit_penalty = 5` — charged as a **structural stall on every load** (arms
    `load_hit_fu`; Issue stalls while busy), regardless of whether the next
    instruction uses the result.
  - `store_write_penalty = 20` — write-through drain latency per store.
- **Fix:** `load_hit_penalty` **5 → 1** (L1 D$ hit latency) and
  `store_write_penalty` **20 → 0** (`CPU_P64_6_Cycle.h:358-359`).
- **Result:** `long_test3` VP **0.364 → 0.727** vs CVA6 **0.727** — an **exact
  match** (800,056 instr / 1,100,602 cycles). Stall breakdown now shows 299,999
  load-use stalls at 1 cycle each, and stores drain in-line.
- **⚠ Caveat — this calibrates to a testbench stub, not to silicon.**
  `store_write_penalty = 0` and `load_hit_penalty = 1` are correct *against the CVA6
  co-sim harness*, whose "memory" is a zero-wait-state `std::map` with no timing
  model (§C). On a real SoC with DRAM behind AXI, both are badly optimistic. The
  perfect long_test3 agreement (0.727 vs 0.727) therefore demonstrates that **the VP
  matches the RTL harness**, not that it predicts silicon performance. If
  `SlaveFromFile` is ever replaced with a timed memory model, **these two constants
  must be re-derived**; they no longer represent any physical latency.
- **Note on the original diagnosis:** the "blanket structural stall vs scoreboard
  dependency" analysis above is still *architecturally* accurate — `load_hit_fu` is
  a single blocking slot, so two independent loads cannot overlap. It simply stopped
  being *observable* once the penalty dropped to 1 cycle, because a 1-cycle blocking
  unit and a 1-cycle scoreboard dependency are indistinguishable in cycle count.
  A workload with two independent loads and enough independent work between them
  would still expose it. Not worth fixing until such a workload matters.

### B6. Branch-misprediction flush penalty one cycle too cheap *(IDENTIFIED — not yet fixed)*
- **Symptom:** `robust_stress` (branch-heavy: multiply-accumulate driven by an LCG
  random number generator, deliberately unpredictable) — VP IPC **0.704** vs CVA6
  **0.685**, i.e. the VP is **+2.8% optimistic** (it runs *faster* than the RTL).
- **Diagnosis — the counters isolate it exactly.** No new benchmark was needed; the
  existing HPM counters separate *rate* from *penalty*:

  | Quantity | CVA6 | VP |
  |---|---:|---:|
  | Instructions retired | 3,127,046 | 3,127,063 |
  | Cycles | 4,567,925 | 4,442,704 |
  | Branch mispredicts (HPM4) | 125,498 | 125,500 |

  Mispredict **counts agree to 2 parts in 125,000** → the branch predictor's
  *accuracy* is modelled correctly, so the gap is not a prediction-rate error.
  The cycle shortfall is `4,567,925 − 4,442,704 = 125,221`, and

  > **125,221 missing cycles ÷ 125,498 mispredicts = 0.998 cycles per mispredict**

  — **exactly one cycle.** The VP's misprediction recovery is one cycle shorter
  than CVA6's. This accounts for **100%** of the residual.
- **Root cause (proposed):** the VP flushes and redirects from EX in one cycle
  (`flush_pipeline` → `prev_cycle_flush` → refetch). Real CVA6 takes one additional
  cycle to steer the frontend after the flush — the same frontend-redirect latency
  already modelled for *correctly-predicted taken* branches (§B2), which was never
  applied on the mispredict path.
- **Proposed fix:** charge one extra bubble on flush, in the same place §B2's
  `taken_redirect_bubble` is consumed in `PCGen_stage`. Expected effect:
  robust_stress 0.704 → ~0.685. Must be validated against the controls — the
  arithmetic benchmarks have 3–5 mispredicts each, so they should not move.
- **⚠ Not yet implemented or measured.** The `0.998 cyc/mispredict` figure is an
  arithmetic decomposition of *one* benchmark, not a discriminating experiment. It
  is consistent with a 1-cycle flush penalty, but `robust_stress` also contains
  loads/stores and 500,500 branches, so a confound (e.g. a taken-branch bubble
  interacting with flushes) is not excluded. The honest test is to implement the
  bubble and confirm (a) robust_stress lands on 0.685 and (b) all seven other
  benchmarks stay put.
- **Supersedes the retracted "lockstep frontend" explanation** (see §D), which was
  wrong on both mechanism and sign.

### B7. Memory-bound: VP over-serialises the D-cache — and the CVA6 cache was mischaracterised *(cause 1 FIXED in §B8; cause 2 OPEN)*
- **Symptom:** on a purpose-built memory-thrash benchmark (`mem_contention_bench`:
  32 KB unrolled read body > 16 KB I$, sweeping a 64 KB region > 32 KB D$, so both
  L1s miss heavily and *concurrently*), the VP runs **much slower** than the RTL:

  | mem_contention | Instr | Cycles | IPC | I$ miss | D$ miss |
  |---|---:|---:|---:|---:|---:|
  | CVA6 co-sim (RTL) | 1,638,463 | 4,334,798 | 0.378 | 332,537 | 167,093 |
  | Fast VP (CYCLE6)  | 1,638,480 | 5,699,038 | 0.288 | 405,627 | 204,804 |
  | CYCLE6_AXI (arb.) | 1,638,480 | 6,018,449 | 0.272 | 405,627 | 204,804 |

  The VP **over-counts cycles by +31%** before any contention modelling.
- **Two distinct root causes:**
  1. **Over-missing ~22% — FIXED (§B8).** Was a replacement-policy mismatch: the
     VP used LRU, CVA6 uses LFSR random. After the fix, miss counts match RTL to
     within 0.1% (I$ 332,743, D$ 166,923) and the fast-VP gap fell +31% → +18%.
  2. **The VP's memory model is too serial vs CVA6's memory-level parallelism —
     OPEN.** The blocking is on the *VP* side, not CVA6's. The VP has a single
     `dcache_miss_fu` slot, flat ~10-cyc penalty, one miss at a time, and **no
     overlap between I$ and D$ misses**. CVA6-WT overlaps: (a) stores via an
     **8-entry coalescing write buffer** (`WtDcacheWbufDepth=8`,
     `MaxOutstandingStores=7`); (b) **I$ and D$ refills are outstanding
     concurrently on the AXI** — `wt_axi_adapter.sv` carries *separate*
     `icache_rtrn_tid` / `dcache_rtrn_tid`, so their miss latencies overlap
     rather than serialise; (c) a single read MSHR + 2 load-buffer entries per
     cache. **CVA6 is NOT a blocking cache.**
- **⚠ CVA6 cache mischaracterisation (correct the docs AND the paper in revision).**
  The co-sim config `cv64a6_imafdc_sv39` selects `DcacheType = config_pkg::WT`
  (write-through). `wt_dcache_missunit.sv` has a **single** read MSHR (`mshr_q`,
  one register — not the multi-entry MSHR array of `std_nbdcache`/`hpdcache`).
  So CVA6-WT is **NOT** a "non-blocking MSHR cache that pipelines concurrent
  misses" — that describes CVA6's *other* (unused) cache options. The paper's
  §IV-D, §VI-E(3) and §VII assert non-blocking/MSHR; that wording is inaccurate
  for the WT config and should be revised to "write-through cache with an
  8-entry write buffer and a single read MSHR." (Paper submitted for initial
  review 2026-08; fold the fix into the post-review revision.)
- **AXI contention experiment (negative result).** A real matchlib `AxiArbiter`
  I$/D$ port-contention model was built as an isolated opt-in build
  (`TIMING_MODEL=CYCLE6_AXI`; vendored matchlib in `third_party/matchlib_kit`,
  builds against the VP's own SystemC 3.0.2). It is *mechanically correct*
  (arbitrates, functionally bit-identical, default build byte-for-byte unchanged)
  but moves accuracy the **wrong way**, and the reason is now confirmed at the
  mechanism level, not just empirically: **its premise is backwards.** Contention
  assumes I$ and D$ *compete* for one port and serialise; the RTL gives them
  *separate AXI transaction IDs* (`wt_axi_adapter.sv`) so they *overlap*. Real
  CVA6 has **more** memory-level parallelism than the VP, not less. Post-§B8
  (LFSR) three-way makes this stark:

  | mem_contention | Cycles | vs RTL |
  |---|---:|---:|
  | CVA6 RTL (overlaps I$/D$)      | 4,334,798 | — |
  | Fast VP LFSR (serialises)      | 5,131,111 | +18.4% |
  | CYCLE6_AXI LFSR (forces serial)| 5,408,510 | +24.8% |

  The arbiter (least parallel) is furthest from the RTL (most parallel). **Keep
  CYCLE6_AXI as an optional build; do not enable by default and do not pursue it
  for memory-bound accuracy.** The correct direction for the residual is the
  *opposite* — give the VP overlapping/outstanding misses (I$ ∥ D$ + write
  buffer), which would pull 5.13M *down* toward 4.33M.
- **Note:** the register-only microbenchmarks never expose this — they are
  cache-resident (≈1 D$ miss), so fast VP, CYCLE6_AXI and co-sim all agree there.
  A both-caches-thrashing workload is required to see it.

### B8. Cache replacement was LRU, but CVA6 uses LFSR random — VP over-missed ~22% *(FIXED)*
- **This is the root cause of B7 cause (1)** (the ~22% miss-count gap). Resolved.
- **Root cause:** `inc/Cache.h` modelled **LRU** replacement; both CVA6 L1 caches
  use **8-bit LFSR pseudo-random** replacement (fill an invalid way first, else
  evict a random way):
  - I$: `cva6_icache.sv:389-391` — *"chose random replacement if all are valid"*,
    `repl_way = all_ways_valid ? rnd_way : inv_way`, `rnd_way` from an LFSR.
  - D$ (WT): `wt_dcache_missunit.sv:200-210` — `lfsr #(.LfsrWidth(8))`,
    `repl_way = all_ways_valid ? rnd_way : inv_way`.
- **Why it mattered:** for a sequential scan of a working set larger than the
  cache, **LRU is pathologically pessimal** — it evicts exactly the line about to
  be reused next pass, so ~100% of accesses miss. Random replacement statistically
  retains a fraction, so it misses less. The geometry was already identical
  (I$ 16 KB/4-way/16 B, D$ 32 KB/8-way/16 B), so replacement was the *only*
  difference — and it produced a systematic +22% miss bias on reuse-with-eviction.
- **Why it hid:** the validated streaming test (§VI-D of the paper) is
  compulsory-miss-only (single sweep), where LRU and random give identical counts;
  register-only microbenchmarks are cache-resident (≈0 misses). Only a
  re-swept-working-set workload (`mem_contention_bench`) exposes it.
- **Fix:** `Cache.h` now fills the first invalid way, else evicts
  `lfsr % WAYS` and advances an 8-bit Fibonacci LFSR (taps x^8+x^6+x^5+x^4+1),
  one LFSR shared across sets — mirroring CVA6's single-LFSR scheme and its
  `update_lfsr = cache_wren & all_ways_valid` timing. `lru_time` removed.
- **Result — miss counts now match RTL to within 0.1%:**

  | mem_contention | I$ miss | D$ miss | Cycles | IPC |
  |---|---:|---:|---:|---:|
  | CVA6 RTL          | 332,537 | 167,093 | 4,334,798 | 0.378 |
  | VP LRU (before)   | 405,627 | 204,804 | 5,699,038 | 0.288 |
  | **VP LFSR (after)** | **332,743** | **166,923** | **5,131,111** | **0.319** |

  I$ +0.06%, D$ −0.10% (were +22% / +23%). The cycle gap to RTL shrank from
  **+31.5% → +18.4%**; the residual is now attributable to B7 cause (2) (the VP's
  fully-blocking D-cache vs CVA6-WT's 8-entry write buffer + single read MSHR),
  no longer confounded by miss counts.
- **Zero regression:** all validated microbenchmarks bit-identical after the swap
  (int_alu 2,400,256; int_nobranch 3,081,167; int_indep 2,249,992; robust_stress
  4,442,265) — their misses are cold/compulsory, hence replacement-independent.
- **⚠ Not bit-exact vs RTL by construction:** matching CVA6's exact miss *sequence*
  would need its precise LFSR seed/polynomial and per-cycle update timing. This
  removes the systematic LRU bias and lands within ~0.1% on counts; small
  per-workload differences from LFSR phase are expected and acceptable.
- **Paper implication (revision):** the SpMV 9.3% residual, attributed in §VI-E(3)
  to the blocking D-cache, is likely **substantially a replacement-policy artifact**
  (LRU vs random) — SpMV's irregular gather has heavy reuse-with-eviction. Re-run
  SpMV with this fix before committing to the "MSHR layer is highest priority"
  framing; the cheap correct fix (this) may absorb most of it.

---

## C. Measurement caveats discovered

- **Instruction-count difference (VP vs CVA6):** a fixed **~18–19 instruction**
  offset — CVA6 reports the `minstret` value read *inside* the program (before the
  halter writes); the VP counts to its actual halt (after the readout epilogue).
  Not an execution difference. Earlier scattered deltas (+14/+46/+125) were
  `--max-instr` overshoot; adding an **HTIF halt** to every benchmark makes the
  offset a constant ~18–19.
- **The counter window differs, so epilogue traffic shows up only in the VP.**
  Each benchmark samples its counters with `csrr` *inside* the program, then runs a
  readout epilogue (halter stores + the HTIF halt store). CVA6's counters are frozen
  at the `csrr`; the VP keeps counting to its actual halt. This produces the constant
  ~18-20 instruction offset **and** small phantom D$ deltas: `int_alu` is
  register-only, so CVA6 reports **0** D$ accesses while the VP reports **1** —
  epilogue memory traffic CVA6 was never alive to see. Not a modelling difference.
  (Contrast `fp_bench`, whose `fld`s execute *before* the `csrr` and so are counted
  by both — once §A2 was fixed.)
- **HPM "access" counters are not directly comparable.** CVA6 I$/D$ *access* events
  count fetch/port *activity* (speculative, multi-port OR), so they exceed retired
  instructions and differ from the VP's exact software counters. Compare **miss
  counts** and **loads/stores**, not the "access" denominators.
- **The co-sim has no main-memory model at all — despite the name `dram`.** The
  instance at `0x80000000–0x8FFFFFFF` (`cva6_matchlib_system.h:41,87`) is a
  `SlaveFromFile<axi::cfg::standard>`, a matchlib **testbench** class whose storage
  is a `std::map<Addr,Data>` (`axi/testbench/SlaveFromFile.h:65`). It has **no
  latency, wait-state or delay parameter**; its only `wait()` calls are AXI
  handshake ticks. From the core it behaves as an idealized zero-wait-state SRAM.
  It is **not DRAM**, and the variable name is misleading.
  - Do not confuse this with the `tc_sram_wrapper` modules seen when grepping for
    "sram" — those are the tag/data arrays **inside** CVA6's L1 I$/D$, not memory.
  - Consequence: co-sim cache misses are nearly free. The VP by contrast applies a
    flat 107-cycle miss penalty (`icache/dcache_miss_penalty`), which is why the VP
    is *pessimistic* on miss-heavy code — and why §B5's memory constants are fitted
    to a testbench stub rather than to hardware.
- **Branch counts differ by definition** (CVA6 counts jumps/calls/returns as
  branches; the VP does not) — compare mispredict *rate*, not raw counts.

---

## D. Accuracy summary

All rows below were re-measured in a single session against the **same `-O1` ELFs**
on both simulators. Instruction counts differ by a constant 17–20 (readout epilogue,
see §C).

| Unit tested | Benchmark | CVA6 IPC | VP IPC | IPC err | Status |
|---|---|---:|---:|---:|---|
| Integer ALU (looped) | int_alu | 0.917 | 0.916 | −0.0% | ✅ B2 |
| Integer ALU (dependency chain, branch-free) | int_nobranch | 0.833 | 0.831 | −0.2% | ✅ B4 |
| Integer ALU (independent ops, branch-free) | int_indep | 0.998 | 0.996 | −0.2% | ✅ control |
| Integer divide | div_stress | 0.165 | 0.166 | +0.3% | ✅ B1 |
| Load/store + MUL | long_test3 | 0.727 | 0.727 | −0.0% | ✅ B5 |
| Branch mispredict recovery | robust_stress | 0.685 | 0.704 | **+2.8%** | ⚠ **B6 open** |
| FP add/mul | fp_bench | 0.500 | 0.518 | +3.6% | ✅ B3 |
| FP add/mul/div/sqrt | fp_stress | 0.145 | 0.153 | +5.1% | ✅ B3 |

**Five of eight benchmarks agree with the RTL to within 0.3%; the worst is +5.1%.**

The three residuals are each understood:
- **robust_stress (+2.8%)** — one missing bubble on mispredict recovery (§B6),
  quantified at 0.998 cyc/mispredict. Not yet fixed.
- **fp_bench (+3.6%), fp_stress (+5.1%)** — the FPnew latency model is approximate
  (ADDMUL depth 4, DIV 20, SQRT 22 are fitted, not derived from the RTL). No
  isolated per-FP-op latency experiment has been run.

### Raw cycle/instruction data (for reproduction)
| Benchmark | CVA6 instr | CVA6 cycles | VP instr | VP cycles |
|---|---:|---:|---:|---:|
| int_alu | 2,200,041 | 2,400,417 | 2,200,061 | 2,400,714 |
| int_nobranch | 2,566,039 | 3,080,384 | 2,566,059 | 3,086,804 |
| int_indep | 2,245,043 | 2,249,473 | 2,245,066 | 2,254,211 |
| div_stress | 186,659 | 1,130,126 | 186,678 | 1,126,502 |
| long_test3 | 800,038 | 1,100,413 | 800,056 | 1,100,602 |
| robust_stress | 3,127,046 | 4,567,925 | 3,127,063 | 4,442,704 |
| fp_bench | 1,300,059 | 2,600,478 | 1,300,077 | 2,510,207 |
| fp_stress | 900,047 | 6,200,440 | 900,065 | 5,900,619 |

### Benchmark design notes
- `int_nobranch` / `int_indep` are **1024×-unrolled** so the loop branch is <0.1% of
  instructions, while the ~4–5 KB code footprint stays inside the 16 KB I-cache
  (I$ misses ≈ code_bytes ÷ line_size: CVA6 324×16 B ≈ VP 85×64 B ≈ 5.3 KB ✓ — an
  independent confirmation that both cache models are correct).
- `div_stress` still contains one data-dependent branch (`if (a<0) a = -a;`),
  producing ~4,000 mispredicts. Harmless (division dominates ~83% of cycles; VP
  3,999 vs CVA6 3,995) but it is the one impurity in the arithmetic set.

### Remaining known gaps
| Workload | CVA6 | VP | Error | Cause |
|---|---:|---:|---:|---|
| Branch-heavy (robust_stress) | 0.685 | 0.793 | **+16%** | **cause not established** — see below |
| Tiny (cam_bench) | 0.682 | 0.195 | −71% | too small — cold-start dominated (not a valid timing measurement) |

**On robust_stress — a retracted explanation.** This was previously attributed to a
"lockstep frontend" that propagates stalls the real CVA6 absorbs in its instruction
queue. That explanation is **wrong on two counts**: (1) the VP *does* model the
4-entry decoupled instruction queue (`fetch_queue`, `CPU_P64_6_Cycle.cpp:166`), and
(2) the sign is backwards — a missing queue would make the VP **pessimistic**, but
the VP is **optimistic** here (it runs *faster* than the RTL). No amount of missing
decoupling can make a model too fast.

Leading hypotheses, untested:
- **Branch predictor mismatch.** robust_stress feeds its branch from an LCG *in
  order to* defeat prediction, so mispredict rate × flush penalty dominates its
  cycle count. If the VP mispredicts less often, or recovers more cheaply, it gains
  cycles exactly where this benchmark spends them. Test: compare mispredict counts
  first (isolates *rate*), then cycles-per-mispredict (isolates *penalty*).
- **Uncalibrated MUL / store buffer.** robust_stress is mul+accumulate. Neither
  unit has an isolated microbenchmark of the kind built for ALU, divide and FPU.

Until a discriminating experiment separates these, this row states a measurement,
not a diagnosis.
