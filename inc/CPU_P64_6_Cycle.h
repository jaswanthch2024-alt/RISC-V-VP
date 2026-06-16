// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P64_6_Cycle.h
 * @brief 6-Stage Pipelined RISC-V 64-bit CPU - Cycle-Accurate Timing Model
 * 
 * Pipeline Stages:
 *   PC -> IF -> ID -> IS -> EX -> MEM -> WB
 */
#pragma once
#ifndef CPU_P64_6_CYCLE_H
#define CPU_P64_6_CYCLE_H

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "systemc"
#include "tlm.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <fstream>
#include "tlm_utils/simple_initiator_socket.h"

#include "CPU.h"
#include "Instruction.h"
#include "MemoryInterface.h"
#include "Registers.h"
#include "Memory.h"
#include "BASE_ISA.h"
#include "C_extension.h"
#include "M_extension.h"
#include "A_extension.h"
#include "F_extension.h"
#include "D_extension.h"
#include "Performance.h"
#include "Scoreboard.h"
#include "StoreBuffer.h"
#include "Cache.h"
#include "CSR_File.h"
#include "MMU.h"

namespace riscv_tlm {

namespace peripherals { class CLINT; }  // forward declaration

class CPURV64P6_Cycle : public CPU {
public:
    using BaseType = std::uint64_t;

    SC_HAS_PROCESS(CPURV64P6_Cycle);

    CPURV64P6_Cycle(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV64P6_Cycle() override;

    void set_clock(sc_core::sc_clock* c) override;
    bool CPU_step() override { return false; }
    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload& m_trans, sc_core::sc_time& delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;
    bool isPipelined() const override { return true; }

    void printStats() const;
    void dumpPipelineTrace(const std::string& filename) const;

    size_t trace_limit{100000};  // Max cycles to record — settable before sc_start()
    peripherals::CLINT* clint_ptr{nullptr};  // for WFI fast-forward
    Registers<BaseType>*    register_bank{nullptr};

    // --- Debug: recent store log (circular buffer for SLUB crash diagnosis) ---
    struct RecentStore {
        uint64_t pa;
        uint64_t data;
        int      size;
        uint64_t pc;
        uint64_t cycle;
    };
    static constexpr size_t RECENT_STORE_LOG_SIZE = 64;
    RecentStore recent_stores[RECENT_STORE_LOG_SIZE]{};
    size_t recent_store_idx{0};

private:
    // =========================================================================
    // Components
    // =========================================================================
    
    BASE_ISA<BaseType>*     base_inst{nullptr};
    A_extension<BaseType>*  a_inst{nullptr};
    C_extension<BaseType>*  c_inst{nullptr};
    M_extension<BaseType>*  m_inst{nullptr};
    F_extension<BaseType>*  f_inst{nullptr};
    D_extension<BaseType>*  d_inst{nullptr};

    BaseType int_cause{0};
    
    sc_core::sc_clock* clk{nullptr};
    sc_core::sc_time clock_period{10, sc_core::SC_NS};

    // =========================================================================
    // Pipeline Latches
    // =========================================================================
    
    // --- Pipeline Latches ---
    // These structures hold the state transferred between pipeline stages on each clock cycle.
    
    // PCGen -> Fetch Latch
    // Holds the next program counter to be fetched.
    struct PCGen_Fetch_Latch {
        uint64_t pc{0};    // Address to fetch from
        bool valid{false}; // Start fetching?
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint64_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } pcgen_fetch_reg, pcgen_fetch_next;


    // Fetch -> ID Latch
    // Holds the fetched instruction and its PC.
    struct Fetch_ID_Latch {
        uint64_t pc{0};
        uint32_t instr{0}; // Raw instruction data (lower 16 bits for compressed)
        bool valid{false};
        bool is_compressed{false}; // True if this is a 16-bit C-extension instruction
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint64_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } fetch_id_reg, fetch_id_next;

    // ID -> Issue Latch
    // Holds decoded instruction details ready for dispatch.
    struct ID_Issue_Latch {
        uint64_t pc{0};
        uint32_t instr{0};
        uint8_t rd{0}, rs1{0}, rs2{0}; // Register indices
        int64_t imm{0};                // Decoded immediate
        uint8_t opcode{0};
        uint8_t funct3{0};
        uint8_t funct7{0};
        bool valid{0};
        bool is_compressed{false};
        bool is_illegal{false};  // True for unrecognized/unsupported instructions

        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint64_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } id_issue_reg, id_issue_next;

    // Issue -> EX Latch
    // Holds dispatched instruction with operands read from register bank.
    // Also contains the allocated ROB index for retirement.
    struct Issue_EX_Latch {
        uint64_t pc{0};
        uint32_t instr{0};         // Raw encoding (for tval in illegal-instruction traps)
        uint64_t rs1_val{0};
        uint64_t rs2_val{0};
        int64_t imm{0};
        uint8_t rd{0};
        uint8_t rs1{0};            // Register index (needed by CSR-imm and SFENCE.VMA)
        uint8_t rs2{0};            // Register index
        uint8_t opcode{0};
        uint8_t funct3{0};
        uint8_t funct7{0};
        int rob_index{-1};   // Index in Reorder Buffer (for tracking completion)
        bool valid{false};
        bool is_compressed{false}; // Needed for correct JAL/JALR link address (+2 vs +4)
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint64_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } issue_ex_reg, issue_ex_next;

    // EX -> Commit (ROB/Architectural Update Interface)
    // Conceptually, EX writes to ROB. Commit reads from ROB.
    // We can use a latch to simulate the "Writeback" port to ROB if needed, 
    // or just let EX update ROB directly.
    // For this model, EX will write to ROB, and Commit will retire from ROB.

    // =========================================================================
    // Control & State
    // =========================================================================
    uint64_t next_pc{0};           // Next Program Counter (calculated by PCGen)
    
    // Stall Signals affecting various stages
    bool stall_pcgen{false};
    bool stall_fetch{false};
    bool stall_issue{false};
    bool stall_ex{false};     // EX replay: partial store-buffer overlap → retry next cycle
    
    // Flush Signal (e.g. for Branch Misprediction)
    bool flush_pipeline{false};
    uint64_t pc_redirect_target{0};
    bool pc_redirect_valid{false};
 
    // Scoreboard — unified issue FIFO + reorder buffer + rd_clobber (CVA6-aligned).
    // Replaces the separate bool scoreboard[32] and ReorderBuffer<32> rob.
    Scoreboard<32> scoreboard;

    // Commit-stage capture (reset each cycle in cycle_thread)
    uint32_t commit_count_this_cycle{0};
    uint64_t committed_pc_this_cycle{0};

    // =========================================================================
    // Branch Prediction Structures (CVA6-Aligned)
    // =========================================================================
    struct BTBEntry {
        uint64_t target{0};
        uint64_t tag{0};    // Full PC tag to prevent aliasing
        bool valid{false};
        bool is_return{false}; // Metadata to identify return instructions for RAS
    };
    
    // Direct-mapped BTB, 128 entries (indexed by PC[8:2])
    BTBEntry btb[128];

    // Branch History Table (BHT), 256 entries of 2-bit saturating counters
    // State: 00 (Strongly Not Taken), 01 (Weakly Not Taken)
    //        10 (Weakly Taken),       11 (Strongly Taken)
    // Indexed by PC[9:2]
    uint8_t bht[256]; 

    // Return Address Stack (RAS), 4-entry LIFO
    std::vector<uint64_t> ras;

    // =========================================================================
    // Statistics
    // =========================================================================
    struct Stats {
        uint64_t cycles{0};
        uint64_t instructions{0};
        uint64_t stalls{0};
        uint64_t branches{0};
        uint64_t branch_mispredicts{0};
        uint64_t flushes{0};
        uint64_t forwarded_reads{0};     // Phase 1: RAW hazards resolved by forwarding
        uint64_t load_store_forwards{0}; // Phase 2: Loads satisfied from store buffer
        uint64_t mul_stall_cycles{0};    // Phase 3: Cycles MUL FU was busy (not idle)
        uint64_t div_stall_cycles{0};    // Phase 3: Cycles DIV FU was busy (not idle)
        uint64_t icache_misses{0};       // Phase 4: I$ miss count
        uint64_t icache_miss_cycles{0};  // Phase 4: Cycles stalled on I$ miss
        uint64_t dcache_misses{0};       // Phase 4: D$ miss count
        uint64_t dcache_miss_cycles{0};  // Phase 4: Cycles stalled on D$ miss
        uint64_t load_use_stall_cycles{0}; // Cycles stalled on 1-cycle load-use hazard
        uint64_t itlb_miss_cycles{0};    // Phase 8: Cycles stalled on ITLB miss + PTW
        uint64_t dtlb_miss_cycles{0};    // Phase 8: Cycles stalled on DTLB miss + PTW
        uint64_t m_instrs{0};            // Instructions committed in M-mode
        uint64_t s_instrs{0};            // Instructions committed in S-mode
        uint64_t u_instrs{0};            // Instructions committed in U-mode

        double get_cpi() const { return instructions > 0 ? (double)cycles / instructions : 0; }
        double get_ipc() const { return cycles > 0 ? (double)instructions / cycles : 0; }
    };
    Stats stats;

    // =========================================================================
    // Pipeline Trace (for visualization)
    // =========================================================================
    struct PipelineTraceEntry {
        uint64_t cycle;
        uint64_t pc_pcgen;     // PC in PCGen stage (0 if invalid)
        uint64_t pc_fetch;     // PC in Fetch stage
        uint64_t pc_id;        // PC in Decode stage
        uint64_t pc_issue;     // PC in Issue stage
        uint64_t pc_ex;        // PC in Execute stage
        uint64_t pc_commit;    // PC being committed this cycle (0 if none)
        bool pcgen_valid;
        bool fetch_valid;
        bool id_valid;
        bool issue_valid;
        bool ex_valid;
        uint32_t commit_valid;     // Number of instructions committed this cycle
        bool is_stall_pcgen;
        bool is_stall_fetch;
        bool is_stall_issue;
        bool is_flush;
    };
    std::vector<PipelineTraceEntry> pipeline_trace;

    // =========================================================================
    // Out-of-Order Execution Components
    // =========================================================================

    // Store buffer: 4 speculative + 4 committed entries (CVA6-aligned split design).
    StoreBuffer<4, 4> store_buffer;

    // Multi-cycle functional unit state (Phase 3).
    // Each FU operates independently — only data-dependent instructions stall
    // via the scoreboard rd_clobber mechanism.
    struct FunctionalUnitState {
        bool     busy{false};
        int      remaining{0};   // Cycles left before scoreboard.complete() is called
        uint64_t result{0};
        int      trans_id{-1};
        uint8_t  rd{0};
    };
    FunctionalUnitState mul_fu;        // 2-cycle multiplier   (MUL/MULH/MULHU/MULHSU/MULW)
    FunctionalUnitState div_fu;        // 64-cycle divider     (DIV/DIVU/REM/REMU/DIVW/…)
    FunctionalUnitState fpu_fu;        // FPU functional unit (variable latency)
    FunctionalUnitState dcache_miss_fu;// Variable-latency LSU (D$ miss deferred completion)
    FunctionalUnitState load_hit_fu;   // 1-cycle load-use stall (D$ hit / store-buf forward)

    // L1 cache instances (Phase 4).
    // I$: 16 KB, 4-way, 64 B lines — matches CVA6 cv64a6 default.
    // D$: 32 KB, 8-way, 64 B lines — matches CVA6 cv64a6 default.
    Cache<64, 4> icache;
    Cache<64, 8> dcache;

    int icache_miss_remaining{0}; // Cycles until current I$ miss resolves
    int icache_miss_penalty{10};  // Settable before sc_start()
    int dcache_miss_penalty{10};  // Settable before sc_start()

    // CSR file — M+S+U privilege support (Phase 5/6/7).
    CSR_File csr;

    // sv39 MMU — ITLB + DTLB + page table walker (Phase 8/9).
    // Initialized in constructor after mem_intf is ready.
    MMU* mmu{nullptr};

    // Pending trap from EX stage: if valid, PCGen redirects and flushes.
    bool     trap_pending{false};
    uint64_t trap_vector_addr{0};

    struct PendingCSRWrite {
        bool     valid{false};
        uint16_t addr{0};
    };
    PendingCSRWrite pending_csr_write;
    bool wfi_stall{false};
    bool     pending_priv_switch{false};
    uint64_t pending_priv_switch_epc{0};
    uint64_t load_reservation_addr{0};
    bool     load_reservation_valid{false};
    uint64_t pc_current{0};

    // =========================================================================
    // Stage Methods
    // =========================================================================
    void PCGen_stage();
    void Fetch_stage();
    void ID_stage();
    void Issue_stage();
    void EX_stage();     // Includes Memory Access (LSU)
    void Commit_stage(); // Handles Retirement

    void cycle_thread();

    // =========================================================================
    // Helpers
    // =========================================================================
    bool fetch_instruction(uint64_t addr, uint32_t& data);
    // DMI uses base class members: dmi_ptr_valid, dmi_ptr (inherited from CPU)
    uint64_t dmi_start_addr{0};
    uint64_t dmi_end_addr{0};
};

} // namespace riscv_tlm

#endif // CPU_P64_6_CYCLE_H