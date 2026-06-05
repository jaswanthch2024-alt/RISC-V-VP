/*!
 \file VPTop.cpp
 \brief Virtual Prototype top-level with multi-timing model support
 \note Supports LT, AT, and CYCLE timing models for CPU, Bus, and Memory
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include "VPTop.h"

// CPU includes based on timing model
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

#ifndef _WIN32
#include "Debug.h"
#endif

namespace vp {

VPTop::VPTop(sc_core::sc_module_name const &name,
             const std::string &hex_file,
             riscv_tlm::cpu_types_t cpu_type,
             bool debug_mode,
             const std::string &bios_file,
             const std::string &dtb_file,
             const std::string &kernel_file,
             const std::string &initrd_file)
    : sc_module(name),
      cpu(nullptr),
      MainMemory(nullptr),
      Bus(nullptr),
      trace(nullptr),
      timer(nullptr),
      uart(nullptr),
      clint(nullptr),
      plic(nullptr),
      dma(nullptr),
      sysif(nullptr),
      m_debug(debug_mode),
      m_cpu_type(cpu_type),
      clk("clk", sc_core::sc_time(10, sc_core::SC_NS))
{
    std::uint32_t start_PC;

    // Print timing model being used
    std::cout << "========================================" << std::endl;
    std::cout << "Virtual Prototype Timing Model: " 
              << riscv_tlm::timing_model_name(getTimingModel()) << std::endl;
    std::cout << "========================================" << std::endl;

    // =========================================================================
    // Create Memory
    // =========================================================================
    const bool boot_mode = !bios_file.empty();
    if (boot_mode) {
        // OpenSBI + DTB + Linux boot: create empty memory, then load binaries
        MainMemory = new riscv_tlm::Memory("Main_Memory");
        MainMemory->loadBin(bios_file,   0x80000000ULL); // OpenSBI at reset vector
        if (!dtb_file.empty())
            MainMemory->loadBin(dtb_file,    0x80100000ULL); // DTB
        if (!kernel_file.empty())
            MainMemory->loadBin(kernel_file, 0x80200000ULL); // Linux Image
        if (!initrd_file.empty())
            MainMemory->loadBin(initrd_file, 0x84000000ULL); // Initramfs
        start_PC = 0x80000000U;
    } else {
        MainMemory = new riscv_tlm::Memory("Main_Memory", hex_file);
        start_PC = MainMemory->getPCfromHEX();
    }

    // =========================================================================
    // Create CPU based on architecture and timing model
    // =========================================================================
    if (m_cpu_type == riscv_tlm::RV32) {
#if defined(ENABLE_PIPELINED_ISS)
  #if defined(ENABLE_CYCLE6_MODEL)
        cpu = new riscv_tlm::CPURV32P6_Cycle("cpu", start_PC, m_debug);
        std::cout << "CPU: RV32 Cycle-Accurate 6-Stage Pipeline" << std::endl;
  #elif defined(ENABLE_CYCLE_MODEL)
        cpu = new riscv_tlm::CPURV32P2_Cycle("cpu", start_PC, m_debug);
        std::cout << "CPU: RV32 Cycle-Accurate 2-Stage Pipeline" << std::endl;
  #elif defined(ENABLE_AT_MODEL)
        cpu = new riscv_tlm::CPURV32P2_AT("cpu", start_PC, m_debug);
        std::cout << "CPU: RV32 AT (Approximately-Timed) 2-Stage Pipeline" << std::endl;
  #else
        cpu = new riscv_tlm::CPURV32P2("cpu", start_PC, m_debug);
        std::cout << "CPU: RV32 LT (Loosely-Timed) 2-Stage Pipeline" << std::endl;
  #endif
#else
        std::cerr << "Error: Pipelined ISS not enabled." << std::endl;
        std::exit(1);
#endif
    } else {
#if defined(ENABLE_PIPELINED_ISS)
  #if defined(ENABLE_CYCLE6_MODEL)
        cpu = new riscv_tlm::CPURV64P6_Cycle("cpu", start_PC, m_debug);
        std::cout << "CPU: RV64 Cycle-Accurate 6-Stage Pipeline" << std::endl;
  #elif defined(ENABLE_CYCLE_MODEL)
        cpu = new riscv_tlm::CPURV64P2_Cycle("cpu", start_PC, m_debug);
        std::cout << "CPU: RV64 Cycle-Accurate 2-Stage Pipeline" << std::endl;
  #elif defined(ENABLE_AT_MODEL)
        cpu = new riscv_tlm::CPURV64P2_AT("cpu", start_PC, m_debug);
        std::cout << "CPU: RV64 AT (Approximately-Timed) 2-Stage Pipeline" << std::endl;
  #else
        cpu = new riscv_tlm::CPURV64P2("cpu", start_PC, m_debug);
        std::cout << "CPU: RV64 LT (Loosely-Timed) 2-Stage Pipeline" << std::endl;
  #endif
#else
        std::cerr << "Error: Pipelined ISS not enabled." << std::endl;
        std::exit(1);
#endif
    }

    cpu->set_clock(&clk);

    // In boot mode set OpenSBI calling convention: a0=hart_id=0, a1=DTB_phys_addr
    if (boot_mode) {
#if defined(ENABLE_PIPELINED_ISS)
  #if defined(ENABLE_CYCLE6_MODEL)
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P6_Cycle*>(cpu);
        if (cpu64 && cpu64->register_bank) {
            using Regs = riscv_tlm::Registers<riscv_tlm::CPURV64P6_Cycle::BaseType>;
            cpu64->register_bank->setValue(Regs::a0, 0ULL);            // a0 = hart ID 0
            cpu64->register_bank->setValue(Regs::a1, 0x80100000ULL);   // a1 = DTB addr
            cpu64->register_bank->setValue(Regs::sp, 0x9FFFFFF8ULL);   // SP = top of 512 MB RAM
        }
  #else
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P2*>(cpu);
        if (cpu64 && cpu64->register_bank) {
            using Regs = riscv_tlm::Registers<riscv_tlm::CPURV64P2::BaseType>;
            cpu64->register_bank->setValue(Regs::a0, 0ULL);            // a0 = hart ID 0
            cpu64->register_bank->setValue(Regs::a1, 0x80100000ULL);   // a1 = DTB addr
            cpu64->register_bank->setValue(Regs::sp, 0x9FFFFFF8ULL);   // SP = top of 512 MB RAM
        }
  #endif
#endif
    }

    // =========================================================================
    // Create Bus and Peripherals
    // =========================================================================
    Bus = new riscv_tlm::BusCtrl("BusCtrl");
    std::cout << "Bus: LT (Loosely-Timed)" << std::endl;

    trace = new riscv_tlm::peripherals::Trace("Trace");
    timer = new riscv_tlm::peripherals::Timer("Timer");
    uart  = new riscv_tlm::peripherals::UART("UART0");
    clint = new riscv_tlm::peripherals::CLINT("CLINT");
    plic  = new riscv_tlm::peripherals::PLIC("PLIC");
    dma   = new riscv_tlm::peripherals::DMA("DMA");
    dma->set_debug(m_debug);
    sysif = new riscv_tlm::peripherals::SyscallIf("SysIf");

    cpu->instr_bus.bind(Bus->cpu_instr_socket);
    cpu->mem_intf->data_bus.bind(Bus->cpu_data_socket);

    Bus->memory_socket.bind(MainMemory->socket);
    Bus->trace_socket.bind(trace->socket);
    Bus->timer_socket.bind(timer->socket);
    Bus->uart_socket.bind(uart->socket);
    Bus->clint_socket.bind(clint->socket);
    Bus->plic_socket.bind(plic->socket);
    Bus->dma_socket.bind(dma->socket);
    Bus->syscall_socket.bind(sysif->socket);

    dma->mem_master.bind(Bus->dma_master_socket);
    timer->irq_line.bind(cpu->irq_line_socket);

    // Wire CLINT timer and software interrupt outputs to CPU input ports
    clint->timer_irq.bind(timer_irq_sig);
    cpu->timer_irq_in.bind(timer_irq_sig);
    clint->msip_irq.bind(msip_irq_sig);
    cpu->msip_irq_in.bind(msip_irq_sig);
    plic->irq_out.bind(ext_irq_sig);
    cpu->ext_irq_in.bind(ext_irq_sig);

    // Wire UART TX interrupt (THRI) to PLIC source 1 (level-triggered)
    uart->set_uart_irq = [this](bool active) {
        plic->set_irq_level(1, active);
    };

    // Give CPU a pointer to CLINT for WFI fast-forward optimization
#if defined(ENABLE_CYCLE6_MODEL)
    if (auto* cpu64_cy = dynamic_cast<riscv_tlm::CPURV64P6_Cycle*>(cpu)) {
        cpu64_cy->clint_ptr = clint;
    }
#endif

    std::cout << "========================================" << std::endl;

#ifndef _WIN32
    if (m_debug) {
        std::cerr << "Warning: Debug not supported for pipelined CPUs." << std::endl;
    }
#endif
}

VPTop::~VPTop() {
    delete sysif;
    delete dma;
    delete plic;
    delete clint;
    delete uart;
    delete timer;
    delete trace;
    delete Bus;
    delete cpu;
    delete MainMemory;
}

} // namespace vp