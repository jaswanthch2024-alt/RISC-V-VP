/*!
 \file MemoryInterface.cpp
 \brief CPU to Memory Interface class
 \author Màrius Montón
 \date May 2020
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MemoryInterface.h"
#include <iostream>
#include <sstream>

namespace riscv_tlm {

    MemoryInterface::MemoryInterface() :
            data_bus("data_bus") {}

/**
 * Access data memory to get data (32-bit)
 * @param  addr address to access to
 * @param size size of the data to read in bytes
 * @return data value read
 */
    std::uint32_t MemoryInterface::readDataMem(std::uint64_t addr, int size) {
        std::uint32_t data = 0;
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

        trans.set_command(tlm::TLM_READ_COMMAND);
        trans.set_data_ptr(reinterpret_cast<unsigned char *>(&data));
        trans.set_data_length(size);
        trans.set_streaming_width(4);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        trans.set_address(addr);

        data_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            std::stringstream error_msg;
            error_msg << "Read memory: 0x" << std::hex << addr;
            SC_REPORT_ERROR("Memory", error_msg.str().c_str());
        }

        return data;
    }

/**
 * Access data memory to get data (64-bit for RV64)
 * @param  addr address to access to
 * @param size size of the data to read in bytes (1, 2, 4, or 8)
 * @return data value read (64-bit)
 */
    std::uint64_t MemoryInterface::readDataMem64(std::uint64_t addr, int size) {
        std::uint64_t data = 0;
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

        trans.set_command(tlm::TLM_READ_COMMAND);
        trans.set_data_ptr(reinterpret_cast<unsigned char *>(&data));
        trans.set_data_length(size);
        trans.set_streaming_width(size);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        trans.set_address(addr);

        data_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            std::stringstream error_msg;
            error_msg << "Read memory (64-bit): 0x" << std::hex << addr;
            SC_REPORT_ERROR("Memory", error_msg.str().c_str());
        }

        return data;
    }

/**
 * Access data memory to write data (32-bit)
 * @brief
 * @param addr addr address to access to
 * @param data data to write
 * @param size size of the data to write in bytes
 */
    void MemoryInterface::writeDataMem(std::uint64_t addr, std::uint32_t data, int size) {
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

        trans.set_command(tlm::TLM_WRITE_COMMAND);
        trans.set_data_ptr(reinterpret_cast<unsigned char *>(&data));
        trans.set_data_length(size);
        trans.set_streaming_width(4);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        trans.set_address(addr);

        data_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            std::stringstream error_msg;
            error_msg << "Write memory: 0x" << std::hex << addr;
            SC_REPORT_ERROR("Memory", error_msg.str().c_str());
        }
    }

/**
 * Access data memory to write data (64-bit for RV64)
 * @brief
 * @param addr addr address to access to
 * @param data data to write (64-bit)
 * @param size size of the data to write in bytes (1, 2, 4, or 8)
 */
    void MemoryInterface::writeDataMem64(std::uint64_t addr, std::uint64_t data, int size) {
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

        trans.set_command(tlm::TLM_WRITE_COMMAND);
        trans.set_data_ptr(reinterpret_cast<unsigned char *>(&data));
        trans.set_data_length(size);
        trans.set_streaming_width(size);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        trans.set_address(addr);

        data_bus->b_transport(trans, delay);

        if (trans.is_response_error()) {
            std::stringstream error_msg;
            error_msg << "Write memory (64-bit): 0x" << std::hex << addr;
            SC_REPORT_ERROR("Memory", error_msg.str().c_str());
        }
    }
}