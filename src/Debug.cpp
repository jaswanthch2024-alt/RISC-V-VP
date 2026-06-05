/*!
 \file Debug.cpp
 \brief GDB connector (stub for pipelined CPUs)
 \author Màrius Montón
 \date February 2021
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <iostream>
#include "Debug.h"

namespace riscv_tlm {
    constexpr char nibble_to_hex[16] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};

    Debug::Debug(riscv_tlm::CPU *cpu, Memory *mem, cpu_types_t type) 
        : sc_module(sc_core::sc_module_name("Debug")) {
        dbg_cpu = cpu;
        register_bank32 = nullptr;
        register_bank64 = nullptr;
        dbg_mem = mem;
        cpu_type = type;
        conn = -1;
        std::cout << "[Debug] GDB remote stub not fully supported for pipelined CPUs." << std::endl;
    }

    Debug::~Debug() = default;

    void Debug::send_packet(int, const std::string &) { /* no-op */ }
    std::string Debug::receive_packet() { return ""; }
    void Debug::handle_gdb_loop() { /* no-op */ }

    std::string Debug::compute_checksum_string(const std::string &msg) {
        unsigned sum = 0;
        for(char c: msg) sum += static_cast<unsigned char>(c);
        sum &= 0xFFu;
        char low = nibble_to_hex[sum & 0xF];
        char high = nibble_to_hex[(sum >> 4) & 0xF];
        return {high, low};
    }
}