// SPDX-License-Identifier: GPL-3.0-or-later
// AxiConfigWide -- wider-than-64-bit AXI data-width configs (128/256/512/1024),
// for side-by-side "what if AxiDataWidth were wider" experiments ONLY. CVA6
// RTL always uses 64-bit AXI (CVA6ConfigAxiDataWidth=64, verified in every
// shipped cva6_config_pkg.sv); no CVA6 target has ever validated any of these
// widths. Do not use results built against these configs as an RTL-accuracy
// claim -- see TIMING_MODEL=CYCLE6_AXI128/256/512/1024 in CMakeLists.txt and
// inc/axi/AxiConfigSelect.h for how one is selected per build.
//
// Field-for-field identical to matchlib's axi::cfg::standard (see
// third_party/matchlib_kit/matchlib-main/cmod/include/axi/axi4_configs.h)
// except dataWidth. Defined here, not by editing the vendored kit, per the
// project's public-source-only vendoring policy.
//
// Beats-per-16B-line at each width (see CPU_P64_6_Cycle.cpp's
// `16 / (AxiCfg::dataWidth/8)` derivation, floored at 1): 128-bit -> 1 beat
// (exact); 256/512/1024-bit -> also 1 beat (a single beat is already wider
// than the whole cache line at these widths -- the floor-at-1 clamp in that
// formula is what makes this correct instead of computing 0).
#pragma once
#ifndef AXI_CONFIG_WIDE_H
#define AXI_CONFIG_WIDE_H

namespace axi {
namespace cfg {

struct wide128 {
  enum {
    dataWidth = 128,
    useVariableBeatSize = 0,
    useMisalignedAddresses = 0,
    useLast = 1,
    useWriteStrobes = 1,
    useBurst = 1, useFixedBurst = 0, useWrapBurst = 0, maxBurstSize = 256,
    useQoS = 0, useLock = 0, useProt = 0, useCache = 0, useRegion = 0,
    aUserWidth = 0, wUserWidth = 0, bUserWidth = 0, rUserWidth = 0,
    addrWidth = 32,
    idWidth = 4,
    useWriteResponses = 1,
  };
};

struct wide256 {
  enum {
    dataWidth = 256,
    useVariableBeatSize = 0,
    useMisalignedAddresses = 0,
    useLast = 1,
    useWriteStrobes = 1,
    useBurst = 1, useFixedBurst = 0, useWrapBurst = 0, maxBurstSize = 256,
    useQoS = 0, useLock = 0, useProt = 0, useCache = 0, useRegion = 0,
    aUserWidth = 0, wUserWidth = 0, bUserWidth = 0, rUserWidth = 0,
    addrWidth = 32,
    idWidth = 4,
    useWriteResponses = 1,
  };
};

struct wide512 {
  enum {
    dataWidth = 512,
    useVariableBeatSize = 0,
    useMisalignedAddresses = 0,
    useLast = 1,
    useWriteStrobes = 1,
    useBurst = 1, useFixedBurst = 0, useWrapBurst = 0, maxBurstSize = 256,
    useQoS = 0, useLock = 0, useProt = 0, useCache = 0, useRegion = 0,
    aUserWidth = 0, wUserWidth = 0, bUserWidth = 0, rUserWidth = 0,
    addrWidth = 32,
    idWidth = 4,
    useWriteResponses = 1,
  };
};

struct wide1024 {
  enum {
    dataWidth = 1024,
    useVariableBeatSize = 0,
    useMisalignedAddresses = 0,
    useLast = 1,
    useWriteStrobes = 1,
    useBurst = 1, useFixedBurst = 0, useWrapBurst = 0, maxBurstSize = 256,
    useQoS = 0, useLock = 0, useProt = 0, useCache = 0, useRegion = 0,
    aUserWidth = 0, wUserWidth = 0, bUserWidth = 0, rUserWidth = 0,
    addrWidth = 32,
    idWidth = 4,
    useWriteResponses = 1,
  };
};

} // namespace cfg
} // namespace axi

#endif // AXI_CONFIG_WIDE_H
