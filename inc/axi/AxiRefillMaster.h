// SPDX-License-Identifier: GPL-3.0-or-later
// AxiRefillMaster — a matchlib AXI4 read/write manager driven by a race-free
// sc_signal handshake from the Cycle6 pipeline. One instance per contending
// source (I$, D$, later store-drain). Used ONLY by the CYCLE6_AXI build.
//
// Handshake (all sc_signal, so ordering within a delta is irrelevant):
//   pipeline -> master : req_valid (pulse/hold), req_addr
//   master -> pipeline : resp_valid (held until pipeline drops req_valid), resp_data
//
// Reads issue a real AXI INCR burst, not a single beat: CVA6's own AXI adapter
// (wt_axi_adapter.sv) computes AxiRdBlenIcache/Dcache = LINE_WIDTH/AxiDataWidth-1
// = 128/64-1 = 1, i.e. a 2-beat burst per 16 B line at the real 64-bit AXI
// width -- verified directly from the RTL formula. BURST_BEATS below mirrors
// that. The slave (MinimalAxiMemSlave) needs no change: its next_multi_read()
// loop already auto-increments the address per INCR beat.
//
// The burst call blocks this module's own SC_THREAD while it traverses the
// arbiter + slave; the pipeline thread meanwhile just polls resp_valid. The
// sc_signal handshake latency each way is the "wrapper overhead" absorbed by
// the slave-latency calibration (N_slave = target - overhead).
#pragma once

#include "axi4_segment.h"

namespace riscv_axi {

class AxiRefillMaster : public sc_module,
                        public axi::axi4_segment<axi::cfg::standard> {
public:
  sc_in<bool>          clk;
  sc_in<bool>          rst_bar;

  // matchlib AXI manager ports (bound to the arbiter in AxiContentionTop).
  r_master<AUTO_PORT>  r_master0;
  w_master<AUTO_PORT>  w_master0;

  // Pipeline-facing handshake (driven/read by CPURV64P6_Cycle).
  sc_in<bool>          req_valid;
  sc_in<uint32_t>      req_addr;
  sc_in<bool>          req_is_write; // false=read (I$/D$), true=write (store-drain)
  sc_in<uint32_t>      req_wdata;
  sc_out<bool>         resp_valid;
  sc_out<uint32_t>     resp_data;

  // 16 B line / 8 B (64-bit) beats = 2 beats -- matches CVA6's
  // AxiRdBlenIcache/Dcache = ICACHE_LINE_WIDTH/AxiDataWidth - 1 = 1 (=2 beats).
  static constexpr int BURST_BEATS = 2;

  SC_HAS_PROCESS(AxiRefillMaster);
  AxiRefillMaster(sc_module_name nm)
      : sc_module(nm), clk("clk"), rst_bar("rst_bar"),
        r_master0("r_master0"), w_master0("w_master0"),
        req_valid("req_valid"), req_addr("req_addr"),
        req_is_write("req_is_write"), req_wdata("req_wdata"),
        resp_valid("resp_valid"), resp_data("resp_data") {
    SC_THREAD(master_process);
    sensitive << clk.pos();
    async_reset_signal_is(rst_bar, false);
  }

  // Real AXI INCR burst read: one AR (len = BURST_BEATS-1), then BURST_BEATS
  // R beats. The library's r_master only exposes single_read() (len=0), so
  // this pushes/pops the raw ar/r ports directly, the same way r_master's own
  // helpers do internally.
  r_payload burst_read(uint32_t addr) {
    ar_payload ar_item;
    ar_item.addr = addr;
    ar_item.len = BURST_BEATS - 1;
    ar_item.burst = Enc::AXBURST::INCR;
    r_master0.ar.Push(ar_item);

    r_payload r;
    for (int beat = 0; beat < BURST_BEATS; beat++)
      r = r_master0.r.Pop(); // block for each beat; last beat completes the miss
    return r;
  }

  void master_process() {
    r_master0.reset();
    w_master0.reset();
    resp_valid.write(false);
    resp_data.write(0);
    wait();

    while (1) {
      // Wait for a request from the pipeline.
      while (!req_valid.read()) wait();

      uint32_t addr = req_addr.read();
      if (req_is_write.read()) {
        w_master0.single_write(addr, req_wdata.read()); // blocks through arbiter+slave
        resp_data.write(0);
      } else {
        r_payload r = burst_read(addr); // 2-beat INCR burst through arbiter+slave
        resp_data.write(r.data.to_uint64());
      }

      // Signal completion and hold until the pipeline acknowledges by
      // dropping req_valid (prevents re-triggering the same miss).
      resp_valid.write(true);
      while (req_valid.read()) wait();
      resp_valid.write(false);
    }
  }
};

} // namespace riscv_axi
