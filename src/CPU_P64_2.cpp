// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file CPU_P64_2.cpp
 * @brief 2-Stage Pipelined RISC-V 64-bit CPU Implementation
 * 
 * True 2-stage pipeline: IF -> [LATCH] -> EX
 * - IF: Fetch instruction, load into latch
 * - EX: Decode + Execute + Memory + Writeback
 * 
 * Pipeline latch holds instruction between stages.
 * Branch taken causes 1-cycle flush penalty.
 */
#include "CPU_P64_2.h"
#include "spdlog/spdlog.h"

namespace riscv_tlm {

CPURV64P2::CPURV64P2(sc_core::sc_module_name const& name, BaseType PC, bool debug)
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

    trans.set_data_ptr(reinterpret_cast<unsigned char*>(&if_ex_latch.instruction));
    trans.set_command(tlm::TLM_READ_COMMAND);
    trans.set_data_length(4);
    trans.set_streaming_width(4);
    trans.set_byte_enable_ptr(nullptr);
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    // Initialize pipeline latch (empty on startup - first cycle is IF only)
    if_ex_latch.instruction = 0;
    if_ex_latch.pc = 0;
    if_ex_latch.valid = false;
    pipeline_flush = false;

    logger->info("Created CPURV64P2 (2-stage pipelined) CPU for VP");
    std::cout << "Created CPURV64P2 (2-stage pipelined) CPU for VP" << std::endl;
}

CPURV64P2::~CPURV64P2() {
    delete register_bank;
    delete mem_intf;
    delete base_inst;
    delete c_inst;
    delete m_inst;
    delete a_inst;
    // m_qk is owned and deleted by the CPU base class destructor
}

// =============================================================================
// IF Stage: Fetch instruction from memory
// =============================================================================
void CPURV64P2::IF_stage() {
    // If flush requested, insert bubble (invalid latch)
    if (pipeline_flush) {
        if_ex_latch.valid = false;
        if_ex_latch.instruction = 0;
        if_ex_latch.pc = 0;
        pipeline_flush = false;  // Clear flush flag
        return;
    }

    // Fetch instruction at current PC
    std::uint64_t current_pc = register_bank->getPC();

    if (dmi_ptr_valid) {
        std::memcpy(&if_ex_latch.instruction, dmi_ptr + current_pc, 4);
    } else {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        tlm::tlm_dmi dmi_data;
        trans.set_address(current_pc);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        instr_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            SC_REPORT_ERROR("CPURV64P2", "Instruction fetch error");
        }
        if (trans.is_dmi_allowed()) {
            dmi_ptr_valid = instr_bus->get_direct_mem_ptr(trans, dmi_data);
            if (dmi_ptr_valid) {
                dmi_ptr = dmi_data.get_dmi_ptr();
            }
        }
    }

    // Load latch with fetched instruction
    if_ex_latch.pc = current_pc;
    if_ex_latch.valid = true;

    // Speculatively increment PC (assumes branch not taken)
    // Check if compressed instruction (16-bit)
    if ((if_ex_latch.instruction & 0x3) != 0x3) {
        register_bank->incPCby2();  // Compressed instruction
    } else {
        register_bank->incPC();     // Normal 32-bit instruction
    }

    perf->codeMemoryRead();
}

// =============================================================================
// EX Stage: Decode and execute instruction from latch
// =============================================================================
bool CPURV64P2::EX_stage() {
    bool breakpoint = false;

    // If latch is invalid (bubble), do nothing
    if (!if_ex_latch.valid) {
        stats.stalls++;
        return false;
    }

    // Get instruction from latch
    std::uint32_t instr = if_ex_latch.instruction;
    inst.setInstr(instr);

    bool pc_changed = false;
    bool is_branch = false;

    // Decode and execute using extension handlers
    base_inst->setInstr(instr);
    auto deco = base_inst->decode();

    if (deco != OP_ERROR) {
        // Check if branch instruction
        std::uint32_t opcode = instr & 0x7F;
        is_branch = (opcode == 0x63 || opcode == 0x6F || opcode == 0x67);

        pc_changed = !base_inst->exec_instruction(inst, &breakpoint, deco);
    } else {
        // Try C extension
        c_inst->setInstr(instr);
        auto c_deco = c_inst->decode();

        if (c_deco != OP_C_ERROR) {
            is_branch = (c_deco == OP_C_J || c_deco == OP_C_JAL || 
                        c_deco == OP_C_JR || c_deco == OP_C_JALR ||
                        c_deco == OP_C_BEQZ || c_deco == OP_C_BNEZ);

            pc_changed = !c_inst->exec_instruction(inst, &breakpoint, c_deco);
        } else {
            // Try M extension
            m_inst->setInstr(instr);
            auto m_deco = m_inst->decode();

            if (m_deco != OP_M_ERROR) {
                pc_changed = !m_inst->exec_instruction(inst, m_deco);
            } else {
                // Try A extension
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

    // If branch taken, flush the IF stage (next cycle will be bubble)
    if (is_branch && pc_changed) {
        pipeline_flush = true;
        stats.flushes++;
        stats.control_hazards++;
    }

    perf->instructionsInc();
    return breakpoint;
}

// =============================================================================
// CPU_step: Execute one pipeline cycle
// =============================================================================
bool CPURV64P2::CPU_step() {
    bool breakpoint = false;

    stats.cycles++;

    // =========================================================================
    // Pipeline Execution Order (simulates parallel execution)
    // 
    // On a real CPU, both stages execute simultaneously on clock edge.
    // In simulation, we execute EX first (uses old latch), then IF (updates latch).
    // This models the latch being written by IF and read by EX on same cycle.
    // =========================================================================

    // EX Stage: Execute instruction from latch (uses previous IF result)
    breakpoint = EX_stage();

    // IF Stage: Fetch next instruction (updates latch for next cycle)
    IF_stage();

    // Account for flush penalty in cycle count
    if (pipeline_flush) {
        stats.cycles++;  // Flush costs 1 extra cycle
    }

    // LT timing: one clock cycle
    sc_core::wait(sc_core::sc_time(10, sc_core::SC_NS));

    return breakpoint;
}

// =============================================================================
// IRQ Processing
// =============================================================================
bool CPURV64P2::cpu_process_IRQ() {
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
            register_bank->setCSR(CSR_MCAUSE, 0x80000000);
            BaseType new_pc = register_bank->getCSR(CSR_MTVEC);
            register_bank->setPC(new_pc);

            // Flush pipeline on interrupt
            pipeline_flush = true;
            if_ex_latch.valid = false;
            stats.flushes++;
            stats.cycles += 2;  // IRQ latency

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

void CPURV64P2::call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) {
    interrupt = true;
    memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
    delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV64P2::getStartDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t0);
}

std::uint64_t CPURV64P2::getEndDumpAddress() {
    return register_bank->getValue(Registers<std::uint64_t>::t1);
}

} // namespace riscv_tlm
