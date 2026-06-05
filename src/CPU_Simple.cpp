// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_Simple.cpp
 * @brief Simple non-pipelined RISC-V CPU Implementation (LT model)
 * 
 * Loosely-Timed functional model for fast simulation.
 * No pipeline timing - just functional execution.
 */
#include "CPU_Simple.h"
#include "spdlog/spdlog.h"

namespace riscv_tlm {

// =============================================================================
// CPURV32Simple Implementation
// =============================================================================

CPURV32Simple::CPURV32Simple(sc_core::sc_module_name const& name, BaseType PC, bool debug)
    : CPU(name, debug) {

    register_bank = new Registers<BaseType>();
    mem_intf = new MemoryInterface();

    register_bank->setPC(PC);
    register_bank->setValue(Registers<BaseType>::sp, (Memory::SIZE / 4) - 1);
    int_cause = 0;

    instr_bus.register_invalidate_direct_mem_ptr(this, &CPU::invalidate_direct_mem_ptr);

    base_inst = new BASE_ISA<BaseType>(0, register_bank, mem_intf);
    c_inst    = new C_extension<BaseType>(0, register_bank, mem_intf);
    m_inst    = new M_extension<BaseType>(0, register_bank, mem_intf);
    a_inst    = new A_extension<BaseType>(0, register_bank, mem_intf);

    trans.set_data_ptr(reinterpret_cast<unsigned char*>(&INSTR));
    trans.set_command(tlm::TLM_READ_COMMAND);
    trans.set_data_length(4);
    trans.set_streaming_width(4);
    trans.set_byte_enable_ptr(nullptr);
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    logger->info("Created CPURV32Simple (non-pipelined LT) CPU");
    std::cout << "Created CPURV32Simple (non-pipelined LT) CPU" << std::endl;
}

CPURV32Simple::~CPURV32Simple() {
    delete register_bank;
    delete mem_intf;
    delete base_inst;
    delete c_inst;
    delete m_inst;
    delete a_inst;
    // m_qk is owned and deleted by the CPU base class destructor
}

bool CPURV32Simple::CPU_step() {
    bool breakpoint = false;

    // Fetch instruction
    if (dmi_ptr_valid) {
        std::memcpy(&INSTR, dmi_ptr + register_bank->getPC(), 4);
    } else {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        tlm::tlm_dmi dmi_data;
        trans.set_address(register_bank->getPC());
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        instr_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            SC_REPORT_ERROR("CPURV32Simple", "Instruction fetch error");
        }
        if (trans.is_dmi_allowed()) {
            dmi_ptr_valid = instr_bus->get_direct_mem_ptr(trans, dmi_data);
            if (dmi_ptr_valid) {
                dmi_ptr = dmi_data.get_dmi_ptr();
            }
        }
    }

    perf->codeMemoryRead();
    inst.setInstr(INSTR);

    // Decode and execute
    base_inst->setInstr(INSTR);
    auto deco = base_inst->decode();

    if (deco != OP_ERROR) {
        bool pc_changed = !base_inst->exec_instruction(inst, &breakpoint, deco);
        if (!pc_changed) {
            register_bank->incPC();
        }
    } else {
        c_inst->setInstr(INSTR);
        auto c_deco = c_inst->decode();

        if (c_deco != OP_C_ERROR) {
            bool pc_changed = !c_inst->exec_instruction(inst, &breakpoint, c_deco);
            if (!pc_changed) {
                register_bank->incPCby2();
            }
        } else {
            m_inst->setInstr(INSTR);
            auto m_deco = m_inst->decode();

            if (m_deco != OP_M_ERROR) {
                bool pc_changed = !m_inst->exec_instruction(inst, m_deco);
                if (!pc_changed) {
                    register_bank->incPC();
                }
            } else {
                a_inst->setInstr(INSTR);
                auto a_deco = a_inst->decode();

                if (a_deco != OP_A_ERROR) {
                    bool pc_changed = !a_inst->exec_instruction(inst, a_deco);
                    if (!pc_changed) {
                        register_bank->incPC();
                    }
                } else {
                    std::cout << "Extension not implemented yet" << std::endl;
                    inst.dump();
                    base_inst->NOP();
                    register_bank->incPC();
                }
            }
        }
    }

    perf->instructionsInc();

    // Simple timing: wait one cycle
    sc_core::wait(sc_core::sc_time(10, sc_core::SC_NS));

    return breakpoint;
}

bool CPURV32Simple::cpu_process_IRQ() {
    BaseType csr_temp;
    bool ret_value = false;

    if (interrupt) {
        csr_temp = register_bank->getCSR(CSR_MSTATUS);
        if ((csr_temp & MSTATUS_MIE) == 0) {
            return ret_value;
        }

        csr_temp = register_bank->getCSR(CSR_MIP);
        if ((csr_temp & MIP_MEIP) == 0) {
            csr_temp |= MIP_MEIP;
            register_bank->setCSR(CSR_MIP, csr_temp);

            BaseType old_pc = register_bank->getPC();
            register_bank->setCSR(CSR_MEPC, old_pc);
            register_bank->setCSR(CSR_MCAUSE, 0x80000000);
            BaseType new_pc = register_bank->getCSR(CSR_MTVEC);
            register_bank->setPC(new_pc);

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

void CPURV32Simple::call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) {
    interrupt = true;
    memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
    delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV32Simple::getStartDumpAddress() {
    return register_bank->getValue(Registers<std::uint32_t>::t0);
}

std::uint64_t CPURV32Simple::getEndDumpAddress() {
    return register_bank->getValue(Registers<std::uint32_t>::t1);
}

// =============================================================================
// CPURV64Simple Implementation
// =============================================================================

CPURV64Simple::CPURV64Simple(sc_core::sc_module_name const& name, BaseType PC, bool debug)
    : CPU(name, debug) {

    register_bank = new Registers<BaseType>();
    mem_intf = new MemoryInterface();

    register_bank->setPC(PC);
    register_bank->setValue(Registers<BaseType>::sp, (Memory::SIZE / 8) - 1);
    int_cause = 0;

    instr_bus.register_invalidate_direct_mem_ptr(this, &CPU::invalidate_direct_mem_ptr);

    base_inst = new BASE_ISA<BaseType>(0, register_bank, mem_intf);
    c_inst    = new C_extension<BaseType>(0, register_bank, mem_intf);
    m_inst    = new M_extension<BaseType>(0, register_bank, mem_intf);
    a_inst    = new A_extension<BaseType>(0, register_bank, mem_intf);

    trans.set_data_ptr(reinterpret_cast<unsigned char*>(&INSTR));
    trans.set_command(tlm::TLM_READ_COMMAND);
    trans.set_data_length(4);
    trans.set_streaming_width(4);
    trans.set_byte_enable_ptr(nullptr);
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    logger->info("Created CPURV64Simple (non-pipelined LT) CPU");
    std::cout << "Created CPURV64Simple (non-pipelined LT) CPU" << std::endl;
}

CPURV64Simple::~CPURV64Simple() {
    delete register_bank;
    delete mem_intf;
    delete base_inst;
    delete c_inst;
    delete m_inst;
    delete a_inst;
    // m_qk is owned and deleted by the CPU base class destructor
}

bool CPURV64Simple::CPU_step() {
    bool breakpoint = false;

    // Fetch instruction
    if (dmi_ptr_valid) {
        std::memcpy(&INSTR, dmi_ptr + register_bank->getPC(), 4);
    } else {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        tlm::tlm_dmi dmi_data;
        trans.set_address(register_bank->getPC());
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        instr_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            SC_REPORT_ERROR("CPURV64Simple", "Instruction fetch error");
        }
        if (trans.is_dmi_allowed()) {
            dmi_ptr_valid = instr_bus->get_direct_mem_ptr(trans, dmi_data);
            if (dmi_ptr_valid) {
                dmi_ptr = dmi_data.get_dmi_ptr();
            }
        }
    }

    perf->codeMemoryRead();
    inst.setInstr(INSTR);

    // Decode and execute
    base_inst->setInstr(INSTR);
    auto deco = base_inst->decode();

    if (deco != OP_ERROR) {
        bool pc_changed = !base_inst->exec_instruction(inst, &breakpoint, deco);
        if (!pc_changed) {
            register_bank->incPC();
        }
    } else {
        c_inst->setInstr(INSTR);
        auto c_deco = c_inst->decode();

        if (c_deco != OP_C_ERROR) {
            bool pc_changed = !c_inst->exec_instruction(inst, &breakpoint, c_deco);
            if (!pc_changed) {
                register_bank->incPCby2();
            }
        } else {
            m_inst->setInstr(INSTR);
            auto m_deco = m_inst->decode();

            if (m_deco != OP_M_ERROR) {
                bool pc_changed = !m_inst->exec_instruction(inst, m_deco);
                if (!pc_changed) {
                    register_bank->incPC();
                }
            } else {
                a_inst->setInstr(INSTR);
                auto a_deco = a_inst->decode();

                if (a_deco != OP_A_ERROR) {
                    bool pc_changed = !a_inst->exec_instruction(inst, a_deco);
                    if (!pc_changed) {
                        register_bank->incPC();
                    }
                } else {
                    std::cout << "Extension not implemented yet" << std::endl;
                    inst.dump();
                    base_inst->NOP();
                    register_bank->incPC();
                }
            }
        }
    }

    perf->instructionsInc();

    // Simple timing: wait one cycle
    sc_core::wait(sc_core::sc_time(10, sc_core::SC_NS));

    return breakpoint;
}

bool CPURV64Simple::cpu_process_IRQ() {
    BaseType csr_temp;
    bool ret_value = false;

    if (interrupt) {
        csr_temp = register_bank->getCSR(CSR_MSTATUS);
        if ((csr_temp & MSTATUS_MIE) == 0) {
            return ret_value;
        }

        csr_temp = register_bank->getCSR(CSR_MIP);
        if ((csr_temp & MIP_MEIP) == 0) {
            csr_temp |= MIP_MEIP;
            register_bank->setCSR(CSR_MIP, csr_temp);

            BaseType old_pc = register_bank->getPC();
            register_bank->setCSR(CSR_MEPC, old_pc);
            register_bank->setCSR(CSR_MCAUSE, 0x80000000);
            BaseType new_pc = register_bank->getCSR(CSR_MTVEC);
            register_bank->setPC(new_pc);

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

void CPURV64Simple::call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) {
    interrupt = true;
    memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
    delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV64Simple::getStartDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t0);
}

std::uint64_t CPURV64Simple::getEndDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t1);
}

} // namespace riscv_tlm
