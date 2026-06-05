# RISC-V TLM Virtual Prototype (RISCV-VP)

A configurable, multi-timing model RISC-V instruction set simulator (Virtual Prototype) implemented in SystemC and TLM-2.0. This project supports both RV32IMAC and RV64IMAC instruction set architectures, offering a unified simulation environment that spans from fast functional simulation (Loosely-Timed) to cycle-accurate micro-architectural simulation.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![SystemC](https://img.shields.io/badge/SystemC-2.3.3+-green.svg)](https://www.accellera.org/downloads/standards/systemc)

---

## Project Overview

RISCV-VP is designed to bridge the gap between software development and hardware verification. The architecture allows switching between different timing abstractions within a single codebase to support various stages of design exploration.

### Simulation Timing Tiers

The simulator supports four distinct timing models depending on the required speed and accuracy:

*   **LT (Loosely-Timed)**: Functional execution using Temporal Decoupling. This is the fastest tier (approaching ~100 MIPS), designed specifically for software verification, application testing, and booting operating systems.
*   **AT (Approximately-Timed)**: Models transaction phases, bus contention, and protocol delays. Best suited for bus/interconnect latency studies and protocol analysis.
*   **Cycle (2-Stage)**: A basic clock-synchronized hardware pipeline (Fetch/Execute) used for hardware-in-the-loop and basic timing checks.
*   **Cycle6 (6-Stage)**: A highly detailed, cycle-accurate micro-architectural model aligned with the CVA6 (Ariane) application-class processor. It implements pipelined execution with hazard detection and branch prediction.

---

## Current Focus & Work-in-Progress

The core development focus is on the **Cycle6 (6-Stage Pipeline)** model located in `src/CPU_P64_6_Cycle.cpp`.

### 6-Stage Pipeline Specifications
*   **Pipeline Structure**: Fetch → Decode → Issue → Execute → Writeback/Commit.
*   **Hazard Handling**: Scoreboard-based hazard detection for Read-After-Write (RAW) sequences and resource conflicts.
*   **Branch Prediction**: Reaches an IPC of ~0.816 using a modular branch predictor:
    *   128-entry Branch Target Buffer (BTB)
    *   256-entry 2-bit Branch History Table (BHT)
    *   4-entry Return Address Stack (RAS)
*   **Memory Subsystem**: Scoreboard hazard detection combined with a split store buffer.
*   **Recent Improvements**: Fixed compilation errors and added missing RV64 word-level operations (`SLLIW`, `SRLIW`, `SRAIW`, `SLLW`, `SRLW`, `SRAW`).

---

## Roadmap & What Needs To Be Done

The following table summarizes active development priorities, missing features, and open tasks:

### High Priority (Blocking Test Pass Rate)

| Task / Feature | Status | Details |
| :--- | :--- | :--- |
| **Misaligned memory traps** | Failing | 8 RV64 and 5 RV32 test cases currently fail due to missing traps on unaligned loads/stores (`misalign-ld/lh/lw/sd/sh/sw`). We need to implement proper exception raising on unaligned access inside the BASE_ISA handlers (`src/BASE_ISA.cpp`). |
| **Verify RV64 word-ops** | Pending | The newly implemented word-level opcodes (SLLIW, SRLIW, SRAIW, SLLW, SRLW, SRAW) need a full suite test pass to verify correctness under all pipeline hazard scenarios. |

### Medium Priority

| Task / Feature | Status | Details |
| :--- | :--- | :--- |
| **MMU / sv39 integration** | Missing | The Memory Management Unit exists in other functional models but is not yet wired into the Cycle6 pipeline. This integration is required to boot virtualized operating systems (like Linux) on the 6-stage model. |
| **C-extension in Cycle6** | Isolated | Compressed instruction support was isolated to simplify hazards. It needs to be carefully re-integrated with the scoreboard hazard detection logic. |
| **PLIC and DMA integration** | Stub Only | The Platform-Level Interrupt Controller (PLIC) and Direct Memory Access (DMA) are currently stubbed out in Cycle6 and need full timing integration. |

### Low Priority

| Task / Feature | Status | Details |
| :--- | :--- | :--- |
| **F-extension (Floating-Point)** | Stub Only | The interface header (`F_extension.h`) exists, but the floating-point unit and its pipeline stages are not yet implemented or integrated. |
| **Cache hierarchy timing** | Missing | The current model supports TLB simulation, but lacks L1/L2 data/instruction cache timing and coherence penalty models. |
| **Branch predictor tuning** | Functional | The current predictor works well but can be tuned further for higher prediction accuracy and branch classification. |

---

## Test Coverage Summary

We run standard RISC-V compliance and unit test suites to verify functional correctness:

*   **RV64**: **56 / 64** tests passing. All 8 failures are due to missing misaligned memory access traps.
*   **RV32**: **29 / 29** I-extension + C/M tests passing. 5 failures remain, all due to the same misaligned memory trap issue.

*Fixing the unaligned trap logic in `src/BASE_ISA.cpp` is the highest impact target, as it will instantly close all 13 remaining test failures across the simulator.*

---

## Build & Execution Instructions

### Prerequisites
*   CMake 3.10 or higher
*   C++17 compliant compiler (GCC 7+, Clang 6+, MSVC 2019+)
*   SystemC 2.3.3 (Bundled as a submodule)

### Building from Source

1.  **Clone the Repository with Submodules**:
    ```bash
    git clone --recurse-submodules https://github.com/jaswanthch2024-alt/RISCV-VP.git
    cd RISCV-VP
    ```

2.  **Configure and Build**:
    You can choose the timing tier during the CMake configuration step by setting the `TIMING_MODEL` variable:

    *To build the 6-stage Cycle-Accurate model (Cycle6):*
    ```bash
    mkdir build && cd build
    cmake -DTIMING_MODEL=CYCLE6 ..
    make -j$(nproc)
    ```

    *To build the Loosely-Timed functional model (LT, Default):*
    ```bash
    mkdir build && cd build
    cmake -DTIMING_MODEL=LT ..
    make -j$(nproc)
    ```

### Execution Options

Run the virtual prototype using the `RISCV_VP` executable:
```bash
./RISCV_VP -f <hex_file_path> -R <32|64> [options]
```

*   `-f <file>`: Path to the input hex program.
*   `-R <32|64>`: Select target architecture register width (32-bit or 64-bit).
*   `-L <level>`: Logging level (0 = Critical, 3 = Info, 4 = Trace).
*   `-D`: Enable the integrated GDB Remote Serial Protocol (RSP) server (default port `5005`) for source-level debugging.

**Example Running 64-bit Test:**
```bash
./RISCV_VP -f ../tests/hex/robust_system_test.hex -R 64 -L 3
```

---

## Project Structure

*   `inc/`: Header files for the processor cores, peripherals, and system interfaces.
*   `src/`: Core implementation source files.
    *   `src/CPU_P64_6_Cycle.cpp`: Implementation of the 6-stage pipelined core.
    *   `src/CPU_P64_2.cpp`: Implementation of the Loosely-Timed (LT) processor core.
    *   `src/BASE_ISA.cpp`: Core ISA execution and instruction handlers.
*   `tests/`: Verification suites, hex binaries, and performance benchmarks (e.g., Dhrystone).
*   `cmake/`: CMake build helpers and configuration scripts.

---

## Acknowledgments

This project is built upon the [RISC-V TLM Simulator](https://github.com/mariusmm/RISC-V-TLM) originally developed by **Màrius Montón**. The core TLM-2.0 infrastructure and functional models are based on his work. We have extended the simulator with:
1.  **CVA6-Aligned Cycle-Accurate Pipeline**: A new 6-stage pipeline model.
2.  **Multi-Timing Architecture**: Enhanced configuration system for switching between timing models.

## Citation

If you use this simulator in your research, please cite the original work:

```bibtex
@inproceedings{montonriscvtlm2020,
    title = {A {RISC}-{V} {SystemC}-{TLM} simulator},
    booktitle = {Workshop on {Computer} {Architecture} {Research} with {RISC}-{V} ({CARRV 2020})},
    author = {Montón, Màrius},
    year = {2020}
}
```

## License

This project is licensed under the GNU General Public License v3.0. See the `LICENSE` file for details.
