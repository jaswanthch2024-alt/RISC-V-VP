// SPDX-License-Identifier: GPL-3.0-or-later
// AxiContentionTop — thin, matchlib-free interface to the AXI I$/D$ contention
// subsystem. The CPU pipeline includes ONLY this header (no matchlib templates
// in CPU_P64_6_Cycle's translation unit); the heavy matchlib instantiation
// lives entirely in AxiContentionTop.cpp behind a PIMPL.
//
// Used only by the CYCLE6_AXI build (ENABLE_AXI_CONTENTION).
#pragma once
#include <cstdint>

namespace sc_core { class sc_clock; }

namespace riscv_axi {

class AxiContentionTop {
public:
  enum Master { ICACHE = 0, DCACHE = 1, STORE = 2, NUM_MASTERS = 3 };

  // Builds the arbiter + per-master AXI managers + memory slave during
  // elaboration, all clocked by `clk`. `slave_latency` is the N-cycle memory
  // response used for calibration (uncontended cost ~= slave_latency + wrapper).
  // `burst_beats` is the INCR burst length used for every refill read
  // (default 2, matching CVA6's real 64-bit AXI width); pass a different
  // value only for side-by-side "wider AxiDataWidth" experiments -- see
  // AxiRefillMaster.h.
  AxiContentionTop(sc_core::sc_clock* clk, int slave_latency, int burst_beats = 2);
  ~AxiContentionTop();

  // Post a memory request for master m (idempotent while held). Call once at a
  // miss trigger, then poll done(m). `wdata` is the real value: for a write,
  // the value being stored; for a read, the value the CPU already fetched via
  // its normal (mem_intf/fetch_instruction) path -- this is handed back
  // verbatim by data() once done(), so the AXI channel carries the CPU's
  // actual data instead of a synthetic placeholder from the internal slave's
  // toy backing array. Pass 0 if not yet known (see set_pending_wdata()).
  void request(int m, uint64_t addr, bool is_write = false, uint64_t wdata = 0);
  // Updates the real value for an already-posted, still-in-flight request
  // (for the rare case -- e.g. a DTLB miss stacked with a D$ miss -- where
  // the real value isn't known yet at request() time). Safe to call any time
  // before done() first returns true.
  void set_pending_wdata(int m, uint64_t wdata);
  // True once the request for m has completed through arbiter + slave.
  bool done(int m) const;
  // Returned read data (valid when done(m)): the real value passed to
  // request()/set_pending_wdata(), handed back through the AXI channel.
  uint64_t data(int m) const;
  // Acknowledge completion: drops the request so the master can serve the next.
  void ack(int m);
  // True while a request for m is in flight (posted, not yet acked).
  bool busy(int m) const;

private:
  struct Impl;
  Impl* impl_;
};

} // namespace riscv_axi
