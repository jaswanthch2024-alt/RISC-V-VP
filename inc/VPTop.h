/*!
 \file VPTop.h
 \brief Virtual Prototype top-level that assembles CPU, Bus, Memory, Timer, Trace (extended)
 \note Supports multiple timing models: LT (Loosely-Timed), AT (Approximately-Timed), CYCLE
 */
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef __VP_TOP_H__
#define __VP_TOP_H__

#pragma once
#define SC_INCLUDE_DYNAMIC_PROCESSES

#include "systemc"
#include <memory>
#include <string>

#include "CPU.h"
#include "TimingModel.h"

// Bus models
#include "BusCtrl.h"

// Peripherals
#include "Trace.h"
#include "Timer.h"
#include "UART.h"
#include "CLINT.h"
#include "PLIC.h"
#include "DMA.h"
#include "SyscallIf.h"

// CPU models based on timing selection
#if defined(ENABLE_PIPELINED_ISS)
  #if defined(ENABLE_CYCLE6_MODEL)
    #include "CPU_P32_6_Cycle.h"
    #include "CPU_P64_6_Cycle.h"
  #elif defined(ENABLE_CYCLE_MODEL)
    #include "CPU_P32_2_Cycle.h"
    #include "CPU_P64_2_Cycle.h"
  #elif defined(ENABLE_AT_MODEL)
    #include "CPU_P32_2_AT.h"
    #include "CPU_P64_2_AT.h"
  #else
    #include "CPU_P32_2.h"
    #include "CPU_P64_2.h"
  #endif
#endif

namespace riscv_tlm { class Debug; }

namespace vp {

/**
 * @brief Virtual Prototype Top-Level Module
 * 
 * Assembles CPU, Bus, Memory, and Peripherals based on selected timing model.
 * 
 * Timing Models:
 * - LT (Loosely-Timed): Fast simulation, b_transport
 * - AT (Approximately-Timed): Phase-accurate, nb_transport
 * - CYCLE: Cycle-accurate, clock-driven
 */
class VPTop : public sc_core::sc_module {
public:
    // CPU (common base pointer)
    riscv_tlm::CPU *cpu;
    // Memory (Always LT implementation as per simplified model)
    riscv_tlm::Memory *MainMemory;
    riscv_tlm::BusCtrl *Bus;
    
    // Peripherals
    riscv_tlm::peripherals::Trace *trace;
    riscv_tlm::peripherals::Timer *timer;
    riscv_tlm::peripherals::UART *uart;
    riscv_tlm::peripherals::CLINT *clint;
    riscv_tlm::peripherals::PLIC *plic;
    riscv_tlm::peripherals::DMA *dma;
    riscv_tlm::peripherals::SyscallIf *sysif;

    SC_HAS_PROCESS(VPTop);

    /**
     * @brief Construct VP with selected components
     * @param name Module name
     * @param hex_file Path to Intel HEX file
     * @param cpu_type RV32 or RV64
     * @param debug_mode Enable debug mode
     */
    VPTop(sc_core::sc_module_name const &name,
          const std::string &hex_file,
          riscv_tlm::cpu_types_t cpu_type,
          bool debug_mode,
          const std::string &bios_file = "",
          const std::string &dtb_file  = "",
          const std::string &kernel_file = "",
          const std::string &initrd_file = "");

    ~VPTop() override;

    /**
     * @brief Get current timing model
     */
    static riscv_tlm::TimingModelType getTimingModel() {
#if defined(ENABLE_CYCLE6_MODEL)
        return riscv_tlm::TimingModelType::CYCLE6;
#elif defined(ENABLE_CYCLE_MODEL)
        return riscv_tlm::TimingModelType::CYCLE;
#elif defined(ENABLE_AT_MODEL)
        return riscv_tlm::TimingModelType::AT;
#else
        return riscv_tlm::TimingModelType::LT;
#endif
    }

private:
    bool m_debug;
    riscv_tlm::cpu_types_t m_cpu_type;
#ifndef _WIN32
    std::unique_ptr<riscv_tlm::Debug> m_debugger;
#endif
    sc_core::sc_clock clk;
    sc_core::sc_signal<bool> timer_irq_sig{"timer_irq_sig"};
    sc_core::sc_signal<bool> msip_irq_sig{"msip_irq_sig"};
    sc_core::sc_signal<bool> ext_irq_sig{"ext_irq_sig"};
};

} // namespace vp
#endif