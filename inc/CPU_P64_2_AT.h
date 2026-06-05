// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P64_2_AT.h
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU - AT (Approximately-Timed) Model
 * 
 * This implements a true cycle-accurate 2-stage pipeline using TLM-2.0 AT protocol:
 *   IF -> [LATCH] -> EX
 * 
 * Key AT model features:
 * - Non-blocking transport (nb_transport_fw/nb_transport_bw)
 * - Explicit timing phases (BEGIN_REQ, END_REQ, BEGIN_RESP, END_RESP)
 * - Clock-driven pipeline stages using sc_clock
 * - Pipeline stages run as separate SC_THREADs synchronized to clock
 */
#pragma once
#ifndef CPU_P64_2_AT_H
#define CPU_P64_2_AT_H

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/peq_with_cb_and_phase.h"

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
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU using AT (Approximately-Timed) model
 * 
 * Pipeline stages:
 *   IF  -> [LATCH] -> EX
 *   Fetch            Decode+Execute+Memory+Writeback
 */
class CPURV64P2_AT : public CPU {
public:
    using BaseType = std::uint64_t;

    CPURV64P2_AT(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV64P2_AT() override;

    void set_clock(sc_core::sc_clock* c) override;

    bool CPU_step() override;

    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;

    bool isPipelined() const override { return true; }

    // Pipeline statistics
    struct PipelineStats {
        uint64_t cycles{0};
        uint64_t instructions{0};
        uint64_t stalls{0};
        uint64_t flushes{0};
        uint64_t control_hazards{0};
        uint64_t if_stalls{0};
        uint64_t mem_latency_cycles{0};
        
        double get_cpi() const { return instructions > 0 ? (double)cycles / instructions : 0; }
        double get_ipc() const { return cycles > 0 ? (double)instructions / cycles : 0; }
    };
    
    PipelineStats getStats() const { return stats; }
    void printStats() const;

    // AT Protocol backward path (overrides base class)
    tlm::tlm_sync_enum nb_transport_bw(tlm::tlm_generic_payload& trans,
                                        tlm::tlm_phase& phase,
                                        sc_core::sc_time& delay) override;

private:
    // Architectural State
    Registers<BaseType>*     register_bank{nullptr};
    BASE_ISA<BaseType>*      base_inst{nullptr};
    C_extension<BaseType>*   c_inst{nullptr};
    M_extension<BaseType>*   m_inst{nullptr};
    A_extension<BaseType>*   a_inst{nullptr};

    BaseType int_cause{0};
    sc_core::sc_clock* clk{nullptr};
    sc_core::sc_time clock_period{10, sc_core::SC_NS};
    PipelineStats stats{};

    // Pipeline Latch: IF -> EX
    struct IF_EX_Latch {
        std::uint32_t instruction{0};
        std::uint64_t pc{0};
        bool valid{false};
    } if_ex_latch;

    IF_EX_Latch if_ex_latch_next;

    // Pipeline Control
    bool pipeline_flush{false};
    bool if_stage_busy{false};
    bool ex_stage_done{false};

    // AT Protocol State
    tlm::tlm_generic_payload* pending_fetch_trans{nullptr};
    std::uint32_t fetched_instruction{0};
    sc_core::sc_event fetch_complete_event;
    tlm_utils::peq_with_cb_and_phase<CPURV64P2_AT> m_peq;

    // Pipeline Methods
    void pipeline_thread();
    void IF_stage();
    bool EX_stage();
    void peq_callback(tlm::tlm_generic_payload& trans, const tlm::tlm_phase& phase);
    bool initiate_fetch(std::uint64_t address);
    std::uint32_t wait_for_fetch();

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
};

} // namespace riscv_tlm

#endif // CPU_P64_2_AT_H
