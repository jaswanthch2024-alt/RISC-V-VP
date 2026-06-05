// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/simple_initiator_socket.h"
#include <cstdint>
#include <iostream>
#include <vector>
#include <atomic>
#include <cstring>

namespace riscv_tlm { namespace peripherals {
// Minimal memory-to-memory DMA: registers for src, dst, length, control (start)
class DMA : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<DMA> socket;        // register interface
    tlm_utils::simple_initiator_socket<DMA> mem_master; // to access system memory via Bus (user must bind)
    bool debug_ = false;
    static std::atomic<bool> in_flight_;
    void set_debug(bool d) { debug_ = d; }
    static bool is_in_flight() { return in_flight_.load(); }

    SC_HAS_PROCESS(DMA);
    explicit DMA(sc_core::sc_module_name const &name) : sc_module(name), socket("socket"), mem_master("mem_master"),
        debug_(false), src(0), dst(0), len(0), control(0) {
        socket.register_b_transport(this, &DMA::b_transport);
    }

private:
    void start_transfer() {
        if (len == 0) return;
        if (mem_master.size() == 0) {
            SC_REPORT_ERROR("DMA", "mem_master socket not bound");
            return;
        }
        if (debug_) std::cout << "[DMA] Starting transfer src=" << src << " dst=" << dst << " len=" << len << std::endl;
        in_flight_.store(true);
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        std::vector<unsigned char> buffer(len);
        // READ
        trans.set_address(src);
        trans.set_command(tlm::TLM_READ_COMMAND);
        trans.set_data_ptr(buffer.data());
        trans.set_data_length(len);
        trans.set_streaming_width(len);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        mem_master->b_transport(trans, delay);
        if (trans.get_response_status() != tlm::TLM_OK_RESPONSE) {
            if (debug_) std::cout << "[DMA] Read error" << std::endl;
            in_flight_.store(false);
            return;
        }
        // WRITE
        trans.set_address(dst);
        trans.set_command(tlm::TLM_WRITE_COMMAND);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        mem_master->b_transport(trans, delay);
        if (debug_) std::cout << "[DMA] Transfer complete" << std::endl;
        control &= ~1u; // clear start bit
        in_flight_.store(false);
    }

    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
        (void)delay;
        auto cmd = trans.get_command();
        uint64_t addr = trans.get_address();
        unsigned char *ptr = trans.get_data_ptr();
        unsigned len_bytes = trans.get_data_length();
        if (len_bytes != 4) { trans.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE); return; }
        uint32_t val = 0;
        if (cmd == tlm::TLM_WRITE_COMMAND) std::memcpy(&val, ptr, 4);
        switch (addr) {
            case 0x00: if (cmd == tlm::TLM_WRITE_COMMAND) src = val; else val = src; break;
            case 0x04: if (cmd == tlm::TLM_WRITE_COMMAND) dst = val; else val = dst; break;
            case 0x08: if (cmd == tlm::TLM_WRITE_COMMAND) this->len = val; else val = this->len; break;
            case 0x0C:
                if (cmd == tlm::TLM_WRITE_COMMAND) {
                    control = val;
                    if (control & 1u) start_transfer();
                } else { val = control; }
                break;
            default: break; // ignore unknown
        }
        if (cmd == tlm::TLM_READ_COMMAND) std::memcpy(ptr, &val, 4);
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    uint32_t src, dst, len, control;
};

// Definition of static in_flight_ flag
inline std::atomic<bool> riscv_tlm::peripherals::DMA::in_flight_{false};
}} // namespace
