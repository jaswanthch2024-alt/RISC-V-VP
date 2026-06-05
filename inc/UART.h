// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include <cstdint>
#include <functional>
#include <iostream>

namespace riscv_tlm { namespace peripherals {

// Minimal 16550-compatible UART with DLAB support.
// Handles both 1-byte stride (reg-io-width=1) and 4-byte stride (reg-io-width=4).
//
// 4-byte stride offsets (QEMU virt style, reg-shift=2):
//   0x00  RBR/THR/DLL  0x04 IER/DLM  0x08 IIR/FCR  0x0C LCR
//   0x10  MCR          0x14 LSR       0x18 MSR       0x1C SCR
//
// 1-byte stride offsets:  0x0 RBR/THR/DLL, 0x1 IER/DLM, ... 0x7 SCR

class UART : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<UART> socket;

    // Wire this callback to signal the PLIC when THRI changes state.
    // Called with true when THRI bit is set in IER, false when cleared.
    std::function<void(bool)> set_uart_irq;

    SC_HAS_PROCESS(UART);
    explicit UART(sc_core::sc_module_name const& name)
        : sc_module(name), socket("socket"), scr(0), dlab(false), m_ier(0), m_thre_ip(true) {
        socket.register_b_transport(this, &UART::b_transport);
    }

private:
    uint8_t scr;
    bool    dlab;   // Divisor Latch Access Bit (LCR bit 7)
    uint8_t m_ier;  // IER shadow register
    bool    m_thre_ip; // Transmitter Holding Register Empty Interrupt Pending

    void update_interrupts() {
        bool active = (m_ier & 0x02) && m_thre_ip;
        if (set_uart_irq) set_uart_irq(active);
    }

    // Map raw byte offset → 16550 register index 0-7.
    // 4-byte stride: offsets 0,4,8,...,28 → regs 0-7.
    // 1-byte stride: offsets 0-7 → regs 0-7.
    static unsigned reg_from_offset(uint64_t off) {
        // 4-byte stride (reg-shift=2, QEMU virt): check first for aligned offsets
        if ((off & 3) == 0 && off <= 0x1C)
            return static_cast<unsigned>(off >> 2);         // 4-byte stride
        // 1-byte stride fallback: odd offsets 1,2,3,5,6,7
        if (off <= 7) return static_cast<unsigned>(off);
        return 0xFF; // unmapped
    }

    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
        (void)delay;
        unsigned char* ptr = trans.get_data_ptr();
        unsigned int   len = trans.get_data_length();
        unsigned       reg = reg_from_offset(trans.get_address());

        if (trans.get_command() == tlm::TLM_WRITE_COMMAND && len > 0) {
            uint8_t val = ptr[0];
            switch (reg) {
                case 0: // THR (DLAB=0) or DLL (DLAB=1)
                    if (!dlab) {
                        std::cout << static_cast<char>(val) << std::flush;
                        m_thre_ip = false; // writing THR clears THRE interrupt
                        m_thre_ip = true;  // immediately becomes empty again
                        update_interrupts();
                    }
                    // DLL write silently ignored (affects baud rate, not needed for sim)
                    break;
                case 1: // IER (DLAB=0) or DLM (DLAB=1)
                    if (!dlab) {
                        bool old_thri = (m_ier & 0x02) != 0;
                        bool new_thri = (val & 0x02) != 0;
                        m_ier = val;
                        if (new_thri && !old_thri) {
                            m_thre_ip = true; // transition to enabled triggers interrupt
                        } else if (!new_thri) {
                            m_thre_ip = false;
                        }
                        update_interrupts();
                    }
                    break;
                case 2: // FCR (write-only) — ignore
                    break;
                case 3: // LCR — update DLAB
                    dlab = (val >> 7) & 1;
                    break;
                case 7: // SCR — scratch register
                    scr = val;
                    break;
                default:
                    break;
            }
        } else if (trans.get_command() == tlm::TLM_READ_COMMAND && len > 0) {
            uint32_t val = 0;
            switch (reg) {
                case 0: val = 0;    break; // RBR: no RX data
                case 1: val = m_ier; break; // IER
                case 2: // IIR: THRI=0x02 (active-low pending is 0, so bit 0 is 0. bits 3:1 = 0b001. So val = 0x02)
                    if ((m_ier & 0x02) && m_thre_ip) {
                        val = 0x02;        // THRE interrupt pending
                        m_thre_ip = false; // reading IIR clears THRE interrupt
                        update_interrupts();
                    } else {
                        val = 0x01;        // no interrupt pending (bit 0 is 1)
                    }
                    break;
                case 3: val = 0;    break; // LCR
                case 4: val = 0;    break; // MCR
                case 5: val = 0x60; break; // LSR: THRE(5)=1, TEMT(6)=1 — TX always ready
                case 6: val = 0;    break; // MSR
                case 7: val = scr;  break; // SCR
                default: val = 0;   break;
            }
            for (unsigned i = 0; i < len && i < 4; ++i)
                ptr[i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFF);
        }

        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

}} // namespace
