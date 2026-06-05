/*!
 \file Simulator.cpp
 \brief Top level simulation entity (Loosely-Timed, non-pipelined)
 \author Màrius Montón
 \date September 2018
 */

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include "systemc"

#include <csignal>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <iomanip>
#include <cmath>
#ifndef _WIN32
#include <unistd.h>
#include <getopt.h>
#else
// Provide minimal POSIX compatibility shims for Windows
#define access _access

// Minimal getopt_long replacement for Windows build
static int optind_win = 1; char* optarg = nullptr; int opterr = 0; struct option { const char* name; int has_arg; int* flag; int val; };
#define required_argument 1
int getopt_long(int argc, char* const argv[], const char* optstring, const option* longopts, int* longindex) {
    (void)longopts; (void)longindex; if (optind_win >= argc) return -1; char* arg = argv[optind_win]; if(arg[0] != '-') return -1; char opt = arg[1]; if(opt == '\0') return -1; optarg = nullptr; if(strchr(optstring,opt)) { if((opt=='f'||opt=='R'||opt=='M'||opt=='B'||opt=='E'||opt=='L') && optind_win+1 < argc) { optarg = argv[++optind_win]; } optind_win++; return opt; } optind_win++; return '?'; }
#define getopt_long_defined 1
#endif
#include <cstdlib>

#include "CPU.h"
#include "CPU_Simple.h"
#include "BusCtrl.h"
#include "Trace.h"
#include "Timer.h"
#include "Debug.h"
#include "Performance.h"

// Peripherals
#include "UART.h"
#include "CLINT.h"
#include "PLIC.h"
#include "DMA.h"
#include "SyscallIf.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/null_sink.h"


std::string filename;
bool debug_session = false;
bool mem_dump = false;
uint32_t dump_addr_st = 0;
uint32_t dump_addr_end = 0;

riscv_tlm::cpu_types_t cpu_type_opt = riscv_tlm::RV32;

/**
 * @class Simulator
 * @brief Top simulation entity using Loosely-Timed (LT) non-pipelined CPU
 * 
 * This simulator uses simple functional CPU models without pipeline timing.
 * For pipelined simulation with AT timing, use RISCV_VP instead.
 */
class Simulator : sc_core::sc_module {
public:
    riscv_tlm::CPU *cpu;
    riscv_tlm::Memory *MainMemory;
    riscv_tlm::BusCtrl *Bus;
    riscv_tlm::peripherals::Trace *trace;
    riscv_tlm::peripherals::Timer *timer;
    riscv_tlm::peripherals::UART *uart;
    riscv_tlm::peripherals::CLINT *clint;
    riscv_tlm::peripherals::PLIC *plic;
    riscv_tlm::peripherals::DMA *dma;
    riscv_tlm::peripherals::SyscallIf *sysif;

    explicit Simulator(sc_core::sc_module_name const &name, riscv_tlm::cpu_types_t cpu_type_m)
    : sc_module(name)
    , cpu(nullptr)
    , MainMemory(nullptr)
    , Bus(nullptr)
    , trace(nullptr)
    , timer(nullptr)
    , clk("clk", sc_core::sc_time(10, sc_core::SC_NS))
    {
        std::uint32_t start_PC;

        MainMemory = new riscv_tlm::Memory("Main_Memory", filename);
        start_PC = MainMemory->getPCfromHEX();

        cpu_type = cpu_type_m;

        // Create simple non-pipelined CPU (LT model)
        if (cpu_type == riscv_tlm::RV32) {
            cpu = new riscv_tlm::CPURV32Simple("cpu", start_PC, debug_session);
        } else {
            cpu = new riscv_tlm::CPURV64Simple("cpu", start_PC, debug_session);
        }
        cpu->set_clock(&clk);

        Bus = new riscv_tlm::BusCtrl("BusCtrl");
        trace = new riscv_tlm::peripherals::Trace("Trace");
        timer = new riscv_tlm::peripherals::Timer("Timer");
        uart  = new riscv_tlm::peripherals::UART("UART0");
        clint = new riscv_tlm::peripherals::CLINT("CLINT");
        plic  = new riscv_tlm::peripherals::PLIC("PLIC");
        dma   = new riscv_tlm::peripherals::DMA("DMA");
        sysif = new riscv_tlm::peripherals::SyscallIf("SysIf");

        cpu->instr_bus.bind(Bus->cpu_instr_socket);
        cpu->mem_intf->data_bus.bind(Bus->cpu_data_socket);

        Bus->memory_socket.bind(MainMemory->socket);
        Bus->trace_socket.bind(trace->socket);
        Bus->timer_socket.bind(timer->socket);
        Bus->uart_socket.bind(uart->socket);
        Bus->clint_socket.bind(clint->socket);
        Bus->plic_socket.bind(plic->socket);
        Bus->dma_socket.bind(dma->socket);
        Bus->syscall_socket.bind(sysif->socket);

        dma->mem_master.bind(Bus->dma_master_socket);
        timer->irq_line.bind(cpu->irq_line_socket);

        if (debug_session) {
            std::cout << "[Debug] GDB debugging enabled." << std::endl;
        }
    }

    ~Simulator() override {
        if (mem_dump) {
            MemoryDump();
        }
        delete sysif;
        delete dma;
        delete plic;
        delete clint;
        delete uart;
        delete MainMemory;
        delete cpu;
        delete Bus;
        delete trace;
        delete timer;
    }

private:
    void MemoryDump() const {
        std::cout << "********** MEMORY DUMP ***********\n";

        uint32_t local_dump_addr_st = dump_addr_st;
        uint32_t local_dump_addr_end = dump_addr_end;

        if (local_dump_addr_st == 0) {
            local_dump_addr_st = cpu->getStartDumpAddress();
        }

        if (local_dump_addr_end == 0) {
            local_dump_addr_end = cpu->getEndDumpAddress();
        }

        std::cout << "from 0x" << std::hex << local_dump_addr_st << " to 0x" << local_dump_addr_end << "\n";
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        std::uint32_t data[4];

        trans.set_command(tlm::TLM_READ_COMMAND);
        trans.set_data_ptr(reinterpret_cast<unsigned char*>(data));
        trans.set_data_length(4);
        trans.set_streaming_width(4);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        std::string base_filename = filename.substr(filename.find_last_of("/\\") + 1);
        std::string base_name = base_filename.substr(0, base_filename.find('.'));
        std::string local_name = base_name + ".signature.output";
        std::cout << "filename is " << local_name << '\n';

        std::ofstream signature_file;
        signature_file.open(local_name);

        for(unsigned int i = local_dump_addr_st; i < local_dump_addr_end; i = i+4) {
            trans.set_address(i);
            MainMemory->b_transport(trans, delay);
            signature_file << std::hex << std::setfill('0') << std::setw(8) << data[0] <<  "\n";
        }

        signature_file.close();
    }

private:
    riscv_tlm::cpu_types_t cpu_type;
    sc_core::sc_clock clk;
};

Simulator *top;
std::shared_ptr<spdlog::logger> logger;

static void intHandler(int dummy) {
    delete top;
    (void) dummy;
    exit(-1);
}

std::uint64_t max_instructions_limit = 0;

static void process_arguments(int argc, char *argv[]) {
    opterr = 0;
    int c;
    long int debug_level = 0;

    debug_session = false;
    cpu_type_opt = riscv_tlm::RV32;

    static struct option long_options[] = {
        {"max-instr", required_argument, nullptr, 'M'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "DTE:B:L:f:R:M:?", long_options, nullptr)) != -1) {
        switch (c) {
        case 'D':
            debug_session = true;
            break;
        case 'T':
            mem_dump = true;
            break;
        case 'B':
            dump_addr_st = std::strtoul (optarg, nullptr, 16);
            break;
        case 'E':
            dump_addr_end = std::strtoul(optarg, nullptr, 16);
            break;
        case 'L':
            debug_level = std::strtol(optarg, nullptr, 10);

            if (debug_level >= 0) {
                spdlog::filename_t log_filename = SPDLOG_FILENAME_T("newlog.txt");
                logger = spdlog::create<spdlog::sinks::basic_file_sink_mt>("my_logger", log_filename, true);
                logger->set_pattern("%v");
                logger->set_level(spdlog::level::info);
            } else {
                logger = nullptr;
            }

            switch (debug_level) {
            case 3:
                logger->set_level(spdlog::level::info);
                break;
            case 2:
                logger->set_level(spdlog::level::warn);
                break;
            case 1:
                logger->set_level(spdlog::level::debug);
                break;
            case 0:
                logger->set_level(spdlog::level::err);
                break;
            default:
                logger->set_level(spdlog::level::info);
                break;
            }
            break;
        case 'f':
            filename = std::string(optarg);
            break;
        case 'R':
            if (strcmp(optarg, "32") == 0) {
                cpu_type_opt = riscv_tlm::RV32;
            } else {
                cpu_type_opt = riscv_tlm::RV64;
            }
            break;
        case 'M':
            if (optarg) {
                max_instructions_limit = std::strtoull(optarg, nullptr, 10);
            }
            break;
        case '?':
            break;
        default:
            break;
        }
    }

    if (filename.empty()) {
        std::cout << "Usage: ./RISCV_TLM -f <file.hex> [-R 32|64] [-L <0..3>] [-M <max_instr>] [-D]" << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

int sc_main(int argc, char *argv[]) {
    Performance *perf = Performance::getInstance();

    signal(SIGINT, intHandler);
    sc_core::sc_set_time_resolution(1, sc_core::SC_NS);

    process_arguments(argc, argv);

    if (logger == nullptr) {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        logger = std::make_shared<spdlog::logger>("my_logger", null_sink);
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::info);
        spdlog::register_logger(logger);
    }

    std::cout << "RISC-V TLM Simulator (Loosely-Timed, non-pipelined)" << std::endl;
    std::cout << "  file: " << filename << std::endl;
    std::cout << "  arch: " << (cpu_type_opt == riscv_tlm::RV32 ? "RV32" : "RV64") << std::endl;
    std::cout << "  mode: LT (functional)" << std::endl;

    top = new Simulator("top", cpu_type_opt);

    auto start = std::chrono::steady_clock::now();

    if (max_instructions_limit > 0) {
        const sc_core::sc_time quantum(1, sc_core::SC_MS);
        while (true) {
            sc_core::sc_start(quantum);
            if (perf->getInstructions() >= max_instructions_limit || sc_core::sc_get_status() == sc_core::SC_STOPPED)
                break;
        }
    } else {
        sc_core::sc_start();
    }

    auto end = std::chrono::steady_clock::now();

    std::chrono::duration<double> elapsed_seconds = end - start;

    std::cout << "\n=== Simulation Results ===" << std::endl;
    std::cout << "Wall time:    " << std::fixed << std::setprecision(3) << elapsed_seconds.count() << " s" << std::endl;
    std::cout << "Instructions: " << perf->getInstructions() << std::endl;

    if (!mem_dump && max_instructions_limit == 0) {
        std::cout << "Press Enter to finish" << std::endl;
        std::cin.ignore();
    }

    delete top;

    return 0;
}
