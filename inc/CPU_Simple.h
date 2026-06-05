// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CPU_SIMPLE_H
#define CPU_SIMPLE_H

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
 * @brief Simple non-pipelined RISC-V 32-bit CPU for TLM simulation
 * 
 * This is a Loosely-Timed (LT) functional model without pipeline timing.
 * Each instruction executes completely before the next begins.
 * Used for fast functional simulation in RISCV_TLM.
 */
class CPURV32Simple : public CPU {
public:
    using BaseType = std::uint32_t;

    CPURV32Simple(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV32Simple() override;

    void set_clock(sc_core::sc_clock* c) override { clk = c; }

    bool CPU_step() override;

    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;

    bool isPipelined() const override { return false; }

private:
    Registers<BaseType>*     register_bank{nullptr};
    BASE_ISA<BaseType>*      base_inst{nullptr};
    C_extension<BaseType>*   c_inst{nullptr};
    M_extension<BaseType>*   m_inst{nullptr};
    A_extension<BaseType>*   a_inst{nullptr};

    std::uint32_t INSTR{0};
    BaseType int_cause{0};
    sc_core::sc_clock* clk{nullptr};

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
};

/**
 * @brief Simple non-pipelined RISC-V 64-bit CPU for TLM simulation
 */
class CPURV64Simple : public CPU {
public:
    using BaseType = std::uint64_t;

    CPURV64Simple(sc_core::sc_module_name const& name, BaseType PC, bool debug);
    ~CPURV64Simple() override;

    void set_clock(sc_core::sc_clock* c) override { clk = c; }

    bool CPU_step() override;

    bool cpu_process_IRQ() override;
    void call_interrupt(tlm::tlm_generic_payload &m_trans, sc_core::sc_time &delay) override;

    std::uint64_t getStartDumpAddress() override;
    std::uint64_t getEndDumpAddress() override;

    bool isPipelined() const override { return false; }

private:
    Registers<BaseType>*     register_bank{nullptr};
    BASE_ISA<BaseType>*      base_inst{nullptr};
    C_extension<BaseType>*   c_inst{nullptr};
    M_extension<BaseType>*   m_inst{nullptr};
    A_extension<BaseType>*   a_inst{nullptr};

    std::uint32_t INSTR{0};
    BaseType int_cause{0};
    sc_core::sc_clock* clk{nullptr};

    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) { dmi_ptr_valid = false; }
};

} // namespace riscv_tlm

#endif // CPU_SIMPLE_H
