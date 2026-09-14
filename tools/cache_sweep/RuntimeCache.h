// SPDX-License-Identifier: GPL-3.0-or-later
// RuntimeCache -- a runtime-parameterized duplicate of inc/Cache.h's
// N-way set-associative cache model, for cache-geometry sweep experiments.
//
// Deliberately a SEPARATE class, not a modification of the production
// riscv_tlm::Cache<SETS,WAYS,LINE_BYTES> template used by the actual VP
// pipeline (inc/CPU_P64_6_Cycle.h) -- this keeps the RTL-validated production
// cache completely untouched. SETS/WAYS/LINE_BYTES are constructor
// arguments here instead of template parameters, so many geometries can be
// swept in one compiled binary with no rebuild between configs.
//
// Semantics are a deliberate 1:1 mirror of Cache.h's access()/policy logic
// (see cache_sweep.cpp's self-check, which runs both classes on an
// identical access sequence and asserts bit-identical hit/miss decisions
// at every step -- this is the correctness proof that this duplicate
// faithfully represents the real cache, not just something similar to it).
#pragma once
#include <cstdint>
#include <cassert>
#include <vector>

class RuntimeCache {
public:
    // MRU is exploration-only (not mirrored in the production inc/Cache.h --
    // it isn't a CVA6-relevant option, unlike LRU which exists there as the
    // documented pre-B8 baseline). Motivated directly by the LFSR-vs-LRU
    // finding: LRU is pathological on a scan larger than the cache because it
    // evicts exactly the line about to be reused -- MRU is the textbook fix
    // for that specific case (evict the line LEAST likely to be needed again
    // soon on such a scan), at the cost of being a bad policy for anything
    // with real temporal locality (where MRU evicts exactly the hot data).
    enum class Policy { LFSR, LRU, MRU };

    RuntimeCache(std::size_t sets, std::size_t ways, std::size_t line_bytes)
        : sets_(sets), ways_(ways), line_bytes_(line_bytes),
          offset_bits_(log2c(line_bytes)),
          tag_shift_(log2c(line_bytes) + log2c(sets)),
          lines_(sets, std::vector<Line>(ways)) {
        assert(is_pow2(sets) && is_pow2(ways) && is_pow2(line_bytes) &&
               "RuntimeCache: SETS, WAYS, LINE_BYTES must all be powers of 2");
    }

    void set_policy(Policy p) { policy_ = p; }
    Policy get_policy() const { return policy_; }

    bool access(uint64_t addr) {
        const uint64_t idx = (addr >> offset_bits_) & (sets_ - 1);
        const uint64_t tag = addr >> tag_shift_;
        total_accesses_++;

        auto &s = lines_[idx];
        for (auto &line : s) {
            if (line.valid && line.tag == tag) {
                line.lru_time = total_accesses_;
                total_hits_++;
                return true;
            }
        }

        std::size_t victim = ways_;
        for (std::size_t w = 0; w < ways_; w++) {
            if (!s[w].valid) { victim = w; break; }
        }
        if (victim == ways_) {
            if (policy_ == Policy::LRU) {
                victim = 0;
                for (std::size_t w = 1; w < ways_; w++)
                    if (s[w].lru_time < s[victim].lru_time) victim = w;
            } else if (policy_ == Policy::MRU) {
                // Mirror image of LRU: evict the MOST recently touched way
                // instead of the least. Reuses the same lru_time bookkeeping.
                victim = 0;
                for (std::size_t w = 1; w < ways_; w++)
                    if (s[w].lru_time > s[victim].lru_time) victim = w;
            } else {
                victim = static_cast<std::size_t>(lfsr_) % ways_;
                const uint8_t fb = ((lfsr_ >> 7) ^ (lfsr_ >> 5) ^ (lfsr_ >> 4) ^ (lfsr_ >> 3)) & 1u;
                lfsr_ = static_cast<uint8_t>((lfsr_ << 1) | fb);
            }
        }
        s[victim] = {true, tag, total_accesses_};
        return false;
    }

    void flush() {
        for (auto &s : lines_)
            for (auto &line : s)
                line.valid = false;
    }

    std::size_t sets() const { return sets_; }
    std::size_t ways() const { return ways_; }
    std::size_t line_bytes() const { return line_bytes_; }
    std::size_t size_bytes() const { return sets_ * ways_ * line_bytes_; }

    uint64_t get_accesses() const { return total_accesses_; }
    uint64_t get_hits()     const { return total_hits_; }
    uint64_t get_misses()   const { return total_accesses_ - total_hits_; }
    double   hit_rate()     const {
        return total_accesses_ ? static_cast<double>(total_hits_) / static_cast<double>(total_accesses_) : 0.0;
    }

private:
    struct Line {
        bool     valid{false};
        uint64_t tag{0};
        uint64_t lru_time{0};
    };

    static bool is_pow2(std::size_t n) { return n && ((n & (n - 1)) == 0); }
    static int log2c(std::size_t n) {
        int r = 0;
        while (n > 1) { n >>= 1; r++; }
        return r;
    }

    std::size_t sets_, ways_, line_bytes_;
    int offset_bits_, tag_shift_;
    std::vector<std::vector<Line>> lines_;

    Policy  policy_{Policy::LFSR};
    uint8_t lfsr_{0xACu};
    uint64_t total_accesses_{0};
    uint64_t total_hits_{0};
};
