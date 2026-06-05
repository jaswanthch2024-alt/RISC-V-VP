// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P64_2_Cycle.cpp
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU - Cycle-Accurate Implementation
 */
#include "CPU_P64_2_Cycle.h"
#include "spdlog/spdlog.h"
#include <iostream>
#include <iomanip>

namespace riscv_tlm {

CPURV64P2_Cycle::CPURV64P2_Cycle(sc_core::sc_module_name const& name, BaseType PC, bool debug)
    : CPU(name, debug) {

    register_bank = new Registers<BaseType>();
    mem_intf = new MemoryInterface();

    register_bank->setPC(PC);
    register_bank->setValue(Registers<BaseType>::sp, (Memory::SIZE / 4) - 1); // This might be small for 64-bit, but follows pattern
    int_cause = 0;

    instr_bus.register_invalidate_direct_mem_ptr(this, &CPU::invalidate_direct_mem_ptr);

    base_inst = new BASE_ISA<BaseType>(0, register_bank, mem_intf);
    c_inst    = new C_extension<BaseType>(0, register_bank, mem_intf);
    m_inst    = new M_extension<BaseType>(0, register_bank, mem_intf);
    a_inst    = new A_extension<BaseType>(0, register_bank, mem_intf);

    if_ex_latch.instruction = 0;
    if_ex_latch.pc = 0;
    if_ex_latch.valid = false;
    
    if_ex_latch_next.instruction = 0;
    if_ex_latch_next.pc = 0;
    if_ex_latch_next.valid = false;
    
    pipeline_flush = false;
    if_stall = false;
    ex_stall = false;
    mem_state = MemState::IDLE;
    mem_latency_remaining = 0;

    SC_THREAD(cycle_thread);

    logger->info("Created CPURV64P2_Cycle (cycle-accurate) CPU for VP");
    std::cout << "Created CPURV64P2_Cycle (cycle-accurate 2-stage pipelined) CPU" << std::endl;
}

CPURV64P2_Cycle::~CPURV64P2_Cycle() {
    printStats();
    delete register_bank;
    delete mem_intf;
    delete base_inst;
    delete c_inst;
    delete m_inst;
    delete a_inst;
    // m_qk is owned and deleted by the CPU base class destructor
}

void CPURV64P2_Cycle::set_clock(sc_core::sc_clock* c) {
    clk = c;
    if (clk) {
        clock_period = clk->period();
    }
}

void CPURV64P2_Cycle::printStats() const {
    std::cout << "\n========== Cycle-Accurate CPU Statistics (RV64) ==========" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total Cycles:          " << stats.total_cycles << std::endl;
    std::cout << "Instructions Retired:  " << stats.instructions_retired << std::endl;
    std::cout << "CPI (Cycles/Instr):    " << stats.get_cpi() << std::endl;
    std::cout << "IPC (Instr/Cycle):     " << stats.get_ipc() << std::endl;
    std::cout << "==========================================================" << std::endl;
}

void CPURV64P2_Cycle::cycle_thread() {
    if (clk) {
        sc_core::wait(clk->posedge_event());
    } else {
        sc_core::wait(clock_period);
    }
    
    while (true) {
        on_posedge();
        sc_core::wait(clock_period / 2);
        on_negedge();
        if (clk) {
            sc_core::wait(clk->posedge_event());
        } else {
            sc_core::wait(clock_period / 2);
        }
    }
}

void CPURV64P2_Cycle::on_posedge() {
    stats.total_cycles++;
    cpu_process_IRQ();
    
    if (pipeline_flush) {
        if_ex_latch.valid = false;
        if_ex_latch.instruction = 0;
        if_ex_latch.pc = 0;
        pipeline_flush = false;
        stats.branch_penalty++;
        return;
    }
    
    if (ex_stall) {
        stats.stall_cycles++;
        return;
    }
    
    if_ex_latch = if_ex_latch_next;
    EX_stage();
}

void CPURV64P2_Cycle::on_negedge() {
    if (mem_state == MemState::FETCH_PENDING) {
        IF_stage();
        return;
    }

    if (if_stall || pipeline_flush) {
        if (if_stall) stats.stall_cycles++;
        return;
    }
    IF_stage();
}

void CPURV64P2_Cycle::IF_stage() {
    if (mem_state == MemState::FETCH_PENDING) {
        if (mem_latency_remaining > 0) {
            mem_latency_remaining--;
            stats.fetch_cycles++;
            if_stall = true;
            return;
        }
        mem_state = MemState::FETCH_COMPLETE;
        if_stall = false;
    }
    
    std::uint64_t current_pc = register_bank->getPC();
    std::uint32_t fetched_instr = 0;
    
    if (fetch_instruction(current_pc, fetched_instr)) {
        if_ex_latch_next.instruction = fetched_instr;
        if_ex_latch_next.pc = current_pc;
        if_ex_latch_next.valid = true;
        
        if ((fetched_instr & 0x3) != 0x3) {
            register_bank->incPCby2();
        } else {
            register_bank->incPC();
        }
        
        stats.fetch_cycles++;
        perf->codeMemoryRead();
    } else {
        if_ex_latch_next.valid = false;
        if_stall = true;
    }
}

bool CPURV64P2_Cycle::fetch_instruction(std::uint64_t pc, std::uint32_t& instruction) {
    if (dmi_ptr_valid) {
        std::memcpy(&instruction, dmi_ptr + pc, 4);
        mem_state = MemState::IDLE;
        return true;
    }
    
    if (mem_state == MemState::IDLE) {
        mem_state = MemState::FETCH_PENDING;
        mem_latency_remaining = latency.fetch_latency;
        
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        trans.set_address(pc);
        trans.set_data_ptr(reinterpret_cast<unsigned char*>(&instruction));
        trans.set_command(tlm::TLM_READ_COMMAND);
        trans.set_data_length(4);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        
        instr_bus->b_transport(trans, delay);
        
        if (trans.is_response_error()) {
            SC_REPORT_ERROR("CPURV64P2_Cycle", "Instruction fetch error");
            return false;
        }
        
        if (trans.is_dmi_allowed()) {
            tlm::tlm_dmi dmi_data;
            dmi_ptr_valid = instr_bus->get_direct_mem_ptr(trans, dmi_data);
            if (dmi_ptr_valid) {
                dmi_ptr = dmi_data.get_dmi_ptr();
            }
        }
        if (mem_latency_remaining > 0) return false;
        mem_state = MemState::FETCH_COMPLETE; // Fixed: Advance state if latency was 0
    }
    
    if (mem_state == MemState::FETCH_COMPLETE) {
        mem_state = MemState::IDLE;
        return true;
    }
    return false;
}

uint32_t CPURV64P2_Cycle::get_instruction_latency(std::uint32_t instruction) {
    std::uint32_t opcode = instruction & 0x7F;
    std::uint32_t funct3 = (instruction >> 12) & 0x7;
    std::uint32_t funct7 = (instruction >> 25) & 0x7F;
    
    if (opcode == 0x33 && funct7 == 0x01) {
        if (funct3 < 4) return latency.mul_latency;
        else return latency.div_latency;
    }
    if (opcode == 0x03) return latency.load_latency;
    if (opcode == 0x23) return latency.store_latency;
    return 1;
}

bool CPURV64P2_Cycle::EX_stage() {
    bool breakpoint = false;

    if (!if_ex_latch.valid) {
        stats.stall_cycles++;
        return false;
    }

    std::uint32_t instr = if_ex_latch.instruction;
    inst.setInstr(instr);

    uint32_t instr_latency = get_instruction_latency(instr);
    stats.instruction_cycles += instr_latency;
    
    if (instr_latency > 1) {
        stats.stall_cycles += (instr_latency - 1);
        stats.total_cycles += (instr_latency - 1);
    }

    bool pc_changed = false;
    bool is_branch = false;

    base_inst->setInstr(instr);
    auto deco = base_inst->decode();

    if (deco != OP_ERROR) {
        std::uint32_t opcode = instr & 0x7F;
        is_branch = (opcode == 0x63 || opcode == 0x6F || opcode == 0x67);
        pc_changed = !base_inst->exec_instruction(inst, &breakpoint, deco);
    } else {
        c_inst->setInstr(instr);
        auto c_deco = c_inst->decode();
        if (c_deco != OP_C_ERROR) {
            is_branch = (c_deco == OP_C_J || c_deco == OP_C_JAL || 
                        c_deco == OP_C_JR || c_deco == OP_C_JALR ||
                        c_deco == OP_C_BEQZ || c_deco == OP_C_BNEZ);
            pc_changed = !c_inst->exec_instruction(inst, &breakpoint, c_deco);
        } else {
            m_inst->setInstr(instr);
            auto m_deco = m_inst->decode();
            if (m_deco != OP_M_ERROR) {
                pc_changed = !m_inst->exec_instruction(inst, m_deco);
            } else {
                a_inst->setInstr(instr);
                auto a_deco = a_inst->decode();
                if (a_deco != OP_A_ERROR) {
                    pc_changed = !a_inst->exec_instruction(inst, a_deco);
                } else {
                     base_inst->NOP();
                }
            }
        }
    }

    if (pc_changed) {
        pipeline_flush = true;
        stats.branch_penalty += latency.branch_penalty;
        stats.total_cycles += latency.branch_penalty;
    }

    stats.instructions_retired++;
    perf->instructionsInc();
    return breakpoint;
}

bool CPURV64P2_Cycle::CPU_step() {
    on_posedge();
    sc_core::wait(clock_period / 2);
    on_negedge();
    sc_core::wait(clock_period / 2);
    return false;
}

bool CPURV64P2_Cycle::cpu_process_IRQ() {
    BaseType csr_temp;
    bool ret_value = false;

    if (interrupt) {
        csr_temp = register_bank->getCSR(CSR_MSTATUS);
        if ((csr_temp & MSTATUS_MIE) == 0) return ret_value;

        csr_temp = register_bank->getCSR(CSR_MIP);
        if ((csr_temp & MIP_MEIP) == 0) {
            csr_temp |= MIP_MEIP;
            register_bank->setCSR(CSR_MIP, csr_temp);

            BaseType old_pc = register_bank->getPC();
            register_bank->setCSR(CSR_MEPC, old_pc);
            register_bank->setCSR(CSR_MCAUSE, 0x80000000 | 11);
            BaseType new_pc = register_bank->getCSR(CSR_MTVEC);
            register_bank->setPC(new_pc);

            pipeline_flush = true;
            if_ex_latch.valid = false;
            if_ex_latch_next.valid = false;
            stats.stall_cycles += 2;
            stats.total_cycles += 2;

            ret_value = true;
            interrupt = false;
            irq_already_down = false;
        }
    } else {
        if (!irq_already_down) {
            csr_temp = register_bank->getCSR(CSR_MIP);
            csr_temp &= ~MIP_MEIP;
            register_bank->setCSR(CSR_MIP, csr_temp);
            irq_already_down = true;
        }
    }
    return ret_value;
}

void CPURV64P2_Cycle::call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) {
    interrupt = true;
    memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
    delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV64P2_Cycle::getStartDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t0);
}

std::uint64_t CPURV64P2_Cycle::getEndDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t1);
}

} // namespace riscv_tlm
