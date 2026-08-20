// SPDX-License-Identifier: GPL-3.0-or-later
// AxiContentionTop implementation — the only translation unit that pulls in the
// heavy matchlib templates. Builds a real matchlib AxiArbiter with NUM_MASTERS
// AXI managers (I$, D$, store-drain) contending for one downstream memory
// slave. Behind the PIMPL in AxiContentionTop.h so CPU_P64_6_Cycle never sees
// matchlib. CYCLE6_AXI build only.
#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "axi/AxiContentionTop.h"

#include <systemc.h>
#include <nvhls_assert.h>
#include <axi/AxiArbiter.h>
#include "axi/AxiRefillMaster.h"
#include "axi/MinimalAxiMemSlave.h"

namespace riscv_axi {

typedef axi::axi4<axi::cfg::standard> local_axi;
static const int NM = AxiContentionTop::NUM_MASTERS;

struct AxiContentionTop::Impl : public sc_module {
  sc_core::sc_clock* clk_;
  sc_signal<bool>     rst_bar;

  // Pipeline handshake, one set per master (driven from the CPU thread).
  sc_signal<bool>     req_valid[NM];
  sc_signal<uint32_t> req_addr[NM];
  sc_signal<bool>     req_is_write[NM];
  sc_signal<uint32_t> req_wdata[NM];
  sc_signal<bool>     resp_valid[NM];
  sc_signal<uint32_t> resp_data[NM];
  bool                busy_[NM];

  AxiRefillMaster*     master[NM];
  MinimalAxiMemSlave*  slave;
  AxiArbiter<axi::cfg::standard, NM, 4>* arbiter;

  local_axi::read::chan<>*  rch[NM];
  local_axi::write::chan<>* wch[NM];
  local_axi::read::chan<>   rs;   // arbiter <-> slave (read)
  local_axi::write::chan<>  ws;   // arbiter <-> slave (write)

  SC_HAS_PROCESS(Impl);
  Impl(sc_module_name nm, sc_core::sc_clock* clk, int slave_latency, int burst_beats)
      : sc_module(nm), clk_(clk), rst_bar("rst_bar"), rs("rs"), ws("ws") {

    arbiter = new AxiArbiter<axi::cfg::standard, NM, 4>("arbiter");
    arbiter->clk(*clk_);
    arbiter->reset_bar(rst_bar);

    slave = new MinimalAxiMemSlave("slave", slave_latency);
    slave->clk(*clk_);
    slave->rst_bar(rst_bar);
    arbiter->axi_rd_s(rs);
    slave->r_slave0(rs);
    arbiter->axi_wr_s(ws);
    slave->w_slave0(ws);

    for (int i = 0; i < NM; i++) {
      busy_[i] = false;
      req_valid[i].write(false);

      rch[i] = new local_axi::read::chan<>(sc_gen_unique_name("rch"));
      wch[i] = new local_axi::write::chan<>(sc_gen_unique_name("wch"));
      master[i] = new AxiRefillMaster(sc_gen_unique_name("axi_master"), burst_beats);
      master[i]->clk(*clk_);
      master[i]->rst_bar(rst_bar);

      // master binds whole channel; arbiter binds the split sub-channels
      master[i]->r_master0(*rch[i]);
      arbiter->axi_rd_m_ar[i]((*rch[i]).ar);
      arbiter->axi_rd_m_r[i]((*rch[i]).r);
      master[i]->w_master0(*wch[i]);
      arbiter->axi_wr_m_aw[i]((*wch[i]).aw);
      arbiter->axi_wr_m_w[i]((*wch[i]).w);
      arbiter->axi_wr_m_b[i]((*wch[i]).b);

      master[i]->req_valid(req_valid[i]);
      master[i]->req_addr(req_addr[i]);
      master[i]->req_is_write(req_is_write[i]);
      master[i]->req_wdata(req_wdata[i]);
      master[i]->resp_valid(resp_valid[i]);
      master[i]->resp_data(resp_data[i]);
    }

    SC_THREAD(reset_gen);
  }

  ~Impl() override {
    for (int i = 0; i < NM; i++) { delete master[i]; delete rch[i]; delete wch[i]; }
    delete slave;
    delete arbiter;
  }

  void reset_gen() {
    rst_bar.write(false);
    for (int i = 0; i < 10; i++) wait(clk_->posedge_event());
    rst_bar.write(true);
    while (true) wait(clk_->posedge_event());
  }
};

// ---- PIMPL forwarding -------------------------------------------------------

AxiContentionTop::AxiContentionTop(sc_core::sc_clock* clk, int slave_latency, int burst_beats)
    : impl_(new Impl("axi_contention", clk, slave_latency, burst_beats)) {}

AxiContentionTop::~AxiContentionTop() { /* impl_ owned by SystemC hierarchy */ }

void AxiContentionTop::request(int m, uint64_t addr, bool is_write, uint32_t wdata) {
  impl_->req_addr[m].write(static_cast<uint32_t>(addr));
  impl_->req_is_write[m].write(is_write);
  impl_->req_wdata[m].write(wdata);
  impl_->req_valid[m].write(true);
  impl_->busy_[m] = true;
}

bool AxiContentionTop::done(int m) const { return impl_->resp_valid[m].read(); }

uint32_t AxiContentionTop::data(int m) const { return impl_->resp_data[m].read(); }

void AxiContentionTop::ack(int m) {
  impl_->req_valid[m].write(false);
  impl_->busy_[m] = false;
}

bool AxiContentionTop::busy(int m) const { return impl_->busy_[m]; }

} // namespace riscv_axi
