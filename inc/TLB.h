// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef TLB_H
#define TLB_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

// sv39 TLB entry.
// Supports 4 KB (level=0), 2 MB (level=1), and 1 GB (level=2) pages.
struct TLBEntry {
    bool     valid{false};
    bool     global{false};  // PTE.G — not evicted by ASID-specific SFENCE.VMA
    uint8_t  perm{0};        // PTE bits [4:1]: U(4) X(3) W(2) R(1) — shifted down by 1
    uint8_t  level{0};       // PTW depth at which leaf was found (0=4KB, 1=2MB, 2=1GB)
    uint16_t asid{0};
    uint64_t vpn{0};         // VA[38:12] — full 27-bit VPN stored; matching uses level mask
    uint64_t ppn{0};         // Physical page number from PTE[53:10]
    uint64_t lru_time{0};
};

// Fully-associative TLB with LRU replacement.
// Used for both ITLB and DTLB.
template<std::size_t SIZE = 32>
class TLB {
public:
    TLB() = default;

    // Translate vaddr. Returns true on hit and fills ppn/perm/level.
    bool lookup(uint64_t vaddr, uint16_t asid,
                uint64_t& ppn_out, uint8_t& perm_out, uint8_t& level_out) {
        const uint64_t vpn = vaddr >> 12;
        lru_clock++;
        for (auto& e : entries) {
            if (!e.valid) continue;
            if (!e.global && e.asid != asid) continue;
            if (vpn_match(vpn, e.vpn, e.level)) {
                ppn_out   = e.ppn;
                perm_out  = e.perm;
                level_out = e.level;
                e.lru_time = lru_clock;
                return true;
            }
        }
        return false;
    }

    // Fill a TLB entry from a completed PTW.
    void insert(uint64_t vaddr, uint64_t ppn, uint16_t asid,
                uint8_t perm, uint8_t level, bool global) {
        std::size_t victim = 0;
        for (std::size_t i = 1; i < SIZE; i++)
            if (!entries[i].valid || entries[i].lru_time < entries[victim].lru_time)
                victim = i;
        entries[victim] = {true, global, perm, level, asid, vaddr >> 12, ppn, lru_clock};
    }

    // SFENCE.VMA rs1=0, rs2=0 — flush all entries including global.
    // Per RISC-V spec: rs1=0 rs2=0 means "flush entire TLB" — global entries are NOT exempt.
    // Only ASID-specific flushes (rs2!=0) preserve global entries.
    void flush_all() {
        for (auto& e : entries) e.valid = false;
    }

    // SFENCE.VMA rs1!=0, rs2=0 — flush all entries matching vaddr (any ASID).
    void flush_va(uint64_t vaddr) {
        const uint64_t vpn = vaddr >> 12;
        for (auto& e : entries)
            if (e.valid && vpn_match(vpn, e.vpn, e.level)) e.valid = false;
    }

    // SFENCE.VMA rs1=0, rs2!=0 — flush all entries matching ASID.
    void flush_asid(uint16_t asid) {
        for (auto& e : entries)
            if (e.valid && !e.global && e.asid == asid) e.valid = false;
    }

    // SFENCE.VMA rs1!=0, rs2!=0 — flush entry matching both vaddr and ASID.
    void flush_va_asid(uint64_t vaddr, uint16_t asid) {
        const uint64_t vpn = vaddr >> 12;
        for (auto& e : entries)
            if (e.valid && !e.global && e.asid == asid && vpn_match(vpn, e.vpn, e.level))
                e.valid = false;
    }

    // Hard flush (e.g. on SATP write or mode change).
    void invalidate_all() {
        for (auto& e : entries) e.valid = false;
    }

private:
    std::array<TLBEntry, SIZE> entries{};
    uint64_t lru_clock{0};

    // Returns true if vpn matches stored_vpn at the given superpage level.
    // Level 0: full 27-bit VPN match (4 KB pages)
    // Level 1: match upper 18 bits — VPN[2:1] (2 MB superpages, ignore VPN[0])
    // Level 2: match upper  9 bits — VPN[2]   (1 GB superpages, ignore VPN[1:0])
    static bool vpn_match(uint64_t vpn, uint64_t stored_vpn, uint8_t level) {
        const int shift = static_cast<int>(level) * 9; // 0, 9, or 18
        return (vpn >> shift) == (stored_vpn >> shift);
    }
};

} // namespace riscv_tlm
#endif // TLB_H
