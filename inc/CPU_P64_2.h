// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CPU_P64_2_H
#define CPU_P64_2_H

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
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU (for VP)
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
class CPURV64P2 : public CPU {
public:
    using BaseType = std::uint64_t;

    CPURV64P2(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV64P2() override;

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

    Registers<BaseType>*     register_bank{nullptr};

private:
    // =========================================================================
    // Architectural State
    // =========================================================================
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
        std::uint32_t instruction{0};   // Fetched instruction (always 32-bit)
        std::uint64_t pc{0};            // PC of fetched instruction (64-bit for RV64)
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
     */
    void IF_stage();

    /**
     * @brief EX Stage: Decode and execute instruction
     * @return true if breakpoint hit, false otherwise
     */
    bool EX_stage();

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
};

} // namespace riscv_tlm

#endif // CPU_P64_2_H
