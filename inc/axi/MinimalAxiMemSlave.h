// SPDX-License-Identifier: GPL-3.0-or-later
// MinimalAxiMemSlave — a small array-backed matchlib AXI4 subordinate with a
// configurable N-cycle read/write response latency. Used ONLY by the
// CYCLE6_AXI build (ENABLE_AXI_CONTENTION) as the downstream memory behind the
// matchlib AxiArbiter. It exists to model *timing* (arbitration + refill
// latency); the pipeline's functional data still comes from its normal path.
//
// Modelled on the kit's ram.h, with an added `latency_` wait so a single
// (uncontended) read costs a controllable number of cycles — this is the
// calibration knob (N_slave) referenced in the plan.
#pragma once

#include "axi4_segment.h"

namespace riscv_axi {

class MinimalAxiMemSlave : public sc_module,
                           public axi::axi4_segment<axi::cfg::standard> {
public:
  sc_in<bool>          clk;
  sc_in<bool>          rst_bar;
  r_slave<AUTO_PORT>   r_slave0;
  w_slave<AUTO_PORT>   w_slave0;

  // Per-transaction response latency in clocks (the calibration knob).
  int latency_{8};

  static const int SZ = 0x10000; // dataWidth-words backing store
  typedef NVUINTW(axi_cfg::dataWidth) arr_t;
  arr_t* array{nullptr};

  SC_HAS_PROCESS(MinimalAxiMemSlave);
  MinimalAxiMemSlave(sc_module_name nm, int latency = 8)
      : sc_module(nm), clk("clk"), rst_bar("rst_bar"),
        r_slave0("r_slave0"), w_slave0("w_slave0"), latency_(latency) {
    array = new arr_t[SZ];
    for (int i = 0; i < SZ; i++) array[i] = arr_t(i * bytesPerBeat);

    SC_THREAD(slave_r_process);
    sensitive << clk.pos();
    async_reset_signal_is(rst_bar, false);

    SC_THREAD(slave_w_process);
    sensitive << clk.pos();
    async_reset_signal_is(rst_bar, false);
  }
  ~MinimalAxiMemSlave() override { delete[] array; }

  void slave_r_process() {
    r_slave0.reset();
    wait();
    while (1) {
      ar_payload ar;
      r_slave0.start_multi_read(ar);
      // Model the memory/refill latency (uncontended cost). Arbitration delay
      // is added on top by the arbiter itself when another master holds it.
      for (int i = 0; i < latency_; i++) wait();
      while (1) {
        r_payload r;
        if (ar.addr >= (SZ * bytesPerBeat)) {
          r.resp = Enc::XRESP::SLVERR;
        } else {
          r.data = array[ar.addr / bytesPerBeat];
        }
        if (!r_slave0.next_multi_read(ar, r)) break;
      }
    }
  }

  void slave_w_process() {
    w_slave0.reset();
    wait();
    while (1) {
      aw_payload aw;
      b_payload b;
      w_slave0.start_multi_write(aw, b);
      for (int i = 0; i < latency_; i++) wait();
      while (1) {
        w_payload w = w_slave0.w.Pop();
        if (aw.addr < (SZ * bytesPerBeat)) {
          array[aw.addr / bytesPerBeat] = w.data.to_uint64();
        } else {
          b.resp = Enc::XRESP::SLVERR;
        }
        if (!w_slave0.next_multi_write(aw)) break;
      }
      w_slave0.b.Push(b);
    }
  }
};

} // namespace riscv_axi
