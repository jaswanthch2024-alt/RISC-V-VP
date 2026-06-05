// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P64_2_Cycle.h
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU - Cycle-Accurate Timing Model
 * 
 * This implements a true cycle-accurate 2-stage pipeline:
 *   IF -> [LATCH] -> EX
 */
#pragma once
#ifndef CPU_P64_2_CYCLE_H
#define CPU_P64_2_CYCLE_H

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

class CPURV64P2_Cycle : public CPU {
public:
    using BaseType = std::uint64_t;

    SC_HAS_PROCESS(CPURV64P2_Cycle);

    CPURV64P2_Cycle(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV64P2_Cycle() override;

    void set_clock(sc_core::sc_clock* c) override;

    bool CPU_step() override;

    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;

    bool isPipelined() const override { return true; }

    // Cycle-accurate statistics
    struct CycleStats {
        uint64_t total_cycles{0};
        uint64_t instruction_cycles{0};
        uint64_t stall_cycles{0};
        uint64_t fetch_cycles{0};
        uint64_t memory_cycles{0};
        uint64_t branch_penalty{0};
        uint64_t instructions_retired{0};
        
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
    Registers<BaseType>*     register_bank{nullptr};
    BASE_ISA<BaseType>*      base_inst{nullptr};
    C_extension<BaseType>*   c_inst{nullptr};
    M_extension<BaseType>*   m_inst{nullptr};
    A_extension<BaseType>*   a_inst{nullptr};

    BaseType int_cause{0};
    
    sc_core::sc_clock* clk{nullptr};
    sc_core::sc_time clock_period{10, sc_core::SC_NS};

    CycleStats stats{};

    struct IF_EX_Latch {
        std::uint32_t instruction{0};
        std::uint64_t pc{0};
        bool valid{false};
    };
    
    IF_EX_Latch if_ex_latch;
    IF_EX_Latch if_ex_latch_next;

    bool pipeline_flush{false};
    bool if_stall{false};
    bool ex_stall{false};
    
    enum class MemState { IDLE, FETCH_PENDING, FETCH_COMPLETE };
    MemState mem_state{MemState::IDLE};
    uint32_t mem_latency_remaining{0};
    
    struct LatencyConfig {
        uint32_t fetch_latency{1};
        uint32_t load_latency{1};
        uint32_t store_latency{1};
        uint32_t mul_latency{3};
        uint32_t div_latency{32};
        uint32_t branch_penalty{1};
    } latency;

    void on_posedge();
    void on_negedge();
    void cycle_thread();

    void IF_stage();
    bool EX_stage();
    
    bool fetch_instruction(std::uint64_t pc, std::uint32_t& instruction);
    
    uint32_t get_instruction_latency(std::uint32_t instruction);

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
    
    // DMI
    bool dmi_ptr_valid{false};
    unsigned char* dmi_ptr{nullptr};
    uint64_t dmi_start_addr{0};
    uint64_t dmi_end_addr{0};
};

} // namespace riscv_tlm

#endif // CPU_P64_2_CYCLE_H