// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include <cstdint>
#include <iostream>
#include <cstring>

namespace riscv_tlm { namespace peripherals {
// Minimal Syscall interface: capture writes to specific offsets and optionally print
class SyscallIf : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<SyscallIf> socket;

    SC_HAS_PROCESS(SyscallIf);
    explicit SyscallIf(sc_core::sc_module_name const &name) : sc_module(name), socket("socket") {
        socket.register_b_transport(this, &SyscallIf::b_transport);
    }

private:
    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
        (void)delay;
        auto cmd = trans.get_command();
        uint64_t addr = trans.get_address();
        unsigned char *ptr = trans.get_data_ptr();
        unsigned len = trans.get_data_length();
        if (cmd == tlm::TLM_WRITE_COMMAND && len == 4) {
            uint32_t val = 0; std::memcpy(&val, ptr, 4);
            // Simple semantics: offset 0 = syscall number, 4 = arg, 8 = write char
            switch (addr) {
                case 0x0: last_syscall = val; break;
                case 0x4: last_arg = val; break;
                case 0x8: std::cout << static_cast<char>(val & 0xFF) << std::flush; break;
                default: break;
            }
        } else if (cmd == tlm::TLM_READ_COMMAND && len == 4) {
            uint32_t val = 0;
            switch (addr) {
                case 0x0: val = last_syscall; break;
                case 0x4: val = last_arg; break;
                case 0xC: val = 0; break; // could return a status code
                default: break;
            }
            std::memcpy(ptr, &val, 4);
        }
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    uint32_t last_syscall = 0;
    uint32_t last_arg = 0;
};
}} // namespace
