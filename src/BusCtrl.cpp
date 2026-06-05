/**
 @file BusCtrl.cpp
 @brief Basic TLM-2 Bus controller (extended)
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BusCtrl.h"
#include <cstring>  // memcpy for HTIF tohost decoding

namespace riscv_tlm {

    SC_HAS_PROCESS(BusCtrl);

    BusCtrl::BusCtrl(sc_core::sc_module_name const &name) :
            sc_module(name),
            cpu_instr_socket("cpu_instr_socket"),
            cpu_data_socket("cpu_data_socket"),
            dma_master_socket("dma_master_socket"),
            memory_socket("memory_socket"),
            trace_socket("trace_socket"),
            timer_socket("timer_socket"),
            uart_socket("uart_socket"),
            clint_socket("clint_socket"),
            plic_socket("plic_socket"),
            dma_socket("dma_socket"),
            syscall_socket("syscall_socket") {

        // All masters enter through the same b_transport
        cpu_instr_socket.register_b_transport(this, &BusCtrl::b_transport);
        cpu_data_socket.register_b_transport(this, &BusCtrl::b_transport);
        dma_master_socket.register_b_transport(this, &BusCtrl::b_transport);

        cpu_instr_socket.register_get_direct_mem_ptr(this,
                                                     &BusCtrl::instr_direct_mem_ptr);
        memory_socket.register_invalidate_direct_mem_ptr(this,
                                                         &BusCtrl::invalidate_direct_mem_ptr);
    }

    void BusCtrl::b_transport(tlm::tlm_generic_payload &trans,
                              sc_core::sc_time &delay) {

        sc_dt::uint64 adr_bytes = trans.get_address();
        sc_dt::uint64 adr = adr_bytes / 4;


        // Legacy TO_HOST (0x90000000) — exact address match for RISC-V tests
        if (adr_bytes == TO_HOST_ADDRESS) {
            uint64_t val = 0;
            if (trans.get_command() == tlm::TLM_WRITE_COMMAND) {
                uint32_t len = trans.get_data_length();
                memcpy(&val, trans.get_data_ptr(), (len < 8) ? len : 8);
            }
            if (val == 1) {
                std::cout << "\n[HTIF] PASS\n" << std::flush;
            } else if (val & 1) {
                std::cout << "\n[HTIF] FAIL (test " << (val >> 1) << ")\n" << std::flush;
            } else {
                std::cout << "\n[HTIF] STOP (val=" << val << ")\n" << std::flush;
            }
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            sc_core::sc_stop();
            return;
        }

        // DRAM region: 0x80000000 – (0x80000000 + DRAM_SIZE - 1)
        // Linux / BBL / OpenSBI load here. Forward to Main Memory after
        // subtracting the DRAM base so the flat mem[] array is addressed
        // from offset 0.
        if (adr_bytes >= DRAM_BASE_ADDRESS && adr_bytes < DRAM_BASE_ADDRESS + DRAM_SIZE) {
            // Intercept HTIF tohost writes (riscv-tests pass/fail mechanism)
            if (adr_bytes == HTIF_TOHOST_ADDR && trans.get_command() == tlm::TLM_WRITE_COMMAND) {
                uint64_t val = 0;
                uint32_t len = trans.get_data_length();
                memcpy(&val, trans.get_data_ptr(), (len < 8) ? len : 8);
                if (val == 1) {
                    std::cout << "\n[HTIF] PASS\n" << std::flush;
                } else if (val & 1) {
                    std::cout << "\n[HTIF] FAIL (test " << (val >> 1) << ")\n" << std::flush;
                } else {
                    std::cout << "\n[HTIF] STOP (val=" << val << ")\n" << std::flush;
                }
                trans.set_response_status(tlm::TLM_OK_RESPONSE);
                sc_core::sc_stop();
                return;
            }
            sc_dt::uint64 saved = trans.get_address();
            trans.set_address(adr_bytes - DRAM_BASE_ADDRESS);
            memory_socket->b_transport(trans, delay);
            trans.set_address(saved);
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }

        // Decode by region (simple range checks). Optional targets are checked for binding.
        if (adr_bytes >= UART0_BASE_ADDRESS && adr_bytes < UART0_BASE_ADDRESS + 0x100) {
            if (uart_socket.size() > 0) {
                trans.set_address(adr_bytes - UART0_BASE_ADDRESS);
                uart_socket->b_transport(trans, delay);
                trans.set_address(adr_bytes);
            }
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }
        if (adr_bytes >= CLINT_BASE_ADDRESS && adr_bytes < CLINT_BASE_ADDRESS + 0x10000) {
            if (clint_socket.size() > 0) {
                trans.set_address(adr_bytes - CLINT_BASE_ADDRESS);
                clint_socket->b_transport(trans, delay);
                trans.set_address(adr_bytes);
            }
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }
        if (adr_bytes >= PLIC_BASE_ADDRESS && adr_bytes < PLIC_BASE_ADDRESS + 0x400000) {
            if (plic_socket.size() > 0) {
                trans.set_address(adr_bytes - PLIC_BASE_ADDRESS);
                plic_socket->b_transport(trans, delay);
                trans.set_address(adr_bytes);
            }
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }
        if (adr_bytes >= DMA_BASE_ADDRESS && adr_bytes < DMA_BASE_ADDRESS + 0x1000) {
            if (dma_socket.size() > 0) {
                trans.set_address(adr_bytes - DMA_BASE_ADDRESS);
                dma_socket->b_transport(trans, delay);
                trans.set_address(adr_bytes);
            }
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }
        if (adr_bytes >= SYSCALL_BASE_ADDRESS && adr_bytes < SYSCALL_BASE_ADDRESS + 0x1000) {
            if (syscall_socket.size() > 0) {
                trans.set_address(adr_bytes - SYSCALL_BASE_ADDRESS);
                syscall_socket->b_transport(trans, delay);
                trans.set_address(adr_bytes);
            }
            trans.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }

        switch (adr) {
            case TIMER_MEMORY_ADDRESS_HI / 4:
            case TIMER_MEMORY_ADDRESS_LO / 4:
            case TIMERCMP_MEMORY_ADDRESS_HI / 4:
            case TIMERCMP_MEMORY_ADDRESS_LO / 4:
                timer_socket->b_transport(trans, delay);
                break;
            case TRACE_MEMORY_ADDRESS / 4:
                trace_socket->b_transport(trans, delay);
                break;
            default:
                memory_socket->b_transport(trans, delay);
                break;
        }

        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    bool BusCtrl::instr_direct_mem_ptr(tlm::tlm_generic_payload &gp,
                                       tlm::tlm_dmi &dmi_data) {
        return memory_socket->get_direct_mem_ptr(gp, dmi_data);
    }

    void BusCtrl::invalidate_direct_mem_ptr(sc_dt::uint64 start,
                                            sc_dt::uint64 end) {
        cpu_instr_socket->invalidate_direct_mem_ptr(start, end);
    }
}