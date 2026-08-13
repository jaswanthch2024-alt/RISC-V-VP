// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CACHE_H
#define CACHE_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

// N-way set-associative cache with LFSR pseudo-random replacement, matching
// the CVA6 L1 caches (cva6_icache.sv and wt_dcache_missunit.sv): on a miss,
// fill an invalid way if one exists, otherwise evict a way chosen by an 8-bit
// LFSR. NOTE: this deliberately does NOT use LRU. LRU is pathologically bad on
// sequential scans of a working set larger than the cache (it evicts exactly
// the line about to be reused), which made the VP over-miss by ~22% vs CVA6 on
// memory-bound code; CVA6 uses random replacement. See docs/BUGS.md §B8.
// SETS, WAYS and LINE_BYTES must be powers of 2.
// Default: 64 sets × 4 ways × 64B lines = 16 KB (illustrative; real
// instances are Cache<256,4,16> / Cache<256,8,16>).
template<std::size_t SETS = 64, std::size_t WAYS = 4, std::size_t LINE_BYTES = 64>
class Cache {
public:
    Cache() = default;

    // Returns true on hit, false on miss.
    // On miss: fills an invalid way, else a random (LFSR-chosen) way.
    bool access(uint64_t addr) {
        const uint64_t idx = (addr >> OFFSET_BITS) & (SETS - 1);
        const uint64_t tag = addr >> TAG_SHIFT;
        total_accesses++;

        auto& s = sets[idx];

        for (auto& line : s) {
            if (line.valid && line.tag == tag) {
                total_hits++;
                return true;
            }
        }

        // Miss: fill the first invalid way; if all ways are valid, evict a
        // random way and advance the LFSR (mirrors CVA6's update_lfsr =
        // cache_wren & all_ways_valid).
        std::size_t victim = WAYS;
        for (std::size_t w = 0; w < WAYS; w++) {
            if (!s[w].valid) { victim = w; break; }
        }
        if (victim == WAYS) {
            victim = static_cast<std::size_t>(lfsr) % WAYS;   // rnd_way
            // 8-bit Fibonacci LFSR, taps x^8+x^6+x^5+x^4+1 (maximal length).
            const uint8_t fb = ((lfsr >> 7) ^ (lfsr >> 5) ^ (lfsr >> 4) ^ (lfsr >> 3)) & 1u;
            lfsr = static_cast<uint8_t>((lfsr << 1) | fb);
        }
        s[victim] = {true, tag};
        return false;
    }

    void flush() {
        for (auto& s : sets)
            for (auto& line : s)
                line.valid = false;
    }

    uint64_t get_accesses() const { return total_accesses; }
    uint64_t get_hits()     const { return total_hits; }
    uint64_t get_misses()   const { return total_accesses - total_hits; }
    double   hit_rate()     const {
        return total_accesses
            ? static_cast<double>(total_hits) / static_cast<double>(total_accesses)
            : 0.0;
    }

private:
    struct Line {
        bool     valid{false};
        uint64_t tag{0};
    };

    // Single 8-bit LFSR shared across all sets, as in CVA6 (one lfsr instance
    // per cache). Nonzero seed (an LFSR must never be all-zero).
    uint8_t lfsr{0xACu};

    static constexpr int log2c(std::size_t n) {
        return (n <= 1) ? 0 : 1 + log2c(n >> 1);
    }
    static constexpr int OFFSET_BITS = log2c(LINE_BYTES);
    static constexpr int TAG_SHIFT   = log2c(LINE_BYTES) + log2c(SETS);

    std::array<std::array<Line, WAYS>, SETS> sets{};
    uint64_t total_accesses{0};
    uint64_t total_hits{0};
};

} // namespace riscv_tlm

#endif // CACHE_H
