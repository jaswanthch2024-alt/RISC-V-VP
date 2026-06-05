// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P32_2_Cycle.h
 * @brief 2-Stage Pipelined RISC-V 32-bit CPU - Cycle-Accurate Timing Model
 * 
 * This implements a true cycle-accurate 2-stage pipeline:
 *   IF -> [LATCH] -> EX
 * 
 * Key Cycle-Accurate Model Features:
 * - Clock-edge driven execution (posedge/negedge sensitivity)
 * - Explicit cycle counting for all operations
 * - Memory access latency modeled in cycles
 * - Pipeline interlocks with precise stall counting
 * - SC_METHOD based for true event-driven simulation
 */
#pragma once
#ifndef CPU_P32_2_CYCLE_H
#define CPU_P32_2_CYCLE_H

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "systemc"
#include "tlm.h"
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
#include "Performance.h"

namespace riscv_tlm {

/**
 * @brief 2-Stage Pipelined RISC-V 32-bit CPU - Cycle-Accurate Model
 * 
 * Pipeline stages executed on clock edges:
 *   Rising Edge:  Latch transfer (IF->EX), EX stage execute
 *   Falling Edge: IF stage fetch
 * 
 * This provides true cycle-by-cycle behavior matching real hardware.
 */
class CPURV32P2_Cycle : public CPU {
public:
    using BaseType = std::uint32_t;

    SC_HAS_PROCESS(CPURV32P2_Cycle);

    CPURV32P2_Cycle(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV32P2_Cycle() override;

    void set_clock(sc_core::sc_clock* c) override;

    bool CPU_step() override;

    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;

    bool isPipelined() const override { return true; }

    // Cycle-accurate statistics
    struct CycleStats {
        uint64_t total_cycles{0};       // Total clock cycles
        uint64_t instruction_cycles{0}; // Cycles executing instructions
        uint64_t stall_cycles{0};       // Cycles stalled
        uint64_t fetch_cycles{0};       // Cycles spent fetching
        uint64_t memory_cycles{0};      // Cycles waiting for memory
        uint64_t branch_penalty{0};     // Cycles lost to branch misprediction
        uint64_t instructions_retired{0}; // Instructions completed
        
        double get_cpi() const { 
            return instructions_retired > 0 ? 
                   static_cast<double>(total_cycles) / instructions_retired : 0.0;
        }
        double get_ipc() const {
            return total_cycles > 0 ?
                   static_cast<double>(instructions_retired) / total_cycles : 0.0;
        }
    };
    
    CycleStats getStats() const { return stats; }
    void printStats() const;

private:
    // =========================================================================
    // Architectural State
    // =========================================================================
    Registers<BaseType>*     register_bank{nullptr};
    BASE_ISA<BaseType>*      base_inst{nullptr};
    C_extension<BaseType>*   c_inst{nullptr};
    M_extension<BaseType>*   m_inst{nullptr};
    A_extension<BaseType>*   a_inst{nullptr};

    BaseType int_cause{0};
    
    // Clock
    sc_core::sc_clock* clk{nullptr};
    sc_core::sc_time clock_period{10, sc_core::SC_NS};

    // Statistics
    CycleStats stats{};

    // =========================================================================
    // Pipeline Latch: IF -> EX
    // =========================================================================
    struct IF_EX_Latch {
        std::uint32_t instruction{0};
        std::uint32_t pc{0};
        bool valid{false};
    };
    
    IF_EX_Latch if_ex_latch;        // Current latch (read by EX)
    IF_EX_Latch if_ex_latch_next;   // Next latch (written by IF)

    // =========================================================================
    // Pipeline Control Signals
    // =========================================================================
    bool pipeline_flush{false};
    bool if_stall{false};           // IF stage stalled (waiting for memory)
    bool ex_stall{false};           // EX stage stalled (data hazard)
    
    // Memory access state
    enum class MemState { IDLE, FETCH_PENDING, FETCH_COMPLETE };
    MemState mem_state{MemState::IDLE};
    uint32_t mem_latency_remaining{0};
    
    // Configurable latencies (in cycles)
    struct LatencyConfig {
        uint32_t fetch_latency{1};      // Instruction fetch latency
        uint32_t load_latency{1};       // Load instruction latency
        uint32_t store_latency{1};      // Store instruction latency
        uint32_t mul_latency{3};        // Multiply latency
        uint32_t div_latency{32};       // Divide latency
        uint32_t branch_penalty{1};     // Branch misprediction penalty
    } latency;

    // =========================================================================
    // Clock-Driven Methods
    // =========================================================================
    
    /**
     * @brief Rising edge: Transfer latch and execute EX stage
     */
    void on_posedge();
    
    /**
     * @brief Falling edge: Execute IF stage (fetch)
     */
    void on_negedge();
    
    /**
     * @brief Main cycle thread
     */
    void cycle_thread();

    // =========================================================================
    // Pipeline Stage Methods
    // =========================================================================
    
    void IF_stage();
    bool EX_stage();
    
    /**
     * @brief Fetch instruction with cycle-accurate latency
     */
    bool fetch_instruction(std::uint32_t pc, std::uint32_t& instruction);
    
    /**
     * @brief Check if instruction is a multi-cycle operation
     */
    uint32_t get_instruction_latency(std::uint32_t instruction);

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
};

} // namespace riscv_tlm

#endif // CPU_P32_2_CYCLE_H
