// SPDX-License-Identifier: GPL-3.0-or-later
// AxiConfigSelect -- picks the AXI config type the whole CYCLE6_AXI subsystem
// builds against: axi::cfg::standard (64-bit, RTL-accurate default) unless
// AXI_WIDE128 is defined at compile time (set by CMake for
// TIMING_MODEL=CYCLE6_AXI128), in which case axi::cfg::wide128 (128-bit,
// comparison-only -- see AxiConfigWide.h) is used instead.
//
// This is a compile-time build-variant selection, NOT a runtime toggle like
// VP_CACHE_POLICY/VP_AXI_BEATS/VP_DCACHE_SLOTS: every matchlib AXI type
// (r_master<Cfg>, AxiArbiter<Cfg,...>, payload types) is a C++ template on
// this config, so dataWidth is baked into the type at compile time.
#pragma once
#ifndef AXI_CONFIG_SELECT_H
#define AXI_CONFIG_SELECT_H

#include <axi/axi4_configs.h>

#ifdef AXI_WIDE128
#include "axi/AxiConfigWide.h"
#endif

namespace riscv_axi {

#ifdef AXI_WIDE128
using AxiCfg = axi::cfg::wide128;
#else
using AxiCfg = axi::cfg::standard;
#endif

} // namespace riscv_axi

#endif // AXI_CONFIG_SELECT_H
