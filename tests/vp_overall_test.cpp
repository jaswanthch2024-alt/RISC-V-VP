// SPDX-License-Identifier: GPL-3.0-or-later
// Simple integration test for VPTop: instantiate VP, run for limited instructions.
#include "systemc"
#include "VPTop.h"
#include "Performance.h"
#include <iostream>

#ifndef TEST_HEX_PATH
#error TEST_HEX_PATH not defined (expected path to a test .hex file)
#endif

int sc_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    sc_core::sc_set_time_resolution(1, sc_core::SC_NS);

    const char* hex = TEST_HEX_PATH;
    vp::VPTop top("vp_top", hex, riscv_tlm::RV32, false);

    Performance* perf = Performance::getInstance();

    const sc_core::sc_time quantum(1, sc_core::SC_MS);
    const uint64_t instr_limit = 50000; // safety cap

    while (perf->getInstructions() < instr_limit && sc_core::sc_get_status() != sc_core::SC_STOPPED) {
        sc_core::sc_start(quantum);
    }

    uint64_t executed = perf->getInstructions();
    std::cout << "[vp_overall_test] Executed " << executed << " instructions\n";
    // Basic assertion: executed should be > 0
    return executed > 0 ? 0 : 1;
}
