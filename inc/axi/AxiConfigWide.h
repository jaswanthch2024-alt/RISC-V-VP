// SPDX-License-Identifier: GPL-3.0-or-later
// AxiConfigWide -- a 128-bit-data-width AXI config, for side-by-side "what if
// AxiDataWidth were wider" experiments ONLY. CVA6 RTL always uses 64-bit AXI
// (CVA6ConfigAxiDataWidth=64, verified in every shipped cva6_config_pkg.sv);
// no CVA6 target has ever validated a 128-bit bus. Do not use results built
// against this config as an RTL-accuracy claim -- see TIMING_MODEL=CYCLE6_AXI128
// in CMakeLists.txt and inc/axi/AxiConfigSelect.h for how this is selected.
//
// Field-for-field identical to matchlib's axi::cfg::standard (see
// third_party/matchlib_kit/matchlib-main/cmod/include/axi/axi4_configs.h)
// except dataWidth. Defined here, not by editing the vendored kit, per the
// project's public-source-only vendoring policy.
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

} // namespace cfg
} // namespace axi

#endif // AXI_CONFIG_WIDE_H
