# RISC-V TLM Virtual Platform – Project Overview

This document summarizes the structure and behavior of the SystemC/TLM RISC?V virtual platform contained in this repository.

## Build and Dependencies
- CMake (>= 3.10), C++17, Ninja generator
- Required packages:
  - SystemC (found via `cmake/FindSystemC.cmake`)
  - Boost headers
  - spdlog (config-based if available; otherwise bundled `spdlog` dir)
- Top-level: `CMakeLists.txt`
  - Builds a core library and two executables

## Targets
- Library: `riscv_tlm_core`
  - All sources except the two `sc_main` units
- Executables:
  - `RISCV_TLM` ? legacy simulator entry (`src/Simulator.cpp`)
  - `RISCV_VP`  ? modern VP entry (`src/VPMain.cpp`)

## Execution Flow
1. `sc_main` parses CLI and sets SystemC time resolution (1 ns)
2. Load an Intel HEX into `Memory` and read start PC
3. Instantiate and wire modules: CPU, Bus, Memory, Timer, Trace (and Debug on POSIX when requested)
4. Start the simulation (`sc_start`)
5. At end, print runtime and instructions/sec; optionally dump memory window

## Top-level Composition
- `inc/VPTop.h`, `src/VPTop.cpp`
  - Assembles the system:
    - CPU: `riscv_tlm::CPURV32` or `riscv_tlm::CPURV64`
    - Bus: `riscv_tlm::BusCtrl`
    - Memory: `riscv_tlm::Memory`
    - Peripherals: `riscv_tlm::peripherals::Timer`, `Trace`
    - Optional GDB debug server (`riscv_tlm::Debug`, POSIX only) when `-D` is passed

## Bus and Address Map
- `inc/BusCtrl.h`, `src/BusCtrl.cpp`
  - TLM target for CPU instruction/data, initiators to Memory/Trace/Timer
  - Address decoding (word-aligned addressing):
    - `TRACE_MEMORY_ADDRESS` (0x4000_0000): write a byte ? console/xterm output
    - `TIMER_MEMORY_ADDRESS_{LO,HI}` (0x4000_4000/04): `mtime` registers
    - `TIMERCMP_MEMORY_ADDRESS_{LO,HI}` (0x4000_4008/0C): `mtimecmp` registers
    - `TO_HOST_ADDRESS` (0x9000_0000): stops simulation
  - Pass-through DMI support for instruction fetch

## Memory
- `inc/Memory.h`, `src/Memory.cpp`
  - 16 MB byte-addressable memory (`std::array<uint8_t, SIZE>`)
  - TLM target implementing `b_transport`, `get_direct_mem_ptr`, `transport_dbg`
  - Loads Intel HEX files, sets the initial PC (`getPCfromHEX()`)
  - Exposes a DMI region when safe (no relocation offset was used)

## Timer (CLINT-like)
- `inc/Timer.h`, `src/Timer.cpp`
  - TLM target registers: `mtime`, `mtimecmp` (low/high via 32-bit accesses)
  - When `mtime >= mtimecmp`, raises a machine-timer interrupt via an initiator socket to the CPU (`irq_line`)

## Trace (UART-like console)
- `src/Trace.cpp`
  - POSIX: launches xterm using a PTY and writes characters; set `TRACE_STDOUT=1` or unset `DISPLAY` to force stdout
  - Windows: prints directly to stdout

## CPU Base and Derivatives
- `inc/CPU.h`, `src/CPU.cpp`, `src/RV32.cpp`, `src/RV64.cpp`
  - `CPU` base:
    - Instruction initiator socket, IRQ target socket
    - Optional DMI for instruction fetch
    - `CPU_thread()` loop (when not in debug mode):
      - Fetch ? Decode ? Execute one instruction
      - Process pending IRQs
      - Wait fixed 10 ns per instruction (or use quantum keeper if enabled)
  - `CPURV32`, `CPURV64`:
    - Hold `Registers<T>`, `MemoryInterface`, and ISA handlers:
      - `BASE_ISA<T>` (RV32/64 base)
      - `M_extension<T>` (multiplication/division)
      - `A_extension<T>` (atomics)
      - `C_extension<T>` (compressed)
    - Fetch path prefers DMI; falls back to `b_transport`
    - On `cpu_process_IRQ()` uses CSRs (`MSTATUS`, `MIP`, `MEPC`, `MCAUSE`, `MTVEC`) to vector into handler

## Instruction and ISA Decoders
- `inc/Instruction.h`, `src/Instruction.cpp`
  - Wraps 32-bit instruction and identifies which extension applies
- `inc/BASE_ISA.h`, `src/BASE_ISA.cpp`
  - Complete RV32/64 integer ISA: ALU, branches, loads/stores, JAL/JALR, CSR ops, FENCE, {E,EBREAK}, {MRET,SRET,WFI,SFENCE}
  - Template specializations for RV32 vs RV64 immediates and shifting
  - `decode()` ? `opCodes`; `exec_instruction()` executes and indicates if PC auto-increments
- `inc/M_extension.h`, `src/M_extension.cpp`
  - M extension ops: `MUL{,H,HSU,HU}`, `DIV{,U}`, `REM{,U}` + W-variants on RV64
  - Uses 128-bit `sc_bigint` where needed on RV64
- `inc/A_extension.h`, `src/A_extension.cpp`
  - Atomics: `LR/SC`, `AMOSWAP/ADD/XOR/AND/OR/MIN/MAX/MINU/MAXU`
  - Simple reservation set models LR/SC success/failure
- `inc/C_extension.h`, `src/C_extension.cpp`
  - Compressed 16-bit op decoding and execution for RV32/64 (arithmetic, loads/stores, control flow)
- `inc/F_extension.h`, `src/F_extension.cpp`
  - Placeholder (disabled) for F extension

## Registers and CSRs
- `inc/Registers.h`, `src/Registers.cpp`
  - Template register file (`T = uint32_t | uint64_t`)
  - 32 GPRs, PC, and a CSR map
  - `CYCLE/TIME` read from SystemC time; hi halves available
  - CSR constants and status/interrupt bit fields defined; `MISA` initialized per XLEN

## Data-side Memory Interface
- `inc/MemoryInterface.h`, `src/MemoryInterface.cpp`
  - `readDataMem(addr, size)`, `writeDataMem(addr, data, size)`
  - Simple initiator socket used by ISA handlers for loads/stores

## Performance and Logging
- `inc/Performance.h`, `src/Performance.cpp`
  - Singleton counts memory/register accesses and instruction retirements
  - Printed at end and used for IPS calculation in `sc_main`
- Logging via `spdlog` (logger name `my_logger` when enabled)

## GDB Remote Debug (POSIX only)
- `src/Debug.cpp`
  - Minimal GDB RSP server on port 1234
  - Supports register/memory read, breakpoints, single-step and continue
  - Creates its own thread of execution inside the SystemC process when `-D`

## Entrypoints
- `src/VPMain.cpp` (preferred):
  - Options: `-f <file.hex>`, `-R 32|64`, `-D` (debug)
  - Prints run summary and IPS
- `src/Simulator.cpp` (legacy):
  - Extra options: logging level, memory dump window (`-T/-B/-E`)

## Run Hints
- Ensure SystemC is compiled with C++17
- On Linux/WSL, set `TRACE_STDOUT=1` to avoid xterm dependency
- Typical run: `RISCV_VP -f tests/hex/hello_fixed.hex -R 32`

---
Generated for the repository at the time of writing; see sources for details.
