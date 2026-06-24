/*!
 \file VPMain.cpp
 \brief Virtual Prototype entry point (sc_main) with CLI and stats
 \note Uses Loosely-Timed (LT) 2-stage pipelined CPU with behavioral cycle counting
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include "systemc"
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

#include "VPTop.h"
#include "Performance.h"
#if defined(ENABLE_PIPELINED_ISS)
  #if defined(ENABLE_CYCLE6_MODEL)
    #include "CPU_P32_6_Cycle.h"
    #include "CPU_P64_6_Cycle.h"
  #elif defined(ENABLE_CYCLE_MODEL)
    #include "CPU_P32_2_Cycle.h"
    #include "CPU_P64_2_Cycle.h"
  #elif defined(ENABLE_AT_MODEL)
    #include "CPU_P32_2_AT.h"
    #include "CPU_P64_2_AT.h"
  #else
    #include "CPU_P32_2.h"
    #include "CPU_P64_2.h"
  #endif
#endif

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/null_sink.h"

static vp::VPTop *g_top = nullptr;
static volatile std::sig_atomic_t g_interrupted = 0;

static void intHandler(int dummy) {
    (void)dummy;
    g_interrupted = 1;
}


struct Options {
    std::string hex_file;
    std::string bios_file;
    std::string dtb_file;
    std::string kernel_file;
    std::string initrd_file;
    bool debug = false;
    riscv_tlm::cpu_types_t cpu_type = riscv_tlm::RV32;
    double timeout_sec = -1.0;
    std::uint64_t max_instructions = 0;
};

static void usage(const char* exe) {
    std::cout << "Usage: " << exe << " -f <file.hex> [-R 32|64] [-D] [-t <seconds>] [--max-instr <N>]\n";
#if defined(ENABLE_PIPELINED_ISS)
  #if defined(ENABLE_CYCLE6_MODEL)
    std::cout << "\nRISC-V Virtual Prototype with Cycle-Accurate 6-Stage Pipelined CPU\n";
  #elif defined(ENABLE_CYCLE_MODEL)
    std::cout << "\nRISC-V Virtual Prototype with Cycle-Accurate 2-Stage Pipelined CPU\n";
  #elif defined(ENABLE_AT_MODEL)
    std::cout << "\nRISC-V Virtual Prototype with AT 2-Stage Pipelined CPU\n";
  #else
    std::cout << "\nRISC-V Virtual Prototype with LT 2-Stage Pipelined CPU\n";
  #endif
#else
    std::cout << "\nRISC-V Virtual Prototype (Single-Cycle LT)\n";
#endif
    std::cout << "\nOptions:\n";
    std::cout << "  -f, --file <file.hex>   Input hex file (required, unless --bios is set)\n";
    std::cout << "  --bios <fw_jump.bin>    OpenSBI binary (loads at 0x80000000, sets boot mode)\n";
    std::cout << "  --dtb  <vp.dtb>         Device tree blob (loads at 0x80100000)\n";
    std::cout << "  --kernel <Image>        Linux kernel binary (loads at 0x80200000)\n";
    std::cout << "  --initrd <file>         Initramfs cpio.gz (loads at 0x84000000)\n";
    std::cout << "  -R, --arch 32|64        Architecture: RV32 or RV64 (default: 32)\n";
    std::cout << "  -D, --debug             Enable debug mode\n";
    std::cout << "  -t, --timeout <sec>     Wall-clock timeout in seconds\n";
    std::cout << "  --max-instr <N>         Maximum instructions to execute\n";
}

static Options parse(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-f") == 0 || std::strcmp(argv[i], "--file") == 0) && i+1 < argc) {
            o.hex_file = argv[++i];
        } else if (std::strcmp(argv[i], "--bios") == 0 && i+1 < argc) {
            o.bios_file = argv[++i];
        } else if (std::strcmp(argv[i], "--dtb") == 0 && i+1 < argc) {
            o.dtb_file = argv[++i];
        } else if (std::strcmp(argv[i], "--kernel") == 0 && i+1 < argc) {
            o.kernel_file = argv[++i];
        } else if (std::strcmp(argv[i], "--initrd") == 0 && i+1 < argc) {
            o.initrd_file = argv[++i];
        } else if (std::strcmp(argv[i], "-D") == 0 || std::strcmp(argv[i], "--debug") == 0) {
            o.debug = true;
        } else if ((std::strcmp(argv[i], "-R") == 0 || std::strcmp(argv[i], "--arch") == 0) && i+1 < argc) {
            o.cpu_type = (std::strcmp(argv[i+1], "64") == 0) ? riscv_tlm::RV64 : riscv_tlm::RV32;
            ++i;
        } else if ((std::strcmp(argv[i], "-t") == 0 || std::strcmp(argv[i], "--timeout") == 0) && i+1 < argc) {
            try {
                o.timeout_sec = std::stod(argv[++i]);
            } catch (...) {
                usage(argv[0]);
                std::exit(1);
            }
        } else if ((std::strcmp(argv[i], "--max-instr") == 0) && i+1 < argc) {
            char* endp = nullptr;
            auto val = std::strtoull(argv[++i], &endp, 10);
            if (endp == nullptr || *endp != '\0') {
                usage(argv[0]);
                std::exit(1);
            }
            o.max_instructions = val;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            std::exit(0);
        } else {
            usage(argv[0]);
            std::exit(1);
        }
    }
    if (o.hex_file.empty() && o.bios_file.empty()) {
        usage(argv[0]);
        std::exit(1);
    }
    return o;
}

int sc_main(int argc, char* argv[]) {
    signal(SIGINT, intHandler);
    sc_core::sc_set_time_resolution(1, sc_core::SC_NS);

    const auto opts = parse(argc, argv);

    // Setup logger
    try {
        auto existing = spdlog::get("my_logger");
        if (!existing) {
            spdlog::filename_t log_filename = SPDLOG_FILENAME_T("vp.log");
            auto logger = spdlog::create<spdlog::sinks::basic_file_sink_mt>("my_logger", log_filename, true);
            logger->set_pattern("%v");
            logger->set_level(spdlog::level::info);
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not setup file logger (" << e.what() << "), using null logger\n";
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("my_logger", null_sink);
        spdlog::register_logger(logger);
    }

    auto perf = Performance::getInstance();

    std::cout << "RISC-V Virtual Prototype (Loosely-Timed with cycle counting)\n";
    std::cout << "  file: " << opts.hex_file << "\n";
    std::cout << "  arch: " << (opts.cpu_type == riscv_tlm::RV32 ? "RV32" : "RV64") << "\n";
#if defined(ENABLE_CYCLE6_MODEL) || defined(ENABLE_CYCLE_MODEL)
    std::cout << "  mode: Loop-based (cycle-accurate)\n";
#elif defined(ENABLE_AT_MODEL)
    std::cout << "  mode: AT (Approximate-Timed)\n";
#else
    std::cout << "  mode: LT (Loosely-Timed)\n";
#endif
#if defined(ENABLE_PIPELINED_ISS)
    #if defined(ENABLE_CYCLE6_MODEL)
        std::cout << "  pipe: 6-stage (PCGen -> Fetch -> ID -> Issue -> EX -> Commit)\n";
    #elif defined(ENABLE_CYCLE_MODEL)
        std::cout << "  pipe: 2-stage (IF -> EX)\n";
    #elif defined(ENABLE_AT_MODEL)
        std::cout << "  pipe: 2-stage (IF -> EX) (AT)\n";
    #else
        std::cout << "  pipe: 2-stage (IF -> EX) (LT)\n";
    #endif
#else
    std::cout << "  pipe: single-cycle (LT)\n";
#endif
    std::cout << "  dbg : " << (opts.debug ? "on" : "off") << "\n";
    if (opts.timeout_sec > 0) {
        std::cout << "  tmo : " << opts.timeout_sec << " s\n";
    }
    if (opts.max_instructions > 0) {
        std::cout << "  max : " << opts.max_instructions << " instr\n";
    }

    g_top = new vp::VPTop("vp_top", opts.hex_file, opts.cpu_type, opts.debug,
                          opts.bios_file, opts.dtb_file, opts.kernel_file,
                          opts.initrd_file);

    // When --max-instr is set, configure the pipeline trace buffer to hold enough cycles
    // (max_instructions * 4 covers worst-case CPI) and shrink the simulation quantum so
    // the instruction-limit check fires before the trace buffer fills.
#if defined(ENABLE_CYCLE6_MODEL)
    if (opts.max_instructions > 0) {
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P6_Cycle*>(g_top->cpu);
        auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P6_Cycle*>(g_top->cpu);
        // Cap trace buffer at 1M entries (~72 MB) — storing 4×max_instructions
        // would OOM for large runs (e.g. 40M instr → 11.5 GB).
        constexpr size_t MAX_TRACE = 1000000;
        size_t lim = std::min(static_cast<size_t>(opts.max_instructions) * 4, MAX_TRACE);
        if (cpu64) cpu64->trace_limit = lim;
    }
#endif

    auto wall_start = std::chrono::steady_clock::now();

    bool boot_detected = false;
    double boot_wall_time = 0.0;
    sc_core::sc_time boot_sim_time = sc_core::SC_ZERO_TIME;
    uint_fast64_t boot_instructions = 0;
    uint64_t boot_cycles = 0;
    bool boot_has_cycles = false;

    if (g_top->uart) {
        g_top->uart->register_tx_callback([&](char ch) {
            static const std::string target = "Starting interactive shell...";
            static size_t match_idx = 0;
            if (boot_detected) return;

            if (ch == target[match_idx]) {
                match_idx++;
                if (match_idx == target.length()) {
                    boot_detected = true;
                    auto now = std::chrono::steady_clock::now();
                    std::chrono::duration<double> elapsed = now - wall_start;
                    boot_wall_time = elapsed.count();
                    boot_sim_time = sc_core::sc_time_stamp();
                    boot_instructions = perf->getInstructions();

                    // Retrieve cycles if applicable
#if defined(ENABLE_CYCLE6_MODEL)
                    auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P6_Cycle*>(g_top->cpu);
                    auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P6_Cycle*>(g_top->cpu);
                    if (cpu64) {
                        boot_cycles = cpu64->stats.cycles;
                        boot_has_cycles = true;
                    } else if (cpu32) {
                        boot_cycles = cpu32->stats.cycles;
                        boot_has_cycles = true;
                    }
#elif defined(ENABLE_CYCLE_MODEL)
                    auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P2_Cycle*>(g_top->cpu);
                    auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P2_Cycle*>(g_top->cpu);
                    if (cpu64) {
                        boot_cycles = cpu64->getStats().total_cycles;
                        boot_has_cycles = true;
                    } else if (cpu32) {
                        boot_cycles = cpu32->getStats().total_cycles;
                        boot_has_cycles = true;
                    }
#endif

                    std::cout << "\n\n========================================\n";
                    std::cout << "         VP Boot Completed!\n";
                    std::cout << "========================================\n";
                    std::cout << "Boot Wall time:    " << std::fixed << std::setprecision(3) << boot_wall_time << " s\n";
                    std::cout << "Boot Sim time:     " << boot_sim_time << "\n";
                    std::cout << "Boot Instructions: " << boot_instructions << "\n";
                    if (boot_has_cycles) {
                        std::cout << "Boot Cycles:       " << boot_cycles << "\n";
                        if (boot_instructions > 0) {
                            std::cout << "Boot CPI:          " << std::fixed << std::setprecision(3) << (double)boot_cycles / boot_instructions << "\n";
                            std::cout << "Boot IPC:          " << std::fixed << std::setprecision(3) << (double)boot_instructions / boot_cycles << "\n";
                        }
                    }
                    double ips = boot_wall_time > 0.0 ? boot_instructions / boot_wall_time : 0.0;
                    std::cout << "Boot IPS:          " << std::fixed << std::setprecision(2) << (ips / 1e6) << " MIPS\n";
                    std::cout << "========================================\n\n";

#if defined(ENABLE_CYCLE6_MODEL)
                    // Print full pipeline snapshot at boot time
                    {
                        auto* c64 = dynamic_cast<riscv_tlm::CPURV64P6_Cycle*>(g_top->cpu);
                        auto* c32 = dynamic_cast<riscv_tlm::CPURV32P6_Cycle*>(g_top->cpu);
                        if (c64 || c32) {
                            std::cout << "=== Boot Pipeline Statistics (6-stage cycle-accurate) ===\n";
                            if (c64) c64->printStats();
                            else     c32->printStats();
                            std::cout << "=========================================================\n\n";
                        }
                    }
#endif
                }
            } else {
                match_idx = (ch == target[0]) ? 1 : 0;
            }
        });
    }

    // Use a fine-grained quantum when an instruction limit is set so the check fires promptly.
    // At 100 MHz (10 ns/cycle), 10 µs = ~1000 cycles per quantum — well below any practical limit.
    const sc_core::sc_time quantum = (opts.max_instructions > 0)
        ? sc_core::sc_time(10, sc_core::SC_US)
        : sc_core::sc_time(1, sc_core::SC_MS);
    bool timed_out = false;
    bool reached_instr_limit = false;

    while (true) {
        sc_core::sc_start(quantum);

        if (g_interrupted) {
            std::cout << "\n[Simulation Interrupted by User]\n";
            sc_core::sc_stop();
            break;
        }

        if (opts.timeout_sec > 0) {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> wall_elapsed = now - wall_start;
            if (wall_elapsed.count() >= opts.timeout_sec) {
                timed_out = true;
                sc_core::sc_stop();
                break;
            }
        }
        
        if (opts.max_instructions > 0 && perf->getInstructions() >= opts.max_instructions) {
            reached_instr_limit = true;
            sc_core::sc_stop();
            break;
        }
        
        if (sc_core::sc_get_status() == sc_core::SC_STOPPED) {
            break;
        }
    }

    auto wall_end = std::chrono::steady_clock::now();

    std::chrono::duration<double> elapsed = wall_end - wall_start;

    if (timed_out) {
        std::cout << "Stopped due to timeout." << std::endl;
    }
    if (reached_instr_limit) {
        std::cout << "Stopped after reaching instruction limit." << std::endl;
    }

    std::cout << "\n=== Simulation Results (LT) ===\n";
    std::cout << "Wall time:    " << std::fixed << std::setprecision(3) << elapsed.count() << " s\n";
    std::cout << "Sim time:     " << sc_core::sc_time_stamp() << "\n";
    std::cout << "Instructions: " << perf->getInstructions() << "\n";

    std::cout << "\n=== Boot Statistics Summary ===\n";
    if (boot_detected) {
        std::cout << "  Boot Wall time:    " << std::fixed << std::setprecision(3) << boot_wall_time << " s\n";
        std::cout << "  Boot Sim time:     " << boot_sim_time << "\n";
        std::cout << "  Boot Instructions: " << boot_instructions << "\n";
        if (boot_has_cycles) {
            std::cout << "  Boot Cycles:       " << boot_cycles << "\n";
            if (boot_instructions > 0) {
                std::cout << "  Boot CPI:          " << std::fixed << std::setprecision(3) << (double)boot_cycles / boot_instructions << "\n";
                std::cout << "  Boot IPC:          " << std::fixed << std::setprecision(3) << (double)boot_instructions / boot_cycles << "\n";
            }
        }
        double ips = boot_wall_time > 0.0 ? boot_instructions / boot_wall_time : 0.0;
        std::cout << "  Boot IPS:          " << std::fixed << std::setprecision(2) << (ips / 1e6) << " MIPS\n";
    } else {
        std::cout << "  Boot was not completed (did not reach shell prompt).\n";
    }

    // Print pipeline statistics
#if defined(ENABLE_PIPELINED_ISS)
    if (g_top && g_top->cpu && g_top->cpu->isPipelined()) {
        
#if defined(ENABLE_CYCLE6_MODEL)
        std::cout << "\n=== Pipeline Statistics (6-stage cycle-accurate) ===\n";
        // Cycle Accurate 6-Stage Models
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P6_Cycle*>(g_top->cpu);
        auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P6_Cycle*>(g_top->cpu);
        if (cpu64) {
            cpu64->printStats();
            cpu64->dumpPipelineTrace("pipeline_trace.csv");
            double ips = perf->getInstructions() / elapsed.count();
            std::cout << "  IPS:          " << std::fixed << std::setprecision(2) << (ips / 1e6) << " MIPS\n";
        } else if (cpu32) {
            cpu32->printStats();
            double ips = perf->getInstructions() / elapsed.count();
            std::cout << "  IPS:          " << std::fixed << std::setprecision(2) << (ips / 1e6) << " MIPS\n";
        } else {
            std::cerr << "Warning: Could not determine CPU type for pipeline stats (6-stage)\n";
        }
#elif defined(ENABLE_CYCLE_MODEL)
        // Cycle Accurate Models
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P2_Cycle*>(g_top->cpu);
        auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P2_Cycle*>(g_top->cpu);
        if (cpu64) {
            auto stats = cpu64->getStats();
            std::cout << "  Pipeline cycles:    " << stats.total_cycles << "\n";
            std::cout << "  Instructions:       " << stats.instructions_retired << "\n";
            if (stats.total_cycles > 0)
                std::cout << "  IPC:                " << std::fixed << std::setprecision(3) << stats.get_ipc() << "\n";
        } else if (cpu32) {
            auto stats = cpu32->getStats();
            std::cout << "  Pipeline cycles:    " << stats.total_cycles << "\n";
            std::cout << "  Instructions:       " << stats.instructions_retired << "\n";
            if (stats.total_cycles > 0)
                std::cout << "  IPC:                " << std::fixed << std::setprecision(3) << stats.get_ipc() << "\n";
        } else {
            std::cerr << "Warning: Could not determine CPU type for pipeline stats (cycle-accurate)\n";
        }
#elif defined(ENABLE_AT_MODEL)
        // AT Models
        std::cout << "\n=== Pipeline Statistics (2-stage AT model) ===\n";
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P2_AT*>(g_top->cpu);
        auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P2_AT*>(g_top->cpu);
        if (cpu64) {
            cpu64->printStats();
            auto stats = cpu64->getStats();
            if (stats.instructions > 0) {
                double ips = stats.instructions / elapsed.count();
                std::cout << "  IPS:          " << std::fixed << std::setprecision(2) << (ips / 1e6) << " MIPS\n";
            }
        } else if (cpu32) {
            cpu32->printStats();
            auto stats = cpu32->getStats();
            if (stats.instructions > 0) {
                double ips = stats.instructions / elapsed.count();
                std::cout << "  IPS:          " << std::fixed << std::setprecision(2) << (ips / 1e6) << " MIPS\n";
            }
        } else {
            std::cerr << "Warning: Could not determine CPU type for pipeline stats (AT)\n";
        }
#else
        // LT Models (Default)
        std::cout << "\n=== Pipeline Statistics (LT model) ===\n";
        auto* cpu64 = dynamic_cast<riscv_tlm::CPURV64P2*>(g_top->cpu);
        auto* cpu32 = dynamic_cast<riscv_tlm::CPURV32P2*>(g_top->cpu);
        if (cpu64) {
            auto stats = cpu64->getStats();
            std::cout << "  Pipeline cycles:    " << stats.cycles << "\n";
            std::cout << "  Pipeline stalls:    " << stats.stalls << "\n";
            std::cout << "  Control hazards:    " << stats.control_hazards << "\n";
            std::cout << "  CPI:                1.00 (Estimated)\n";
            std::cout << "  IPC:                1.00 (Estimated)\n";
        } else if (cpu32) {
            auto stats = cpu32->getStats();
            std::cout << "  Pipeline cycles:    " << stats.cycles << "\n";
            std::cout << "  Pipeline stalls:    " << stats.stalls << "\n";
            std::cout << "  Control hazards:    " << stats.control_hazards << "\n";
            std::cout << "  CPI:                1.00 (Estimated)\n";
            std::cout << "  IPC:                1.00 (Estimated)\n";
        } else {
            std::cerr << "Warning: Could not determine CPU type for pipeline stats (LT)\n";
        }
#endif
    }
#endif

    delete g_top;
    g_top = nullptr;

    return 0;
}