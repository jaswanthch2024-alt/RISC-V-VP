// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CACHE_H
#define CACHE_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

// N-way set-associative cache with a SELECTABLE replacement policy.
//
// Default is LFSR pseudo-random, matching the CVA6 L1 caches (cva6_icache.sv
// and wt_dcache_missunit.sv): on a miss, fill an invalid way if one exists,
// otherwise evict a way chosen by an 8-bit LFSR. This is the policy real CVA6
// silicon implements, and is what every RTL-accuracy comparison in this
// project uses — see docs/BUGS.md §B8 (LRU was pathologically bad on
// sequential scans larger than the cache: it evicts exactly the line about to
// be reused, which made the VP over-miss by ~22% vs CVA6 on memory-bound
// code).
//
// LRU is also implemented and selectable via set_policy(), but ONLY for
// side-by-side policy comparison on your own workloads -- it is NOT a
// second CVA6-accurate mode (real CVA6 never uses LRU). Do not use LRU
// results as an RTL-accuracy claim.
//
// IMPORTANT when comparing LRU vs LFSR: the benchmarks built during the §B8
// investigation (large sequential scans of a working set > cache size) are
// LRU's textbook WORST case and LFSR's best case -- testing only with those
// is circular, since that access pattern is exactly why LFSR was adopted.
// A fair comparison needs workloads with genuine temporal locality (a hot
// working set reused many times, ideally close to or under cache capacity),
// where LRU's "recently used = likely reused soon" assumption actually holds.
//
// SETS, WAYS and LINE_BYTES must be powers of 2.
// Default: 64 sets × 4 ways × 64B lines = 16 KB (illustrative; real
// instances are Cache<256,4,16> / Cache<256,8,16>).
template<std::size_t SETS = 64, std::size_t WAYS = 4, std::size_t LINE_BYTES = 64>
class Cache {
public:
    enum class Policy { LFSR, LRU };

    Cache() = default;

    // Selects the eviction policy. Default (LFSR) matches CVA6 RTL; LRU is
    // provided for comparison only. Safe to call before or between runs
    // (does not itself touch cache state -- call flush() too if you want a
    // clean start after switching mid-run).
    void set_policy(Policy p) { policy_ = p; }
    Policy get_policy() const { return policy_; }

    // Returns true on hit, false on miss.
    // On miss: fills an invalid way, else evicts per the active policy.
    bool access(uint64_t addr) {
        const uint64_t idx = (addr >> OFFSET_BITS) & (SETS - 1);
        const uint64_t tag = addr >> TAG_SHIFT;
        total_accesses++;

        auto& s = sets[idx];

        for (auto& line : s) {
            if (line.valid && line.tag == tag) {
                line.lru_time = total_accesses; // only consulted in LRU mode
                total_hits++;
                return true;
            }
        }

        // Miss: fill the first invalid way; if all ways are valid, evict
        // per policy.
        std::size_t victim = WAYS;
        for (std::size_t w = 0; w < WAYS; w++) {
            if (!s[w].valid) { victim = w; break; }
        }
        if (victim == WAYS) {
            if (policy_ == Policy::LRU) {
                victim = 0;
                for (std::size_t w = 1; w < WAYS; w++)
                    if (s[w].lru_time < s[victim].lru_time) victim = w;
            } else {
                // LFSR (mirrors CVA6's update_lfsr = cache_wren & all_ways_valid).
                victim = static_cast<std::size_t>(lfsr) % WAYS;   // rnd_way
                // 8-bit Fibonacci LFSR, taps x^8+x^6+x^5+x^4+1 (maximal length).
                const uint8_t fb = ((lfsr >> 7) ^ (lfsr >> 5) ^ (lfsr >> 4) ^ (lfsr >> 3)) & 1u;
                lfsr = static_cast<uint8_t>((lfsr << 1) | fb);
            }
        }
        s[victim] = {true, tag, total_accesses};
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
        uint64_t lru_time{0}; // only maintained/consulted in LRU mode
    };

    Policy policy_{Policy::LFSR}; // default matches CVA6 RTL

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
