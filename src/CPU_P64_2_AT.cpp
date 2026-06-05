// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P64_2_AT.cpp
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU - AT (Approximately-Timed) Implementation
 */
#include "CPU_P64_2_AT.h"
#include "spdlog/spdlog.h"

namespace riscv_tlm {

// =============================================================================
// Constructor / Destructor
// =============================================================================

CPURV64P2_AT::CPURV64P2_AT(sc_core::sc_module_name const& name, BaseType PC, bool debug)
    : CPU(name, debug),
      m_peq(this, &CPURV64P2_AT::peq_callback) {

    register_bank = new Registers<BaseType>();
    mem_intf = new MemoryInterface();

    register_bank->setPC(PC);
    register_bank->setValue(Registers<BaseType>::sp, (Memory::SIZE / 4) - 1);
    int_cause = 0;

    // nb_transport_bw is registered in base class and uses virtual dispatch
    // to call our overridden implementation
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
    if_stage_busy = false;
    ex_stage_done = false;

    // Register the clock-driven pipeline thread using sc_spawn
    sc_core::sc_spawn_options opts;
    sc_core::sc_spawn(sc_bind(&CPURV64P2_AT::pipeline_thread, this), "pipeline_thread", &opts);

    logger->info("Created CPURV64P2_AT (2-stage pipelined, AT model) CPU for VP");
    std::cout << "Created CPURV64P2_AT (2-stage pipelined, AT model) CPU for VP" << std::endl;
}

CPURV64P2_AT::~CPURV64P2_AT() {
    delete register_bank;
    delete mem_intf;
    delete base_inst;
    delete c_inst;
    delete m_inst;
    delete a_inst;
    // m_qk is owned and deleted by the CPU base class destructor
}

void CPURV64P2_AT::set_clock(sc_core::sc_clock* c) {
    clk = c;
    if (clk) {
        clock_period = clk->period();
    }
}

// =============================================================================
// AT Protocol: Non-blocking backward transport callback
// =============================================================================

tlm::tlm_sync_enum CPURV64P2_AT::nb_transport_bw(
    tlm::tlm_generic_payload& trans,
    tlm::tlm_phase& phase,
    sc_core::sc_time& delay) {
    
    m_peq.notify(trans, phase, delay);
    return tlm::TLM_ACCEPTED;
}

// =============================================================================
// PEQ Callback
// =============================================================================

void CPURV64P2_AT::peq_callback(tlm::tlm_generic_payload& trans, 
                                 const tlm::tlm_phase& phase) {
    switch (phase) {
        case tlm::END_REQ:
            logger->debug("AT: END_REQ received for fetch at PC=0x{:x}", 
                         trans.get_address());
            break;
            
        case tlm::BEGIN_RESP:
            if (trans.is_response_ok()) {
                fetched_instruction = *reinterpret_cast<std::uint32_t*>(trans.get_data_ptr());
                logger->debug("AT: BEGIN_RESP - fetched instruction 0x{:08x} at PC=0x{:x}",
                             fetched_instruction, trans.get_address());
            } else {
                SC_REPORT_ERROR("CPURV64P2_AT", "Instruction fetch error in AT response");
            }
            
            if_stage_busy = false;
            fetch_complete_event.notify();
            
            {
                tlm::tlm_phase end_phase = tlm::END_RESP;
                sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
                instr_bus->nb_transport_fw(trans, end_phase, delay);
            }
            break;
            
        default:
            SC_REPORT_ERROR("CPURV64P2_AT", "Unexpected AT phase in backward path");
            break;
    }
}

// =============================================================================
// Initiate Non-blocking Fetch Transaction
// =============================================================================

bool CPURV64P2_AT::initiate_fetch(std::uint64_t address) {
    static tlm::tlm_generic_payload fetch_trans;
    static std::uint32_t instr_buffer;
    
    fetch_trans.set_command(tlm::TLM_READ_COMMAND);
    fetch_trans.set_address(address);
    fetch_trans.set_data_ptr(reinterpret_cast<unsigned char*>(&instr_buffer));
    fetch_trans.set_data_length(4);
    fetch_trans.set_streaming_width(4);
    fetch_trans.set_byte_enable_ptr(nullptr);
    fetch_trans.set_dmi_allowed(false);
    fetch_trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    
    pending_fetch_trans = &fetch_trans;
    
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    
    tlm::tlm_sync_enum status = instr_bus->nb_transport_fw(fetch_trans, phase, delay);
    
    switch (status) {
        case tlm::TLM_ACCEPTED:
            if_stage_busy = true;
            logger->debug("AT: BEGIN_REQ accepted for PC=0x{:x}", address);
            return true;
            
        case tlm::TLM_UPDATED:
            if (phase == tlm::END_REQ) {
                if_stage_busy = true;
                return true;
            } else if (phase == tlm::BEGIN_RESP) {
                if (fetch_trans.is_response_ok()) {
                    fetched_instruction = instr_buffer;
                    if_stage_busy = false;
                    
                    tlm::tlm_phase end_phase = tlm::END_RESP;
                    sc_core::sc_time end_delay = sc_core::SC_ZERO_TIME;
                    instr_bus->nb_transport_fw(fetch_trans, end_phase, end_delay);
                    
                    fetch_complete_event.notify();
                    return true;
                }
            }
            break;
            
        case tlm::TLM_COMPLETED:
            if (fetch_trans.is_response_ok()) {
                fetched_instruction = instr_buffer;
                if_stage_busy = false;
                fetch_complete_event.notify();
                return true;
            }
            SC_REPORT_ERROR("CPURV64P2_AT", "Immediate fetch failed");
            return false;
    }
    
    return false;
}

// =============================================================================
// Wait for pending fetch to complete
// =============================================================================

std::uint32_t CPURV64P2_AT::wait_for_fetch() {
    if (if_stage_busy) {
        sc_core::sc_time timeout = clock_period * 100;
        sc_core::sc_time start = sc_core::sc_time_stamp();
        
        while (if_stage_busy) {
            sc_core::wait(fetch_complete_event | clk->posedge_event());
            stats.if_stalls++;
            stats.mem_latency_cycles++;
            
            if ((sc_core::sc_time_stamp() - start) > timeout) {
                SC_REPORT_ERROR("CPURV64P2_AT", "Fetch timeout - memory not responding");
                break;
            }
        }
    }
    return fetched_instruction;
}

// =============================================================================
// Main Pipeline Thread
// =============================================================================

void CPURV64P2_AT::pipeline_thread() {
    if (clk) {
        sc_core::wait(clk->posedge_event());
    } else {
        sc_core::wait(clock_period);
    }
    
    while (true) {
        stats.cycles++;
        
        if_ex_latch = if_ex_latch_next;
        
        bool breakpoint = EX_stage();
        IF_stage();
        cpu_process_IRQ();
        
        if (breakpoint) {
            logger->info("Breakpoint hit at PC=0x{:x}", if_ex_latch.pc);
        }
        
        if (clk) {
            sc_core::wait(clk->posedge_event());
        } else {
            sc_core::wait(clock_period);
        }
    }
}

// =============================================================================
// IF Stage
// =============================================================================

void CPURV64P2_AT::IF_stage() {
    if (pipeline_flush) {
        if_ex_latch_next.valid = false;
        if_ex_latch_next.instruction = 0;
        if_ex_latch_next.pc = 0;
        pipeline_flush = false;
        stats.flushes++;
        return;
    }

    std::uint64_t current_pc = register_bank->getPC();

    if (dmi_ptr_valid) {
        std::memcpy(&if_ex_latch_next.instruction, dmi_ptr + current_pc, 4);
        if_ex_latch_next.pc = current_pc;
        if_ex_latch_next.valid = true;
    } else {
        if (initiate_fetch(current_pc)) {
            std::uint32_t instr = wait_for_fetch();
            
            if_ex_latch_next.instruction = instr;
            if_ex_latch_next.pc = current_pc;
            if_ex_latch_next.valid = true;
        } else {
            if_ex_latch_next.valid = false;
            if_ex_latch_next.instruction = 0;
            if_ex_latch_next.pc = 0;
            stats.stalls++;
            return;
        }
    }

    if ((if_ex_latch_next.instruction & 0x3) != 0x3) {
        register_bank->incPCby2();
    } else {
        register_bank->incPC();
    }

    perf->codeMemoryRead();
}

// =============================================================================
// EX Stage
// =============================================================================

bool CPURV64P2_AT::EX_stage() {
    bool breakpoint = false;

    if (!if_ex_latch.valid) {
        stats.stalls++;
        return false;
    }

    std::uint32_t instr = if_ex_latch.instruction;
    inst.setInstr(instr);

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
                    std::cout << "Extension not implemented yet" << std::endl;
                    inst.dump();
                    base_inst->NOP();
                }
            }
        }
    }

    if (is_branch && pc_changed) {
        pipeline_flush = true;
        stats.control_hazards++;
    }

    stats.instructions++;
    perf->instructionsInc();
    return breakpoint;
}

// =============================================================================
// CPU_step (for debug mode compatibility)
// =============================================================================

bool CPURV64P2_AT::CPU_step() {
    bool breakpoint = false;

    stats.cycles++;
    if_ex_latch = if_ex_latch_next;
    breakpoint = EX_stage();
    IF_stage();

    if (clk) {
        sc_core::wait(clk->posedge_event());
    } else {
        sc_core::wait(clock_period);
    }

    return breakpoint;
}

// =============================================================================
// IRQ Processing
// =============================================================================

bool CPURV64P2_AT::cpu_process_IRQ() {
    BaseType csr_temp;
    bool ret_value = false;

    if (interrupt) {
        csr_temp = register_bank->getCSR(CSR_MSTATUS);
        if ((csr_temp & MSTATUS_MIE) == 0) {
            logger->debug("{} ns. PC: 0x{:x}. Interrupt delayed", 
                         sc_core::sc_time_stamp().value(), register_bank->getPC());
            return ret_value;
        }

        csr_temp = register_bank->getCSR(CSR_MIP);
        if ((csr_temp & MIP_MEIP) == 0) {
            csr_temp |= MIP_MEIP;
            register_bank->setCSR(CSR_MIP, csr_temp);

            logger->debug("{} ns. PC: 0x{:x}. Interrupt!", 
                         sc_core::sc_time_stamp().value(), register_bank->getPC());

            BaseType old_pc = register_bank->getPC();
            register_bank->setCSR(CSR_MEPC, old_pc);
            register_bank->setCSR(CSR_MCAUSE, 0x8000000000000000ULL);
            BaseType new_pc = register_bank->getCSR(CSR_MTVEC);
            register_bank->setPC(new_pc);

            pipeline_flush = true;
            if_ex_latch.valid = false;
            if_ex_latch_next.valid = false;
            stats.flushes++;
            stats.cycles += 2;

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

void CPURV64P2_AT::call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) {
    interrupt = true;
    memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
    delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV64P2_AT::getStartDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t0);
}

std::uint64_t CPURV64P2_AT::getEndDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t1);
}

void CPURV64P2_AT::printStats() const {
    std::cout << "  Architecture: RV64 (AT 2-Stage Pipeline)\n";
    std::cout << "  Cycles:       " << stats.cycles << "\n";
    std::cout << "  Instructions: " << stats.instructions << "\n";
    std::cout << "  CPI:          " << std::fixed << std::setprecision(2) << stats.get_cpi() << "\n";
    std::cout << "  IPC:          " << std::fixed << std::setprecision(3) << stats.get_ipc() << "\n";
    std::cout << "  Stalls:       " << stats.stalls << "\n";
    std::cout << "  Flushes:      " << stats.flushes << "\n";
}

} // namespace riscv_tlm
