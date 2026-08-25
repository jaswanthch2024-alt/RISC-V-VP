## This project requires systemC to be compiled with cmake using C++17:
## cd <systemc directory>
## mkdir build
## cd build
## cmake ../ -DCMAKE_CXX_STANDARD=17
## make

cmake_minimum_required(VERSION 3.10)
project(RISCV_VP)

# Prefer Ninja generator if available
message(STATUS "Using Ninja generator")
 
# Use C++20 on MSVC to support [[likely]] attribute; otherwise default to 17
if(MSVC)
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Build Options
# =============================================================================
option(ENABLE_STRICT "Treat warnings as errors" OFF)
option(BUILD_DOC "Build documentation" ON)
option(BUILD_TESTING "Build tests" OFF)
option(ENABLE_PIPELINED_ISS "Enable pipelined ISS" ON)
option(USE_LOCAL_SYSTEMC "Use vendored SystemC located in systemc/ subdir" ON)
option(BUILD_ROBUST_HEX "Build robust_system_test hex images" ON)

# Timing Model Selection (mutually exclusive)
set(TIMING_MODEL "LT" CACHE STRING "CPU Timing Model: LT, AT, CYCLE, CYCLE6, CYCLE6_AXI, or CYCLE6_AXI128")
set_property(CACHE TIMING_MODEL PROPERTY STRINGS "LT" "AT" "CYCLE" "CYCLE6" "CYCLE6_AXI" "CYCLE6_AXI128")

# Validate timing model
if(NOT TIMING_MODEL MATCHES "^(LT|AT|CYCLE|CYCLE6|CYCLE6_AXI|CYCLE6_AXI128)$")
  message(FATAL_ERROR "Invalid TIMING_MODEL: ${TIMING_MODEL}. Must be LT, AT, CYCLE, CYCLE6, CYCLE6_AXI, or CYCLE6_AXI128.")
endif()

message(STATUS "========================================")
message(STATUS "Timing Model: ${TIMING_MODEL}")
message(STATUS "========================================")

include_directories(./inc/)

# Try system installation first
find_package(SystemC QUIET)

if(NOT SystemC_FOUND AND USE_LOCAL_SYSTEMC)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systemc/CMakeLists.txt")
    message(STATUS "SystemC not found via find_package. Using local systemc/ subdirectory.")
    add_subdirectory(systemc)
    # The upstream build usually defines target 'systemc'. Provide an alias matching find_package usage.
    if(TARGET systemc AND NOT TARGET SystemC::systemc)
      add_library(SystemC::systemc ALIAS systemc)
    endif()
    set(SystemC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/systemc/src")
  else()
    message(FATAL_ERROR "SystemC not found. Clone it into systemc/ or install it so find_package(SystemC) works.")
  endif()
endif()

if(TARGET SystemC::systemc)
  message(STATUS "SystemC target available: SystemC::systemc")
else()
  message(FATAL_ERROR "Failed to obtain SystemC library target.")
endif()

message(STATUS "SystemC include dirs: ${SystemC_INCLUDE_DIRS}")

# Prefer system spdlog, fall back to bundled
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
  message(STATUS "spdlog not found via package config. Using bundled spdlog subdirectory.")
  add_subdirectory(spdlog)
endif()

# =============================================================================
# Source File Collection
# =============================================================================
file(GLOB SRC_ALL "src/*.cpp")

set(SRC_SIMULATOR "src/Simulator.cpp")
set(SRC_VP_MAIN  "src/VPMain.cpp")

# Core sources - exclude mains and timing-model-specific files
set(SRC_CORE ${SRC_ALL})
list(FILTER SRC_CORE EXCLUDE REGEX ".*/(Simulator|VPMain)\\.cpp$")
list(FILTER SRC_CORE EXCLUDE REGEX ".*/M_extension\\.cpp$")
# Exclude all 7-stage pipeline files
list(FILTER SRC_CORE EXCLUDE REGEX ".*/CPU_P32\\.cpp$")
list(FILTER SRC_CORE EXCLUDE REGEX ".*/CPU_P64\\.cpp$")
# Exclude all 2-stage timing variants (will add selected ones below)
list(FILTER SRC_CORE EXCLUDE REGEX ".*/CPU_P32_2.*\\.cpp$")
list(FILTER SRC_CORE EXCLUDE REGEX ".*/CPU_P64_2.*\\.cpp$")


# =============================================================================
# Core Library
# =============================================================================
# Core library with all SystemC/TLM modules
add_library(riscv_vp_core ${SRC_CORE})
set_property(TARGET riscv_vp_core PROPERTY POSITION_INDEPENDENT_CODE ON)
if(TARGET spdlog::spdlog)
  target_link_libraries(riscv_vp_core PUBLIC SystemC::systemc spdlog::spdlog)
else()
  target_link_libraries(riscv_vp_core PUBLIC SystemC::systemc spdlog::spdlog_header_only)
endif()

# Target-specific warnings and optimizations
if(MSVC)
  target_compile_options(riscv_vp_core PRIVATE /W3 /EHsc /permissive-)
  if(ENABLE_STRICT)
    target_compile_options(riscv_vp_core PRIVATE /WX)
  endif()
  # Do not force /O2 here to avoid conflict with Debug runtime flags (/RTC1). Use generator-set configs.
else()
  target_compile_options(riscv_vp_core PRIVATE -O3 -g -Wall -Wextra -Wunused-function -Wpedantic)
  if(ENABLE_STRICT)
    target_compile_options(riscv_vp_core PRIVATE -Werror)
  endif()
endif()

# Allow deprecated IEEE API usages (SC_HAS_PROCESS etc.)
target_compile_definitions(riscv_vp_core PRIVATE SC_ALLOW_DEPRECATED_IEEE_API)

# Ensure public headers are visible to dependents
target_include_directories(riscv_vp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc ${SystemC_INCLUDE_DIRS})

# If pipelined ISS is enabled, add 2-stage pipeline sources (for VP only)
# If pipelined ISS is enabled, select the appropriate CPU model implementation
if(ENABLE_PIPELINED_ISS)
  if(TIMING_MODEL STREQUAL "CYCLE6")
    # CYCLE6 (6-Stage Cycle-Accurate) model
    target_sources(riscv_vp_core PRIVATE
      src/CPU_P32_6_Cycle.cpp
      src/CPU_P64_6_Cycle.cpp
    )
    target_compile_definitions(riscv_vp_core PUBLIC ENABLE_PIPELINED_ISS=1 ENABLE_CYCLE6_MODEL=1)
    message(STATUS "Pipelined ISS enabled with CYCLE6 (6-Stage Cycle-Accurate) model")
  elseif(TIMING_MODEL STREQUAL "CYCLE6_AXI" OR TIMING_MODEL STREQUAL "CYCLE6_AXI128")
    # CYCLE6_AXI: same 6-stage pipeline + real matchlib AxiArbiter I$/D$
    # contention model (microbenchmark-only; NOT for Linux boot). Adds the
    # vendored, header-only matchlib kit and its required defines. Builds
    # against the VP's own SystemC 3.0.2 (verified in Phase 0).
    #
    # CYCLE6_AXI128 is the same subsystem built against a 128-bit AXI data
    # width (inc/axi/AxiConfigWide.h) instead of the RTL-accurate 64-bit
    # default -- a compile-time build variant (matchlib AXI types are C++
    # templates on the config, so this can't be a runtime flag like
    # VP_AXI_BEATS/VP_CACHE_POLICY/VP_DCACHE_SLOTS). Comparison-only: no CVA6
    # target has ever shipped a 128-bit AXI config, see AxiConfigWide.h.
    set(MATCHLIB_KIT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/matchlib_kit")
    if(NOT EXISTS "${MATCHLIB_KIT}/matchlib-main/cmod/include/axi/AxiArbiter.h")
      message(FATAL_ERROR "${TIMING_MODEL} needs the vendored matchlib kit at ${MATCHLIB_KIT} (missing).")
    endif()
    target_sources(riscv_vp_core PRIVATE
      src/CPU_P32_6_Cycle.cpp
      src/CPU_P64_6_Cycle.cpp
      src/axi/AxiContentionTop.cpp
    )
    target_include_directories(riscv_vp_core PUBLIC
      ${MATCHLIB_KIT}/matchlib_examples/include
      ${MATCHLIB_KIT}/matchlib-main/cmod/include
      ${MATCHLIB_KIT}/matchlib_connections-master/include
      ${MATCHLIB_KIT}/ac_types-master/include
      ${MATCHLIB_KIT}/ac_simutils-master/include
      ${MATCHLIB_KIT}/ac_math-master/include
      ${MATCHLIB_KIT}/rapidjson-master/include
      ${MATCHLIB_KIT}/Boost-Preprocessor-master/include
    )
    target_compile_definitions(riscv_vp_core PUBLIC
      ENABLE_PIPELINED_ISS=1 ENABLE_CYCLE6_MODEL=1 ENABLE_AXI_CONTENTION=1
      CONNECTIONS_ACCURATE_SIM=1 HLS_CATAPULT=1 ENABLE_AC_VCD_TRACE=0)
    if(TIMING_MODEL STREQUAL "CYCLE6_AXI128")
      target_compile_definitions(riscv_vp_core PUBLIC AXI_WIDE128=1)
    endif()
    # matchlib headers trip -Wall/-Wextra/-Wpedantic heavily; relax for the core
    # in this target so the (unmodified, vendored) kit compiles clean.
    target_compile_options(riscv_vp_core PRIVATE -Wno-error -Wno-unused-parameter -Wno-unused-variable -Wno-deprecated-declarations)
    message(STATUS "Pipelined ISS enabled with ${TIMING_MODEL} (matchlib AxiArbiter I\$/D\$ contention)")
  elseif(TIMING_MODEL STREQUAL "CYCLE")
    # CYCLE (Cycle-Accurate) model - uses blocking transport with cycle-accurate logic
    target_sources(riscv_vp_core PRIVATE 
      src/CPU_P32_2_Cycle.cpp 
      src/CPU_P64_2_Cycle.cpp
    )
    target_compile_definitions(riscv_vp_core PUBLIC ENABLE_PIPELINED_ISS=1 ENABLE_CYCLE_MODEL=1)
    message(STATUS "Pipelined ISS enabled with CYCLE (Cycle-Accurate) model")
  elseif(TIMING_MODEL STREQUAL "AT")
    # AT (Approximately-Timed) model - uses non-blocking transport
    # Note: Using standard Bus/Memory (LT) as per user request
    target_sources(riscv_vp_core PRIVATE 
      src/CPU_P32_2_AT.cpp 
      src/CPU_P64_2_AT.cpp
    )
    target_compile_definitions(riscv_vp_core PUBLIC ENABLE_PIPELINED_ISS=1 ENABLE_AT_MODEL=1)
    message(STATUS "Pipelined ISS enabled with AT (Approximately-Timed) model")
  else()
    # LT (Loosely-Timed) model - uses blocking transport (default)
    # Uses explicit wait() calls for simple simulation pacing
    target_sources(riscv_vp_core PRIVATE 
      src/CPU_P32_2.cpp 
      src/CPU_P64_2.cpp
    )
    target_compile_definitions(riscv_vp_core PUBLIC ENABLE_PIPELINED_ISS=1)
    message(STATUS "Pipelined ISS enabled with LT (Loosely-Timed) model")
  endif()
  target_include_directories(riscv_vp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
endif()

# Always add simple non-pipelined CPU (for TLM simulator)
target_sources(riscv_vp_core PRIVATE 
  src/CPU_Simple.cpp
)

# =============================================================================
# Executables
# =============================================================================

# Legacy simulator (uses non-pipelined LT CPU)
add_executable(RISCV_TLM ${SRC_SIMULATOR})
target_link_libraries(RISCV_TLM PRIVATE riscv_vp_core)
if(NOT MSVC)
  target_compile_options(RISCV_TLM PRIVATE -O3)
endif()

# Virtual Prototype executable
add_executable(RISCV_VP ${SRC_VP_MAIN} src/VPTop.cpp)
target_link_libraries(RISCV_VP PRIVATE riscv_vp_core)
if(NOT MSVC)
  target_compile_options(RISCV_VP PRIVATE -O3)
endif()

# =============================================================================
# Print Configuration Summary
# =============================================================================
message(STATUS "")
message(STATUS "Build Configuration:")
message(STATUS "  Timing Model:     ${TIMING_MODEL}")
message(STATUS "  Pipelined ISS:    ${ENABLE_PIPELINED_ISS}")
message(STATUS "  C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "")

# =============================================================================
# Robust test hex targets
# =============================================================================
if(BUILD_ROBUST_HEX)
  find_program(RISCV32_GCC riscv32-unknown-elf-gcc)
  find_program(RISCV32_OBJCOPY riscv32-unknown-elf-objcopy)
  find_program(RISCV64_GCC riscv64-unknown-elf-gcc)
  find_program(RISCV64_OBJCOPY riscv64-unknown-elf-objcopy)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex)
  if(RISCV64_GCC AND RISCV64_OBJCOPY)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/robust_system_test64.hex
      COMMAND ${RISCV64_GCC} -march=rv64imac -mabi=lp64 -O2 -nostdlib -Wl,--entry=main -Wl,--gc-sections
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/full_system/robust_system_test.c -o robust_rv64.elf
      COMMAND ${RISCV64_OBJCOPY} -O ihex robust_rv64.elf ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/robust_system_test64.hex
      BYPRODUCTS robust_rv64.elf
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Building RV64 robust_system_test hex"
      VERBATIM)
    add_custom_target(robust_rv64_hex DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/robust_system_test64.hex)
  else()
    message(STATUS "RV64 toolchain not found; robust_system_test64.hex target disabled")
  endif()
  if(RISCV32_GCC AND RISCV32_OBJCOPY)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/robust_system_test.hex
      COMMAND ${RISCV32_GCC} -march=rv32imac -mabi=ilp32 -O2 -nostdlib -Wl,--entry=main -Wl,--gc-sections
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/full_system/robust_system_test.c -o robust_rv32.elf
      COMMAND ${RISCV32_OBJCOPY} -O ihex robust_rv32.elf ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/robust_system_test.hex
      BYPRODUCTS robust_rv32.elf
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Building RV32 robust_system_test hex"
      VERBATIM)
    add_custom_target(robust_rv32_hex DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/robust_system_test.hex)
  endif()
endif()

# =============================================================================
# Dhrystone Benchmark Build Targets
# =============================================================================
if(BUILD_ROBUST_HEX)
  # Dhrystone for RV32
  if(RISCV32_GCC AND RISCV32_OBJCOPY)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/dhrystone32.hex
      COMMAND ${RISCV32_GCC} -march=rv32imac -mabi=ilp32 -O2 -nostdlib 
              -Wl,--entry=main -Wl,--gc-sections
              -I${CMAKE_CURRENT_SOURCE_DIR}/tests/C/dhrystone
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/C/dhrystone/dhrystone.c
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/C/dhrystone/helper_functions.c
              -o dhrystone_rv32.elf
      COMMAND ${RISCV32_OBJCOPY} -O ihex dhrystone_rv32.elf 
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/dhrystone32.hex
      BYPRODUCTS dhrystone_rv32.elf
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Building RV32 Dhrystone benchmark hex"
      VERBATIM)
    add_custom_target(dhrystone_rv32_hex DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/dhrystone32.hex)
    message(STATUS "Dhrystone RV32 benchmark target enabled")
  endif()

  # Dhrystone for RV64
  if(RISCV64_GCC AND RISCV64_OBJCOPY)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/dhrystone64.hex
      COMMAND ${RISCV64_GCC} -march=rv64imac -mabi=lp64 -O2 -nostdlib -ffreestanding 
              -Wl,--entry=main -Wl,--gc-sections
              -I${CMAKE_CURRENT_SOURCE_DIR}/tests/C/dhrystone
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/C/dhrystone/dhrystone.c
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/C/dhrystone/helper_functions.c
              -o dhrystone_rv64.elf
      COMMAND ${RISCV64_OBJCOPY} -O ihex dhrystone_rv64.elf 
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/dhrystone64.hex
      BYPRODUCTS dhrystone_rv64.elf
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Building RV64 Dhrystone benchmark hex"
      VERBATIM)
    add_custom_target(dhrystone_rv64_hex DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/hex/dhrystone64.hex)
    message(STATUS "Dhrystone RV64 benchmark target enabled")
  endif()
endif()

# Combined benchmark target
add_custom_target(benchmarks)
if(TARGET dhrystone_rv32_hex)
  add_dependencies(benchmarks dhrystone_rv32_hex)
endif()
if(TARGET dhrystone_rv64_hex)
  add_dependencies(benchmarks dhrystone_rv64_hex)
endif()
if(TARGET robust_rv32_hex)
  add_dependencies(benchmarks robust_rv32_hex)
endif()
if(TARGET robust_rv64_hex)
  add_dependencies(benchmarks robust_rv64_hex)
endif()

find_package(Doxygen)
if (DOXYGEN_FOUND AND BUILD_DOC)
    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_SOURCE_DIR}/doc)
    message(STATUS "Doxygen build enabled")
    add_custom_target(doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_IN}
        WORKING_DIRECTORY ${DOXYGEN_OUT}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM)
else()
    if(BUILD_DOC)
      message(STATUS "Doxygen not found; documentation will not be generated")
    endif()
endif()

# =============================================================================
# riscv-tests conformance runner
# =============================================================================
find_program(RISCV64_OBJCOPY_FOR_TESTS riscv64-unknown-elf-objcopy)
if(RISCV64_OBJCOPY_FOR_TESTS)
  add_custom_target(riscv-tests
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_riscv_tests.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running riscv-tests ISA conformance suite (rv64ui-p, rv64si-p, rv64mi-p)"
    VERBATIM
  )
  message(STATUS "Target 'riscv-tests' enabled")
else()
  message(STATUS "riscv64-unknown-elf-objcopy not found; 'riscv-tests' target disabled")
endif()
