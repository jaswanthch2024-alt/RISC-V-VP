// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ROB.h
 * @brief Reorder Buffer (ROB) for Out-of-Order Execution with In-Order Commit
 * @note Circular buffer that tracks in-flight instructions from Issue to Commit
 */
#pragma once
#ifndef ROB_H
#define ROB_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

/**
 * @brief ROB Entry Structure
 * Each entry represents one in-flight instruction
 */
struct ROBEntry {
    bool valid{false};              // Is this entry allocated?
    bool ready{false};              // Has the instruction completed execution?
    uint8_t dest_reg{0};            // Destination register (0-31)
    uint64_t result{0};             // Computed result value
    bool is_store{false};           // Is this a store instruction?
    bool is_branch{false};          // Is this a branch/jump instruction?
    bool exception{false};          // Did an exception occur?
    uint64_t pc{0};                 // PC of this instruction (for debugging/exceptions)
};

/**
 * @brief Reorder Buffer Class
 * Manages out-of-order instruction completion with in-order commit
 */
template<std::size_t SIZE = 32>
class ReorderBuffer {
public:
    ReorderBuffer() = default;

    /**
     * @brief Allocate a new ROB entry at tail
     * @return ROB index if successful, -1 if full
     */
    int allocate() {
        if (is_full()) return -1;
        
        int index = tail;
        entries[tail].valid = true;
        entries[tail].ready = false;
        entries[tail].exception = false;
        
        tail = (tail + 1) % SIZE;
        count++;
        return index;
    }

    /**
     * @brief Mark a ROB entry as complete with result
     * @param index ROB index
     * @param result Computed value
     * @param dest_reg Destination register
     */
    void complete(int index, uint64_t result, uint8_t dest_reg) {
        if (index < 0 || static_cast<std::size_t>(index) >= SIZE) return;
        entries[index].ready = true;
        entries[index].result = result;
        entries[index].dest_reg = dest_reg;
    }

    /**
     * @brief Check if head entry is ready to commit
     */
    bool head_ready() const {
        return entries[head].valid && entries[head].ready;
    }

    /**
     * @brief Get the head entry for commit
     */
    const ROBEntry& get_head() const {
        return entries[head];
    }

    /**
     * @brief Commit (deallocate) the head entry
     */
    void retire() {
        if (is_empty()) return;
        entries[head].valid = false;
        head = (head + 1) % SIZE;
        count--;
    }

    /**
     * @brief Check if ROB is full
     */
    bool is_full() const {
        return count == SIZE;
    }

    /**
     * @brief Check if ROB is empty
     */
    bool is_empty() const {
        return count == 0;
    }

    /**
     * @brief Get current occupancy
     */
    std::size_t get_count() const {
        return count;
    }

    /**
     * @brief Get the index of the head entry (for commit)
     */
    int get_head_index() const {
        return head;
    }

    /**
     * @brief Get entry at index (for writeback to mark ready)
     */
    ROBEntry& operator[](int index) {
        return entries[index];
    }

    /**
     * @brief Flush entire ROB (on exception/branch mispredict)
     */
    void flush() {
        for (auto& entry : entries) {
            entry.valid = false;
            entry.ready = false;
        }
        head = 0;
        tail = 0;
        count = 0;
    }

private:
    std::array<ROBEntry, SIZE> entries;
    int head{0};        // Points to oldest (next to commit)
    int tail{0};        // Points to next free slot
    std::size_t count{0};
};

} // namespace riscv_tlm

#endif // ROB_H
