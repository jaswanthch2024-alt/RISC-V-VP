// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef MMU_H
#define MMU_H

#include <cstdint>
#include <cstdio>
#include "TLB.h"
#include "CSR_File.h"
#include "MemoryInterface.h"

namespace riscv_tlm {

// sv39 Memory Management Unit — TLBs + Page Table Walker.
//
// Design (CVA6-aligned):
//   ITLB: 32-entry fully associative (instruction-side)
//   DTLB: 32-entry fully associative (data-side)
//   PTW:  3-level sv39 walk using physical memory via MemoryInterface.
//         Each PTE read is modelled at ptw_mem_cycles cost (default 2 cycles,
//         representing a warm PTW cache).
//
// Access types (access_t):
//   FETCH = 0   — instruction fetch (uses ITLB)
//   LOAD  = 1   — data read  (uses DTLB)
//   STORE = 2   — data write (uses DTLB)
class MMU {
public:
    enum access_t : uint8_t { FETCH = 0, LOAD = 1, STORE = 2 };

    struct Result {
        bool     ok{false};         // Translation succeeded
        bool     fault{false};      // Page fault raised
        uint64_t fault_cause{0};    // CAUSE code on fault
        uint64_t paddr{0};          // Physical address (valid when ok=true)
        int      stall_cycles{0};   // Extra cycles added by TLB miss / PTW
    };

    // ptw_mem_cycles: cost per PTE memory read (models PTW-private cache).
    explicit MMU(MemoryInterface* m, int ptw_mem_cycles = 2)
        : mem(m), ptw_cycles_per_level(ptw_mem_cycles) {}

    // -------------------------------------------------------------------------
    // Main translation entry point.
    // When satp.MODE == 0 (bare) or priv == M: returns vaddr as paddr directly.
    // -------------------------------------------------------------------------
    Result translate(uint64_t vaddr, const CSR_File& csr, access_t atype) {
        const uint64_t satp = csr.satp;
        const PrivMode priv = csr.priv;

        // Bare-metal or M-mode with MPRV=0: no translation.
        bool vm_active = ((satp >> 60) == 8); // MODE field == 8 → sv39
        if (!vm_active || priv == PrivMode::M) {
            return {true, false, 0, vaddr, 0};
        }

        const uint16_t asid = static_cast<uint16_t>((satp >> 44) & 0xFFFF);

        // Check appropriate TLB.
        TLB<32>& tlb = (atype == FETCH) ? itlb : dtlb;
        uint64_t ppn = 0; uint8_t perm = 0; uint8_t level = 0;

        if (tlb.lookup(vaddr, asid, ppn, perm, level)) {
            // TLB hit.
            if (atype == FETCH) stats.itlb_hits++; else stats.dtlb_hits++;
            Result r;
            r.ok    = true;
            r.paddr = make_paddr(ppn, vaddr, level);
            r.fault = !check_perm(perm, level, atype, priv, csr.mstatus);
            if (r.fault) {
                r.fault_cause = fault_cause(atype);
            }
            return r;
        }

        // TLB miss → PTW.
        if (atype == FETCH) stats.itlb_misses++; else stats.dtlb_misses++;
        stats.ptw_walks++;

        const uint64_t root_ppn = satp & ((1ULL << 44) - 1);
        uint64_t leaf_ppn = 0; uint8_t leaf_perm = 0; uint8_t leaf_level = 0;
        bool global_bit = false;
        int  walk_cycles = 0;
        uint64_t fault_c = 0;

        if (!ptw(root_ppn, vaddr, atype, priv, csr.mstatus,
                 leaf_ppn, leaf_perm, leaf_level, global_bit, walk_cycles, fault_c)) {
            return {false, true, fault_c, 0, walk_cycles};
        }

        // Fill TLB with new translation.
        tlb.insert(vaddr, leaf_ppn, asid, leaf_perm, leaf_level, global_bit);

        Result r;
        r.ok           = true;
        r.paddr        = make_paddr(leaf_ppn, vaddr, leaf_level);
        r.stall_cycles = walk_cycles;
        return r;
    }

    // -------------------------------------------------------------------------
    // SFENCE.VMA — Phase 9.
    // rs1=0 means flush all VA; rs2=0 means flush all ASIDs.
    // -------------------------------------------------------------------------
    void sfence_vma(uint64_t rs1_val, uint64_t rs2_val, bool rs1_zero, bool rs2_zero) {
        const uint16_t asid = static_cast<uint16_t>(rs2_val & 0xFFFF);
        sfence_call_count++;
        if (rs1_zero && rs2_zero) {
            itlb.flush_all();
            dtlb.flush_all();
        } else if (rs1_zero) {
            itlb.flush_asid(asid);
            dtlb.flush_asid(asid);
        } else if (rs2_zero) {
            itlb.flush_va(rs1_val);
            dtlb.flush_va(rs1_val);
        } else {
            itlb.flush_va_asid(rs1_val, asid);
            dtlb.flush_va_asid(rs1_val, asid);
        }
    }

    // Hard flush both TLBs (e.g. on satp write).
    void flush_all() {
        itlb.invalidate_all();
        dtlb.invalidate_all();
    }

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------
    struct Stats {
        uint64_t itlb_hits{0};
        uint64_t itlb_misses{0};
        uint64_t dtlb_hits{0};
        uint64_t dtlb_misses{0};
        uint64_t ptw_walks{0};
    } stats;

private:
    TLB<32>         itlb;
    TLB<32>         dtlb;
    MemoryInterface* mem;
    int             ptw_cycles_per_level;
    int             sfence_call_count{0};

    // 3-level sv39 page table walk.
    // Returns true on success, false on fault.
    bool ptw(uint64_t root_ppn, uint64_t vaddr, access_t atype,
             PrivMode priv, uint64_t mstatus,
             uint64_t& ppn_out, uint8_t& perm_out, uint8_t& level_out,
             bool& global_out, int& cycles_out, uint64_t& fault_cause_out) {

        // sv39 VPN fields
        const uint64_t vpn[3] = {
            (vaddr >> 12) & 0x1FF,  // VPN[0]
            (vaddr >> 21) & 0x1FF,  // VPN[1]
            (vaddr >> 30) & 0x1FF   // VPN[2]
        };

        uint64_t table_ppn = root_ppn;
        cycles_out = 0;

        for (int level = 2; level >= 0; level--) {
            uint64_t pte_addr = (table_ppn << 12) | (vpn[level] << 3);
            uint64_t pte      = mem->readDataMem64(pte_addr, 8);
            cycles_out += ptw_cycles_per_level;

            // PTE.V must be set
            if (!(pte & 0x1)) {
                fault_cause_out = fault_cause(atype);
                return false;
            }

            const bool pte_r = (pte >> 1) & 1;
            const bool pte_x = (pte >> 3) & 1;

            if (!pte_r && !pte_x) {
                // Non-leaf (pointer PTE): descend one level
                table_ppn = (pte >> 10) & ((1ULL << 44) - 1);
                continue;
            }

            // Leaf PTE found
            // Reserved bits check: superpage PTEs must have lower PPN fields = 0
            if (level > 0) {
                uint64_t ppn_low_mask = (level == 2) ? 0x3FFFFULL : 0x1FFULL;
                if (((pte >> 10) & ppn_low_mask) != 0) {
                    fault_cause_out = fault_cause(atype);
                    return false;
                }
            }

            // Pack perm: bits [4:1] of PTE → stored as bits [3:0] in perm byte
            uint8_t perm = static_cast<uint8_t>((pte >> 1) & 0xF); // R,W,X,U

            // Permission check at PTW time
            if (!check_perm(perm, static_cast<uint8_t>(level), atype, priv, mstatus)) {
                fault_cause_out = fault_cause(atype);
                return false;
            }

            // Update A/D bits (simplified: always set; real HW may take a fault instead)
            uint64_t new_pte = pte | (1ULL << 6); // set A
            if (atype == STORE) new_pte |= (1ULL << 7); // set D
            if (new_pte != pte) mem->writeDataMem64(pte_addr, new_pte, 8);

            ppn_out    = (pte >> 10) & ((1ULL << 44) - 1);
            perm_out   = perm;
            level_out  = static_cast<uint8_t>(level);
            global_out = (pte >> 5) & 1;
            return true;
        }

        // All levels traversed without finding a leaf
        fault_cause_out = fault_cause(atype);
        return false;
    }

    // Build physical address from PPN + virtual page offset (level-aware for superpages).
    static uint64_t make_paddr(uint64_t ppn, uint64_t vaddr, uint8_t level) {
        // For 4KB pages: PA = (ppn << 12) | VA[11:0]
        // For 2MB pages: PA = (ppn[43:9] << 21) | VA[20:0]
        // For 1GB pages: PA = (ppn[43:18] << 30) | VA[29:0]
        const int offset_bits = 12 + static_cast<int>(level) * 9;
        const uint64_t offset_mask = (1ULL << offset_bits) - 1;
        return ((ppn >> (static_cast<int>(level) * 9)) << offset_bits) | (vaddr & offset_mask);
    }

    // Check PTE permissions for the requested access type and privilege.
    // perm = PTE[4:1] shifted: bit0=R, bit1=W, bit2=X, bit3=U
    static bool check_perm(uint8_t perm, uint8_t /*level*/,
                           access_t atype, PrivMode priv, uint64_t mstatus) {
        const bool pte_r = (perm >> 0) & 1;
        const bool pte_w = (perm >> 1) & 1;
        const bool pte_x = (perm >> 2) & 1;
        const bool pte_u = (perm >> 3) & 1;
        const bool sum   = (mstatus >> 18) & 1; // mstatus.SUM
        const bool mxr   = (mstatus >> 19) & 1; // mstatus.MXR

        // User / supervisor page access checks
        if (priv == PrivMode::U && !pte_u) return false;
        if (priv == PrivMode::S && pte_u) {
            if (atype == FETCH || !sum) return false;
        }

        switch (atype) {
            case FETCH: return pte_x;
            case LOAD:  return pte_r || (mxr && pte_x);
            case STORE: return pte_w;
        }
        return false;
    }

    // Map access type to RISC-V page fault cause code.
    static uint64_t fault_cause(access_t atype) {
        switch (atype) {
            case FETCH: return CAUSE::INSTR_PAGE_FAULT;
            case LOAD:  return CAUSE::LOAD_PAGE_FAULT;
            case STORE: return CAUSE::STORE_PAGE_FAULT;
        }
        return CAUSE::LOAD_PAGE_FAULT;
    }
};

} // namespace riscv_tlm
#endif // MMU_H
