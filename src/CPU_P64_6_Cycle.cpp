// SPDX-License-Identifier: GPL-3.0-or-later
#include "CPU_P64_6_Cycle.h"
#include "CLINT.h"
#include "spdlog/spdlog.h"
#include <iostream>

uint64_t global_cpu_cycle = 0;

namespace riscv_tlm {

static inline int clz64(uint64_t val) {
  if (val == 0) return 64;
  int n = 0;
  if ((val & 0xFFFFFFFF00000000ULL) == 0) { n += 32; val <<= 32; }
  if ((val & 0xFFFF000000000000ULL) == 0) { n += 16; val <<= 16; }
  if ((val & 0xFF00000000000000ULL) == 0) { n += 8;  val <<= 8;  }
  if ((val & 0xF000000000000000ULL) == 0) { n += 4;  val <<= 4;  }
  if ((val & 0xC000000000000000ULL) == 0) { n += 2;  val <<= 2;  }
  if ((val & 0x8000000000000000ULL) == 0) { n += 1;  val <<= 1;  }
  return n;
}

CPURV64P6_Cycle::CPURV64P6_Cycle(sc_core::sc_module_name const &name,
                                 BaseType PC, bool debug)
    : CPU(name, debug) {

  // Initialize Register Bank and Memory Interface
  register_bank = new Registers<BaseType>();
  mem_intf = new MemoryInterface();

  // Set Initial State
  register_bank->setPC(PC);
  // Initialize Stack Pointer (SP) to the top of memory (minus 8 bytes for
  // alignment)
  register_bank->setValue(Registers<BaseType>::sp, Memory::SIZE - 8);
  // Initialize Global Pointer (GP) for bare-metal global variables
  // GP = 0x10FF3: places buffer (gp-2035) at 0x10800 — 4-byte aligned, above
  // .text end (0x10722)
  register_bank->setValue(Registers<BaseType>::gp, 0x10FF3);
  int_cause = 0;

  // Register callback for DMI invalidations from the bus
  instr_bus.register_invalidate_direct_mem_ptr(this,
                                               &CPU::invalidate_direct_mem_ptr);

  // Initialize ISA Extensions
  base_inst = new BASE_ISA<BaseType>(0, register_bank, mem_intf);
  a_inst = new A_extension<BaseType>(0, register_bank, mem_intf);
  c_inst = new C_extension<BaseType>(0, register_bank, mem_intf);
  m_inst = new M_extension<BaseType>(0, register_bank, mem_intf);
  f_inst = new F_extension<BaseType>(0, register_bank, mem_intf);
  d_inst = new D_extension<BaseType>(0, register_bank, mem_intf);

  next_pc = PC; // Initialize the Next Program Counter
  store_drain_remaining = 0;

  // MMU: initialized here so it can use the already-constructed mem_intf.
  mmu = new MMU(mem_intf, /*ptw_mem_cycles=*/2);

  // Start the main simulation thread
  SC_THREAD(cycle_thread);

  logger->info("Created CPURV64P6_Cycle (6-stage CVA6-aligned) CPU");
}

CPURV64P6_Cycle::~CPURV64P6_Cycle() {
  delete register_bank;
  delete mem_intf;
  delete base_inst;
  delete a_inst;
  delete c_inst;
  delete m_inst;
  delete f_inst;
  delete d_inst;
  delete mmu;
}

void CPURV64P6_Cycle::set_clock(sc_core::sc_clock *c) {
  clk = c;
  if (clk)
    clock_period = clk->period();
#ifdef ENABLE_AXI_CONTENTION
  // Build the AXI contention subsystem now that the clock exists. Elaboration
  // happens here (before sc_start), which is legal during module setup.
  if (clk && !axi_top) {
    axi_top = new riscv_axi::AxiContentionTop(clk, axi_slave_latency);
  }
#endif
}

// =============================================================================
// Process
// =============================================================================

void CPURV64P6_Cycle::cycle_thread() {
  // Initialize performance statistics
  stats.cycles = 0;
  stats.instructions = 0;

  // Reserve space for pipeline trace to avoid reallocations
  pipeline_trace.reserve(trace_limit);

  bool prev_cycle_flush = false;

  // --- Main Simulation Loop ---
  while (true) {
    // WFI fast-forward: jump CLINT mtime to mtimecmp in one step,
    // then wait a single clock edge for the signal to propagate.
    if (wfi_stall) {
      if (clint_ptr) {
        clint_ptr->fast_forward_to_deadline();
      }
      // Per RISC-V spec WFI wakes when any interrupt is pending.
      // Linux RISC-V idle pattern: WFI executes with SIE=0, then
      // "csrsi sstatus,2" at WFI+4 re-enables SIE so STIP fires.
      // So: if csr.mip & csr.mie != 0, wake immediately (wfi_stall=false)
      // and let the pipeline resume at WFI+4 to enable SIE.
      bool csr_irq_pending = (csr.mip & csr.mie) != 0 || timer_irq_in.read() ||
                             msip_irq_in.read() || ext_irq_in.read();
      if (!csr_irq_pending) {
        // Wait for any IRQ signal to change.
        // Timeout matches CLINT_FF_GUARD ticks (100 ms) so after fast-forward
        // we wait ~100 ms of simtime before the timer fires.  This gives the
        // kernel and user processes ~10 M CPU cycles between interrupts.
        sc_core::wait(sc_core::sc_time(100000, sc_core::SC_US),
                      timer_irq_in.value_changed_event() |
                          msip_irq_in.value_changed_event() |
                          ext_irq_in.value_changed_event());
      } else {
        wfi_stall = false;
      }
      // Re-sync to the next clock edge
      if (clk)
        sc_core::wait(clk->posedge_event());
      else
        sc_core::wait(clock_period);
    } else {
      // Synchronize with the SystemC simulation kernel
      if (clk) {
        sc_core::wait(clk->posedge_event());
      } else {
        // Fallback if clock is not bound (should not happen in proper VP setup)
        sc_core::wait(clock_period);
      }
    }

    // Reset per-cycle commit capture
    commit_count_this_cycle = 0;
    committed_pc_this_cycle = 0;
    pending_csr_write.valid = false;
    global_cpu_cycle = stats.cycles;

    // --- Pipeline Latch Transfer ---
    // Move data from "Next" latches to "Current" latches to simulate clock edge
    // updates. When stall_ex is set (partial store-buffer overlap), keep ALL
    // latches unchanged so the load retries and upstream stages freeze.
    if (prev_cycle_flush) {
      issue_ex_reg.valid = false;
      id_issue_reg.valid = false;
      fetch_id_reg.valid = false;
      pcgen_fetch_reg.valid = false;
    } else {
      // 1. Backend Transfer (EX Stage Latch)
      if (!stall_ex) {
        issue_ex_reg = issue_ex_next;
      }

      // 2. Midstage Transfer (Issue Stage Latch)
      if (!stall_issue) {
        id_issue_reg = id_issue_next;
      }

      // 3. Frontend Decoupled FIFO Transfer (Push to queue)
      if (fetch_id_next.valid) {
        if (fetch_queue.size() < FETCH_QUEUE_CAPACITY) {
          fetch_queue.push_back({
            fetch_id_next.pc,
            fetch_id_next.instr,
            fetch_id_next.is_compressed,
            fetch_id_next.predicted_taken,
            fetch_id_next.predicted_target,
            fetch_id_next.is_ras_call,
            fetch_id_next.is_ras_return
          });
        }
        fetch_id_next.valid = false;
      }

      // 4. Decode Stage Latch Transfer (Pop from queue)
      if (!stall_issue) {
        if (!fetch_queue.empty()) {
          auto entry = fetch_queue.front();
          fetch_id_reg.pc = entry.pc;
          fetch_id_reg.instr = entry.instr;
          fetch_id_reg.is_compressed = entry.is_compressed;
          fetch_id_reg.predicted_taken = entry.predicted_taken;
          fetch_id_reg.predicted_target = entry.predicted_target;
          fetch_id_reg.is_ras_call = entry.is_ras_call;
          fetch_id_reg.is_ras_return = entry.is_ras_return;
          fetch_id_reg.valid = true;
          fetch_queue.erase(fetch_queue.begin());
        } else {
          fetch_id_reg.valid = false;
          diag_if_empty_cycles++; // TEMP DIAGNOSTIC: true RTL if_empty equivalent
        }
      }
    }

    // 5. Fetch Stage Latch Transfer
    pcgen_fetch_reg = pcgen_fetch_next;

    // --- Sync CLINT interrupt lines into mip ---
    if (timer_irq_in.read())
      csr.mip |= (1ULL << 7); // MTIP
    else
      csr.mip &= ~(1ULL << 7);
    if (msip_irq_in.read())
      csr.mip |= (1ULL << 3); // MSIP
    else
      csr.mip &= ~(1ULL << 3);
    if (ext_irq_in.read())
      csr.mip |= (1ULL << 9); // SEIP (supervisor external)
    else
      csr.mip &= ~(1ULL << 9);

    // Process any pending and enabled interrupts
    cpu_process_IRQ();

    // --- Execute Pipeline Stages (Reverse Order) ---
    // Executing in reverse order allows stages to read the state produced by
    // the previous stage in the CURRENT cycle (before it gets overwritten),
    // simulating parallel hardware.

    // 6. Commit: Retire instructions and update architectural state.
    Commit_stage();

    // 5. Execute: Perform ALU operations and memory address calculations.
    EX_stage();

    // Capture flush flag here: EX_stage() sets it, PCGen_stage() will reset it.
    // Must be captured before PCGen_stage() clears it so the trace records
    // correctly.
    bool this_cycle_flush = flush_pipeline;

    // 4. Issue: Dispatch instructions to execution units and allocate ROB
    // entries.
    Issue_stage();

    // 3. Decode: Decode instructions and read register operands.
    ID_stage();

    // 2. Fetch: Retrieve instructions from memory.
    Fetch_stage();

    // 1. PC Generation: Determine the next Program Counter.
    // NOTE: PCGen_stage() resets flush_pipeline to false — use this_cycle_flush
    // for trace.
    PCGen_stage();

    // Update global cycle count and CSR hardware counters
    stats.cycles++;
    if (commit_count_this_cycle == 0) { // TEMP DIAGNOSTIC
      diag_zero_commit_cycles++;
      diag_stall_run_len++;
    } else if (diag_stall_run_len > 0) {
      diag_stall_run_count++;
      diag_stall_run_sum += diag_stall_run_len;
      if (diag_stall_run_len > diag_stall_run_max) diag_stall_run_max = diag_stall_run_len;
      diag_stall_run_len = 0;
    }

    csr.tick_counters(commit_count_this_cycle);



    // --- Pipeline Trace Recording ---
    // Record the state of every pipeline stage for visualization.
    // Only record up to trace_limit entries to bound memory usage.
    {
      PipelineTraceEntry te;
      te.cycle = stats.cycles;
      te.pc_pcgen = pcgen_fetch_next.valid ? pcgen_fetch_next.pc : 0;
      te.pc_fetch = pcgen_fetch_reg.valid ? pcgen_fetch_reg.pc : 0;
      te.pc_id = fetch_id_reg.valid ? fetch_id_reg.pc : 0;
      te.pc_issue = id_issue_reg.valid ? id_issue_reg.pc : 0;
      te.pc_ex = issue_ex_reg.valid ? issue_ex_reg.pc : 0;
      te.pc_commit = committed_pc_this_cycle;
      te.pcgen_valid = pcgen_fetch_next.valid;
      te.fetch_valid = pcgen_fetch_reg.valid;
      te.id_valid = fetch_id_reg.valid;
      te.issue_valid = id_issue_reg.valid;
      te.ex_valid = issue_ex_reg.valid;
      te.commit_valid = commit_count_this_cycle;
      te.is_stall_pcgen = stall_pcgen;
      te.is_stall_fetch = stall_fetch;
      te.is_stall_issue = stall_issue;
      te.is_flush = this_cycle_flush;

      if (pipeline_trace.size() < trace_limit) {
        pipeline_trace.push_back(te);
      } else {
        pipeline_trace[trace_idx] = te;
      }
      trace_idx = (trace_idx + 1) % trace_limit;
    }

    // --- Termination Logic ---
    // Stop simulation if the pipeline is completely empty and no new
    // instructions are being fetched. We add a grace period (> 100 cycles) to
    // allow the pipeline to fill up initially. During a pipeline flush/redirect
    // or while in WFI stall waiting for interrupts, the pipeline may be
    // temporarily empty but we must NOT terminate the simulation.
    if (stats.cycles > 100 && !this_cycle_flush && !wfi_stall &&
        !pcgen_fetch_reg.valid &&
        !pcgen_fetch_next
             .valid && // Ensure no new instruction is being generated
        !fetch_id_reg.valid &&
        !id_issue_reg.valid && !issue_ex_reg.valid && scoreboard.is_empty()) {

      std::cout << "Pipeline Empty & ROB Empty. Stopping Simulation."
                << std::endl;
      std::cout << "  mcause: 0x" << std::hex << csr.mcause << " | mepc: 0x"
                << csr.mepc << " | mtval: 0x" << csr.mtval << " | mstatus: 0x"
                << csr.mstatus << std::dec << std::endl;
      printStats();
      sc_core::sc_stop();
      break;
    }

    prev_cycle_flush = this_cycle_flush;
  }
}

// =============================================================================
// PCGen Stage (Program Counter Generation)
// =============================================================================

void CPURV64P6_Cycle::PCGen_stage() {
  // 1. Check for Flush/Redirect from EX Stage (Highest Priority)
  // If a branch/jump misprediction or exception occurred, we must redirect the
  // PC immediately.
  if (flush_pipeline) {
    next_pc = pc_redirect_target;
    flush_pipeline = false;
    pc_redirect_valid = false;
    trap_pending = false; // Trap redirect consumed
    taken_redirect_bubble = false; // flush supersedes any pending redirect bubble
    pcgen_fetch_next.valid = false;
    
    // Decoupled frontend: flush instruction fetch queue and pipeline stages
    fetch_queue.clear();
    fetch_id_next.valid = false;
    id_issue_next.valid = false;
    issue_ex_next.valid = false;
    return;
  }

  if (wfi_stall) {
    stall_fetch = true;
    stall_pcgen = true;
    pcgen_fetch_next.valid = false;
    return;
  }

  // 2. Stall Check
  // Decoupled frontend: stall PCGen if instruction queue is full
  if (fetch_queue.size() >= FETCH_QUEUE_CAPACITY) {
    stall_pcgen = true;
    return;
  }

  // If a structural hazard or backpressure exists (e.g., ROB full), do not
  // update the PC.
  if (stall_pcgen) {
    // Keep current state (effectively repeating the same PC for next cycle if
    // we were outputting it) or just doing nothing creates a bubble if not
    // careful, but here 'next_pc' is persistent state.
    return;
  }

  // Taken-branch redirect bubble: a correctly-predicted taken branch/jump costs
  // one fetch cycle to steer the frontend to the target (CVA6 BTB redirect
  // latency). Consume it here as a one-cycle bubble; next_pc already = target.
  if (taken_redirect_bubble) {
    taken_redirect_bubble = false;
    pcgen_fetch_next.valid = false;
    stats.stalls++;
    return;
  }

  // 3. Normal Operation
  uint64_t current_pc = next_pc;
  pcgen_fetch_next.pc = current_pc;
  pcgen_fetch_next.valid = true;

  // --- Branch Prediction (CVA6-Aligned) ---
  uint32_t btb_idx = (current_pc >> 2) % 32; // Word-aligned indexing
  uint32_t bht_idx = (current_pc >> 2) % 128;

  bool predict_taken = false;
  uint64_t pt_target = 0;

  if (btb[btb_idx].valid && btb[btb_idx].tag == current_pc) {
    if (btb[btb_idx].is_return) {
      // RAS Prediction
      if (!ras.empty()) {
        pt_target = ras.back();
        ras.pop_back(); // Speculatively pop
        predict_taken = true;
      }
    } else if (bht[bht_idx] >= 2) {
      // Predict Taken
      pt_target = btb[btb_idx].target;
      predict_taken = true;
    }
  }

  pcgen_fetch_next.predicted_taken = predict_taken;
  pcgen_fetch_next.predicted_target = pt_target;

  if (predict_taken) {
    next_pc = pt_target;
    taken_redirect_bubble = true; // charge 1 fetch bubble to redirect to target
  } else {
    next_pc = current_pc + 4; // Default increment
  }
}

// =============================================================================
// Fetch Stage
// =============================================================================

void CPURV64P6_Cycle::Fetch_stage() {
  // Flush: cancel pending I$ miss and inject bubble.
  if (flush_pipeline) {
    icache_miss_remaining = 0;
#ifdef ENABLE_AXI_CONTENTION
    // Release any in-flight I$ refill so the master drains cleanly (its read
    // completes and is discarded; ack drops req so it won't stall on us).
    if (icache_axi_pending) {
      axi_top->ack(riscv_axi::AxiContentionTop::ICACHE);
      icache_axi_pending = false;
    }
#endif
    fetch_id_next.valid = false;
    return;
  }

  // Decoupled frontend: stall Fetch if instruction queue is full
  if (fetch_queue.size() >= FETCH_QUEUE_CAPACITY) {
    stall_fetch = true;
    return;
  }

  if (stall_fetch) {
    return; // Retain current output state
  }

  if (!pcgen_fetch_reg.valid) {
    fetch_id_next.valid = false;
    return;
  }

  // --- I$ miss penalty countdown ---
  // While counting down, hold the PC (stall_pcgen) and inject bubbles
  // downstream. When the counter reaches 0 we fall through and re-fetch (the
  // cache line is now warm).
#ifdef ENABLE_AXI_CONTENTION
  // AXI mode: an in-flight I$ refill completes when the arbiter+slave return
  // the read. Poll done(); on completion, ack and fall through to re-fetch.
  if (icache_axi_pending) {
    if (axi_top->done(riscv_axi::AxiContentionTop::ICACHE)) {
      axi_top->ack(riscv_axi::AxiContentionTop::ICACHE);
      icache_axi_pending = false;
      // fall through: cache line now warm, re-fetch this cycle
    } else {
      stats.icache_miss_cycles++;
      stall_pcgen = true;
      fetch_id_next.valid = false;
      return;
    }
  }
#else
  if (icache_miss_remaining > 0) {
    icache_miss_remaining--;
    stats.icache_miss_cycles++;
    stall_pcgen = true;
    fetch_id_next.valid = false;
    return;
  }
#endif

  uint64_t current_pc = pcgen_fetch_reg.pc;
  uint32_t instr = 0;

  // --- ITLB / MMU translation (Phase 8) ---
  // When sv39 is active and not in M-mode, translate the instruction VA to PA.
  uint64_t fetch_pa = current_pc;
  {
    auto tr = mmu->translate(current_pc, csr, MMU::FETCH);
    if (tr.fault) {
      // Instruction page fault — redirect to trap handler.
      // tval = faulting virtual address (current_pc) per RISC-V spec.
      uint64_t tvec =
          csr.take_exception(tr.fault_cause, current_pc, current_pc);
      pc_redirect_target = tvec;
      pc_redirect_valid = true;
      flush_pipeline = true;
      trap_pending = true;
      fetch_id_next.valid = false;
      return;
    }
    if (tr.stall_cycles > 0) {
      // ITLB miss: add PTW latency to the I$ miss countdown.
      icache_miss_remaining += tr.stall_cycles;
      stats.itlb_miss_cycles += static_cast<uint64_t>(tr.stall_cycles);
      stall_pcgen = true;
      fetch_id_next.valid = false;
      return;
    }
    fetch_pa = tr.paddr;
    if (!csr.pmp_check(fetch_pa, 4, false, true)) {
      uint64_t tvec =
          csr.take_exception(CAUSE::INSTR_ACCESS, current_pc, current_pc);
      pc_redirect_target = tvec;
      pc_redirect_valid = true;
      flush_pipeline = true;
      trap_pending = true;
      fetch_id_next.valid = false;
      return;
    }
  }

  if (!fetch_instruction(fetch_pa, instr)) {
    fetch_id_next.valid = false;
    fetch_id_next.is_compressed = false;
    return;
  }

  // --- I$ hit/miss check ---
  if (!icache.access(current_pc)) { // I$ indexed by VA (VIPT)
    stats.icache_misses++;
#ifdef ENABLE_AXI_CONTENTION
    // Post the refill through the shared AXI port; latency (incl. contention
    // behind a D$ miss) emerges from the arbiter + slave, not a flat counter.
    axi_top->request(riscv_axi::AxiContentionTop::ICACHE, fetch_pa);
    icache_axi_pending = true;
#else
    icache_miss_remaining = icache_miss_penalty - 1;
#endif
    stats.icache_miss_cycles++;
    stall_pcgen = true;
    fetch_id_next.valid = false;
    return;
  }

  // --- I$ hit: deliver instruction to ID stage ---
  bool is_c = (instr & 0x3) != 0x3;
  fetch_id_next.pc = current_pc;
  fetch_id_next.instr = instr;
  fetch_id_next.is_compressed = is_c;
  fetch_id_next.valid = true;
  fetch_id_next.predicted_taken = pcgen_fetch_reg.predicted_taken;
  fetch_id_next.predicted_target = pcgen_fetch_reg.predicted_target;

  if (is_c && !pcgen_fetch_reg.predicted_taken)
    next_pc -= 2;
}

bool CPURV64P6_Cycle::fetch_instruction(uint64_t addr, uint32_t &data) {
  if (dmi_ptr_valid && addr >= dmi_start_addr && (addr + 4) <= dmi_end_addr) {
    std::memcpy(&data, dmi_ptr + (addr - dmi_start_addr), 4);
    return true;
  }

  tlm::tlm_generic_payload trans;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

  trans.set_command(tlm::TLM_READ_COMMAND);
  trans.set_address(addr);
  trans.set_data_ptr(reinterpret_cast<unsigned char *>(&data));
  trans.set_data_length(4);
  trans.set_streaming_width(4);
  trans.set_byte_enable_ptr(nullptr);
  trans.set_dmi_allowed(false);
  trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

  instr_bus->b_transport(trans, delay);

  if (trans.is_response_error())
    return false;

  if (trans.is_dmi_allowed()) {
    tlm::tlm_dmi dmi_data;
    if (instr_bus->get_direct_mem_ptr(trans, dmi_data)) {
      dmi_ptr_valid = true;
      dmi_ptr = dmi_data.get_dmi_ptr();
      dmi_start_addr = dmi_data.get_start_address();
      dmi_end_addr = dmi_data.get_end_address();
    }
  }
  return true;
}

// =============================================================================
// ID Stage (Decode)
// =============================================================================

void CPURV64P6_Cycle::ID_stage() {
  // Check for Flush
  if (flush_pipeline) {
    id_issue_next.valid = false;
    return;
  }

  // Check for Backpressure from Issue Stage
  if (stall_issue) {
    return; // Hold current state
  }

  // Check Validity of Input
  if (!fetch_id_reg.valid) {
    id_issue_next.valid = false;
    return;
  }

  uint32_t instr = fetch_id_reg.instr;
  id_issue_next.pc = fetch_id_reg.pc;
  id_issue_next.instr = instr;
  id_issue_next.funct7 = 0;
  id_issue_next.imm = 0;
  id_issue_next.is_illegal = false;

  // Pass Branch Prediction Metadata
  id_issue_next.predicted_taken = fetch_id_reg.predicted_taken;
  id_issue_next.predicted_target = fetch_id_reg.predicted_target;
  id_issue_next.is_ras_call = false;
  id_issue_next.is_ras_return = false;

  // =========================================================================
  // Compressed (C-extension) Instruction Expansion
  // Expand 16-bit instruction to equivalent 32-bit decoded fields so that the
  // Issue and EX stages need no knowledge of compressed encoding.
  // =========================================================================
  if (fetch_id_reg.is_compressed) {
    id_issue_next.is_compressed = true;

    // Use lower 16 bits; upper bits may contain the next instruction's bytes
    uint16_t ci = static_cast<uint16_t>(instr & 0xFFFF);
    c_inst->setInstr(static_cast<uint32_t>(ci));
    op_C_Codes c_deco = c_inst->decode();

    switch (c_deco) {
    // ── Quadrant 0 ────────────────────────────────────────────────────
    case OP_C_ADDI4SPN: {
      // ADDI rd', x2, nzuimm — nzuimm=0 is illegal (spec §16.3)
      int64_t addi4spn_imm = static_cast<int64_t>(c_inst->get_imm_ADDI4SPN());
      if (addi4spn_imm == 0) {
        id_issue_next.is_illegal = true;
        id_issue_next.valid = true;
        return;
      }
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = c_inst->get_rdp();
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = addi4spn_imm;
      break;
    }
    case OP_C_LW:
      // LW rd', offset(rs1')
      id_issue_next.opcode = 0x03;
      id_issue_next.funct3 = 2;
      id_issue_next.rd = c_inst->get_rdp();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_L());
      break;
    case OP_C_LD:
      // LD rd', offset(rs1')  [RV64; also covers C.FLW in RV64 decoded as LD]
      id_issue_next.opcode = 0x03;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = c_inst->get_rdp();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CL());
      break;
    case OP_C_SW:
      // SW rs2', offset(rs1')
      id_issue_next.opcode = 0x23;
      id_issue_next.funct3 = 2;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_L());
      break;
    case OP_C_SD:
      // SD rs2', offset(rs1')  [RV64; also covers C.FSW in RV64 decoded as SD]
      id_issue_next.opcode = 0x23;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CL());
      break;
    case OP_C_FLD:
      // FLD rd', offset(rs1')
      id_issue_next.opcode = 0x07;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = c_inst->get_rdp();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CL());
      break;
    case OP_C_FSD:
      // FSD rs2', offset(rs1')
      id_issue_next.opcode = 0x27;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CL());
      break;

    // ── Quadrant 1 ────────────────────────────────────────────────────
    case OP_C_NOP:
      // NOP → ADDI x0, x0, 0
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = 0;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = 0;
      break;
    case OP_C_ADDI:
      // ADDI rd, rd, nzimm
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = c_inst->get_rs1(); // bits[11:7] = rd
      id_issue_next.rs1 = c_inst->get_rs1();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_ADDI());
      break;
    case OP_C_ADDIW:
      // ADDIW rd, rd, imm  [RV64 — C.JAL maps here in RV64]
      id_issue_next.opcode = 0x1B;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = c_inst->get_rs1();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_ADDI());
      break;
    case OP_C_LI:
      // ADDI rd, x0, imm
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = 0;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_ADDI());
      break;
    case OP_C_ADDI16SP: {
      // Distinguishing C.ADDI16SP (rd==x2) from C.LUI (rd!=x2)
      uint32_t rd_f = (ci >> 7) & 0x1F;
      if (rd_f == 2) {
        // ADDI x2, x2, nzimm
        id_issue_next.opcode = 0x13;
        id_issue_next.funct3 = 0;
        id_issue_next.rd = 2;
        id_issue_next.rs1 = 2;
        id_issue_next.rs2 = 0;
        id_issue_next.imm = static_cast<int64_t>(
            static_cast<int32_t>(c_inst->get_imm_ADDI16SP()));
      } else {
        // LUI rd, nzimm
        id_issue_next.opcode = 0x37;
        id_issue_next.rd = rd_f;
        id_issue_next.rs1 = 0;
        id_issue_next.rs2 = 0;
        id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_LUI());
      }
      break;
    }
    case OP_C_SRLI: {
      // SRLI rd', rd', shamt
      uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 5;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(shamt); // SRLI: imm[10]=0
      break;
    }
    case OP_C_SRAI: {
      // SRAI rd', rd', shamt  — imm[10]=1 signals arithmetic shift in 64-bit
      // SRLI/SRAI
      uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 5;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(shamt | 0x400);
      break;
    }
    case OP_C_ANDI:
      // ANDI rd', rd', imm
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 7;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_ADDI());
      break;
    case OP_C_SUB:
      // SUB rd', rd', rs2'
      id_issue_next.opcode = 0x33;
      id_issue_next.funct3 = 0;
      id_issue_next.funct7 = 0x20;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      break;
    case OP_C_XOR:
      // XOR rd', rd', rs2'
      id_issue_next.opcode = 0x33;
      id_issue_next.funct3 = 4;
      id_issue_next.funct7 = 0;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      break;
    case OP_C_OR:
      // OR rd', rd', rs2'
      id_issue_next.opcode = 0x33;
      id_issue_next.funct3 = 6;
      id_issue_next.funct7 = 0;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      break;
    case OP_C_AND:
      // AND rd', rd', rs2'
      id_issue_next.opcode = 0x33;
      id_issue_next.funct3 = 7;
      id_issue_next.funct7 = 0;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      break;
    case OP_C_SUBW:
      // SUBW rd', rd', rs2'  [RV64]
      id_issue_next.opcode = 0x3B;
      id_issue_next.funct3 = 0;
      id_issue_next.funct7 = 0x20;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      break;
    case OP_C_ADDW:
      // ADDW rd', rd', rs2'  [RV64]
      id_issue_next.opcode = 0x3B;
      id_issue_next.funct3 = 0;
      id_issue_next.funct7 = 0;
      id_issue_next.rd = c_inst->get_rs1p();
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = c_inst->get_rs2p();
      break;
    case OP_C_J:
      // JAL x0, offset  (C.J — no link, rd=0)
      id_issue_next.opcode = 0x6F;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = 0;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_J());
      break;
    case OP_C_BEQZ:
      // BEQ rs1', x0, offset
      id_issue_next.opcode = 0x63;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm =
          static_cast<int64_t>(static_cast<int32_t>(c_inst->get_imm_CB()));
      break;
    case OP_C_BNEZ:
      // BNE rs1', x0, offset
      id_issue_next.opcode = 0x63;
      id_issue_next.funct3 = 1;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = c_inst->get_rs1p();
      id_issue_next.rs2 = 0;
      id_issue_next.imm =
          static_cast<int64_t>(static_cast<int32_t>(c_inst->get_imm_CB()));
      break;

    // ── Quadrant 2 ────────────────────────────────────────────────────
    case OP_C_SLLI: {
      // SLLI rd, rd, shamt
      uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
      id_issue_next.opcode = 0x13;
      id_issue_next.funct3 = 1;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = c_inst->get_rs1();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(shamt);
      break;
    }
    case OP_C_LWSP:
      // LW rd, offset(x2)
      id_issue_next.opcode = 0x03;
      id_issue_next.funct3 = 2;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_LWSP());
      break;
    case OP_C_LDSP:
      // LD rd, offset(x2)  [RV64]
      id_issue_next.opcode = 0x03;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_LDSP());
      break;
    case OP_C_JR:
      // JALR x0, 0(rs1)
      id_issue_next.opcode = 0x67;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = c_inst->get_rs1();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = 0;
      break;
    case OP_C_MV:
      // ADD rd, x0, rs2
      id_issue_next.opcode = 0x33;
      id_issue_next.funct3 = 0;
      id_issue_next.funct7 = 0;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = 0;
      id_issue_next.rs2 = c_inst->get_rs2();
      break;
    case OP_C_EBREAK:
      // Map to EBREAK (opcode=0x73, imm=1)
      id_issue_next.opcode = 0x73;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = 0;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = 1;
      break;
    case OP_C_JALR:
      // JALR x1, 0(rs1)  — stores pc+2 in ra (handled in EX via is_compressed)
      id_issue_next.opcode = 0x67;
      id_issue_next.funct3 = 0;
      id_issue_next.rd = 1; // ra
      id_issue_next.rs1 = c_inst->get_rs1();
      id_issue_next.rs2 = 0;
      id_issue_next.imm = 0;
      break;
    case OP_C_ADD:
      // ADD rd, rd, rs2
      id_issue_next.opcode = 0x33;
      id_issue_next.funct3 = 0;
      id_issue_next.funct7 = 0;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = c_inst->get_rs1();
      id_issue_next.rs2 = c_inst->get_rs2();
      break;
    case OP_C_SWSP:
      // SW rs2, offset(x2)
      id_issue_next.opcode = 0x23;
      id_issue_next.funct3 = 2;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = c_inst->get_rs2();
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CSS());
      break;
    case OP_C_SDSP:
      // SD rs2, offset(x2)  [RV64]
      id_issue_next.opcode = 0x23;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = c_inst->get_rs2();
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CSDSP());
      break;
    case OP_C_FLDSP:
      // FLD rd, offset(x2)
      id_issue_next.opcode = 0x07;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = c_inst->get_rs1();
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = 0;
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_LDSP());
      break;
    case OP_C_FSDSP:
      // FSD rs2, offset(x2)
      id_issue_next.opcode = 0x27;
      id_issue_next.funct3 = 3;
      id_issue_next.rd = 0;
      id_issue_next.rs1 = 2;
      id_issue_next.rs2 = c_inst->get_rs2();
      id_issue_next.imm = static_cast<int64_t>(c_inst->get_imm_CSDSP());
      break;
    default:
      // Unrecognized or unsupported compressed instruction (e.g. C.FSDSP,
      // C.FLD). Mark as illegal so Issue_stage raises ILLEGAL_INSTR trap.
      id_issue_next.is_illegal = true;
      id_issue_next.valid = true;
      return;
    }
    id_issue_next.valid = true;

    // Correct branch predictor if it incorrectly predicted a non-branch
    // compressed instruction as taken
    bool is_branch_instr =
        (id_issue_next.opcode == 0x63 || id_issue_next.opcode == 0x6F ||
         id_issue_next.opcode == 0x67);
    if (!is_branch_instr && fetch_id_reg.predicted_taken) {
      pc_redirect_target =
          fetch_id_reg.pc + 2; // Compressed instructions are always 2 bytes
      pc_redirect_valid = true;
      flush_pipeline = true;
      stats.flushes++;
      stats.branch_mispredicts++;
    }
    return;
  }

  // =========================================================================
  // Standard 32-bit Instruction Decode
  // =========================================================================
  id_issue_next.is_compressed = false;
  id_issue_next.opcode = instr & 0x7F;
  id_issue_next.funct3 = (instr >> 12) & 0x7;
  id_issue_next.rs1 = (instr >> 15) & 0x1F;
  id_issue_next.rs2 = (instr >> 20) & 0x1F;

  // Mask unused registers to prevent false data hazard stalls
  if (id_issue_next.opcode == 0x37 || id_issue_next.opcode == 0x17 ||
      id_issue_next.opcode == 0x6F) {
    id_issue_next.rs1 = 0;
    id_issue_next.rs2 = 0;
  } else if (id_issue_next.opcode == 0x13 || id_issue_next.opcode == 0x1B ||
             id_issue_next.opcode == 0x03 || id_issue_next.opcode == 0x07 || id_issue_next.opcode == 0x67 ||
             (id_issue_next.opcode == 0x73 &&
              !(((instr >> 25) & 0x7F) == 0x09 &&
                ((instr >> 12) & 0x7) == 0))) {
    id_issue_next.rs2 = 0;
  }

  id_issue_next.funct7 = (instr >> 25) & 0x7F;

  // Decode Destination Register (rd)
  // S-Type (Store) and B-Type (Branch) do not have a destination register.
  if (id_issue_next.opcode == 0x23 || id_issue_next.opcode == 0x27 || id_issue_next.opcode == 0x63) {
    id_issue_next.rd = 0;
  } else {
    id_issue_next.rd = (instr >> 7) & 0x1F;
  }

  // Detect function calls and returns for RAS
  if (id_issue_next.opcode == 0x6F || id_issue_next.opcode == 0x67) {
    bool is_call = (id_issue_next.rd == 1 || id_issue_next.rd == 5);
    bool is_ret = (id_issue_next.opcode == 0x67 &&
                   (id_issue_next.rs1 == 1 || id_issue_next.rs1 == 5) &&
                   id_issue_next.rs1 != id_issue_next.rd);
    id_issue_next.is_ras_call = is_call;
    id_issue_next.is_ras_return = is_ret;
  }

  // Decode Immediate Value (Sign-Extended)
  switch (id_issue_next.opcode) {
  case 0x13: // I-type ALU
  case 0x1B: // I-type ALU-32 (ADDIW)
  case 0x03: // Load
  case 0x07: // FP Load (FLW/FLD)
  case 0x67: // JALR
    id_issue_next.imm = static_cast<int64_t>(static_cast<int32_t>(instr) >> 20);
    break;
  case 0x23: // S-type
  case 0x27: // FP Store (FSW/FSD)
    id_issue_next.imm = static_cast<int64_t>(
        static_cast<int32_t>(((instr >> 25) << 5) | ((instr >> 7) & 0x1F))
            << 20 >>
        20);
    break;
  case 0x63: // B-type
    id_issue_next.imm = static_cast<int64_t>(
        static_cast<int32_t>(
            ((instr >> 31) << 12) | (((instr >> 7) & 1) << 11) |
            (((instr >> 25) & 0x3F) << 5) | (((instr >> 8) & 0xF) << 1))
            << 19 >>
        19);
    break;
  case 0x37: // LUI
  case 0x17: // AUIPC
    id_issue_next.imm =
        static_cast<int64_t>(static_cast<int32_t>(instr & 0xFFFFF000));
    break;
  case 0x6F: // JAL
    id_issue_next.imm = static_cast<int64_t>(
        static_cast<int32_t>(
            ((instr >> 31) << 20) | (((instr >> 12) & 0xFF) << 12) |
            (((instr >> 20) & 1) << 11) | (((instr >> 21) & 0x3FF) << 1))
            << 11 >>
        11);
    break;
  case 0x73: // SYSTEM
    id_issue_next.imm = static_cast<int64_t>(static_cast<int32_t>(instr) >> 20);
    break;
  default:
    id_issue_next.imm = 0;
  }

  id_issue_next.valid = true;

  // Correct branch predictor if it incorrectly predicted a non-branch
  // instruction as taken
  bool is_branch_instr =
      (id_issue_next.opcode == 0x63 || id_issue_next.opcode == 0x6F ||
       id_issue_next.opcode == 0x67);
  if (!is_branch_instr && fetch_id_reg.predicted_taken) {
    pc_redirect_target = fetch_id_reg.pc + (fetch_id_reg.is_compressed ? 2 : 4);
    pc_redirect_valid = true;
    flush_pipeline = true;
    stats.flushes++;
    stats.branch_mispredicts++;
  }
}

// =============================================================================
// Issue Stage (Dispatch / Register Read)
// =============================================================================

void CPURV64P6_Cycle::Issue_stage() {
  // When EX is replaying a load (partial store-buffer overlap), freeze
  // upstream.
  if (stall_ex) {
    stall_issue = true;
    issue_ex_next.valid = false;
    return;
  }

  // Reset control signals initially
  stall_pcgen = false;
  stall_fetch = false;
  stall_issue = false;

  // Check for Pipeline Flush
  if (flush_pipeline) {
    issue_ex_next.valid = false;
    return;
  }

  // Check validity of input
  if (!id_issue_reg.valid) {
    issue_ex_next.valid = false;
    return;
  }

  // --- Load->address dependency bubble (gather/pointer-chase, BUGS §B9) ---
  // Consume any pending extra cycles from a load result feeding a load/store
  // address before dispatching this instruction.
  if (load_addr_extra > 0) {
    load_addr_extra--;
    stall_issue = true;
    issue_ex_next.valid = false;
    stats.stalls++;
    return;
  }

  // --- Functional unit classification (must precede structural hazard check)
  // --- For M-extension: funct3 0-3 = multiply family, funct3 4-7 =
  // divide/remainder family.
  FunctionalUnit fu = FunctionalUnit::ALU;
  {
    bool is_m = (id_issue_reg.funct7 == 0x01) &&
                (id_issue_reg.opcode == 0x33 || id_issue_reg.opcode == 0x3B);
    switch (id_issue_reg.opcode) {
    case 0x03:
    case 0x23:
    case 0x2F:
      fu = FunctionalUnit::LSU;
      break;
    case 0x63:
    case 0x6F:
    case 0x67:
      fu = FunctionalUnit::BRANCH;
      break;
    case 0x73:
      fu = FunctionalUnit::CSR;
      break;
    case 0x07:
    case 0x27:
    case 0x43:
    case 0x47:
    case 0x4B:
    case 0x4F:
    case 0x53:
      fu = FunctionalUnit::FPU;
      break;
    default:
      if (is_m)
        fu = (id_issue_reg.funct3 >= 4) ? FunctionalUnit::DIV
                                        : FunctionalUnit::MUL;
      break;
    }
  }

  bool writes_to_int_reg = true;
  if (fu == FunctionalUnit::FPU) {
    writes_to_int_reg = false;
    if (id_issue_reg.opcode == 0x53) {
      uint32_t f7 = id_issue_reg.funct7;
      if (f7 == 0x50 || f7 == 0x51 || f7 == 0x60 || f7 == 0x61 || f7 == 0x70 ||
          f7 == 0x71) {
        writes_to_int_reg = true;
      }
    }
  }
  bool writes_to_fp_reg = false;
  if (fu == FunctionalUnit::FPU) {
    writes_to_fp_reg = !writes_to_int_reg;
  }

  // --- Operand resolution with scoreboard forwarding (CVA6 rd_clobber path)
  // --- For each source register: if rd_clobber says it is busy, check whether
  // the producing scoreboard entry is already ready (EX ran before Issue this
  // cycle). If ready  → forward the result directly; no stall needed. If not
  // yet ready → stall until EX completes the producing instruction.
  uint64_t rs1_val = 0, rs2_val = 0;
  bool need_stall = false;

  bool reads_rs1_as_int = true;
  if (fu == FunctionalUnit::FPU) {
    reads_rs1_as_int = false;
    if (id_issue_reg.opcode == 0x07 || id_issue_reg.opcode == 0x27) {
      reads_rs1_as_int = true;
    } else if (id_issue_reg.opcode == 0x53) {
      uint32_t f7 = id_issue_reg.funct7;
      if (f7 == 0x68 || f7 == 0x69 || f7 == 0x78 || f7 == 0x79) {
        reads_rs1_as_int = true;
      }
    }
  }
  bool reads_rs2_as_int = (fu != FunctionalUnit::FPU);

  if (id_issue_reg.rs1 != 0 && reads_rs1_as_int) {
    const auto &clob = scoreboard.rd_clobber[id_issue_reg.rs1];
    if (clob.busy) {
      const auto &prod = scoreboard.get_entry(clob.trans_id);
      if (prod.ready) {
        rs1_val = prod.result;
        stats.forwarded_reads++;
      } else {
        need_stall = true;
      }
    } else {
      rs1_val = register_bank->getValue(id_issue_reg.rs1);
    }
  } else if (!reads_rs1_as_int) {
    const auto &clob = scoreboard.fp_clobber[id_issue_reg.rs1];
    if (clob.busy) {
      const auto &prod = scoreboard.get_entry(clob.trans_id);
      if (!prod.ready) {
        need_stall = true;
      } else {
        rs1_val = register_bank->getFPValue(id_issue_reg.rs1);
      }
    } else {
      rs1_val = register_bank->getFPValue(id_issue_reg.rs1);
    }
  }

  if (id_issue_reg.rs2 != 0 && reads_rs2_as_int) {
    const auto &clob = scoreboard.rd_clobber[id_issue_reg.rs2];
    if (clob.busy) {
      const auto &prod = scoreboard.get_entry(clob.trans_id);
      if (prod.ready) {
        rs2_val = prod.result;
        stats.forwarded_reads++;
      } else {
        need_stall = true;
      }
    } else {
      rs2_val = register_bank->getValue(id_issue_reg.rs2);
    }
  } else if (!reads_rs2_as_int) {
    const auto &clob = scoreboard.fp_clobber[id_issue_reg.rs2];
    if (clob.busy) {
      const auto &prod = scoreboard.get_entry(clob.trans_id);
      if (!prod.ready) {
        need_stall = true;
      } else {
        rs2_val = register_bank->getFPValue(id_issue_reg.rs2);
      }
    } else {
      rs2_val = register_bank->getFPValue(id_issue_reg.rs2);
    }
  }

  // Check rs3 for FMADD/FMSUB/FNMSUB/FNMADD instructions
  bool reads_rs3 =
      (id_issue_reg.opcode == 0x43 || id_issue_reg.opcode == 0x47 ||
       id_issue_reg.opcode == 0x4B || id_issue_reg.opcode == 0x4F);
  if (reads_rs3) {
    uint8_t rs3 = static_cast<uint8_t>((id_issue_reg.instr >> 27) & 0x1F);
    const auto &clob = scoreboard.fp_clobber[rs3];
    if (clob.busy) {
      const auto &prod = scoreboard.get_entry(clob.trans_id);
      if (!prod.ready) {
        need_stall = true;
      }
    }
  }

  // 1-cycle pipeline bubble/stall for shift-to-adder/branch/store dependency
  // If the producer instruction currently in EX is a shift instruction, and the
  // consumer in Issue stage reads the register written by it, we force a 1-cycle stall
  // UNLESS the consumer is a fast logical instruction (AND/OR/XOR) which can be bypassed.
  if (issue_ex_reg.valid) {
    uint32_t ex_opcode = issue_ex_reg.instr & 0x7F;
    uint32_t ex_funct3 = (issue_ex_reg.instr >> 12) & 0x7;
    bool ex_is_shift = false;
    if (ex_opcode == 0x33 || ex_opcode == 0x3B || ex_opcode == 0x13 || ex_opcode == 0x1B) {
      if (ex_funct3 == 1 || ex_funct3 == 5) {
        ex_is_shift = true;
      }
    }
    if (ex_is_shift) {
      uint8_t ex_rd = issue_ex_reg.rd;
      if (ex_rd != 0) {
        if ((id_issue_reg.rs1 == ex_rd && reads_rs1_as_int) ||
            (id_issue_reg.rs2 == ex_rd && reads_rs2_as_int)) {
          // Check if consumer is a logical instruction (XOR=4, OR=6, AND=7)
          uint32_t op = id_issue_reg.opcode;
          uint32_t f3 = id_issue_reg.funct3;
          bool consumer_is_logical = (op == 0x33 || op == 0x3B || op == 0x13 || op == 0x1B) && 
                                     (f3 == 4 || f3 == 6 || f3 == 7);
          if (!consumer_is_logical) {
            need_stall = true;
          }
        }
      }
    }
  }

  // CSR RAW hazard: if previous instruction wrote a CSR that this one reads
  if (id_issue_reg.opcode == 0x73 && id_issue_reg.funct3 != 0) {
    uint16_t csr_addr = static_cast<uint16_t>(id_issue_reg.instr >> 20);
    if (pending_csr_write.valid && pending_csr_write.addr == csr_addr) {
      need_stall = true;
    }
  }

  // CSR/FPU RAW hazard: if previous instruction wrote an FPU CSR (fcsr, frm,
  // fflags) and this is an FPU instruction
  if (fu == FunctionalUnit::FPU) {
    if (pending_csr_write.valid &&
        (pending_csr_write.addr == 0x001 || pending_csr_write.addr == 0x002 ||
         pending_csr_write.addr == 0x003)) {
      need_stall = true;
    }
  }

  if (need_stall) {
    stall_issue = true;
    issue_ex_next.valid = false;
    stats.stalls++;
    return;
  }

  // --- Load->address dependency (gather/pointer-chase, BUGS §B9) ---
  // A loaded value that flows (through address arithmetic) into a subsequent
  // load/store ADDRESS costs CVA6 ~load_addr_penalty extra cycles vs the VP.
  // Track a per-register "derives from a recent load" taint: loads taint their
  // rd (at completion, in EX); address-arithmetic ALU ops (add/shift/etc.)
  // propagate the taint; a memory op whose base (rs1) is tainted pays the bubble.
  {
    uint32_t op = id_issue_reg.opcode;
    uint8_t  rs1 = id_issue_reg.rs1, rd = id_issue_reg.rd;
    bool is_mem = (op == 0x03 || op == 0x23 || op == 0x07 || op == 0x27 || op == 0x2F);

    if (load_addr_penalty > 0 && is_mem && rs1 != 0 &&
        ((reg_load_tainted >> rs1) & 1u)) {
      // The detection cycle is already one stall, so the countdown adds the
      // remaining (penalty-1) to make the total extra latency == load_addr_penalty.
      load_addr_extra = load_addr_penalty - 1;
      stats.load_addr_events++;
      reg_load_tainted &= ~(1u << rs1); // charge once per produced address value
      stall_issue = true;
      issue_ex_next.valid = false;
      stats.stalls++;
      return;
    }

    // Taint propagation for the *next* instruction. Address arithmetic
    // (OP-IMM/OP, incl. word forms) that reads a tainted source keeps the taint;
    // any other non-load writer clears it. Loads taint rd at completion (EX).
    if (rd != 0) {
      bool is_addr_alu = (op == 0x13 || op == 0x33 || op == 0x1B || op == 0x3B);
      bool src_tainted = ((reg_load_tainted >> rs1) & 1u) ||
                         ((reg_load_tainted >> id_issue_reg.rs2) & 1u);
      if (is_addr_alu && src_tainted) {
        // Forward the taint as a single-use TOKEN: consume the source(s) so a
        // value loaded once and reused (e.g. a PIC/GOT base pointer) does not
        // keep re-tainting addresses. A genuine gather/chase re-loads each
        // iteration, so its token is re-created and keeps firing.
        reg_load_tainted &= ~(1u << rs1);
        reg_load_tainted &= ~(1u << id_issue_reg.rs2);
        reg_load_tainted |= (1u << rd);
      } else if (op != 0x03 && op != 0x07) { // not a load (loads taint in EX)
        reg_load_tainted &= ~(1u << rd);
      }
    }
  }

  // --- Structural hazard: multi-cycle FU still occupied ---
  // The scoreboard handles data hazards; this handles the case where two
  // independent MUL/DIV/FPU instructions compete for the same physical unit.
  // AMOs (0x2F) also use the dcache_miss_fu for D$ miss latency.
  if ((fu == FunctionalUnit::MUL && mul_fu.busy) ||
      (fu == FunctionalUnit::DIV && div_fu.busy) ||
      (fu == FunctionalUnit::FPU && (fpu_pipe_full() || fpu_divsqrt_fu.busy)) || // pipelined addmul; div/sqrt blocks
      // FP loads (0x07) are classified as FPU but share the single LSU/D$ path,
      // so they must also wait for dcache_miss_fu — otherwise a second FP load
      // overwrites the in-flight miss and its scoreboard entry never completes.
      (id_issue_reg.opcode == 0x07 && dcache_miss_fu_full()) ||
      (fu == FunctionalUnit::LSU &&
       (id_issue_reg.opcode == 0x03 || id_issue_reg.opcode == 0x2F) &&
       (dcache_miss_fu_full() || load_hit_fu.busy))) {
    stall_issue = true;
    issue_ex_next.valid = false;
    stats.stalls++;
    return;
  }

  // --- Store buffer speculative-queue full check ---
  // Only regular stores (0x23/0x27) need a spec buffer slot.
  // AMOs (0x2F) write directly to mem_intf at EX time for atomicity.
  if (id_issue_reg.opcode == 0x23 && store_buffer.is_full()) {
    stall_issue = true;
    issue_ex_next.valid = false;
    stats.stalls++;
    return;
  }

  // --- AMO (0x2F) store buffer drain stall ---
  // AMOs require absolute memory atomicity and order. They must stall in Issue
  // until the store buffer has completely drained to memory to prevent reading
  // or writing stale data.
  if (id_issue_reg.opcode == 0x2F && !store_buffer.is_empty()) {
    stall_issue = true;
    issue_ex_next.valid = false;
    stats.stalls++;
    return;
  }

  // --- SYSTEM (0x73) serialization stall ---
  // Root cause: when an ECALL/EBREAK fires a trap, EX calls scoreboard.flush()
  // which discards ALL in-flight ROB entries.  If a multi-cycle instruction
  // (D-cache miss load, MUL, DIV, FPU) is still executing, its result is lost
  // and its destination register is left with stale data (the init crash bug).
  //
  // Correct fix: stall the SYSTEM instruction in Issue while ANY multi-cycle FU
  // is busy.  Once all FUs are idle, every prior ROB entry is guaranteed to
  // have ready=true.  When EX then fires the trap, the trap_pending drain loop:
  //   while (head_ready() && head_index != ecall_rob_index) { commit; retire; }
  // will write every prior result to the architectural register file before
  // scoreboard.flush() runs — so no result is lost.
  //
  // This is simpler and deadlock-free: FU countdowns always terminate, there is
  // no ROB-head PC comparison, and no duplicate-allocation edge case.
  if (id_issue_reg.opcode == 0x73) {
    bool multi_cycle_in_flight =
        dcache_miss_fu_any_busy() || mul_fu.busy || div_fu.busy || fpu_any_busy() ||
        load_hit_fu.busy;
    if (multi_cycle_in_flight) {
      stall_issue = true;
      issue_ex_next.valid = false;
      stats.stalls++;
      return;
    }
  }

  // --- Scoreboard allocation (acts as issue FIFO / reorder buffer) ---
  int rob_idx = scoreboard.allocate();
  if (rob_idx < 0) {
    stall_issue = true;
    issue_ex_next.valid = false;
    stats.stalls++;
    return;
  }

  // --- Populate scoreboard entry metadata ---
  scoreboard[rob_idx].pc = id_issue_reg.pc;
  // AMOs (0x2F) write directly to mem_intf at EX time — they never use the
  // store buffer, so is_store must be false to prevent a spurious
  // commit_store() call.
  scoreboard[rob_idx].is_store =
      (id_issue_reg.opcode == 0x23 || id_issue_reg.opcode == 0x27);
  scoreboard[rob_idx].is_branch =
      (id_issue_reg.opcode == 0x63 || id_issue_reg.opcode == 0x6F ||
       id_issue_reg.opcode == 0x67);
  scoreboard[rob_idx].fu = fu;

  // --- Illegal Instruction Check ---
  if (id_issue_reg.is_illegal) {
    // Raise ILLEGAL_INSTR exception — trap handler will deal with it.
    // First, drain all older ready instructions so their register writes
    // are architecturally visible before the trap handler runs.
    while (scoreboard.head_ready()) {
      const auto &entry = scoreboard.get_head();
      if (entry.rd != 0) {
        register_bank->setValue(entry.rd, entry.result);
      }
      if (entry.is_store) {
        store_buffer.commit_store(scoreboard.get_head_index());
      }
      scoreboard.retire();
    }
    {
      uint64_t sb_addr, sb_data;
      int sb_size;
      while (store_buffer.drain_one(sb_addr, sb_data, sb_size)) {
        if (sb_size == 8)
          mem_intf->writeDataMem64(sb_addr, sb_data, sb_size);
        else
          mem_intf->writeDataMem(sb_addr, static_cast<uint32_t>(sb_data),
                                 sb_size);
      }
    }
    scoreboard.flush();
    store_buffer.flush_speculative();
    for (int i = 0; i < DCACHE_MISS_SLOTS; i++) {
#ifdef ENABLE_AXI_CONTENTION
      if (dcache_miss_fu[i].busy) axi_top->ack(riscv_axi::AxiContentionTop::DCACHE);
#endif
      dcache_miss_fu[i].busy = false;
    }
    mul_fu.busy = div_fu.busy =
        load_hit_fu.busy = false;
    for (int fs = 0; fs < FPU_SLOTS; fs++) fpu_pipe[fs].busy = false;
  fpu_divsqrt_fu.busy = false;
    uint64_t tvec = csr.take_exception(CAUSE::ILLEGAL_INSTR, id_issue_reg.pc,
                                       id_issue_reg.instr);
    pc_redirect_target = tvec;
    pc_redirect_valid = true;
    flush_pipeline = true;
    trap_pending = true;
    issue_ex_next.valid = false;
    return;
  }

  // --- Dispatch & Operand Read ---
  issue_ex_next.pc = id_issue_reg.pc;
  issue_ex_next.instr = id_issue_reg.instr;
  issue_ex_next.rs1_val = rs1_val;
  issue_ex_next.rs2_val = rs2_val;
  issue_ex_next.imm = id_issue_reg.imm;
  issue_ex_next.rd = id_issue_reg.rd;
  issue_ex_next.rs1 = id_issue_reg.rs1;
  issue_ex_next.rs2 = id_issue_reg.rs2;
  issue_ex_next.opcode = id_issue_reg.opcode;
  issue_ex_next.funct3 = id_issue_reg.funct3;
  issue_ex_next.funct7 = id_issue_reg.funct7;
  issue_ex_next.rob_index = rob_idx;
  issue_ex_next.is_compressed = id_issue_reg.is_compressed;
  issue_ex_next.valid = true;

  issue_ex_next.predicted_taken = id_issue_reg.predicted_taken;
  issue_ex_next.predicted_target = id_issue_reg.predicted_target;
  issue_ex_next.is_ras_call = id_issue_reg.is_ras_call;
  issue_ex_next.is_ras_return = id_issue_reg.is_ras_return;

  scoreboard[rob_idx].writes_to_fp_reg = writes_to_fp_reg;

  if (id_issue_reg.rd != 0 && writes_to_int_reg) {
    scoreboard.rd_clobber[id_issue_reg.rd] = {true, fu, rob_idx};
  } else if (writes_to_fp_reg) {
    scoreboard.fp_clobber[id_issue_reg.rd] = {true, fu, rob_idx};
  }
}
// =============================================================================
// EX Stage (Execute & Memory Access)
// =============================================================================

void CPURV64P6_Cycle::EX_stage() {
  // Clear stall_ex from previous cycle — will be re-set below if still needed.
  stall_ex = false;

  // --- Tick multi-cycle functional units (runs every cycle regardless of
  // issue_ex_reg) --- Each FU operates independently; the scoreboard handles
  // data-dependent stalls.
  if (mul_fu.busy) {
    stats.mul_stall_cycles++;
    if (--mul_fu.remaining == 0) {
      scoreboard.complete(mul_fu.trans_id, mul_fu.result, mul_fu.rd);
      mul_fu.busy = false;
    }
  }
  if (div_fu.busy) {
    stats.div_stall_cycles++;
    if (--div_fu.remaining == 0) {
      scoreboard.complete(div_fu.trans_id, div_fu.result, div_fu.rd);
      div_fu.busy = false;
    }
  }
  for (int dms = 0; dms < DCACHE_MISS_SLOTS; dms++) {
    auto &dmf = dcache_miss_fu[dms];
    if (!dmf.busy) continue;
    stats.dcache_miss_cycles++;
    // TEMP DIAGNOSTIC: does an I$ miss and D$ miss ever actually overlap?
    if (dms == 0 && icache_miss_remaining > 0) diag_costall_cycles++;
#ifdef ENABLE_AXI_CONTENTION
    // AXI mode: the D$ refill completes when the arbiter+slave return the read
    // (may be delayed behind an I$ miss holding the shared port = contention).
    if (axi_top->done(riscv_axi::AxiContentionTop::DCACHE)) {
      axi_top->ack(riscv_axi::AxiContentionTop::DCACHE);
      scoreboard.complete(dmf.trans_id, dmf.result, dmf.rd);
      if (dmf.rd != 0) reg_load_tainted |= (1u << dmf.rd); // §B9
      dmf.busy = false;
    }
#else
    if (--dmf.remaining == 0) {
      scoreboard.complete(dmf.trans_id, dmf.result, dmf.rd);
      if (dmf.rd != 0) reg_load_tainted |= (1u << dmf.rd); // §B9
      dmf.busy = false;
    }
#endif
  }
  if (load_hit_fu.busy) {
    stats.load_use_stall_cycles++;
    if (--load_hit_fu.remaining == 0) {
      scoreboard.complete(load_hit_fu.trans_id, load_hit_fu.result,
                          load_hit_fu.rd);
      if (load_hit_fu.rd != 0) reg_load_tainted |= (1u << load_hit_fu.rd); // §B9
      load_hit_fu.busy = false;
    }
  }
  // Pipelined FPU: tick every in-flight slot; each completes independently.
  for (int fs = 0; fs < FPU_SLOTS; fs++) {
    if (fpu_pipe[fs].busy) {
      if (--fpu_pipe[fs].remaining == 0) {
        scoreboard.complete(fpu_pipe[fs].trans_id, fpu_pipe[fs].result, fpu_pipe[fs].rd);
        fpu_pipe[fs].busy = false;
      }
    }
  }
  // FP divide/sqrt: iterative, non-pipelined blocking unit.
  if (fpu_divsqrt_fu.busy) {
    if (--fpu_divsqrt_fu.remaining == 0) {
      scoreboard.complete(fpu_divsqrt_fu.trans_id, fpu_divsqrt_fu.result, fpu_divsqrt_fu.rd);
      fpu_divsqrt_fu.busy = false;
    }
  }

  if (!issue_ex_reg.valid)
    return;

  // Guard: if cpu_process_IRQ already took control this cycle
  // (flush_pipeline=true), the ROB is flushed and sepc/irq_epc are set to the
  // correct resume address. Executing issue_ex_reg now would:
  //   (a) corrupt flushed ROB entries via scoreboard.complete()
  //   (b) call take_exception() if the instruction faults, overwriting the
  //   correct sepc
  // This is the root cause of the init SIGBUS: IRQ sepc=0x1014a (AUIPC) was
  // being overwritten by a load-page-fault sepc=0x1014e before the kernel ever
  // saw 0x1014a.
  if (flush_pipeline) {
    return;
  }

  uint64_t result = 0;
  bool branch_taken = false;
  uint64_t branch_target = 0;
  bool multi_cycle_dispatched =
      false; // Set when MUL/DIV starts; skips scoreboard.complete()

  // Precise-exception guard: if the faulting instruction is not at the ROB
  // head, older in-flight instructions (e.g., a dcache-miss load) must commit
  // first. Stall EX and retry next cycle rather than firing the trap
  // prematurely and skipping those instructions (which would corrupt register
  // state visible to the trap handler, and cause SRET to resume past the
  // skipped instructions).
  auto has_unready_older = [&]() -> bool {
    return !scoreboard.is_empty() &&
           scoreboard.get_head_index() != issue_ex_reg.rob_index;
  };

  // 1. Execute ALU Operations
  switch (issue_ex_reg.opcode) {
  case 0x33: // R-type (Register-Register)
    if (issue_ex_reg.funct7 == 0x01) {
      // M-extension (RV64M): MUL/MULH/MULHSU/MULHU/DIV/DIVU/REM/REMU
      const int64_t s1 = static_cast<int64_t>(issue_ex_reg.rs1_val);
      const int64_t s2 = static_cast<int64_t>(issue_ex_reg.rs2_val);
      const uint64_t u1 = issue_ex_reg.rs1_val;
      const uint64_t u2 = issue_ex_reg.rs2_val;
      uint64_t mul_result = 0;
      switch (issue_ex_reg.funct3) {
      case 0x0:
        mul_result = static_cast<uint64_t>(s1 * s2);
        break;
      case 0x1:
        mul_result =
            static_cast<uint64_t>((static_cast<__int128>(s1) * s2) >> 64);
        break;
      case 0x2:
        mul_result = static_cast<uint64_t>(
            (static_cast<__int128>(s1) * static_cast<unsigned __int128>(u2)) >>
            64);
        break;
      case 0x3:
        mul_result = static_cast<uint64_t>(
            (static_cast<unsigned __int128>(u1) * u2) >> 64);
        break;
      case 0x4: // DIV (signed) — division by zero and overflow per spec
        mul_result = (s2 == 0) ? static_cast<uint64_t>(-1LL)
                     : (s1 == INT64_MIN && s2 == -1LL)
                         ? static_cast<uint64_t>(s1)
                         : static_cast<uint64_t>(s1 / s2);
        break;
      case 0x5: // DIVU (unsigned)
        mul_result = (u2 == 0) ? UINT64_MAX : u1 / u2;
        break;
      case 0x6: // REM (signed)
        mul_result = (s2 == 0) ? static_cast<uint64_t>(s1)
                     : (s1 == INT64_MIN && s2 == -1LL)
                         ? 0
                         : static_cast<uint64_t>(s1 % s2);
        break;
      case 0x7: // REMU (unsigned)
        mul_result = (u2 == 0) ? u1 : u1 % u2;
        break;
      }
      bool is_div_op = (issue_ex_reg.funct3 >= 4);
      FunctionalUnitState &fu = is_div_op ? div_fu : mul_fu;
      fu.busy = true;

      int div_cycles = 1;
      if (is_div_op) {
        uint64_t A = issue_ex_reg.rs1_val;
        uint64_t B = issue_ex_reg.rs2_val;
        bool is_signed = (issue_ex_reg.funct3 == 4 || issue_ex_reg.funct3 == 6);
        bool op_a_sign = is_signed && (static_cast<int64_t>(A) < 0);
        bool op_b_sign = is_signed && (static_cast<int64_t>(B) < 0);

        uint64_t lzc_a_input = op_a_sign ? (~A + 1) : A;
        uint64_t lzc_b_input = op_b_sign ? ~B : B;

        int lzc_a_result = clz64(lzc_a_input);
        int lzc_b_result = clz64(lzc_b_input);

        bool lzc_a_no_one = (lzc_a_input == 0);
        bool lzc_b_no_one = (lzc_b_input == 0);

        int shift_a = lzc_a_no_one ? 64 : lzc_a_result;
        int div_shift = lzc_b_result - shift_a;

        bool op_b_zero = lzc_b_no_one && !op_b_sign;
        bool op_b_neg_one = lzc_b_no_one && op_b_sign;
        bool div_res_zero = (div_shift < 0);

        if (div_res_zero || op_b_zero || op_b_neg_one) {
          div_cycles = 1;
        } else {
          div_cycles = div_shift + 6;
        }
      }

      fu.remaining = is_div_op ? div_cycles : 1;
      fu.result = mul_result;
      fu.trans_id = issue_ex_reg.rob_index;
      fu.rd = issue_ex_reg.rd;
      multi_cycle_dispatched = true;
      break;
    }
    switch (issue_ex_reg.funct3) {
    case 0x0:
      if (issue_ex_reg.funct7 == 0x20)
        result = issue_ex_reg.rs1_val - issue_ex_reg.rs2_val;
      else
        result = issue_ex_reg.rs1_val + issue_ex_reg.rs2_val;
      break;
    case 0x1:
      result = issue_ex_reg.rs1_val << (issue_ex_reg.rs2_val & 0x3F);
      break;
    case 0x2:
      result = (static_cast<int64_t>(issue_ex_reg.rs1_val) <
                static_cast<int64_t>(issue_ex_reg.rs2_val));
      break;
    case 0x3:
      result = (issue_ex_reg.rs1_val < issue_ex_reg.rs2_val);
      break;
    case 0x4:
      result = issue_ex_reg.rs1_val ^ issue_ex_reg.rs2_val;
      break;
    case 0x5:
      if (issue_ex_reg.funct7 == 0x20)
        result = static_cast<int64_t>(issue_ex_reg.rs1_val) >>
                 (issue_ex_reg.rs2_val & 0x3F);
      else
        result = issue_ex_reg.rs1_val >> (issue_ex_reg.rs2_val & 0x3F);
      break;
    case 0x6:
      result = issue_ex_reg.rs1_val | issue_ex_reg.rs2_val;
      break;
    case 0x7:
      result = issue_ex_reg.rs1_val & issue_ex_reg.rs2_val;
      break;
    }
    break;
  case 0x13: // I-type ALU (Immediate-Register)
    switch (issue_ex_reg.funct3) {
    case 0x0:
      result = issue_ex_reg.rs1_val + issue_ex_reg.imm;
      break;
    case 0x2:
      result = (static_cast<int64_t>(issue_ex_reg.rs1_val) < issue_ex_reg.imm);
      break;
    case 0x3:
      result = (issue_ex_reg.rs1_val < static_cast<uint64_t>(issue_ex_reg.imm));
      break;
    case 0x4:
      result = issue_ex_reg.rs1_val ^ issue_ex_reg.imm;
      break;
    case 0x6:
      result = issue_ex_reg.rs1_val | issue_ex_reg.imm;
      break;
    case 0x7:
      result = issue_ex_reg.rs1_val & issue_ex_reg.imm;
      break;
    case 0x1:
      result = issue_ex_reg.rs1_val << (issue_ex_reg.imm & 0x3F);
      break;
    case 0x5:
      if ((issue_ex_reg.imm & 0x400) != 0)
        result = static_cast<int64_t>(issue_ex_reg.rs1_val) >>
                 (issue_ex_reg.imm & 0x3F);
      else
        result = issue_ex_reg.rs1_val >> (issue_ex_reg.imm & 0x3F);
      break;
    }
    break;
  case 0x37:
    result = issue_ex_reg.imm;
    break; // LUI
  case 0x17:
    result = issue_ex_reg.pc + issue_ex_reg.imm;
    break;   // AUIPC
  case 0x1B: // OP-IMM-32: ADDIW / SLLIW / SRLIW / SRAIW
    switch (issue_ex_reg.funct3) {
    case 0x0: // ADDIW
      result = static_cast<int64_t>(static_cast<int32_t>(issue_ex_reg.rs1_val) +
                                    static_cast<int32_t>(issue_ex_reg.imm));
      break;
    case 0x1: // SLLIW
      result = static_cast<int64_t>(
          static_cast<int32_t>(static_cast<uint32_t>(issue_ex_reg.rs1_val)
                               << (issue_ex_reg.imm & 0x1F)));
      break;
    case 0x5: // SRLIW (funct7=0) / SRAIW (funct7=0x20)
      if (issue_ex_reg.funct7 == 0x20)
        result =
            static_cast<int64_t>(static_cast<int32_t>(issue_ex_reg.rs1_val) >>
                                 (issue_ex_reg.imm & 0x1F));
      else
        result = static_cast<int64_t>(
            static_cast<int32_t>(static_cast<uint32_t>(issue_ex_reg.rs1_val) >>
                                 (issue_ex_reg.imm & 0x1F)));
      break;
    }
    break;
  case 0x3B: // OP-32: ADDW/SUBW + M-extension W-type
             // (MULW/DIVW/DIVUW/REMW/REMUW)
    if (issue_ex_reg.funct7 == 0x01) {
      const int32_t ws1 = static_cast<int32_t>(issue_ex_reg.rs1_val);
      const int32_t ws2 = static_cast<int32_t>(issue_ex_reg.rs2_val);
      const uint32_t wu1 = static_cast<uint32_t>(issue_ex_reg.rs1_val);
      const uint32_t wu2 = static_cast<uint32_t>(issue_ex_reg.rs2_val);
      int64_t w_result = 0;
      switch (issue_ex_reg.funct3) {
      case 0x0: // MULW
        w_result = static_cast<int64_t>(static_cast<int32_t>(ws1 * ws2));
        break;
      case 0x4: // DIVW — div-by-zero and INT32_MIN/-1 overflow per spec
        w_result = (ws2 == 0) ? static_cast<int64_t>(-1)
                   : (ws1 == INT32_MIN && ws2 == -1)
                       ? static_cast<int64_t>(ws1)
                       : static_cast<int64_t>(ws1 / ws2);
        break;
      case 0x5: // DIVUW
        w_result = (wu2 == 0)
                       ? static_cast<int64_t>(static_cast<int32_t>(-1))
                       : static_cast<int64_t>(static_cast<int32_t>(wu1 / wu2));
        break;
      case 0x6: // REMW
        w_result = (ws2 == 0) ? static_cast<int64_t>(ws1)
                   : (ws1 == INT32_MIN && ws2 == -1)
                       ? 0LL
                       : static_cast<int64_t>(ws1 % ws2);
        break;
      case 0x7: // REMUW
        w_result = (wu2 == 0)
                       ? static_cast<int64_t>(static_cast<int32_t>(wu1))
                       : static_cast<int64_t>(static_cast<int32_t>(wu1 % wu2));
        break;
      }
      bool is_div_w = (issue_ex_reg.funct3 >= 4);
      FunctionalUnitState &fw = is_div_w ? div_fu : mul_fu;
      fw.busy = true;

      int div_cycles = 1;
      if (is_div_w) {
        bool is_signed = (issue_ex_reg.funct3 == 4 || issue_ex_reg.funct3 == 6);
        uint64_t A, B;
        if (is_signed) {
          A = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(issue_ex_reg.rs1_val)));
          B = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(issue_ex_reg.rs2_val)));
        } else {
          A = static_cast<uint64_t>(static_cast<uint32_t>(issue_ex_reg.rs1_val));
          B = static_cast<uint64_t>(static_cast<uint32_t>(issue_ex_reg.rs2_val));
        }

        bool op_a_sign = is_signed && (static_cast<int64_t>(A) < 0);
        bool op_b_sign = is_signed && (static_cast<int64_t>(B) < 0);

        uint64_t lzc_a_input = op_a_sign ? (~A + 1) : A;
        uint64_t lzc_b_input = op_b_sign ? ~B : B;

        int lzc_a_result = clz64(lzc_a_input);
        int lzc_b_result = clz64(lzc_b_input);

        bool lzc_a_no_one = (lzc_a_input == 0);
        bool lzc_b_no_one = (lzc_b_input == 0);

        int shift_a = lzc_a_no_one ? 64 : lzc_a_result;
        int div_shift = lzc_b_result - shift_a;

        bool op_b_zero = lzc_b_no_one && !op_b_sign;
        bool op_b_neg_one = lzc_b_no_one && op_b_sign;
        bool div_res_zero = (div_shift < 0);

        if (div_res_zero || op_b_zero || op_b_neg_one) {
          div_cycles = 1;
        } else {
          div_cycles = div_shift + 6;
        }
      }

      fw.remaining = is_div_w ? div_cycles : 1;
      fw.result = static_cast<uint64_t>(w_result);
      fw.trans_id = issue_ex_reg.rob_index;
      fw.rd = issue_ex_reg.rd;
      multi_cycle_dispatched = true;
      break;
    }
    {
      const int32_t a = static_cast<int32_t>(issue_ex_reg.rs1_val);
      const int32_t b = static_cast<int32_t>(issue_ex_reg.rs2_val);
      const uint32_t ua = static_cast<uint32_t>(issue_ex_reg.rs1_val);
      const uint32_t shamt = static_cast<uint32_t>(issue_ex_reg.rs2_val) & 0x1F;
      switch (issue_ex_reg.funct3) {
      case 0x0: // ADDW / SUBW
        result = static_cast<int64_t>((issue_ex_reg.funct7 == 0x20) ? (a - b)
                                                                    : (a + b));
        break;
      case 0x1: // SLLW
        result = static_cast<int64_t>(static_cast<int32_t>(ua << shamt));
        break;
      case 0x5: // SRLW (funct7=0) / SRAW (funct7=0x20)
        if (issue_ex_reg.funct7 == 0x20)
          result = static_cast<int64_t>(a >> shamt);
        else
          result = static_cast<int64_t>(static_cast<int32_t>(ua >> shamt));
        break;
      default:
        result = 0;
        break;
      }
    }
    break;
  case 0x6F: // JAL
    // Link address is pc+2 for compressed C.J (but rd=0, so it's discarded
    // anyway)
    result = issue_ex_reg.pc + (issue_ex_reg.is_compressed ? 2u : 4u);
    branch_target = issue_ex_reg.pc + issue_ex_reg.imm;
    branch_taken = true;
    break;
  case 0x67: // JALR
    // C.JALR stores pc+2 in ra; C.JR has rd=0 so the value is discarded
    result = issue_ex_reg.pc + (issue_ex_reg.is_compressed ? 2u : 4u);
    branch_target =
        (issue_ex_reg.rs1_val + issue_ex_reg.imm) & ~static_cast<uint64_t>(1);
    branch_taken = true;
    break;
  case 0x63:          // Branch
    stats.branches++; // Count Branch
    branch_target = issue_ex_reg.pc + issue_ex_reg.imm;
    switch (issue_ex_reg.funct3) {
    case 0x0:
      branch_taken = (issue_ex_reg.rs1_val == issue_ex_reg.rs2_val);
      break;
    case 0x1:
      branch_taken = (issue_ex_reg.rs1_val != issue_ex_reg.rs2_val);
      break;
    case 0x4:
      branch_taken = (static_cast<int64_t>(issue_ex_reg.rs1_val) <
                      static_cast<int64_t>(issue_ex_reg.rs2_val));
      break;
    case 0x5:
      branch_taken = (static_cast<int64_t>(issue_ex_reg.rs1_val) >=
                      static_cast<int64_t>(issue_ex_reg.rs2_val));
      break;
    case 0x6:
      branch_taken = (issue_ex_reg.rs1_val < issue_ex_reg.rs2_val);
      break;
    case 0x7:
      branch_taken = (issue_ex_reg.rs1_val >= issue_ex_reg.rs2_val);
      break;
    }
    break;
  }

  // 2. Load/Store Execution (LSU)

  if (issue_ex_reg.opcode == 0x03) { // Load
    uint64_t vaddr = issue_ex_reg.rs1_val + issue_ex_reg.imm;
    uint64_t addr =
        vaddr; // declared before goto to avoid crossing initialization
    int cur_dcache_slot = -1; // slot allocated for THIS load's miss (if any)

    // Alignment check
    bool misaligned = false;
    switch (issue_ex_reg.funct3) {
    case 0x1:
    case 0x5: // halfword
      if (vaddr & 1)
        misaligned = true;
      break;
    case 0x2:
    case 0x6: // word
      if (vaddr & 3)
        misaligned = true;
      break;
    case 0x3: // doubleword
      if (vaddr & 7)
        misaligned = true;
      break;
    }
    if (misaligned) {
      // Handle natively (like hardware with misaligned support).
      // Translate VA, then do byte-level reads from the physical address.
      auto tr_ua = mmu->translate(vaddr, csr, MMU::LOAD);
      if (tr_ua.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr_ua.fault_cause, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      uint64_t pa_ua = tr_ua.paddr;
      int ua_bytes =
          (issue_ex_reg.funct3 == 0x3)
              ? 8
              : ((issue_ex_reg.funct3 == 0x2 || issue_ex_reg.funct3 == 0x6)
                     ? 4
                     : 2);
      uint64_t raw_ua = 0;
      for (int i = 0; i < ua_bytes; i++)
        raw_ua |= ((uint64_t)mem_intf->readDataMem(pa_ua + i, 1)) << (8 * i);
      switch (issue_ex_reg.funct3) {
      case 0x1:
        result = static_cast<int64_t>(static_cast<int16_t>(raw_ua));
        break; // LH
      case 0x2:
        result = static_cast<int64_t>(static_cast<int32_t>(raw_ua));
        break; // LW
      case 0x3:
        result = static_cast<int64_t>(raw_ua);
        break; // LD
      case 0x5:
        result = raw_ua & 0xFFFF;
        break; // LHU
      case 0x6:
        result = raw_ua & 0xFFFFFFFF;
        break; // LWU
      default:
        result = raw_ua;
        break;
      }
      scoreboard.complete(issue_ex_reg.rob_index, result, issue_ex_reg.rd);
      goto ex_done;
    }

    // DTLB translation (Phase 8)
    {
      auto tr = mmu->translate(vaddr, csr, MMU::LOAD);
      if (tr.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr.fault_cause, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0); // unblock ROB
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      if (tr.stall_cycles > 0) {
        // DTLB miss: add PTW latency to effective load latency.
        stats.dtlb_miss_cycles += static_cast<uint64_t>(tr.stall_cycles);
        // Issue's structural hazard check (dcache_miss_fu_full()) already
        // guaranteed a free slot before this load was dispatched.
        cur_dcache_slot = dcache_miss_free_slot();
        if (cur_dcache_slot < 0) cur_dcache_slot = 0; // defensive fallback
        auto &dmf = dcache_miss_fu[cur_dcache_slot];
        dmf.busy = true;
#ifdef ENABLE_AXI_CONTENTION
        axi_top->request(riscv_axi::AxiContentionTop::DCACHE, tr.paddr);
#else
        dmf.remaining = tr.stall_cycles + dcache_miss_penalty - 1;
#endif
        dmf.result = 0; // placeholder, overwritten below after real read
        dmf.trans_id = issue_ex_reg.rob_index;
        dmf.rd = issue_ex_reg.rd;
        multi_cycle_dispatched = true;
        addr = tr.paddr;
        // Still perform the memory read so the result is ready when the stall
        // expires. (We overwrite dcache_miss_fu.result below.)
      } else {
        addr = tr.paddr;
      }
    }
    int load_size = 0;
    switch (issue_ex_reg.funct3) {
    case 0x0:
    case 0x4:
      load_size = 1;
      break;
    case 0x1:
    case 0x5:
      load_size = 2;
      break;
    case 0x2:
    case 0x6:
      load_size = 4;
      break;
    case 0x3:
      load_size = 8;
      break;
    }

    if (!csr.pmp_check(addr, load_size, false, false)) {
      if (has_unready_older()) {
        stall_ex = true;
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      uint64_t tvec =
          csr.take_exception(CAUSE::LOAD_ACCESS, issue_ex_reg.pc, vaddr);
      pc_redirect_target = tvec;
      pc_redirect_valid = true;
      flush_pipeline = true;
      trap_pending = true;
      scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
      multi_cycle_dispatched = true;
      goto ex_done;
    }
    uint64_t fwd = 0;
    auto fwd_result = store_buffer.forward_load(addr, load_size, fwd);

    if (fwd_result == StoreBuffer<4, 4>::FwdResult::PARTIAL_OVERLAP) {
      // A buffered store partially overlaps this load but doesn't fully
      // contain it.  We cannot forward (mix of store-buffer + memory data)
      // and we cannot read memory (the buffered store hasn't drained yet).
      // Set stall_ex so the latch is frozen and the load retries next cycle.
      // The overlapping store will eventually drain via Commit_stage.
      stall_ex = true;
      multi_cycle_dispatched = true; // prevent spurious scoreboard.complete()
      goto ex_done;
    }
    if (fwd_result == StoreBuffer<4, 4>::FwdResult::HIT) {
      // Load-to-store forwarding: data comes from the store buffer
      // (CVA6 12-bit page-offset alias check confirmed a safe match).
      uint64_t fwd_result_val = 0;
      switch (issue_ex_reg.funct3) {
      case 0x0:
        fwd_result_val = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int8_t>(fwd)));
        break;
      case 0x1:
        fwd_result_val = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int16_t>(fwd)));
        break;
      case 0x2:
        fwd_result_val = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int32_t>(fwd)));
        break;
      case 0x3:
        fwd_result_val = fwd;
        break;
      case 0x4:
        fwd_result_val = fwd & 0xFFULL;
        break;
      case 0x5:
        fwd_result_val = fwd & 0xFFFFULL;
        break;
      case 0x6:
        fwd_result_val = fwd & 0xFFFFFFFFULL;
        break;
      }
      stats.load_store_forwards++;
      // load-use stall: configured by load_hit_penalty to represent AXI bus latency on hits.
      if (!load_hit_fu.busy) {
        load_hit_fu.busy = true;
        load_hit_fu.remaining = load_hit_penalty;
        load_hit_fu.result = fwd_result_val;
        load_hit_fu.trans_id = issue_ex_reg.rob_index;
        load_hit_fu.rd = issue_ex_reg.rd;
      }
      multi_cycle_dispatched = true;
    } else {
      // FwdResult::MISS — no overlapping stores in the buffer; safe to read
      // memory.
      uint64_t mem_result = 0;
      switch (issue_ex_reg.funct3) {
      case 0x0:
        mem_result = static_cast<uint64_t>(static_cast<int64_t>(
            static_cast<int8_t>(mem_intf->readDataMem(addr, 1))));
        break;
      case 0x1:
        mem_result = static_cast<uint64_t>(static_cast<int64_t>(
            static_cast<int16_t>(mem_intf->readDataMem(addr, 2))));
        break;
      case 0x2:
        mem_result = static_cast<uint64_t>(static_cast<int64_t>(
            static_cast<int32_t>(mem_intf->readDataMem(addr, 4))));
        break;
      case 0x3:
        mem_result = mem_intf->readDataMem64(addr, 8);
        break;
      case 0x4:
        mem_result = mem_intf->readDataMem(addr, 1);
        break;
      case 0x5:
        mem_result = mem_intf->readDataMem(addr, 2);
        break;
      case 0x6:
        mem_result = mem_intf->readDataMem(addr, 4);
        break;
      }
      if (multi_cycle_dispatched) {
        // DTLB miss already started dcache_miss_fu[cur_dcache_slot]; fill in
        // the real result on that same slot.
        dcache_miss_fu[cur_dcache_slot >= 0 ? cur_dcache_slot : 0].result = mem_result;
      } else {
        // D$ timing check: hit → result available this cycle; miss → defer via
        // FU.
        if (!dcache.access(addr)) {
          stats.dcache_misses++;
          cur_dcache_slot = dcache_miss_free_slot();
          if (cur_dcache_slot < 0) cur_dcache_slot = 0; // defensive fallback
          auto &dmf = dcache_miss_fu[cur_dcache_slot];
          dmf.busy = true;
#ifdef ENABLE_AXI_CONTENTION
          axi_top->request(riscv_axi::AxiContentionTop::DCACHE, addr);
#else
          dmf.remaining = dcache_miss_penalty - 1;
#endif
          dmf.result = mem_result;
          dmf.trans_id = issue_ex_reg.rob_index;
          dmf.rd = issue_ex_reg.rd;
          multi_cycle_dispatched = true;
        } else {
          // D$ hit: result available next cycle — load-use stall configured by load_hit_penalty.
          if (!load_hit_fu.busy) {
            load_hit_fu.busy = true;
            load_hit_fu.remaining = load_hit_penalty;
            load_hit_fu.result = mem_result;
            load_hit_fu.trans_id = issue_ex_reg.rob_index;
            load_hit_fu.rd = issue_ex_reg.rd;
          }
          multi_cycle_dispatched = true;
        }
      }
    }
  } else if (issue_ex_reg.opcode ==
             0x23) { // Store → translate VA then add to speculative queue
    uint64_t vaddr_s = issue_ex_reg.rs1_val + issue_ex_reg.imm;
    uint64_t store_pa =
        vaddr_s; // declared before goto to avoid crossing initialization

    // Alignment check
    bool misaligned = false;
    switch (issue_ex_reg.funct3) {
    case 0x1: // halfword
      if (vaddr_s & 1)
        misaligned = true;
      break;
    case 0x2: // word
      if (vaddr_s & 3)
        misaligned = true;
      break;
    case 0x3: // doubleword
      if (vaddr_s & 7)
        misaligned = true;
      break;
    }
    if (misaligned) {
      // Handle natively — byte-level write after VA→PA translation.
      auto tr_us = mmu->translate(vaddr_s, csr, MMU::STORE);
      if (tr_us.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr_us.fault_cause, issue_ex_reg.pc, vaddr_s);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      uint64_t pa_us = tr_us.paddr;
      int us_bytes = (issue_ex_reg.funct3 == 0x3)
                         ? 8
                         : (issue_ex_reg.funct3 == 0x2 ? 4 : 2);
      uint64_t sval = issue_ex_reg.rs2_val;
      for (int i = 0; i < us_bytes; i++)
        mem_intf->writeDataMem(
            pa_us + i, static_cast<uint32_t>((sval >> (8 * i)) & 0xFF), 1);
      // Misaligned store bypassed store buffer — clear is_store so
      // Commit_stage does not call commit_store() (which would pop the
      // wrong spec entry and desynchronise the FIFO).
      scoreboard[issue_ex_reg.rob_index].is_store = false;
      scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
      goto ex_done;
    }

    {
      auto tr = mmu->translate(vaddr_s, csr, MMU::STORE);
      if (tr.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr.fault_cause, issue_ex_reg.pc, vaddr_s);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      store_pa = tr.paddr;
    }
    int size = 0;
    switch (issue_ex_reg.funct3) {
    case 0x0:
      size = 1;
      break;
    case 0x1:
      size = 2;
      break;
    case 0x2:
      size = 4;
      break;
    case 0x3:
      size = 8;
      break;
    }

    if (!csr.pmp_check(store_pa, size, true, false)) {
      if (has_unready_older()) {
        stall_ex = true;
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      uint64_t tvec =
          csr.take_exception(CAUSE::STORE_ACCESS, issue_ex_reg.pc, vaddr_s);
      pc_redirect_target = tvec;
      pc_redirect_valid = true;
      flush_pipeline = true;
      trap_pending = true;
      scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
      multi_cycle_dispatched = true;
      goto ex_done;
    }

    store_buffer.add_store(store_pa, issue_ex_reg.rs2_val, size,
                           issue_ex_reg.rob_index);
    if (load_reservation_valid && load_reservation_addr >= store_pa &&
        load_reservation_addr < store_pa + size) {
      load_reservation_valid = false;
    }
  } else if (issue_ex_reg.opcode ==
             0x2F) { // AMO — Atomic Memory Operations (A extension)
    uint64_t vaddr_a = issue_ex_reg.rs1_val;
    uint64_t pa_a = vaddr_a;
    bool is_dword = (issue_ex_reg.funct3 == 0x3);
    uint32_t funct5 = issue_ex_reg.funct7 >> 2; // bits[31:27]

    // VA -> PA translation.
    // LR is a load — use LOAD permission. SC and AMO RMW require STORE
    // permission.
    {
      MMU::access_t amo_atype = (funct5 == 0x2) ? MMU::LOAD : MMU::STORE;
      auto tr = mmu->translate(vaddr_a, csr, amo_atype);
      if (tr.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr.fault_cause, issue_ex_reg.pc, vaddr_a);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      pa_a = tr.paddr;
    }

    if (funct5 == 0x2) { // LR.W / LR.D
      // Check store buffer first — committed-but-not-yet-drained stores must be
      // visible to LR (same ordering guarantee as regular loads).
      // Note: Issue stage stalls AMOs on non-empty store buffer, so this
      // should normally be MISS. Handling HIT/PARTIAL for robustness.
      int lr_size = is_dword ? 8 : 4;
      uint64_t fwd_lr = 0;
      auto lr_fwd = store_buffer.forward_load(pa_a, lr_size, fwd_lr);
      if (lr_fwd == StoreBuffer<4, 4>::FwdResult::PARTIAL_OVERLAP) {
        stall_ex = true;
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      if (lr_fwd == StoreBuffer<4, 4>::FwdResult::HIT) {
        result = is_dword ? static_cast<int64_t>(fwd_lr)
                          : static_cast<int64_t>(static_cast<int32_t>(fwd_lr));
      } else if (is_dword) {
        result = static_cast<int64_t>(mem_intf->readDataMem64(pa_a, 8));
      } else {
        result = static_cast<int64_t>(
            static_cast<int32_t>(mem_intf->readDataMem(pa_a, 4)));
      }
      load_reservation_addr = pa_a;
      load_reservation_valid = true;
    } else if (funct5 == 0x3) { // SC.W / SC.D
      if (load_reservation_valid && load_reservation_addr == pa_a) {
        // SC success: write directly to memory for immediate atomicity.
        // Invalidate any store buffer entry for this address so that a
        // subsequent load sees the SC result and not stale buffered data.
        int sc_size = is_dword ? 8 : 4;
        if (is_dword)
          mem_intf->writeDataMem64(pa_a, issue_ex_reg.rs2_val, 8);
        else
          mem_intf->writeDataMem(
              pa_a, static_cast<uint32_t>(issue_ex_reg.rs2_val), 4);
        store_buffer.invalidate_address(pa_a, sc_size);
        result = 0; // success
      } else {
        result = 1; // failure
      }
      load_reservation_valid = false;
    } else { // AMO read-modify-write: read old value, compute new, write
             // directly (atomic)
      int amo_size = is_dword ? 8 : 4;
      if (is_dword) {
        uint64_t old64 = mem_intf->readDataMem64(pa_a, 8);
        uint64_t op64 = issue_ex_reg.rs2_val;
        uint64_t new64;
        switch (funct5) {
        case 0x1:
          new64 = op64;
          break; // AMOSWAP
        case 0x0:
          new64 = old64 + op64;
          break; // AMOADD
        case 0x4:
          new64 = old64 ^ op64;
          break; // AMOXOR
        case 0xC:
          new64 = old64 & op64;
          break; // AMOAND
        case 0x8:
          new64 = old64 | op64;
          break; // AMOOR
        case 0x10:
          new64 = (static_cast<int64_t>(old64) < static_cast<int64_t>(op64))
                      ? old64
                      : op64;
          break; // AMOMIN
        case 0x14:
          new64 = (static_cast<int64_t>(old64) > static_cast<int64_t>(op64))
                      ? old64
                      : op64;
          break; // AMOMAX
        case 0x18:
          new64 = (old64 < op64) ? old64 : op64;
          break; // AMOMINU
        case 0x1C:
          new64 = (old64 > op64) ? old64 : op64;
          break; // AMOMAXU
        default:
          new64 = old64;
          break;
        }
        mem_intf->writeDataMem64(pa_a, new64, amo_size);
        // Invalidate store buffer entries for this PA so subsequent
        // loads are not forwarded stale pre-AMO data.
        store_buffer.invalidate_address(pa_a, amo_size);
        load_reservation_valid = false;
        result = static_cast<int64_t>(old64); // rd gets old value
      } else {
        uint32_t old32 = mem_intf->readDataMem(pa_a, 4);
        uint32_t op32 = static_cast<uint32_t>(issue_ex_reg.rs2_val);
        uint32_t new32;
        switch (funct5) {
        case 0x1:
          new32 = op32;
          break; // AMOSWAP.W
        case 0x0:
          new32 = old32 + op32;
          break; // AMOADD.W
        case 0x4:
          new32 = old32 ^ op32;
          break; // AMOXOR.W
        case 0xC:
          new32 = old32 & op32;
          break; // AMOAND.W
        case 0x8:
          new32 = old32 | op32;
          break; // AMOOR.W
        case 0x10:
          new32 = (static_cast<int32_t>(old32) < static_cast<int32_t>(op32))
                      ? old32
                      : op32;
          break; // AMOMIN.W
        case 0x14:
          new32 = (static_cast<int32_t>(old32) > static_cast<int32_t>(op32))
                      ? old32
                      : op32;
          break; // AMOMAX.W
        case 0x18:
          new32 = (old32 < op32) ? old32 : op32;
          break; // AMOMINU.W
        case 0x1C:
          new32 = (old32 > op32) ? old32 : op32;
          break; // AMOMAXU.W
        default:
          new32 = old32;
          break;
        }
        mem_intf->writeDataMem(pa_a, new32, amo_size);
        // Invalidate store buffer entries for this PA so subsequent
        // loads are not forwarded stale pre-AMO data.
        store_buffer.invalidate_address(pa_a, amo_size);
        load_reservation_valid = false;
        result = static_cast<int64_t>(
            static_cast<int32_t>(old32)); // sign-extend to 64-bit
      }
    }
  }

  // 3. Branch Prediction Verification & Training
  bool is_branch_instruction; // declared without initializer to allow goto from
                              // above
  is_branch_instruction =
      (issue_ex_reg.opcode == 0x63 || issue_ex_reg.opcode == 0x6F ||
       issue_ex_reg.opcode == 0x67);

  if (is_branch_instruction) {
    // Alignment check
    if (branch_taken && (branch_target & 1)) {
      if (has_unready_older()) {
        stall_ex = true;
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      uint64_t tvec = csr.take_exception(CAUSE::INSTR_MISALIGN, issue_ex_reg.pc,
                                         branch_target);
      pc_redirect_target = tvec;
      pc_redirect_valid = true;
      flush_pipeline = true;
      trap_pending = true;
      scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
      multi_cycle_dispatched = true;
      goto ex_done;
    }

    // Verify Prediction
    bool mispredict =
        (branch_taken != issue_ex_reg.predicted_taken) ||
        (branch_taken && (branch_target != issue_ex_reg.predicted_target));

    // Train Predictor
    uint32_t btb_idx = (issue_ex_reg.pc >> 2) % 32;
    uint32_t bht_idx = (issue_ex_reg.pc >> 2) % 128;

    if (issue_ex_reg.opcode == 0x63) { // Conditional Branch
      uint8_t current_bht = bht[bht_idx];
      if (branch_taken) {
        if (current_bht < 3)
          bht[bht_idx]++;
      } else {
        if (current_bht > 0)
          bht[bht_idx]--;
      }
      btb[btb_idx].tag = issue_ex_reg.pc; // Tag to prevent aliasing
      btb[btb_idx].target = branch_target;
      btb[btb_idx].valid = true;
      btb[btb_idx].is_return = false;
    } else {                              // Unconditional Jump (JAL/JALR)
      btb[btb_idx].tag = issue_ex_reg.pc; // Tag to prevent aliasing
      btb[btb_idx].target = branch_target;
      btb[btb_idx].valid = true;
      bht[bht_idx] = 3; // Force Strongly Taken

      // RAS Maintenance
      if (issue_ex_reg.is_ras_call) {
        ras.push_back(issue_ex_reg.pc + (issue_ex_reg.is_compressed ? 2 : 4));
        if (ras.size() > 2)
          ras.erase(ras.begin());
      }
      if (issue_ex_reg.is_ras_return) {
        btb[btb_idx].is_return = true;
      }
    }

    // Redirect on Mispredict
    if (mispredict) {
      pc_redirect_target =
          branch_taken
              ? branch_target
              : (issue_ex_reg.pc + (issue_ex_reg.is_compressed ? 2 : 4));
      pc_redirect_valid = true;
      flush_pipeline = true;
      stats.flushes++;
      stats.branch_mispredicts++;
      ras.clear(); // Safe recovery strategy on mispredict

      // Flush only speculative instructions younger than this branch
      scoreboard.flush_speculative(issue_ex_reg.rob_index);
      store_buffer.flush_speculative(issue_ex_reg.rob_index, scoreboard);
      // Cancel multi-cycle FUs whose ROB entries were just flushed;
      // without this they'd write results to reallocated slots later.
      auto cancel_if_flushed = [&](auto &fu) {
        if (fu.busy && fu.trans_id >= 0 &&
            !scoreboard.get_entry(fu.trans_id).valid)
          fu.busy = false;
      };
      cancel_if_flushed(mul_fu);
      cancel_if_flushed(div_fu);
      for (int fs = 0; fs < FPU_SLOTS; fs++) cancel_if_flushed(fpu_pipe[fs]);
      cancel_if_flushed(fpu_divsqrt_fu);
      for (int i = 0; i < DCACHE_MISS_SLOTS; i++) {
#ifdef ENABLE_AXI_CONTENTION
        // Selective flush: release the D$ AXI refill only if this branch
        // actually cancelled it (younger than the mispredicted branch). An
        // older, still-valid D$ miss keeps its in-flight request.
        bool was_busy = dcache_miss_fu[i].busy;
        cancel_if_flushed(dcache_miss_fu[i]);
        if (was_busy && !dcache_miss_fu[i].busy)
          axi_top->ack(riscv_axi::AxiContentionTop::DCACHE);
#else
        cancel_if_flushed(dcache_miss_fu[i]);
#endif
      }
      cancel_if_flushed(load_hit_fu);
    }
  }

  // 4. System instructions (opcode 0x73): CSR, ECALL, EBREAK, MRET, SRET
  if (issue_ex_reg.opcode == 0x73) {
    const uint8_t f3 = issue_ex_reg.funct3;
    const uint16_t csr_addr = static_cast<uint16_t>(issue_ex_reg.imm & 0xFFF);

    if (f3 == 0) {
      // ECALL / EBREAK / MRET / SRET — distinguished by imm
      if (csr_addr == 0x000) {
        // ECALL — raise exception
        uint64_t cause;
        switch (csr.priv) {
        case PrivMode::U:
          cause = CAUSE::ECALL_U;
          break;
        case PrivMode::S:
          cause = CAUSE::ECALL_S;
          break;
        default:
          cause = CAUSE::ECALL_M;
          break;
        }
        // Bare-metal syscall shim (when running without OS at M-mode)
        if (csr.priv == PrivMode::M) {
          uint64_t syscall_num = register_bank->getValue(17);
          uint64_t a0 = register_bank->getValue(10);
          uint64_t a1 = register_bank->getValue(11);
          uint64_t a2 = register_bank->getValue(12);
          switch (syscall_num) {
          case 64: { // write(fd, buf, count)
            FILE *out = (a0 == 1) ? stdout : (a0 == 2) ? stderr : nullptr;
            if (out && a2 > 0) {
              for (uint64_t i = 0; i < a2; i++) {
                uint8_t byte =
                    static_cast<uint8_t>(mem_intf->readDataMem(a1 + i, 1));
                fputc(byte, out);
              }
              fflush(out);
            }
            register_bank->setValue(10, a2);
            break;
          }
          case 1:  // exit (legacy Newlib / bare-metal)
          case 93: // exit (standard Linux / proxy kernel)
            std::cout << "\n[ECALL] exit(" << a0 << ")" << std::endl;
            sc_core::sc_stop();
            break;
          case 214:
            register_bank->setValue(10, 0);
            break;
          default:
            register_bank->setValue(10, static_cast<uint64_t>(-38)); // -ENOSYS
            break;
          }
        } else {
          // S/U mode ECALL — trap to handler
          if (has_unready_older()) {
            stall_ex = true;
            multi_cycle_dispatched = true;
            goto ex_done;
          }
          trap_vector_addr = csr.take_exception(cause, issue_ex_reg.pc, 0);
          trap_pending = true;
          flush_pipeline = true;
          pc_redirect_target = trap_vector_addr;
          pc_redirect_valid = true;
        }
      } else if (csr_addr == 0x001) {
        // EBREAK — raise Breakpoint exception (cause=3).
        // OpenSBI uses EBREAK to probe for debugger presence; it must
        // trap to the M-mode handler (mtvec), NOT halt the simulation.
        // Per RISC-V spec: mtval=0 for breakpoints (not the faulting PC).

        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(CAUSE::BREAKPOINT, issue_ex_reg.pc, 0);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
      } else if (csr_addr == 0x302) {
        // MRET — privilege check: must be in M-mode
        if (csr.priv != PrivMode::M) {
          uint64_t tvec = csr.take_exception(
              CAUSE::ILLEGAL_INSTR, issue_ex_reg.pc, issue_ex_reg.instr);
          pc_redirect_target = tvec;
          pc_redirect_valid = true;
          flush_pipeline = true;
        } else {
          uint64_t ret_pc = csr.mret();
          pc_redirect_target = ret_pc;
          pc_redirect_valid = true;
          flush_pipeline = true;
          mmu->flush_all(); // satp may have changed
          load_reservation_valid = false;
          pending_priv_switch = true;
          pending_priv_switch_epc = ret_pc;
        }
      } else if (csr_addr == 0x102) {
        // SRET — privilege check: must be in S or M mode; TW trap check
        if (csr.priv == PrivMode::U ||
            (csr.priv == PrivMode::S && (csr.mstatus & MSTATUS::TSR))) {
          uint64_t tvec = csr.take_exception(
              CAUSE::ILLEGAL_INSTR, issue_ex_reg.pc, issue_ex_reg.instr);
          pc_redirect_target = tvec;
          pc_redirect_valid = true;
          flush_pipeline = true;
        } else {
          uint64_t ret_pc = csr.sret();
          pc_redirect_target = ret_pc;
          pc_redirect_valid = true;
          flush_pipeline = true;
          load_reservation_valid = false;
          pending_priv_switch = true;
          pending_priv_switch_epc = ret_pc;
        }
      } else if (csr_addr == 0x105) {
        // WFI — wait for interrupt.
        // TW: trap on WFI in S/U mode if mstatus.TW=1
        if (csr.priv != PrivMode::M && (csr.mstatus & MSTATUS::TW)) {
          uint64_t tvec = csr.take_exception(
              CAUSE::ILLEGAL_INSTR, issue_ex_reg.pc, issue_ex_reg.instr);
          pc_redirect_target = tvec;
          pc_redirect_valid = true;
          flush_pipeline = true;
        } else {
          wfi_stall = true;
          result = 0;
          // Flush pipeline behind WFI and set resume PC to WFI+4.
          // Without this, instructions already in-flight (after WFI)
          // drain through the pipeline; their ret redirects fetch,
          // and the idle loop spins without advancing sim-time.
          flush_pipeline = true;
          pc_redirect_target = issue_ex_reg.pc + 4;
          pc_redirect_valid = true;
        }
      } else {
        // Check for SFENCE.VMA: funct7=0x09, funct3=0, rs1/rs2 in instruction
        // fields Encoded as: imm[31:20]=0b000100101XX where bits[24:20]=rs2,
        // bits[19:15]=rs1
        uint32_t raw = issue_ex_reg.instr;
        uint8_t f7 = static_cast<uint8_t>(raw >> 25);
        if (f7 == 0x09) {
          // SFENCE.VMA: flush TLBs per rs1/rs2
          bool rs1_zero = (issue_ex_reg.rs1 == 0);
          bool rs2_zero = (static_cast<uint8_t>((raw >> 20) & 0x1F) == 0);
          mmu->sfence_vma(issue_ex_reg.rs1_val, issue_ex_reg.rs2_val, rs1_zero,
                          rs2_zero);
          flush_pipeline = true; // Redirect to PC+4 to drain pipeline
          pc_redirect_target = issue_ex_reg.pc + 4;
          pc_redirect_valid = true;
        }
        // Other unknown funct12 — treat as NOP (future: illegal instruction
        // trap)
      }
    } else {
      // CSR instructions: CSRRW(I)=1/5, CSRRS(I)=2/6, CSRRC(I)=3/7
      bool is_imm = (f3 >= 5);
      uint64_t uimm = (issue_ex_reg.rs1) & 0x1F; // zimm for *I variants
      uint64_t rs1_v = is_imm ? uimm : issue_ex_reg.rs1_val;

      bool ok = false;
      uint64_t old_val = csr.read(csr_addr, ok);
      if (!ok) {
        // Illegal CSR access — trap
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        trap_vector_addr = csr.take_exception(
            CAUSE::ILLEGAL_INSTR, issue_ex_reg.pc, issue_ex_reg.instr);
        trap_pending = true;
        flush_pipeline = true;
        pc_redirect_target = trap_vector_addr;
        pc_redirect_valid = true;
      } else {
        // Read-modify-write semantics per spec
        uint8_t op = f3 & 0x3; // 1=RW, 2=RS, 3=RC
        bool written = false;
        if (op == 1) {
          csr.write(csr_addr, rs1_v); // CSRRW
          written = true;
        } else if (op == 2 && rs1_v != 0) {
          csr.write(csr_addr, old_val | rs1_v); // CSRRS
          written = true;
        } else if (op == 3 && rs1_v != 0) {
          csr.write(csr_addr, old_val & ~rs1_v); // CSRRC
          written = true;
        }
        if (written) {
          pending_csr_write = {true, csr_addr};
        }
        result = old_val; // CSR rd gets previous value
        // Changing satp (address space switch) invalidates all TLBs and must
        // serialise the pipeline.  Real CVA6 hardware flushes the pipeline on
        // satp writes so that the Fetch stage never re-translates an already
        // in-flight PC through the newly-active page tables.  Without this flush
        // Fetch (which runs in the same sim-cycle as EX) would immediately try
        // to translate a stale physical PC like 0x80201048 through the new sv39
        // page tables, find no identity mapping, take an instruction page-fault,
        // and redirect to stvec — hanging the simulator at early boot.
        if (csr_addr == CSR::SATP) {
          mmu->flush_all();
          // Redirect to PC+4 through the new translation mode.
          flush_pipeline = true;
          pc_redirect_target =
              issue_ex_reg.pc + (issue_ex_reg.is_compressed ? 2 : 4);
          pc_redirect_valid = true;
        }
      }
    }
  }

  // 4.5 Floating-Point Unit (FPU) Operations
  if (scoreboard[issue_ex_reg.rob_index].fu == FunctionalUnit::FPU) {
    bool writes_to_int_reg = false;
    if (issue_ex_reg.opcode == 0x53) {
      uint32_t f7 = issue_ex_reg.funct7;
      if (f7 == 0x50 || f7 == 0x51 || f7 == 0x60 || f7 == 0x61 || f7 == 0x70 ||
          f7 == 0x71) {
        writes_to_int_reg = true;
      }
    }

    bool is_double =
        (issue_ex_reg.opcode == 0x07 && issue_ex_reg.funct3 == 3) ||
        (issue_ex_reg.opcode == 0x27 && issue_ex_reg.funct3 == 3) ||
        (issue_ex_reg.opcode != 0x07 && issue_ex_reg.opcode != 0x27 &&
         ((issue_ex_reg.instr >> 25) & 0x3) == 1);

    // Handle address translation and PMP checking for float loads/stores
    uint64_t pa = 0;
    bool fp_load_dcache_miss = false;
    uint64_t fp_load_pa = 0; // carries FP-load paddr to the miss-handling site (AXI)
    if (issue_ex_reg.opcode == 0x07) { // Load-FP
      uint64_t vaddr = issue_ex_reg.rs1_val + issue_ex_reg.imm;
      // Alignment check
      bool misaligned = false;
      if (issue_ex_reg.funct3 == 2) {
        if (vaddr & 3)
          misaligned = true;
      } else if (issue_ex_reg.funct3 == 3) {
        if (vaddr & 7)
          misaligned = true;
      }
      if (misaligned) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(CAUSE::LOAD_MISALIGN, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      auto tr = mmu->translate(vaddr, csr, MMU::LOAD);
      if (tr.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr.fault_cause, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      pa = tr.paddr;
      if (!csr.pmp_check(pa, is_double ? 8 : 4, false, false)) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(CAUSE::LOAD_ACCESS, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      // FP loads go through the same L1 D$ as integer loads (CVA6 has one LSU).
      if (!dcache.access(pa)) {
        stats.dcache_misses++;
        fp_load_dcache_miss = true;
        fp_load_pa = pa;
      }
    } else if (issue_ex_reg.opcode == 0x27) { // Store-FP
      uint64_t vaddr = issue_ex_reg.rs1_val + issue_ex_reg.imm;
      // Alignment check
      bool misaligned = false;
      if (issue_ex_reg.funct3 == 2) {
        if (vaddr & 3)
          misaligned = true;
      } else if (issue_ex_reg.funct3 == 3) {
        if (vaddr & 7)
          misaligned = true;
      }
      if (misaligned) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(CAUSE::STORE_MISALIGN, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      auto tr = mmu->translate(vaddr, csr, MMU::STORE);
      if (tr.fault) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(tr.fault_cause, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      pa = tr.paddr;
      if (!csr.pmp_check(pa, is_double ? 8 : 4, true, false)) {
        if (has_unready_older()) {
          stall_ex = true;
          multi_cycle_dispatched = true;
          goto ex_done;
        }
        uint64_t tvec =
            csr.take_exception(CAUSE::STORE_ACCESS, issue_ex_reg.pc, vaddr);
        pc_redirect_target = tvec;
        pc_redirect_valid = true;
        flush_pipeline = true;
        trap_pending = true;
        scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
        multi_cycle_dispatched = true;
        goto ex_done;
      }
    }

    uint32_t fcsr_flags = 0;
    int latency = 0;
    uint64_t fpu_result = 0;

    uint32_t instr_to_decode = issue_ex_reg.instr;
    if (issue_ex_reg.is_compressed) {
      if (issue_ex_reg.opcode == 0x07) {
        instr_to_decode =
            ((issue_ex_reg.imm & 0xFFF) << 20) | (issue_ex_reg.rs1 << 15) |
            (issue_ex_reg.funct3 << 12) | (issue_ex_reg.rd << 7) | 0x07;
      } else if (issue_ex_reg.opcode == 0x27) {
        uint32_t imm = static_cast<uint32_t>(issue_ex_reg.imm);
        uint32_t imm_5 = imm & 0x1F;
        uint32_t imm_11 = (imm >> 5) & 0x7F;
        instr_to_decode = (imm_11 << 25) | (issue_ex_reg.rs2 << 20) |
                          (issue_ex_reg.rs1 << 15) |
                          (issue_ex_reg.funct3 << 12) | (imm_5 << 7) | 0x27;
      }
    }

    f_inst->setInstr(instr_to_decode);
    d_inst->setInstr(instr_to_decode);

    op_F_Codes f_op = f_inst->decode();
    op_D_Codes d_op = d_inst->decode();

    if (f_op != OP_F_ERROR) {
      latency =
          f_inst->execute(f_op, issue_ex_reg.rs1_val, issue_ex_reg.rs2_val, 0,
                          issue_ex_reg.rd, pa, fcsr_flags);
      if (writes_to_int_reg) {
        fpu_result = register_bank->getValue(issue_ex_reg.rd);
      }
    } else if (d_op != OP_D_ERROR) {
      latency =
          d_inst->execute(d_op, issue_ex_reg.rs1_val, issue_ex_reg.rs2_val, 0,
                          issue_ex_reg.rd, pa, fcsr_flags);
      if (writes_to_int_reg) {
        fpu_result = register_bank->getValue(issue_ex_reg.rd);
      }
    } else {
      if (has_unready_older()) {
        stall_ex = true;
        multi_cycle_dispatched = true;
        goto ex_done;
      }
      uint64_t tvec = csr.take_exception(CAUSE::ILLEGAL_INSTR, issue_ex_reg.pc,
                                         issue_ex_reg.instr);
      pc_redirect_target = tvec;
      pc_redirect_valid = true;
      flush_pipeline = true;
      trap_pending = true;
      scoreboard.complete(issue_ex_reg.rob_index, 0, 0);
      multi_cycle_dispatched = true;
      goto ex_done;
    }

    if (fcsr_flags != 0) {
      csr.set_fflags(fcsr_flags);
    }

    // FP stores (opcode 0x27) wrote directly to memory via f_inst/d_inst,
    // bypassing the store buffer.  Clear is_store so Commit_stage does not
    // call commit_store() (which would desynchronise the spec FIFO).
    if (issue_ex_reg.opcode == 0x27) {
      scoreboard[issue_ex_reg.rob_index].is_store = false;
    }

    if (fp_load_dcache_miss) {
      // FP load that missed in the D$: defer completion by the miss penalty,
      // exactly as the integer load path does. The FP register file was already
      // written by execute(); only the timing is modelled here.
      int fp_slot = dcache_miss_free_slot();
      if (fp_slot < 0) fp_slot = 0; // defensive fallback
      auto &dmf = dcache_miss_fu[fp_slot];
      dmf.busy = true;
#ifdef ENABLE_AXI_CONTENTION
      axi_top->request(riscv_axi::AxiContentionTop::DCACHE, fp_load_pa);
#else
      dmf.remaining = dcache_miss_penalty - 1;
#endif
      dmf.result = fpu_result;
      dmf.trans_id = issue_ex_reg.rob_index;
      dmf.rd = issue_ex_reg.rd;
    } else if (latency <= 1) {
      // Single-cycle FPU ops (FMV, FCLASS, etc.): complete immediately.
      scoreboard.complete(issue_ex_reg.rob_index, fpu_result, issue_ex_reg.rd);
    } else if (latency > 5) {
      // FP divide/sqrt: iterative, non-pipelined — blocking unit (stalls issue).
      fpu_divsqrt_fu.busy = true;
      fpu_divsqrt_fu.remaining = latency - 1;
      fpu_divsqrt_fu.result = fpu_result;
      fpu_divsqrt_fu.trans_id = issue_ex_reg.rob_index;
      fpu_divsqrt_fu.rd = issue_ex_reg.rd;
    } else {
      // Pipelined addmul/convert: place in a free slot (Issue stalls if all busy).
      int slot = -1;
      for (int fs = 0; fs < FPU_SLOTS; fs++) if (!fpu_pipe[fs].busy) { slot = fs; break; }
      if (slot < 0) slot = 0; // fallback (should not happen: issue stalls when full)
      fpu_pipe[slot].busy = true;
      fpu_pipe[slot].remaining = latency - 1;
      fpu_pipe[slot].result = fpu_result;
      fpu_pipe[slot].trans_id = issue_ex_reg.rob_index;
      fpu_pipe[slot].rd = issue_ex_reg.rd;
    }
    multi_cycle_dispatched = true;
  }

// 5. Complete instruction in scoreboard — result stored here until commit.
//    Multi-cycle FUs (MUL/DIV/D$-miss) call complete() themselves when they
//    finish.
ex_done:
  if (!multi_cycle_dispatched && issue_ex_reg.rob_index >= 0) {
    scoreboard.complete(issue_ex_reg.rob_index, result, issue_ex_reg.rd);
  }

  if (trap_pending) {
    // Synchronous traps (ECALL, page fault, illegal instr, etc.):
    // All instructions older than the trapping instruction are architecturally
    // committed — their register writes must be visible before we enter the
    // trap handler.  Drain ready ROB entries from head up to (but not
    // including) the trapping instruction's rob_index.
    //

    while (scoreboard.head_ready() &&
           scoreboard.get_head_index() != issue_ex_reg.rob_index) {
      const auto &entry = scoreboard.get_head();
      if (entry.rd != 0 && !entry.writes_to_fp_reg) {

        register_bank->setValue(entry.rd, entry.result);
      }
      if (entry.is_store) {
        store_buffer.commit_store(scoreboard.get_head_index());
      }
      scoreboard.retire();
    }
    // Now drain committed stores to memory so trap handler sees them.
    {
      uint64_t sb_addr, sb_data;
      int sb_size;
      while (store_buffer.drain_one(sb_addr, sb_data, sb_size)) {
        if (sb_size == 8)
          mem_intf->writeDataMem64(sb_addr, sb_data, sb_size);
        else
          mem_intf->writeDataMem(sb_addr, static_cast<uint32_t>(sb_data),
                                 sb_size);
      }
    }

    scoreboard.flush();
    store_buffer.flush_speculative();
    for (int i = 0; i < DCACHE_MISS_SLOTS; i++) {
#ifdef ENABLE_AXI_CONTENTION
      if (dcache_miss_fu[i].busy) axi_top->ack(riscv_axi::AxiContentionTop::DCACHE);
#endif
      dcache_miss_fu[i].busy = false;
    }
    mul_fu.busy = div_fu.busy =
        load_hit_fu.busy = false;
    for (int fs = 0; fs < FPU_SLOTS; fs++) fpu_pipe[fs].busy = false;
  fpu_divsqrt_fu.busy = false;
    multi_cycle_dispatched = true;
    load_reservation_valid = false;
  }
}

// =============================================================================
// Commit Stage (Architectural Commit)
// =============================================================================

void CPURV64P6_Cycle::Commit_stage() {
  if (store_drain_remaining > 0) {
    store_drain_remaining--;
  }

  // Always try to drain one committed store to memory each cycle,
  // independent of whether a new instruction is committing.
  // Restricted by write-through DRAM latency stalls.
  if (store_drain_remaining == 0) {
    uint64_t addr, data;
    int size;
    if (store_buffer.drain_one(addr, data, size)) {
      if (size == 8)
        mem_intf->writeDataMem64(addr, data, size);
      else
        mem_intf->writeDataMem(addr, static_cast<uint32_t>(data), size);

      store_drain_remaining = store_write_penalty;

      // Log recent stores for post-mortem analysis
      auto &rs = recent_stores[recent_store_idx % RECENT_STORE_LOG_SIZE];
      rs.pa = addr;
      rs.data = data;
      rs.size = size;
      rs.pc = pc_current;
      rs.cycle = stats.cycles;
      recent_store_idx++;
    }
  }

  for (int port = 0; port < 2; ++port) {
    if (!scoreboard.head_ready())
      break;

    const auto &entry = scoreboard.get_head();
    const int head_idx = scoreboard.get_head_index();

    // 1. Store commit: move from speculative queue to committed queue.
    //    drain_one() above will write it to memory next cycle.
    if (entry.is_store) {
      if (store_buffer.commit_is_full()) {
        break; // Stall commit until committed queue drains
      }
      bool ok = store_buffer.commit_store(head_idx);
      if (!ok) {
        std::cerr << "[BUG] commit_store failed: spec queue empty at PC=0x"
                  << std::hex << entry.pc << std::dec << "\n";
      }
    }

    // 2. Architectural register update.
    if (entry.rd != 0) {
      if (entry.writes_to_fp_reg) {
        auto &clob = scoreboard.fp_clobber[entry.rd];
        if (clob.trans_id == head_idx) {
          clob.busy = false;
          clob.trans_id = -1;
        }
      } else {
        register_bank->setValue(entry.rd, entry.result);
        auto &clob = scoreboard.rd_clobber[entry.rd];
        if (clob.trans_id == head_idx) {
          clob.busy = false;
          clob.trans_id = -1;
        }
      }
    }

    // 3. Statistics and trace.
    stats.instructions++;
    if (perf)
      perf->instructionsInc();
    pending_priv_switch =
        false; // instruction committed → priv switch is architecturally visible
    commit_count_this_cycle++;
    committed_pc_this_cycle = entry.pc;
    pc_current = entry.pc;

    // Per-privilege instruction counter
    if (csr.priv == PrivMode::M)
      stats.m_instrs++;
    else if (csr.priv == PrivMode::S) {
      stats.s_instrs++;
    } else {
      stats.u_instrs++;
    }

    // 4. Retire — advance scoreboard head pointer.
    scoreboard.retire();
  }

  if (commit_count_this_cycle == 2)
    stats.dual_commits++;
}

// =============================================================================
// Helpers
// =============================================================================

bool CPURV64P6_Cycle::cpu_process_IRQ() {
  int cause = csr.pending_interrupt();
  if (cause < 0)
    return false;

  // Determine EPC BEFORE flushing the scoreboard.
  // After scoreboard.flush() the ROB is empty; computing EPC post-flush falls
  // through to id_issue_reg and picks the wrong (younger) instruction.
  // The oldest in-flight instruction is the scoreboard head; if the ROB is
  // already empty but issue_ex_reg or later latches still carry instructions
  // those are the oldest un-committed work and must be re-executed.
  uint64_t irq_epc;
  if (pending_priv_switch) {
    irq_epc = pending_priv_switch_epc;
    pending_priv_switch = false;
  } else {
    if (!scoreboard.is_empty()) {
      irq_epc = scoreboard.get_head().pc;
    } else if (issue_ex_reg.valid) {
      irq_epc = issue_ex_reg.pc;
    } else if (id_issue_reg.valid) {
      irq_epc = id_issue_reg.pc;
    } else if (fetch_id_reg.valid) {
      irq_epc = fetch_id_reg.pc;
    } else if (pcgen_fetch_reg.valid) {
      irq_epc = pcgen_fetch_reg.pc;
    } else {
      irq_epc = next_pc;
    }
  }

  // Flush pipeline and redirect to interrupt handler

  scoreboard.flush();
  store_buffer.flush_speculative();
  for (int i = 0; i < DCACHE_MISS_SLOTS; i++) {
#ifdef ENABLE_AXI_CONTENTION
    if (dcache_miss_fu[i].busy) axi_top->ack(riscv_axi::AxiContentionTop::DCACHE);
#endif
    dcache_miss_fu[i].busy = false;
  }
  mul_fu.busy = div_fu.busy = false;
  for (int fs = 0; fs < FPU_SLOTS; fs++) fpu_pipe[fs].busy = false;
  fpu_divsqrt_fu.busy = false;
  stall_ex = false; // Cancel any pending EX replay

  // Invalidate any outstanding LR reservation — per RISC-V spec, interrupts
  // must break LR/SC sequences to prevent livelock across interrupt handlers.
  load_reservation_valid = false;

  uint64_t tvec = csr.take_interrupt(static_cast<uint64_t>(cause), irq_epc);
  pc_redirect_target = tvec;
  pc_redirect_valid = true;
  flush_pipeline = true;
  trap_pending = true;
  wfi_stall = false; // Wake from WFI
  return true;
}

void CPURV64P6_Cycle::call_interrupt(tlm::tlm_generic_payload &m_trans,
                                     sc_core::sc_time &delay) {
  interrupt = true;
  memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
  delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV64P6_Cycle::getStartDumpAddress() {
  return register_bank->getValue(Registers<BaseType>::t0);
}

std::uint64_t CPURV64P6_Cycle::getEndDumpAddress() {
  return register_bank->getValue(Registers<BaseType>::t1);
}

void CPURV64P6_Cycle::printStats() const {
  const char *priv_str = (csr.priv == PrivMode::M)   ? "M"
                         : (csr.priv == PrivMode::S) ? "S"
                                                     : "U";
  std::cout << "  Architecture: RV64IMAC (CVA6 6-Stage, M+S+U, sv39 MMU)\n";
  std::cout << "  Final Priv:   " << priv_str << "-mode\n";
  std::cout << "  Final PC:     0x" << std::hex << pc_current << std::dec
            << "\n";
  std::cout << "  M-mode Instr: " << stats.m_instrs << "\n";
  std::cout << "  S-mode Instr: " << stats.s_instrs << "\n";
  std::cout << "  U-mode Instr: " << stats.u_instrs << "\n";
  std::cout << "  Cycles:       " << stats.cycles << "\n";
  std::cout << "  Instructions: " << stats.instructions << "\n";
  std::cout << "  CPI:          " << std::fixed << std::setprecision(2)
            << stats.get_cpi() << "\n";
  std::cout << "  IPC:          " << std::fixed << std::setprecision(3)
            << stats.get_ipc() << "\n";
  std::cout << "  Stalls:       " << stats.stalls << "\n";
  std::cout << "  Forwarded:    " << stats.forwarded_reads
            << "  (RAW resolved by fwd)\n";
  std::cout << "  LD/ST Fwd:    " << stats.load_store_forwards
            << "  (loads from store buf)\n";
  std::cout << "  MUL busy:     " << stats.mul_stall_cycles
            << "  (cycles multiplier active)\n";
  std::cout << "  DIV busy:     " << stats.div_stall_cycles
            << "  (cycles divider active)\n";
  {
    uint64_t ia = icache.get_accesses(), im = icache.get_misses();
    uint64_t da = dcache.get_accesses(), dm = dcache.get_misses();
    std::cout << "  I$ accesses:  " << ia << "\n";
    if (ia > 0)
      std::cout << "  I$ miss rate: " << std::fixed << std::setprecision(2)
                << (100.0 * im / ia) << "%  (" << im << " misses, "
                << stats.icache_miss_cycles << " stall cycles)\n";
    std::cout << "  D$ accesses:  " << da << "\n";
    if (da > 0)
      std::cout << "  D$ miss rate: " << std::fixed << std::setprecision(2)
                << (100.0 * dm / da) << "%  (" << dm << " misses, "
                << stats.dcache_miss_cycles << " stall cycles)\n";
  }
  std::cout << "  Load-use stalls: " << stats.load_use_stall_cycles
            << "  (1-cycle penalty per D$-hit/fwd load)\n";
  if (mmu) {
    const auto &ms = mmu->stats;
    uint64_t it = ms.itlb_hits + ms.itlb_misses;
    uint64_t dt = ms.dtlb_hits + ms.dtlb_misses;
    if (it > 0)
      std::cout << "  ITLB miss rt: " << std::fixed << std::setprecision(2)
                << (100.0 * ms.itlb_misses / it) << "%  (" << ms.itlb_misses
                << " misses, " << stats.itlb_miss_cycles << " stall cycles)\n";
    if (dt > 0)
      std::cout << "  DTLB miss rt: " << std::fixed << std::setprecision(2)
                << (100.0 * ms.dtlb_misses / dt) << "%  (" << ms.dtlb_misses
                << " misses, " << stats.dtlb_miss_cycles << " stall cycles, "
                << ms.ptw_walks << " PTW walks)\n";
  }
  std::cout << "  Dual commits: " << stats.dual_commits
            << "  (cycles where 2 instrs committed)\n";
  std::cout << "  Load->addr bubbles: " << stats.load_addr_events
            << "  (B9 gather/pointer-chase penalties)\n";
  std::cout << "  DIAG I\$+D\$ co-stall cycles: " << diag_costall_cycles
            << "  (I$ miss AND D$ miss both active same cycle)\n";
  std::cout << "  DIAG zero-commit cycles: " << diag_zero_commit_cycles
            << "  (" << (stats.cycles > 0 ? 100.0 * diag_zero_commit_cycles / stats.cycles : 0)
            << "% of total cycles produce NO forward progress)\n";
  std::cout << "  DIAG stall runs: " << diag_stall_run_count
            << "  avg_len=" << (diag_stall_run_count ? (double)diag_stall_run_sum / diag_stall_run_count : 0.0)
            << "  max_len=" << diag_stall_run_max << "\n";
  std::cout << "  DIAG if_empty cycles: " << diag_if_empty_cycles
            << "  (" << (stats.cycles > 0 ? 100.0 * diag_if_empty_cycles / stats.cycles : 0)
            << "% -- RTL if_empty was 6.8%)\n";
  if (stats.cycles > 0)
    std::cout << "  Dual commit rate: " << std::fixed << std::setprecision(1)
              << (100.0 * stats.dual_commits / stats.cycles) << "%\n";
  std::cout << "  Flushes:      " << stats.flushes << "\n";
  std::cout << "  Branches:     " << stats.branches << "\n";
  std::cout << "  Mispredicts:  " << stats.branch_mispredicts << "\n";
  if (stats.branches > 0) {
    std::cout << "  Predict Rate: " << std::fixed << std::setprecision(1)
              << (100.0 * (stats.branches - stats.branch_mispredicts) /
                  stats.branches)
              << "%\n";
  }
  std::cout << "  Trace entries: " << pipeline_trace.size() << "\n";
}

void CPURV64P6_Cycle::dumpPipelineTrace(const std::string &filename) const {
  std::ofstream ofs(filename);
  if (!ofs.is_open()) {
    std::cerr << "[ERROR] Could not open " << filename << " for writing."
              << std::endl;
    return;
  }

  // CSV Header
  ofs << "cycle,pc_pcgen,pc_fetch,pc_id,pc_issue,pc_ex,pc_commit,"
      << "pcgen_valid,fetch_valid,id_valid,issue_valid,ex_valid,commit_valid,"
      << "stall_pcgen,stall_fetch,stall_issue,flush\n";

  // CSV Rows
  size_t size = pipeline_trace.size();
  size_t start = (size < trace_limit) ? 0 : trace_idx;
  for (size_t i = 0; i < size; ++i) {
    const auto &e = pipeline_trace[(start + i) % trace_limit];
    ofs << e.cycle << ","
        << "0x" << std::hex << e.pc_pcgen << ","
        << "0x" << e.pc_fetch << ","
        << "0x" << e.pc_id << ","
        << "0x" << e.pc_issue << ","
        << "0x" << e.pc_ex << ","
        << "0x" << e.pc_commit << "," << std::dec << e.pcgen_valid << ","
        << e.fetch_valid << "," << e.id_valid << "," << e.issue_valid << ","
        << e.ex_valid << "," << e.commit_valid << "," << e.is_stall_pcgen << ","
        << e.is_stall_fetch << "," << e.is_stall_issue << "," << e.is_flush
        << "\n";
  }

  ofs.close();
  std::cout << "  Pipeline trace written to: " << filename << " ("
            << pipeline_trace.size() << " entries)" << std::endl;
}

} // namespace riscv_tlm
