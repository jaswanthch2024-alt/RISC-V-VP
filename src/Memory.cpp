/*!
 \file Memory.cpp
 \brief Basic TLM-2 memory model
 \author Màrius Montón
 \date August 2018
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Memory.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <iomanip>

namespace riscv_tlm {

 SC_HAS_PROCESS(Memory);

 Memory::Memory(sc_core::sc_module_name const &name, std::string const &filename) :
 sc_module(name), socket("socket"), LATENCY(sc_core::SC_ZERO_TIME) {
 // Register callbacks for incoming interface method calls
 socket.register_b_transport(this, &Memory::b_transport);
 socket.register_get_direct_mem_ptr(this, &Memory::get_direct_mem_ptr);
 socket.register_transport_dbg(this, &Memory::transport_dbg);

 dmi_allowed = false;
 program_counter =0;
 readHexFile(filename);

 // Optional runtime latency: env RVSIM_MEM_LAT_NS (nanoseconds)
 if (const char* env = std::getenv("RVSIM_MEM_LAT_NS")) {
     try {
         long ns = std::strtol(env, nullptr, 10);
         if (ns > 0) m_latency = sc_core::sc_time(ns, sc_core::SC_NS);
     } catch (...) { /* ignore */ }
 }

 logger = spdlog::get("my_logger");
 if (!logger) {
 auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
 logger = std::make_shared<spdlog::logger>("my_logger", null_sink);
 logger->set_pattern("%v");
 logger->set_level(spdlog::level::info);
 spdlog::register_logger(logger);
 }
 logger->debug("Using file {}", filename);
 }

 Memory::Memory(sc_core::sc_module_name const &name) :
 sc_module(name), socket("socket"), LATENCY(sc_core::SC_ZERO_TIME) {
 socket.register_b_transport(this, &Memory::b_transport);
 socket.register_get_direct_mem_ptr(this, &Memory::get_direct_mem_ptr);
 socket.register_transport_dbg(this, &Memory::transport_dbg);

 	dmi_allowed = false;
 program_counter =0;

 logger = spdlog::get("my_logger");
 if (!logger) {
 auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
 logger = std::make_shared<spdlog::logger>("my_logger", null_sink);
 logger->set_pattern("%v");
 logger->set_level(spdlog::level::info);
 spdlog::register_logger(logger);
 }
 logger->debug("Memory instantiated without file");
 }

 Memory::~Memory() = default;

 std::uint32_t Memory::getPCfromHEX() {
 return program_counter;

 }

 void Memory::b_transport(tlm::tlm_generic_payload &trans,
 sc_core::sc_time &delay) {
 tlm::tlm_command cmd = trans.get_command();
 sc_dt::uint64 adr = trans.get_address();
 unsigned char *ptr = trans.get_data_ptr();
 unsigned int len = trans.get_data_length();
 unsigned char *byt = trans.get_byte_enable_ptr();
 unsigned int wid = trans.get_streaming_width();

 // *********************************************
 // Generate the appropriate error response
 // *********************************************
 if (adr >= sc_dt::uint64(Memory::SIZE) || (adr + len) > sc_dt::uint64(Memory::SIZE)) {
 trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
 return;
 }
 if (byt != nullptr) {
 trans.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
 return;
 }
 // Obliged to implement read and write commands
 if (cmd == tlm::TLM_READ_COMMAND) {
 std::copy_n(mem.cbegin() + adr, len, ptr);
 } else if (cmd == tlm::TLM_WRITE_COMMAND) {
 std::copy_n(ptr, len, mem.begin() + adr);
 }

 // Accumulate configured latency (simulate memory/bus delay)
 delay += m_latency;

 // Reset timing annotation after waiting
 // Keep annotation for initiator to honor; do not zero it here
 // delay = sc_core::SC_ZERO_TIME; // removed to preserve latency

 // *********************************************
 // Set DMI hint to indicated that DMI is supported
 // *********************************************
 trans.set_dmi_allowed(dmi_allowed);

 // Obliged to set response status to indicate successful completion
 trans.set_response_status(tlm::TLM_OK_RESPONSE);
 }

 bool Memory::get_direct_mem_ptr(tlm::tlm_generic_payload &trans,
 tlm::tlm_dmi &dmi_data) {

 (void) trans;

 // Allow disabling DMI via environment for benchmarking
 if (std::getenv("DISABLE_DMI")) {
     return false;
 }

 if (!dmi_allowed) {
 return false;
 }

 // Permit read and write access
 dmi_data.allow_read_write();

 // Set other details of DMI region
 dmi_data.set_dmi_ptr(reinterpret_cast<unsigned char *>(mem.data()));
 dmi_data.set_start_address(0);
 dmi_data.set_end_address(Memory::SIZE -1);
 dmi_data.set_read_latency(m_latency);
 dmi_data.set_write_latency(m_latency);

 return true;
 }

 unsigned int Memory::transport_dbg(tlm::tlm_generic_payload &trans) {
 tlm::tlm_command cmd = trans.get_command();
 sc_dt::uint64 adr = trans.get_address();
 unsigned char *ptr = trans.get_data_ptr();
 unsigned int len = trans.get_data_length();

 if (adr >= sc_dt::uint64(Memory::SIZE)) {
 trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
 return 0;
 }

 // Calculate the number of bytes to be actually copied
 unsigned int num_bytes = static_cast<unsigned int>
 (std::min<sc_dt::uint64>(len, sc_dt::uint64(Memory::SIZE) - adr));

 if (cmd == tlm::TLM_READ_COMMAND) {
 std::copy_n(mem.cbegin() + adr, num_bytes, ptr);
 } else if (cmd == tlm::TLM_WRITE_COMMAND) {
 std::copy_n(ptr, num_bytes, mem.begin() + adr);
 }

 return num_bytes;
 }

 void Memory::readHexFile(std::string const &filename) {
 std::ifstream hexfile;
 std::string line;
 std::uint32_t memory_offset =0;

 hexfile.open(filename);

 if (hexfile.is_open()) {
 std::uint32_t extended_address =0;

 while (getline(hexfile, line)) {
 // Skip empty lines or lines that are not Intel HEX records
 if (line.empty() || line[0] != ':') continue;

 // Minimum Intel HEX record length:
 // ':' + 2(count) + 4(addr) + 2(type) + 2(checksum) = 11
 if (line.length() < 11) continue;

 if (line.substr(7,2) == "00") {
 /* Data */
 int byte_count;
 std::uint32_t address;
 byte_count = std::stoi(line.substr(1,2), nullptr,16);
 address = std::stoi(line.substr(3,4), nullptr,16);
 address = address + extended_address + memory_offset;

 for (int i =0; i < byte_count; i++) {
                            // 'address' already includes extended_address + memory_offset
                            // (computed above). Apply the DRAM base offset if needed.
                            std::uint64_t a64 = static_cast<std::uint64_t>(address) + i;
                            // If this falls in the Linux DRAM window (0x80000000+),
                            // map it down to offset 0 in the flat mem[] array.
                            const std::uint64_t DRAM_BASE = 0x80000000ULL;
                            const std::uint64_t DRAM_END  = DRAM_BASE + static_cast<std::uint64_t>(Memory::SIZE);
                            if (a64 >= DRAM_BASE && a64 < DRAM_END) {
                                a64 -= DRAM_BASE;
                            }
                            std::uint32_t a = static_cast<std::uint32_t>(a64);
                            // Validate both memory bounds and line bounds before accessing
                            if (a < Memory::SIZE &&
                                static_cast<std::size_t>(9 + (i * 2) + 2) <= line.length()) {
                                mem[a] = stol(line.substr(9 + (i *2),2), nullptr,16);
                            }
 }
 } else if (line.substr(7,2) == "02") {
 /* Extended segment address — needs at least 15 chars */
 if (line.length() < 15) continue;
 extended_address = stol(line.substr(9,4), nullptr,16)
 *16;
 std::cout << "02 extended address 0x" << std::hex
 << extended_address << std::dec << std::endl;
 } else if (line.substr(7,2) == "03") {
 /* Start segment address — needs at least 19 chars */
 if (line.length() < 19) continue;
 std::uint32_t code_segment;
 code_segment = stol(line.substr(9,4), nullptr,16) *16; /* ? */
 program_counter = stol(line.substr(13,4), nullptr,16);
 program_counter = program_counter + code_segment;
 std::cout << "03 PC set to 0x" << std::hex
 << program_counter << std::dec << std::endl;
 } else if (line.substr(7,2) == "04") {
 /* Extended linear address — needs at least 15 chars */
 if (line.length() < 15) continue;
 // Cast to uint32_t before shifting to avoid signed integer overflow
 memory_offset = static_cast<std::uint32_t>(stol(line.substr(9,4), nullptr,16)) << 16;
 extended_address =0;
 } else if (line.substr(7,2) == "05") {
 /* Start linear address — needs at least 19 chars */
 if (line.length() < 19) continue;
 program_counter = stol(line.substr(9,8), nullptr,16);
 std::cout << "05 PC set to 0x" << std::hex
 << program_counter << std::dec << std::endl;
 }
 }
 hexfile.close();

 // Debug: print first 16 bytes loaded at DRAM base (mem[0..15] = physical 0x80000000)
 std::cout << "[Memory] First 16 bytes at DRAM base (mem[0]): ";
 for (int _di = 0; _di < 16; _di++) {
     std::cout << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(mem[_di]) << " ";
 }
 std::cout << std::dec << std::endl;

 if (memory_offset !=0) {
 dmi_allowed = false;
 } else {
 dmi_allowed = true;
 }

 } else {
 SC_REPORT_ERROR("Memory", "Open file error");
 }
 }

 void Memory::loadBin(const std::string &filename, std::uint64_t phys_offset) {
     static constexpr std::uint64_t DRAM_BASE = 0x80000000ULL;
     if (phys_offset < DRAM_BASE || phys_offset >= DRAM_BASE + static_cast<std::uint64_t>(SIZE)) {
         SC_REPORT_ERROR("Memory", "loadBin: phys_offset out of DRAM range");
         return;
     }
     std::uint64_t mem_offset = phys_offset - DRAM_BASE;

     std::ifstream f(filename, std::ios::binary);
     if (!f.is_open()) {
         std::cerr << "[Memory] loadBin: cannot open " << filename << std::endl;
         SC_REPORT_ERROR("Memory", "loadBin: file open error");
         return;
     }
     f.seekg(0, std::ios::end);
     std::streamsize sz = f.tellg();
     f.seekg(0, std::ios::beg);

     if (mem_offset + static_cast<std::uint64_t>(sz) > static_cast<std::uint64_t>(SIZE)) {
         std::cerr << "[Memory] loadBin: file too large to fit at offset 0x"
                   << std::hex << phys_offset << std::dec << std::endl;
         SC_REPORT_ERROR("Memory", "loadBin: file too large");
         return;
     }
     f.read(reinterpret_cast<char*>(&mem[mem_offset]), sz);
     std::cout << "[Memory] loadBin: loaded " << sz << " bytes at phys 0x"
               << std::hex << phys_offset << std::dec
               << " (\"" << filename << "\")" << std::endl;
     dmi_allowed = false;
 }
}
