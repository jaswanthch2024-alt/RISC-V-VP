// SPDX-License-Identifier: GPL-3.0-or-later
// axi_external_demo -- proves the VP's AXI master logic can drive a REAL
// external AXI port boundary correctly, at a configurable width (128-bit by
// default here), with a real D$ cache gating which accesses actually go out
// over AXI.
//
// This is a STANDALONE top-level, separate from the shipped RISCV_VP
// executable. It does not touch VPTop/CPU_P64_6_Cycle/AxiContentionTop --
// those remain exactly as verified elsewhere this session. What this proves:
//   1. AxiRefillMaster (the exact class the VP's internal arbiter uses) can
//      be wired to an EXTERNAL module (MinimalAxiMemSlave, standing in for
//      real memory/RTL) via a genuine matchlib channel/port binding, not a
//      function call -- the "is there a port" question, answered directly.
//   2. A real riscv_tlm::Cache<256,8,16> (the production D$ geometry,
//      unmodified) gates traffic: only actual misses generate AXI
//      transactions, exactly matching CPU_P64_6_Cycle.cpp's real D$ miss
//      path -- not a raw synthetic replay of every address.
//   3. Data integrity survives the real port round-trip -- reusing the
//      Track C mechanism (req_wdata/resp_data), the driver supplies a known
//      test value per miss and confirms it comes back unchanged.
//
// What this does NOT prove: conformance against a real third-party AXI
// verification IP or actual RTL (never tested), or that the shipped
// RISCV_VP executable itself has an external port (it doesn't -- this is a
// separate harness demonstrating the underlying plumbing works).
//
// Addresses replayed are REAL (captured via VP_DCACHE_TRACE from an actual
// VP run -- see tools/cache_sweep/dcache_trace.txt). The expected data value
// per access is driver-supplied (deterministic, derived from the address),
// not literally the VP's real fetched data -- this standalone harness has no
// mem_intf to source that from. It is still a fully valid test of port-level
// data integrity on the AXI path itself.
#define SC_INCLUDE_DYNAMIC_PROCESSES
#include <systemc.h>

#include "Cache.h"
#include "axi/AxiConfigSelect.h"
#include "axi/AxiRefillMaster.h"
#include "axi/MinimalAxiMemSlave.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

static std::vector<uint64_t> load_trace(const char *path) {
  std::vector<uint64_t> trace;
  std::FILE *f = std::fopen(path, "r");
  if (!f) {
    std::fprintf(stderr, "Could not open trace file: %s\n", path);
    std::exit(1);
  }
  char line[64];
  while (std::fgets(line, sizeof(line), f))
    trace.push_back(std::strtoull(line, nullptr, 16));
  std::fclose(f);
  return trace;
}

// A deterministic per-address test value -- see file header: this harness
// has no real mem_intf, so the "CPU's real value" for each access is
// synthesized here instead of literally replayed from the VP.
static uint64_t expected_value(uint64_t addr) { return addr ^ 0x5A5A5A5A5A5A5A5AULL; }

SC_MODULE(Driver) {
  sc_in<bool>       clk;
  sc_out<bool>      req_valid;
  sc_out<uint32_t>  req_addr;
  sc_out<bool>      req_is_write;
  sc_out<uint64_t>  req_wdata;
  sc_in<bool>       resp_valid;
  sc_in<uint64_t>   resp_data;

  riscv_tlm::Cache<256, 8, 16> dcache; // production D$ geometry, unmodified
  std::vector<uint64_t> trace;
  uint64_t hits{0}, misses{0}, mismatches{0};

  void run() {
    req_valid.write(false);
    wait();
    for (uint64_t addr : trace) {
      if (dcache.access(addr)) { hits++; continue; }
      misses++;
      uint64_t expect = expected_value(addr);
      req_addr.write(static_cast<uint32_t>(addr));
      req_is_write.write(false);
      req_wdata.write(expect);
      req_valid.write(true);
      do { wait(); } while (!resp_valid.read());
      uint64_t got = resp_data.read();
      if (got != expect) {
        mismatches++;
        std::fprintf(stderr,
            "MISMATCH at addr %llx: expected %llx, got %llx over the real external AXI port\n",
            (unsigned long long)addr, (unsigned long long)expect, (unsigned long long)got);
      }
      req_valid.write(false);
      wait();
    }
    std::printf("\n=== axi_external_demo summary (AXI data width: %d bits) ===\n",
                (int)riscv_axi::AxiCfg::dataWidth);
    std::printf("D$ accesses: %llu (hits resolved locally: %llu, misses sent over real external AXI port: %llu)\n",
                (unsigned long long)(hits + misses), (unsigned long long)hits, (unsigned long long)misses);
    std::printf("Data mismatches on the external round-trip: %llu %s\n",
                (unsigned long long)mismatches, mismatches == 0 ? "-- ALL CORRECT" : "-- FAILURE");
    sc_stop();
  }

  SC_CTOR(Driver) {
    trace = load_trace("dcache_trace.txt");
    std::printf("Loaded real trace: %zu addresses (from tools/cache_sweep/dcache_trace.txt, "
                "captured via VP_DCACHE_TRACE on the actual VP)\n", trace.size());
    SC_THREAD(run);
    sensitive << clk.pos();
  }
};

SC_MODULE(ResetGen) {
  sc_in<bool>  clk;
  sc_out<bool> rst_bar;
  void run() {
    rst_bar.write(false);
    for (int i = 0; i < 10; i++) wait();
    rst_bar.write(true);
  }
  SC_CTOR(ResetGen) {
    SC_THREAD(run);
    sensitive << clk.pos();
  }
};

typedef axi::axi4<riscv_axi::AxiCfg> local_axi;

// Everything below (channels included) must live inside one enclosing
// sc_module -- matchlib's Connections::Combinational channels (what
// local_axi::read::chan<>/write::chan<> resolve to) require an active
// module context to construct, the same reason AxiContentionTop.cpp wraps
// its own arbiter/masters/slave/channels inside a private Impl : sc_module
// instead of constructing them as loose sc_main locals.
SC_MODULE(DemoTop) {
  sc_in<bool> clk{"clk"};

  sc_signal<bool>     rst_bar{"rst_bar"};
  sc_signal<bool>     req_valid{"req_valid"};
  sc_signal<uint32_t> req_addr{"req_addr"};
  sc_signal<bool>     req_is_write{"req_is_write"};
  sc_signal<uint64_t> req_wdata{"req_wdata"};
  sc_signal<bool>     resp_valid{"resp_valid"};
  sc_signal<uint64_t> resp_data{"resp_data"};

  riscv_axi::AxiRefillMaster    master;
  riscv_axi::MinimalAxiMemSlave slave;
  local_axi::read::chan<>       rch{"rch"};
  local_axi::write::chan<>      wch{"wch"};
  Driver                        driver;
  ResetGen                      reset_gen;

  SC_CTOR(DemoTop)
      : master("axi_master", /*burst_beats=*/1), // 16B line / 128-bit beat = 1
        slave("external_slave", /*latency=*/8),
        driver("driver"), reset_gen("reset_gen") {
    master.clk(clk);
    master.rst_bar(rst_bar);
    master.req_valid(req_valid);
    master.req_addr(req_addr);
    master.req_is_write(req_is_write);
    master.req_wdata(req_wdata);
    master.resp_valid(resp_valid);
    master.resp_data(resp_data);

    slave.clk(clk);
    slave.rst_bar(rst_bar);
    master.r_master0(rch);
    slave.r_slave0(rch);
    master.w_master0(wch);
    slave.w_slave0(wch);

    driver.clk(clk);
    driver.req_valid(req_valid);
    driver.req_addr(req_addr);
    driver.req_is_write(req_is_write);
    driver.req_wdata(req_wdata);
    driver.resp_valid(resp_valid);
    driver.resp_data(resp_data);

    reset_gen.clk(clk);
    reset_gen.rst_bar(rst_bar);
  }
};

int sc_main(int argc, char *argv[]) {
  sc_clock clk("clk", sc_time(10, SC_NS));
  DemoTop top("top");
  top.clk(clk);
  sc_start();
  return 0;
}
