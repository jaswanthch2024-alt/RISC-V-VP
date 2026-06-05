// SPDX-License-Identifier: GPL-3.0-or-later
// Unified robust test for RV32 and RV64 architectures
// Compile with: -D__RV32__ for 32-bit or -D__RV64__ for 64-bit
//
// Build RV32:
//   riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib \
//       -D__RV32__ -Wl,--entry=main -Wl,--gc-sections \
//       robust_fast_unified.c -o robust_fast32.elf
//   riscv32-unknown-elf-objcopy -O ihex robust_fast32.elf tests/hex/robust_fast32.hex
//
// Build RV64:
//   riscv64-unknown-elf-gcc -march=rv64imac -mabi=lp64 -O2 -nostdlib \
//       -D__RV64__ -Wl,--entry=main -Wl,--gc-sections \
//       robust_fast_unified.c -o robust_fast64.elf
//   riscv64-unknown-elf-objcopy -O ihex robust_fast64.elf tests/hex/robust_fast64.hex
//
// Run with simulator:
//   ./RISCV_VP -f tests/hex/robust_fast32.hex -R 32
//   ./RISCV_VP -f tests/hex/robust_fast64.hex -R 64

// ============================================================================
// Type definitions (no libc headers needed)
// ============================================================================

typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

#if defined(__RV64__) || defined(__LP64__) || (__SIZEOF_POINTER__ == 8)
    #define XLEN 64
    typedef unsigned long      size_t;
    typedef unsigned long      uintptr_t;
    typedef long               intptr_t;
    typedef unsigned long      reg_t;
    #define REGBYTES 8
#else
    #define XLEN 32
    typedef unsigned int       size_t;
    typedef unsigned int       uintptr_t;
    typedef int                intptr_t;
    typedef unsigned int       reg_t;
    #define REGBYTES 4
#endif

// ============================================================================
// Memory mapped peripherals (must match BusCtrl.h constants)
// ============================================================================

#define TRACE_ADDR             0x40000000u
#define MTIME_LO_ADDR          0x40004000u
#define MTIME_HI_ADDR          0x40004004u
#define MTIMECMP_LO_ADDR       0x40004008u
#define MTIMECMP_HI_ADDR       0x4000400Cu
#define DMA_BASE_ADDRESS       0x30000000u

#define TRACE       (*(volatile uint8_t*)TRACE_ADDR)
#define MTIME_LO    (*(volatile uint32_t*)MTIME_LO_ADDR)
#define MTIME_HI    (*(volatile uint32_t*)MTIME_HI_ADDR)
#define MTIMECMP_LO (*(volatile uint32_t*)MTIMECMP_LO_ADDR)
#define MTIMECMP_HI (*(volatile uint32_t*)MTIMECMP_HI_ADDR)

#define DMA_SRC     (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x00))
#define DMA_DST     (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x04))
#define DMA_LEN     (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x08))
#define DMA_CTRL    (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x0C))

// ============================================================================
// Output helpers
// ============================================================================

static inline void putch(char c) { 
    TRACE = (uint8_t)c; 
}

static void print(const char* s) { 
    while (*s) putch(*s++); 
}

static void print_hex32(uint32_t v) { 
    for (int sh = 28; sh >= 0; sh -= 4) { 
        uint8_t n = (v >> sh) & 0xF; 
        putch(n < 10 ? '0' + n : 'A' + n - 10);
    } 
}

static void print_hex64(uint64_t v) { 
    for (int sh = 60; sh >= 0; sh -= 4) { 
        uint8_t n = (v >> sh) & 0xF; 
        putch(n < 10 ? '0' + n : 'A' + n - 10);
    } 
}

static void print_dec(uint32_t v) {
    char buf[12];
    int i = 0;
    if (v == 0) { putch('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) putch(buf[--i]);
}

// ============================================================================
// CSR helpers (architecture-independent)
// ============================================================================

static inline reg_t read_mstatus(void) { 
    reg_t v; 
    __asm__ volatile("csrr %0, mstatus" : "=r"(v)); 
    return v; 
}

static inline void write_mstatus(reg_t v) { 
    __asm__ volatile("csrw mstatus, %0" :: "r"(v)); 
}

static inline void write_mtvec(reg_t v) { 
    __asm__ volatile("csrw mtvec, %0" :: "r"(v)); 
}

static inline void write_mie(reg_t v) { 
    __asm__ volatile("csrw mie, %0" :: "r"(v)); 
}

static inline uint32_t read_mcycle(void) { 
    uint32_t v; 
    __asm__ volatile("csrr %0, mcycle" : "=r"(v)); 
    return v; 
}

static inline uint32_t read_minstret(void) { 
    uint32_t v; 
    __asm__ volatile("csrr %0, minstret" : "=r"(v)); 
    return v; 
}

// ============================================================================
// Atomic operations (A extension)
// ============================================================================

static inline uint32_t atomic_add(volatile uint32_t* addr, uint32_t val) {
    uint32_t old;
    __asm__ volatile("amoadd.w %0, %2, %1" : "=r"(old), "+A"(*addr) : "r"(val));
    return old;
}

// ============================================================================
// Interrupt handling
// ============================================================================

volatile uint32_t irq_count = 0;
volatile uint32_t dma_done_count = 0;

void trap_handler(void) {
    irq_count++;
    // Re-arm timer
    uint64_t now = ((uint64_t)MTIME_HI << 32) | MTIME_LO;
    uint64_t next = now + 8000ull;
    MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFu);
    MTIMECMP_HI = (uint32_t)(next >> 32);
}

// Architecture-specific trap entry point
#if XLEN == 64
__attribute__((naked)) void _start_trap(void) {
    __asm__ volatile(
        "addi sp, sp, -64\n"
        "sd ra, 56(sp)\n"
        "sd t0, 48(sp)\n"
        "sd t1, 40(sp)\n"
        "sd t2, 32(sp)\n"
        "sd a0, 24(sp)\n"
        "sd a1, 16(sp)\n"
        "call trap_handler\n"
        "ld a1, 16(sp)\n"
        "ld a0, 24(sp)\n"
        "ld t2, 32(sp)\n"
        "ld t1, 40(sp)\n"
        "ld t0, 48(sp)\n"
        "ld ra, 56(sp)\n"
        "addi sp, sp, 64\n"
        "mret\n"
    );
}
#else
__attribute__((naked)) void _start_trap(void) {
    __asm__ volatile(
        "addi sp, sp, -32\n"
        "sw ra, 28(sp)\n"
        "sw t0, 24(sp)\n"
        "sw t1, 20(sp)\n"
        "sw t2, 16(sp)\n"
        "sw a0, 12(sp)\n"
        "sw a1, 8(sp)\n"
        "call trap_handler\n"
        "lw a1, 8(sp)\n"
        "lw a0, 12(sp)\n"
        "lw t2, 16(sp)\n"
        "lw t1, 20(sp)\n"
        "lw t0, 24(sp)\n"
        "lw ra, 28(sp)\n"
        "addi sp, sp, 32\n"
        "mret\n"
    );
}
#endif

static void setup_timer_irq(void) {
    write_mtvec((reg_t)(uintptr_t)_start_trap);
    write_mie(1u << 7);    // MTIE
    write_mstatus(read_mstatus() | 0x8u); // MIE global
    uint64_t now = ((uint64_t)MTIME_HI << 32) | MTIME_LO;
    uint64_t next = now + 8000ull;
    MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFu);
    MTIMECMP_HI = (uint32_t)(next >> 32);
}

// ============================================================================
// Test workload configuration
// ============================================================================

#define BUF_WORDS   256   // Work buffer size
#define DMA_WORDS   128   // DMA transfer size
#define ITERATIONS  50    // Main loop iterations

static uint32_t src_buf[DMA_WORDS];
static uint32_t dst_buf[DMA_WORDS];
static uint32_t work_buf[BUF_WORDS];

// ============================================================================
// Main test function
// ============================================================================

int main(void) {
    // Print architecture info
#if XLEN == 64
    print("[ROBUST-RV64] start\n");
    print("  Architecture: RV64IMAC\n");
#else
    print("[ROBUST-RV32] start\n");
    print("  Architecture: RV32IMAC\n");
#endif
    print("  Buffer words: "); print_dec(BUF_WORDS); putch('\n');
    print("  DMA words: "); print_dec(DMA_WORDS); putch('\n');
    print("  Iterations: "); print_dec(ITERATIONS); putch('\n');

    setup_timer_irq();

    // Initialize buffers
    for (uint32_t i = 0; i < DMA_WORDS; i++) { 
        src_buf[i] = i * 0x01010101u + 0x55AA1234u; 
    }
    for (uint32_t i = 0; i < BUF_WORDS; i++) { 
        work_buf[i] = (i << 1) ^ 0xA5A5A5A5u; 
    }

    // Launch DMA transfer src_buf -> dst_buf
    DMA_SRC = (uint32_t)(uintptr_t)src_buf;
    DMA_DST = (uint32_t)(uintptr_t)dst_buf;
    DMA_LEN = DMA_WORDS * sizeof(uint32_t);
    DMA_CTRL = 1u; // start

    // Mixed compute while DMA in flight
    volatile uint32_t atomic_counter = 0;
    uint64_t accum = 0;
    
    for (uint32_t outer = 0; outer < ITERATIONS; ++outer) {
        // Arithmetic mix (M extension ops: mul, div)
        uint32_t a = work_buf[(outer * 13) & (BUF_WORDS - 1)];
        uint32_t b = work_buf[(outer * 29) & (BUF_WORDS - 1)];
        uint32_t mul = a * b + (b ? (a / (b | 1)) : 0);
        accum += mul;

        // Atomic updates (A extension)
        atomic_add(&atomic_counter, 1);

        // Memory walking (stress caches / bus)
        for (uint32_t i = outer & 0x3; i < BUF_WORDS; i += 32) { 
            work_buf[i] ^= (outer + i); 
        }

        // Poll DMA completion occasionally
        if ((outer & 0x7) == 0) {
            if ((DMA_CTRL & 1u) == 0) { 
                dma_done_count++; 
            }
        }

        // Memory barrier
        __asm__ volatile("fence iorw, iorw");
    }

    // Wait for DMA to complete
    while (DMA_CTRL & 1u) { /* spin */ }
    dma_done_count++;

    // Verify DMA correctness
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < DMA_WORDS; i++) { 
        if (dst_buf[i] != src_buf[i]) { 
            mismatches++; 
        } 
    }

    // Gather performance counters
    uint32_t ccycle = read_mcycle();
    uint32_t cinstr = read_minstret();

    // Print results
    print("\n[RESULTS]\n");
    print("  accum=0x"); print_hex64(accum); putch('\n');
    print("  atomic_count="); print_dec(atomic_counter); putch('\n');
    print("  dma_done="); print_dec(dma_done_count); putch('\n');
    print("  dma_mismatch="); print_dec(mismatches); putch('\n');
    print("  irq_count="); print_dec(irq_count); putch('\n');
    print("  mcycle="); print_dec(ccycle); putch('\n');
    print("  minstret="); print_dec(cinstr); putch('\n');

    // Final status
    if (mismatches == 0) {
        print("\n[ROBUST] PASS - All tests completed successfully\n");
    } else {
        print("\n[ROBUST] FAIL - DMA verification failed\n");
    }

#if XLEN == 64
    print("[ROBUST-RV64] end\n");
#else
    print("[ROBUST-RV32] end\n");
#endif

    __asm__ volatile("ecall"); // signal end to simulator
    return 0;
}
