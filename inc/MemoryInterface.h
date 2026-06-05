/*!
 \file MemoryInterface.h
 \brief CPU to Memory Interface class
 \author Màrius Montón
 \date May 2020
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef INC_MEMORYINTERFACE_H_
#define INC_MEMORYINTERFACE_H_

#include <cstdint>
#include "systemc"

#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/tlm_quantumkeeper.h"

#include "Memory.h"
#include <cstdint>

namespace riscv_tlm {

/**
 * @brief Memory Interface
 */
    class MemoryInterface {
    public:

        tlm_utils::simple_initiator_socket<MemoryInterface> data_bus;

        MemoryInterface();

        std::uint32_t readDataMem(std::uint64_t addr, int size);
        
        /**
         * @brief Read 64-bit data from memory (for RV64 support)
         * @param addr address to access
         * @param size size of the data to read in bytes (1, 2, 4, or 8)
         * @return data value read (64-bit)
         */
        std::uint64_t readDataMem64(std::uint64_t addr, int size);

        void writeDataMem(std::uint64_t addr, std::uint32_t data, int size);
        
        /**
         * @brief Write 64-bit data to memory (for RV64 support)
         * @param addr address to access
         * @param data data to write (64-bit)
         * @param size size of the data to write in bytes (1, 2, 4, or 8)
         */
        void writeDataMem64(std::uint64_t addr, std::uint64_t data, int size);
    };
}
#endif /* INC_MEMORYINTERFACE_H_ */
