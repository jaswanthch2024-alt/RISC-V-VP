// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file TimingModel.h
 * @brief Timing model selection for Virtual Prototype components
 * 
 * This header defines the timing model types available for simulation:
 * - LT (Loosely-Timed): Fast simulation, approximate timing
 * - AT (Approximately-Timed): Slower simulation, accurate transaction phases
 * - CYCLE: Cycle-accurate simulation, most accurate but slowest
 */
#pragma once
#ifndef TIMING_MODEL_H
#define TIMING_MODEL_H

#include "systemc"

namespace riscv_tlm {

/**
 * @brief Timing model enumeration
 */
enum class TimingModelType {
    LT,     ///< Loosely-Timed: b_transport, fast simulation
    AT,     ///< Approximately-Timed: nb_transport, phase-accurate
    CYCLE,   ///< Cycle-Accurate: clock-driven, RTL-correlatable
    CYCLE6  ///< Cycle-Accurate 6-Stage Pipeline
};

/**
 * @brief Get timing model name as string
 */
inline const char* timing_model_name(TimingModelType model) {
    switch (model) {
        case TimingModelType::LT:    return "LT (Loosely-Timed)";
        case TimingModelType::AT:    return "AT (Approximately-Timed)";
        case TimingModelType::CYCLE: return "CYCLE (Cycle-Accurate)";
        case TimingModelType::CYCLE6: return "CYCLE6 (6-Stage Cycle-Accurate)";
        default: return "Unknown";
    }
}

/**
 * @brief Components that support timing model selection
 */
enum class ComponentType {
    CPU,
    BUS,
    MEMORY,
    TIMER,
    UART,
    DMA
};

/**
 * @brief Timing characteristics for AT model
 */
struct ATTimingConfig {
    sc_core::sc_time request_delay{1, sc_core::SC_NS};   ///< BEGIN_REQ to END_REQ
    sc_core::sc_time response_delay{5, sc_core::SC_NS};  ///< END_REQ to BEGIN_RESP
    sc_core::sc_time accept_delay{1, sc_core::SC_NS};    ///< BEGIN_RESP to END_RESP
};

/**
 * @brief Timing characteristics for Cycle model
 */
struct CycleTimingConfig {
    uint32_t fetch_cycles{1};
    uint32_t load_cycles{2};
    uint32_t store_cycles{1};
    uint32_t mul_cycles{3};
    uint32_t div_cycles{32};
    uint32_t branch_penalty{1};
};

} // namespace riscv_tlm

#endif // TIMING_MODEL_H
