// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CPU_P32_2_H
#define CPU_P32_2_H

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "systemc"
#include "tlm.h"

#include "CPU.h"
#include "Instruction.h"
#include "MemoryInterface.h"
#include "Registers.h"
#include "Memory.h"
#include "BASE_ISA.h"
#include "C_extension.h"
#include "M_extension.h"
#include "A_extension.h"
#include "Performance.h"

namespace riscv_tlm {

/**
 * @brief 2-Stage Pipelined RISC-V 32-bit CPU (for VP)
 * 
 * Pipeline stages:
 *   IF  -> [LATCH] -> EX
 *   Fetch            Decode+Execute+Memory+Writeback
 * 
 * Features:
 * - IF/EX pipeline latch holds instruction between stages
 * - Branch taken causes 1-cycle flush (bubble)
 * - Stages execute conceptually in parallel each cycle
 * - Uses LT (b_transport) for memory access
 */
class CPURV32P2 : public CPU {
public:
    using BaseType = std::uint32_t;

    CPURV32P2(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV32P2() override;

    void set_clock(sc_core::sc_clock* c) override { clk = c; }

    bool CPU_step() override;

    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;

    bool isPipelined() const override { return true; }

    // Pipeline statistics
    struct PipelineStats {
        uint64_t cycles{0};
        uint64_t stalls{0};
        uint64_t flushes{0};
        uint64_t control_hazards{0};
    };
    
    PipelineStats getStats() const { return stats; }

private:
    // =========================================================================
    // Architectural State
    // =========================================================================
    Registers<BaseType>*     register_bank{nullptr};
    BASE_ISA<BaseType>*      base_inst{nullptr};
    C_extension<BaseType>*   c_inst{nullptr};
    M_extension<BaseType>*   m_inst{nullptr};
    A_extension<BaseType>*   a_inst{nullptr};

    // IRQ bookkeeping
    BaseType int_cause{0};

    sc_core::sc_clock* clk{nullptr};

    // Statistics
    PipelineStats stats{};

    // =========================================================================
    // Pipeline Latch: IF -> EX
    // =========================================================================
    struct IF_EX_Latch {
        std::uint32_t instruction{0};   // Fetched instruction
        std::uint32_t pc{0};            // PC of fetched instruction
        bool valid{false};              // Is latch data valid? (false = bubble)
    } if_ex_latch;

    // =========================================================================
    // Pipeline Control
    // =========================================================================
    bool pipeline_flush{false};         // Flush IF stage (branch taken)

    // =========================================================================
    // Pipeline Stage Methods
    // =========================================================================
    
    /**
     * @brief IF Stage: Fetch instruction from memory
     * 
     * Fetches instruction at current PC and loads it into IF/EX latch.
     * Speculatively increments PC (assumes no branch taken).
     */
    void IF_stage();

    /**
     * @brief EX Stage: Decode and execute instruction
     * 
     * Takes instruction from IF/EX latch, decodes and executes it.
     * Sets pipeline_flush if branch is taken.
     * 
     * @return true if breakpoint hit, false otherwise
     */
    bool EX_stage();

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
};

} // namespace riscv_tlm

#endif // CPU_P32_2_H
