/*!
 \file Memory.h
 \brief Basic TLM-2 memory model
 \author Màrius Montón
 \date August 2018
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef __MEMORY_H__
#define __MEMORY_H__

#include <iostream>
#include <fstream>
#include <array>

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include "systemc"

#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

namespace riscv_tlm {
/**
 * @brief Basic TLM-2 memory
 */
    class Memory : sc_core::sc_module {
    public:
        // TLM-2 socket, defaults to 32-bits wide, base protocol
        tlm_utils::simple_target_socket<Memory> socket;

        /* 16 MBytes */
        enum {
            SIZE = 0x20000000
        };
        const sc_core::sc_time LATENCY; // legacy const (kept at zero)

        Memory(sc_core::sc_module_name const &name, std::string const &filename);

        explicit Memory(const sc_core::sc_module_name &name);

        ~Memory() override;

        /**
         * @brief Returns Program Counter read from hexfile
         * @return Initial PC
         */
        virtual std::uint32_t getPCfromHEX();

        // TLM-2 blocking transport method
        virtual void b_transport(tlm::tlm_generic_payload &trans,
                                 sc_core::sc_time &delay);

        // *********************************************
        // TLM-2 forward DMI method
        // *********************************************
        virtual bool get_direct_mem_ptr(tlm::tlm_generic_payload &trans,
                                        tlm::tlm_dmi &dmi_data);

        // *********************************************
        // TLM-2 debug transport method
        // *********************************************
        virtual unsigned int transport_dbg(tlm::tlm_generic_payload &trans);

        /**
         * @brief Load a raw binary file into memory at a given physical offset
         * @param filename path to binary file
         * @param phys_offset physical address offset (e.g. 0x80000000 for OpenSBI,
         *                    0x80100000 for DTB, 0x80200000 for Linux Image)
         */
        void loadBin(const std::string &filename, std::uint64_t phys_offset);

    private:

        /**
         * @brief Memory array in bytes
         */
        std::array<uint8_t, Memory::SIZE> mem{};

        /**
         * @brief Log class
         */
        std::shared_ptr<spdlog::logger> logger;

        /**
         * @brief Program counter (PC) read from hex file
         */
        std::uint32_t program_counter;

        /**
         * @brief DMI can be used?
         */
        bool dmi_allowed;

        /**
         * @brief Optional configured latency (via env RVSIM_MEM_LAT_NS)
         */
        sc_core::sc_time m_latency{sc_core::SC_ZERO_TIME};

        /**
         * @brief Read Intel hex file
         * @param filename file name to read
         */
        void readHexFile(const std::string &filename);
    };
}
#endif /* __MEMORY_H__ */
