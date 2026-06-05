// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CACHE_H
#define CACHE_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

// N-way set-associative cache with LRU replacement.
// SETS and LINE_BYTES must be powers of 2.
// Default: 64 sets × 4 ways × 64B lines = 16 KB (matches CVA6 L1 I$).
template<std::size_t SETS = 64, std::size_t WAYS = 4, std::size_t LINE_BYTES = 64>
class Cache {
public:
    Cache() = default;

    // Returns true on hit, false on miss.
    // On miss: fills the LRU way so the next access to the same line is a hit.
    bool access(uint64_t addr) {
        const uint64_t idx = (addr >> OFFSET_BITS) & (SETS - 1);
        const uint64_t tag = addr >> TAG_SHIFT;
        total_accesses++;

        auto& s = sets[idx];

        for (auto& line : s) {
            if (line.valid && line.tag == tag) {
                line.lru_time = total_accesses;
                total_hits++;
                return true;
            }
        }

        // Miss: replace LRU (lowest lru_time, or first invalid way).
        std::size_t victim = 0;
        for (std::size_t w = 1; w < WAYS; w++) {
            if (!s[w].valid || s[w].lru_time < s[victim].lru_time)
                victim = w;
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
        uint64_t lru_time{0};
    };

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
