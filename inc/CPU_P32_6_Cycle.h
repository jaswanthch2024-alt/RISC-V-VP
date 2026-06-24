// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P32_6_Cycle.h
 * @brief 6-Stage Pipelined RISC-V 32-bit CPU - Cycle-Accurate Timing Model (CVA6-Aligned)
 * 
 * Pipeline Stages:
 *   PCGen -> Fetch -> ID -> Issue -> EX -> Commit
 */
#pragma once
#ifndef CPU_P32_6_CYCLE_H
#define CPU_P32_6_CYCLE_H

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
#include "M_extension.h"
#include "Performance.h"
#include "ROB.h"
#include "StoreBuffer.h"

namespace riscv_tlm {

class CPURV32P6_Cycle : public CPU {
public:
    using BaseType = std::uint32_t;

    SC_HAS_PROCESS(CPURV32P6_Cycle);

    CPURV32P6_Cycle(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV32P6_Cycle() override;

    void set_clock(sc_core::sc_clock* c) override;
    bool CPU_step() override { return false; }
    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload& m_trans, sc_core::sc_time& delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;
    bool isPipelined() const override { return true; }

    struct Stats {
        uint64_t cycles{0};
        uint64_t instructions{0};
        uint64_t stalls{0};
        uint64_t branches{0};
        uint64_t branch_mispredicts{0};
        
        double get_cpi() const { return instructions > 0 ? (double)cycles / instructions : 0; }
        double get_ipc() const { return cycles > 0 ? (double)instructions / cycles : 0; }
    };
    Stats stats;
    const Stats& getStats() const { return stats; }

    void printStats() const;

private:
    // =========================================================================
    // Components
    // =========================================================================
    Registers<BaseType>*    register_bank{nullptr};
    
    BASE_ISA<BaseType>*     base_inst{nullptr};
    M_extension<BaseType>*  m_inst{nullptr};

    BaseType int_cause{0};
    
    sc_core::sc_clock* clk{nullptr};
    sc_core::sc_time clock_period{10, sc_core::SC_NS};

    // =========================================================================
    // Pipeline Latches (CVA6-Aligned)
    // =========================================================================
    
    // PCGen -> Fetch Latch
    struct PCGen_Fetch_Latch {
        uint32_t pc{0};
        bool valid{false};
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint32_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } pcgen_fetch_reg, pcgen_fetch_next;

    // Fetch -> ID Latch
    struct Fetch_ID_Latch {
        uint32_t pc{0};
        uint32_t instr{0};
        bool valid{false};
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint32_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } fetch_id_reg, fetch_id_next;

    // ID -> Issue Latch
    struct ID_Issue_Latch {
        uint32_t pc{0};
        uint32_t instr{0};
        uint8_t rd{0}, rs1{0}, rs2{0};
        int32_t imm{0};
        uint8_t opcode{0};
        uint8_t funct3{0};
        uint8_t funct7{0};
        bool valid{false};
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint32_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } id_issue_reg, id_issue_next;

    // Issue -> EX Latch
    struct Issue_EX_Latch {
        uint32_t pc{0};
        uint32_t rs1_val{0};
        uint32_t rs2_val{0};
        int32_t imm{0};
        uint8_t rd{0};
        uint8_t opcode{0};
        uint8_t funct3{0};
        uint8_t funct7{0};
        int rob_index{-1};
        bool valid{false};
        
        // Branch Prediction Metadata
        bool predicted_taken{false};
        uint32_t predicted_target{0};
        bool is_ras_call{false};
        bool is_ras_return{false};
    } issue_ex_reg, issue_ex_next;

    // EX -> Commit Latch (for proper pipeline depth)
    struct EX_Commit_Latch {
        uint32_t result{0};
        uint8_t rd{0};
        int rob_index{-1};
        bool is_store{false};
        uint32_t store_addr{0};
        uint32_t store_data{0};
        int store_size{0};
        bool valid{false};
    } ex_commit_reg, ex_commit_next;

    // =========================================================================
    // Control & State
    // =========================================================================
    uint32_t next_pc{0};
    
    bool stall_pcgen{false};
    bool stall_fetch{false};
    bool stall_issue{false};
    
    bool flush_pipeline{false};
    uint32_t pc_redirect_target{0};
    bool pc_redirect_valid{false};

    bool scoreboard[32]{false};

    // =========================================================================
    // Branch Prediction Structures (CVA6-Aligned)
    // =========================================================================
    struct BTBEntry {
        uint32_t target{0};
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
    std::vector<uint32_t> ras;

    // =========================================================================
    // Statistics (Moved to public section)
    // =========================================================================

    // =========================================================================
    // Out-of-Order Execution Components
    // =========================================================================
    ReorderBuffer<32> rob;
    StoreBuffer<8> store_buffer;

    // =========================================================================
    // Stage Methods (CVA6-Aligned)
    // =========================================================================
    void PCGen_stage();
    void Fetch_stage();
    void ID_stage();
    void Issue_stage();
    void EX_stage();
    void Commit_stage();

    void cycle_thread();

    // =========================================================================
    // Helpers
    // =========================================================================
    bool fetch_instruction(uint32_t addr, uint32_t& data);
    sc_dt::uint64 dmi_start_addr{0};
    sc_dt::uint64 dmi_end_addr{0};
};

} // namespace riscv_tlm

#endif // CPU_P32_6_CYCLE_H
