// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include <cstdint>
#include <iostream>
#include <cstring>

namespace riscv_tlm { namespace peripherals {
// Minimal CLINT model exposing mtime/mtimecmp (no MSIP implemented yet)
class CLINT : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<CLINT> socket;
    sc_core::sc_out<bool> timer_irq{"timer_irq"};
    sc_core::sc_out<bool> msip_irq{"msip_irq"};

    SC_HAS_PROCESS(CLINT);
    explicit CLINT(sc_core::sc_module_name const &name)
        : sc_module(name), socket("socket"), m_mtime(0), m_mtimecmp(0xFFFFFFFFFFFFFFFFULL) {
        socket.register_b_transport(this, &CLINT::b_transport);
        
        SC_THREAD(tick);
        
        SC_METHOD(update_irq_method);
        sensitive << m_update_event;

        timer_irq.initialize(false);
        msip_irq.initialize(false);
    }

    // Fast-forward mtime to one tick before mtimecmp for WFI optimization.
    // The next tick() iteration (1 ms later) will fire the interrupt.
    // Returns the number of ticks skipped.
    uint64_t fast_forward_to_deadline() {
        if (m_mtimecmp != 0xFFFFFFFFFFFFFFFFULL && m_mtime + 1 < m_mtimecmp) {
            uint64_t skip = m_mtimecmp - 1 - m_mtime;
            m_mtime = m_mtimecmp - 1;
            m_update_event.notify();
            return skip;
        }
        return 0;
    }

    uint64_t get_mtime()    const { return m_mtime; }
    uint64_t get_mtimecmp() const { return m_mtimecmp; }

private:
    sc_core::sc_event m_update_event;

    void update_irq_method() {
        timer_irq.write(m_mtime >= m_mtimecmp);
    }

    void tick() {
        while (true) {
            // Tick at 1ms intervals of simulation time.
            // The Linux kernel programs mtimecmp = mtime + 1000 for a 1ms timer
            // period at a 1MHz CLINT reference clock.  With 1µs ticks (one per
            // CPU cycle) the timer fires every 1000 cycles — far too frequently
            // for the kernel's syscall handlers which take hundreds of thousands
            // of cycles to complete.  At 1ms per tick, one timer interrupt fires
            // per ~1M CPU cycles, giving the kernel adequate time between ticks.
            wait(1000, sc_core::SC_US);
            ++m_mtime;
            m_update_event.notify();
        }
    }

    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
        (void)delay;
        auto cmd = trans.get_command();
        uint64_t addr = trans.get_address();
        unsigned char *ptr = trans.get_data_ptr();
        unsigned len = trans.get_data_length();
        // Offsets (RV privileged spec) — using only 64-bit mtimecmp/mtime
        // 0x4000: mtimecmp (low 32) 0x4004: high 32  (we accept 8B)
        // 0xBFF8: mtime     (low 32) 0xBFFC: high 32
        if (len == 8) {
            if (cmd == tlm::TLM_WRITE_COMMAND) {
                uint64_t value = 0; std::memcpy(&value, ptr, 8);
                if (addr == 0x4000) { m_mtimecmp = value; m_update_event.notify(); }
                else if (addr == 0xBFF8) { m_mtime = value; m_update_event.notify(); }
            } else if (cmd == tlm::TLM_READ_COMMAND) {
                uint64_t value = 0;
                if (addr == 0x4000) value = m_mtimecmp;
                else if (addr == 0xBFF8) value = m_mtime;
                std::memcpy(ptr, &value, 8);
            }
        } else if (len == 4) { // allow 32-bit accesses
            uint32_t value32 = 0;
            if (cmd == tlm::TLM_WRITE_COMMAND) {
                std::memcpy(&value32, ptr, 4);
                if (addr == 0x0000) {
                    m_msip = value32 & 1;
                    msip_irq.write(static_cast<bool>(m_msip & 1));
                }
                else if (addr == 0x4000) { m_mtimecmp = (m_mtimecmp & 0xFFFFFFFF00000000ULL) | value32; m_update_event.notify(); }
                else if (addr == 0x4004) { m_mtimecmp = (m_mtimecmp & 0xFFFFFFFFULL) | (uint64_t(value32) << 32); m_update_event.notify(); }
                else if (addr == 0xBFF8) { m_mtime = (m_mtime & 0xFFFFFFFF00000000ULL) | value32; m_update_event.notify(); }
                else if (addr == 0xBFFC) { m_mtime = (m_mtime & 0xFFFFFFFFULL) | (uint64_t(value32) << 32); m_update_event.notify(); }
            } else if (cmd == tlm::TLM_READ_COMMAND) {
                if (addr == 0x0000) { value32 = m_msip; }
                else if (addr == 0x4000) value32 = uint32_t(m_mtimecmp & 0xFFFFFFFFULL);
                else if (addr == 0x4004) value32 = uint32_t(m_mtimecmp >> 32);
                else if (addr == 0xBFF8) value32 = uint32_t(m_mtime & 0xFFFFFFFFULL);
                else if (addr == 0xBFFC) value32 = uint32_t(m_mtime >> 32);
                std::memcpy(ptr, &value32, 4);
            }
        }

        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    uint64_t m_mtime;
    uint64_t m_mtimecmp;
    uint32_t m_msip{0};
};
}} // namespace
