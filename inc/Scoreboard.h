// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef SCOREBOARD_H
#define SCOREBOARD_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

// Functional unit classification — matches CVA6 fu_t encoding
enum class FunctionalUnit : uint8_t {
    NONE   = 0,
    ALU    = 1,   // 1-cycle arithmetic/logic
    LSU    = 2,   // Load/store unit (variable latency)
    BRANCH = 3,   // Branch/jump unit (1-cycle)
    CSR    = 4,   // CSR unit (executes at commit)
    MUL    = 5,   // Multiplier (2-cycle, Phase 3)
    DIV    = 6,   // Divider (up to 64-cycle, Phase 3)
    FPU    = 7    // Floating-point unit (variable latency)
};

// CVA6-aligned scoreboard — acts as both issue FIFO and reorder buffer.
// Results are stored inside entries (confirmed by CVA6 scoreboard.sv).
// rd_clobber tracks which FU is producing each destination register.
template<std::size_t SIZE = 32>
class Scoreboard {
public:
    struct Entry {
        bool valid{false};
        bool ready{false};          // EX has written result
        uint8_t rd{0};              // Destination register
        uint64_t result{0};         // Stored here until commit (CVA6-confirmed)
        FunctionalUnit fu{FunctionalUnit::NONE};
        uint64_t pc{0};
        bool is_store{false};
        bool is_branch{false};
        bool exception{false};
        uint64_t exception_cause{0};
        int trans_id{-1};           // == index in this array
        bool writes_to_fp_reg{false};
    };

    // Per-register clobber info — CVA6 rd_clobber signal.
    // Tells Issue stage which FU is producing a given register and
    // whether a forwarded value is already available.
    struct ClobberInfo {
        bool busy{false};
        FunctionalUnit fu{FunctionalUnit::NONE};
        int trans_id{-1};
    };

    ClobberInfo rd_clobber[32]{};
    ClobberInfo fp_clobber[32]{};

    Scoreboard() = default;

    // Allocate next entry at tail (in-order issue).
    // Returns index (== trans_id) or -1 if full.
    int allocate() {
        if (is_full()) return -1;
        int idx = tail;
        entries[idx] = Entry{};
        entries[idx].valid    = true;
        entries[idx].trans_id = idx;
        tail = (tail + 1) % static_cast<int>(SIZE);
        count++;
        return idx;
    }

    // Called by EX when an instruction finishes — stores result in entry.
    void complete(int idx, uint64_t result, uint8_t rd) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= SIZE) return;
        entries[idx].ready  = true;
        entries[idx].result = result;
        entries[idx].rd     = rd;
    }

    // --- Head / commit interface ---

    bool head_ready() const {
        return count > 0 && entries[head].valid && entries[head].ready;
    }

    const Entry& get_head() const { return entries[head]; }
    int           get_head_index() const { return head; }

    // Retire head entry after commit (in-order).
    void retire() {
        if (is_empty()) return;
        entries[head].valid = false;
        head = (head + 1) % static_cast<int>(SIZE);
        count--;
    }

    // --- Random-access (for EX writeback / forwarding lookup) ---

    Entry&       operator[](int idx)       { return entries[idx]; }
    const Entry& get_entry(int idx) const  { return entries[idx]; }

    // --- State queries ---

    bool        is_full()  const { return count == SIZE; }
    bool        is_empty() const { return count == 0; }
    std::size_t get_count() const { return count; }

    // Flush all entries and clobber state (exception / illegal instruction).
    void flush() {
        for (auto& e : entries) { e.valid = false; e.ready = false; }
        for (auto& c : rd_clobber) { c.busy = false; c.trans_id = -1; }
        for (auto& c : fp_clobber) { c.busy = false; c.trans_id = -1; }
        head = 0; tail = 0; count = 0;
    }

    // Flush only speculative entries younger than the branch instruction
    void flush_speculative(int branch_rob_idx) {
        if (branch_rob_idx < 0 || static_cast<std::size_t>(branch_rob_idx) >= SIZE) return;
        
        // The tail is the next allocation slot.
        // Speculative entries are from (branch_rob_idx + 1) % SIZE to tail.
        int curr = (branch_rob_idx + 1) % static_cast<int>(SIZE);
        int end_tail = tail;
        
        while (curr != end_tail) {
            if (entries[curr].valid) {
                uint8_t rd = entries[curr].rd;
                // Clear clobber if it points to this speculative entry
                if (rd_clobber[rd].busy && rd_clobber[rd].trans_id == curr) {
                    rd_clobber[rd].busy = false;
                    rd_clobber[rd].trans_id = -1;
                }
                if (fp_clobber[rd].busy && fp_clobber[rd].trans_id == curr) {
                    fp_clobber[rd].busy = false;
                    fp_clobber[rd].trans_id = -1;
                }
                entries[curr].valid = false;
                entries[curr].ready = false;
                count--;
            }
            curr = (curr + 1) % static_cast<int>(SIZE);
        }
        tail = (branch_rob_idx + 1) % static_cast<int>(SIZE);
    }

private:
    std::array<Entry, SIZE> entries{};
    int         head{0};
    int         tail{0};
    std::size_t count{0};
};

} // namespace riscv_tlm

#endif // SCOREBOARD_H
