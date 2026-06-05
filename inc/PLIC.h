// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include <cstdint>
#include <array>
#include <iostream>
#include <cstring>
#include <unordered_set>

namespace riscv_tlm { namespace peripherals {
// Minimal PLIC: fixed number of interrupt sources, priority and pending registers
class PLIC : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<PLIC> socket;
    sc_core::sc_out<bool> irq_out{"irq_out"};

    static constexpr size_t MAX_SOURCES = 32; // simple

    SC_HAS_PROCESS(PLIC);
    explicit PLIC(sc_core::sc_module_name const &name) : sc_module(name), socket("socket") {
        socket.register_b_transport(this, &PLIC::b_transport);
        priorities.fill(0);
        pending_bits = 0;
        enabled_bits = 0;
        threshold = 0;
        claim_complete = 0;
        irq_out.initialize(false);
    }

    // Raise an interrupt source (called by peripheral models)
    void raise(uint32_t id) {
        if (id > 0 && id < MAX_SOURCES) {
            pending_bits |= (1u << id);
            update_irq_out();
        }
    }

    // Level-triggered interrupt: keeps source pending until de-asserted.
    // When active=true the source is marked pending; when active=false it is cleared.
    // On COMPLETE the pending bit is re-raised if the source is still active.
    void set_irq_level(uint32_t id, bool active) {
        if (id == 0 || id >= MAX_SOURCES) return;
        if (active) {
            level_bits   |= (1u << id);
            pending_bits |= (1u << id);
        } else {
            level_bits   &= ~(1u << id);
            pending_bits &= ~(1u << id);
        }
        update_irq_out();
    }

private:
    // Register map (offsets chosen similar to spec subset)
    // 0x0000 .. priorities (4 bytes each)
    // 0x1000 pending bits (4 bytes)
    // 0x2000 enable bits (hart 0) (4 bytes)
    // 0x200000 threshold (4 bytes)
    // 0x200004 claim/complete (4 bytes)
    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
        (void)delay;
        auto cmd = trans.get_command();
        uint64_t addr = trans.get_address();
        unsigned char *ptr = trans.get_data_ptr();
        unsigned len = trans.get_data_length();
        if (len != 4) { trans.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE); return; }
        uint32_t data = 0;
        if (cmd == tlm::TLM_WRITE_COMMAND) std::memcpy(&data, ptr, 4);
        if (addr < 0x1000) { // priorities
            size_t idx = addr / 4;
            if (idx < MAX_SOURCES) {
                if (cmd == tlm::TLM_WRITE_COMMAND) priorities[idx] = data & 0x7; // 3-bit priority
                else data = priorities[idx];
            }
        } else if (addr == 0x1000) { // pending (read only)
            if (cmd == tlm::TLM_READ_COMMAND) data = pending_bits;
        } else if (addr == 0x2000) { // enable bits
            if (cmd == tlm::TLM_WRITE_COMMAND) enabled_bits = data;
            else data = enabled_bits;
        } else if (addr == 0x200000) { // threshold
            if (cmd == tlm::TLM_WRITE_COMMAND) threshold = data & 0x7;
            else data = threshold;
        } else if (addr == 0x200004) { // claim / complete
            if (cmd == tlm::TLM_READ_COMMAND) {
                // return highest-priority pending enabled source > threshold
                uint32_t best = 0; uint32_t best_prio = 0;
                for (uint32_t i = 1; i < MAX_SOURCES; ++i) {
                    if ((pending_bits & (1u << i)) && (enabled_bits & (1u << i))) {
                        uint32_t p = priorities[i];
                        if (p > best_prio && p > threshold) { best_prio = p; best = i; }
                    }
                }
                data = best;
                claim_complete = best;
                if (best) pending_bits &= ~(1u << best); // auto clear on claim
                update_irq_out();
            } else { // write = complete
                // writing the source id signals completion
                pending_bits &= ~(1u << data);
                claim_complete = 0;
                // Re-raise if level source still asserted
                if (data > 0 && data < MAX_SOURCES && (level_bits & (1u << data)))
                    pending_bits |= (1u << data);
                update_irq_out();
            }
        }
        if (cmd == tlm::TLM_READ_COMMAND) std::memcpy(ptr, &data, 4);
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    // Drive irq_out high if any enabled pending source has priority > threshold
    void update_irq_out() {
        for (uint32_t i = 1; i < MAX_SOURCES; ++i) {
            if ((pending_bits & (1u << i)) && (enabled_bits & (1u << i)) && priorities[i] > threshold) {
                irq_out.write(true);
                return;
            }
        }
        irq_out.write(false);
    }

    std::array<uint32_t, MAX_SOURCES> priorities;
    uint32_t pending_bits;
    uint32_t enabled_bits;
    uint32_t threshold;
    uint32_t claim_complete;
    uint32_t level_bits{0}; // bitmask of level-triggered sources currently asserted
};
}} // namespace
