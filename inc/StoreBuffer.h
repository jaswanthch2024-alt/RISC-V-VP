// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef STOREBUFFER_H
#define STOREBUFFER_H

#include <cstdint>
#include <array>
#include "Scoreboard.h"

namespace riscv_tlm {

struct StoreBufferEntry {
    bool     valid{false};
    uint64_t address{0};
    uint64_t data{0};
    int      size{0};       // bytes: 1, 2, 4, or 8
    int      trans_id{-1};  // scoreboard transaction ID
};

// CVA6-aligned split store buffer — proper FIFO implementation.
//   SPEC_DEPTH   — speculative queue: stores added when address is computed (EX),
//                  before instruction commits.
//   COMMIT_DEPTH — committed queue: stores moved here at commit, drained to
//                  memory one-per-cycle asynchronously.
//
// Both queues use head/tail FIFO pointers so that chronological order is always
// preserved, enabling correct newest-first forwarding regardless of slot reuse.
//
// forward_load search order: spec (newest→oldest) then committed (newest→oldest).
// Spec stores are YOUNGER than committed stores — they executed more recently and
// have not yet been written to memory — so spec takes priority in forwarding.
//
// On branch mispredict: speculative queue is NOT flushed in this pipeline because
// no wrong-path instruction reaches EX before the mispredict is detected.
// flush_speculative() is provided for future MMU exception recovery.
template<std::size_t SPEC_DEPTH = 4, std::size_t COMMIT_DEPTH = 4>
class StoreBuffer {
public:
    StoreBuffer() = default;

    // Push store onto speculative FIFO tail (called in EX when address is computed).
    // Returns the slot index written, or -1 if full.
    int add_store(uint64_t address, uint64_t data, int size, int trans_id) {
        if (spec_count == SPEC_DEPTH) return -1;
        spec[spec_tail] = {true, address, data, size, trans_id};
        int slot = static_cast<int>(spec_tail);
        spec_tail = (spec_tail + 1) % SPEC_DEPTH;
        spec_count++;
        return slot;
    }

    // Pop the oldest spec entry and push it onto the committed FIFO tail.
    // Called at commit for store instructions. In-order pipeline guarantee:
    // spec_head always holds the store currently at the scoreboard head.
    // Returns false if the committed queue is full (caller should stall commit).
    bool commit_store(int /*trans_id*/) {
        if (spec_count == 0)            return false;
        if (commit_count == COMMIT_DEPTH) return false; // committed queue full
        committed[commit_tail] = spec[spec_head];
        committed[commit_tail].valid = true;
        commit_tail = (commit_tail + 1) % COMMIT_DEPTH;
        commit_count++;
        spec[spec_head].valid = false;
        spec_head = (spec_head + 1) % SPEC_DEPTH;
        spec_count--;
        return true;
    }

    // Drain the oldest committed entry to memory (call once per cycle).
    bool drain_one(uint64_t& addr, uint64_t& data, int& size) {
        while (commit_count > 0) {
            addr = committed[commit_head].address;
            data = committed[commit_head].data;
            size = committed[commit_head].size;
            bool was_valid = committed[commit_head].valid;

            committed[commit_head].valid = false;
            commit_head = (commit_head + 1) % COMMIT_DEPTH;
            commit_count--;

            if (was_valid) {
                return true;
            }
        }
        return false;
    }

    // Load-to-store forwarding result.
    enum class FwdResult {
        HIT,            // Store fully contains load; fwd_data is valid
        PARTIAL_OVERLAP,// Store overlaps load but doesn't contain it; load must STALL
        MISS            // No overlapping store; safe to read from memory
    };

    // Load-to-store forwarding using CVA6's 12-bit page-offset alias check.
    //
    // Searches spec newest→oldest first (spec stores are younger than committed),
    // then committed newest→oldest.  Returns the value of the youngest store older
    // than the load that targets the same address and access size.
    //
    // PARTIAL_OVERLAP: when a buffered store overlaps the load but doesn't fully
    // contain it, the load cannot be forwarded (we'd return a mix of store-buffer
    // and memory data).  The load must stall until the overlapping store drains.
    FwdResult forward_load(uint64_t load_addr, int load_size, uint64_t& fwd_data) {
        // Spec queue: newest (tail-1) → oldest (head)
        for (std::size_t k = 0; k < spec_count; k++) {
            std::size_t i = (spec_tail + SPEC_DEPTH - 1 - k) % SPEC_DEPTH;
            if (spec[i].valid) {
                uint64_t store_addr = spec[i].address;
                int store_size = spec[i].size;

                // Check if there is any overlap between load and store ranges
                uint64_t overlap_start = std::max(load_addr, store_addr);
                uint64_t overlap_end = std::min(load_addr + load_size, store_addr + store_size);

                if (overlap_start < overlap_end) {
                    // Overlap exists. Check if load is fully contained within the store
                    if (load_addr >= store_addr && (load_addr + load_size) <= (store_addr + store_size)) {
                        int offset = load_addr - store_addr;
                        uint64_t shifted = spec[i].data >> (offset * 8);
                        uint64_t mask = (load_size == 8) ? ~0ULL : ((1ULL << (load_size * 8)) - 1);
                        fwd_data = shifted & mask;
                        return FwdResult::HIT;
                    }
                    // Younger store overlaps but does not fully contain the load.
                    // Caller must stall until overlapping stores drain to memory.
                    return FwdResult::PARTIAL_OVERLAP;
                }
            }
        }
        // Committed queue: newest (tail-1) → oldest (head)
        for (std::size_t k = 0; k < commit_count; k++) {
            std::size_t i = (commit_tail + COMMIT_DEPTH - 1 - k) % COMMIT_DEPTH;
            if (committed[i].valid) {
                uint64_t store_addr = committed[i].address;
                int store_size = committed[i].size;

                // Check if there is any overlap between load and store ranges
                uint64_t overlap_start = std::max(load_addr, store_addr);
                uint64_t overlap_end = std::min(load_addr + load_size, store_addr + store_size);

                if (overlap_start < overlap_end) {
                    // Overlap exists. Check if load is fully contained within the store
                    if (load_addr >= store_addr && (load_addr + load_size) <= (store_addr + store_size)) {
                        int offset = load_addr - store_addr;
                        uint64_t shifted = committed[i].data >> (offset * 8);
                        uint64_t mask = (load_size == 8) ? ~0ULL : ((1ULL << (load_size * 8)) - 1);
                        fwd_data = shifted & mask;
                        return FwdResult::HIT;
                    }
                    // Younger store overlaps but does not fully contain the load.
                    // Caller must stall until overlapping stores drain to memory.
                    return FwdResult::PARTIAL_OVERLAP;
                }
            }
        }
        return FwdResult::MISS;
    }

    // Invalidate all store buffer entries (spec + committed) whose byte ranges
    // overlap [addr, addr+size).  Must be called whenever an AMO or successful
    // SC.W/SC.D writes directly to physical memory, bypassing this store buffer,
    // so that a subsequent load to the same address reads from memory rather than
    // being served stale pre-AMO data by forward_load().
    void invalidate_address(uint64_t addr, int size) {
        uint64_t end = addr + static_cast<uint64_t>(size);
        for (auto& e : spec) {
            if (e.valid) {
                uint64_t e_end = e.address + static_cast<uint64_t>(e.size);
                if (addr < e_end && end > e.address) e.valid = false;
            }
        }
        for (auto& e : committed) {
            if (e.valid) {
                uint64_t e_end = e.address + static_cast<uint64_t>(e.size);
                if (addr < e_end && end > e.address) e.valid = false;
            }
        }
    }

    // --- Capacity checks ---

    bool spec_is_full()   const { return spec_count   == SPEC_DEPTH;   }
    bool commit_is_full() const { return commit_count  == COMMIT_DEPTH; }
    bool is_full()        const { return spec_is_full(); }
    bool is_empty()       const { return spec_count == 0 && commit_count == 0; }

    // --- Flush helpers ---

    void flush_speculative() {
        for (auto& e : spec) e.valid = false;
        spec_head = spec_tail = spec_count = 0;
    }

    void flush_speculative(int branch_rob_idx, const Scoreboard<32>& scoreboard) {
        while (spec_count > 0) {
            std::size_t prev = (spec_tail + SPEC_DEPTH - 1) % SPEC_DEPTH;
            int tid = spec[prev].trans_id;
            if (tid >= 0 && !scoreboard.get_entry(tid).valid) {
                spec[prev].valid = false;
                spec_tail = prev;
                spec_count--;
            } else {
                break;
            }
        }
    }

    void flush_all() {
        flush_speculative();
        for (auto& e : committed) e.valid = false;
        commit_head = commit_tail = commit_count = 0;
    }

    void flush() { flush_all(); }

private:
    std::array<StoreBufferEntry, SPEC_DEPTH>   spec{};
    std::size_t spec_head{0}, spec_tail{0}, spec_count{0};

    std::array<StoreBufferEntry, COMMIT_DEPTH> committed{};
    std::size_t commit_head{0}, commit_tail{0}, commit_count{0};

    static bool alias_check(uint64_t a, uint64_t b) {
        return (a & 0xFFFULL) == (b & 0xFFFULL);
    }
};

} // namespace riscv_tlm

#endif // STOREBUFFER_H
