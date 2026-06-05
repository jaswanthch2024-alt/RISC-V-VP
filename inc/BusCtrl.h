/**
 @file BusCtrl.h
 @brief Basic TLM-2 Bus controller (extended with UART, CLINT, PLIC, DMA, Syscall stubs)
 */
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef __BUSCTRL_H__
#define __BUSCTRL_H__

#include <iostream>
#include <fstream>

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"

namespace riscv_tlm {

// Existing mapped peripherals
#define TRACE_MEMORY_ADDRESS      0x40000000
#define TIMER_MEMORY_ADDRESS_LO   0x40004000
#define TIMER_MEMORY_ADDRESS_HI   0x40004004
#define TIMERCMP_MEMORY_ADDRESS_LO 0x40004008
#define TIMERCMP_MEMORY_ADDRESS_HI 0x4000400C

#define UART0_BASE_ADDRESS        0x10000000

// New stub blocks (addresses follow common RISC-V conventions)
#define CLINT_BASE_ADDRESS        0x02000000
#define PLIC_BASE_ADDRESS         0x0C000000
#define DMA_BASE_ADDRESS          0x30000000
// NOTE: SYSCALL_BASE_ADDRESS must NOT overlap with Linux DRAM (0x80000000+).
// Moved to 0x60000000 to leave 0x80000000+ free for Linux firmware payload.
#define SYSCALL_BASE_ADDRESS      0x60000000

#define TO_HOST_ADDRESS           0x90000000
// HTIF tohost address used by riscv-tests (default env/p linker script)
// Write 1 = PASS; write (n<<1)|1 = FAIL test n
#define HTIF_TOHOST_ADDR          0x80001000ULL

// Linux/BBL physical DRAM base — the bus forwards this range to Main Memory.
// The memory model uses a flat array; the bus subtracts DRAM_BASE to get the
// memory-local offset before forwarding.
#define DRAM_BASE_ADDRESS         0x80000000ULL
#define DRAM_SIZE                 0x20000000ULL  // 512 MB

class BusCtrl : sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<BusCtrl>    cpu_instr_socket;
    tlm_utils::simple_target_socket<BusCtrl>    cpu_data_socket;

    // Additional target socket to accept DMA master transactions into the Bus
    tlm_utils::simple_target_socket<BusCtrl>    dma_master_socket;

    tlm_utils::simple_initiator_socket<BusCtrl> memory_socket;
    tlm_utils::simple_initiator_socket<BusCtrl> trace_socket;
    tlm_utils::simple_initiator_socket<BusCtrl> timer_socket;

    // Optional peripherals
    tlm_utils::simple_initiator_socket<BusCtrl> uart_socket;
    tlm_utils::simple_initiator_socket<BusCtrl> clint_socket;   // new
    tlm_utils::simple_initiator_socket<BusCtrl> plic_socket;    // new
    tlm_utils::simple_initiator_socket<BusCtrl> dma_socket;     // new (register interface)
    tlm_utils::simple_initiator_socket<BusCtrl> syscall_socket; // new

    explicit BusCtrl(sc_core::sc_module_name const &name);

    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);

private:
    bool instr_direct_mem_ptr(tlm::tlm_generic_payload &, tlm::tlm_dmi &dmi_data);
    void invalidate_direct_mem_ptr(sc_dt::uint64 start, sc_dt::uint64 end);
};
}
#endif
