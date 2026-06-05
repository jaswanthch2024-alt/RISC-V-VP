// SPDX-License-Identifier: GPL-3.0-or-later
// Robust full-system RISC-V bare?metal test exercising timer IRQs, DMA, atomics,
// arithmetic (M extension), memory bandwidth and Trace output.
// Designed to generate diverse instruction & memory mixes and observable counters.
// Build (RV32IMAC example):
//   riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib -Wl,--entry=main \
//      -Wl,--gc-sections robust_system_test.c -o robust_system_test.elf
//   riscv32-unknown-elf-objcopy -O ihex robust_system_test.elf robust_system_test.hex
// Run with simulator: ./RISCV_VP -f tests/hex/robust_system_test.hex -R 32
// (Adjust path after converting .hex.)

#include <stdint.h>
#include <stddef.h>

// Memory mapped peripherals (must match BusCtrl.h constants)
#define TRACE_ADDR             0x40000000u
#define MTIME_LO_ADDR          0x40004000u
#define MTIME_HI_ADDR          0x40004004u
#define MTIMECMP_LO_ADDR       0x40004008u
#define MTIMECMP_HI_ADDR       0x4000400Cu
#define DMA_BASE_ADDRESS       0x30000000u

#define TRACE      (*(volatile uint8_t*)TRACE_ADDR)
#define MTIME_LO   (*(volatile uint32_t*)MTIME_LO_ADDR)
#define MTIME_HI   (*(volatile uint32_t*)MTIME_HI_ADDR)
#define MTIMECMP_LO (*(volatile uint32_t*)MTIMECMP_LO_ADDR)
#define MTIMECMP_HI (*(volatile uint32_t*)MTIMECMP_HI_ADDR)

// DMA registers (see peripherals/DMA.h) layout: src,dst,len,control(start bit=1)
#define DMA_SRC    (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x00))
#define DMA_DST    (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x04))
#define DMA_LEN    (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x08))
#define DMA_CTRL   (*(volatile uint32_t*)(DMA_BASE_ADDRESS + 0x0C))

// Simple output
static inline void putch(char c){ TRACE = (uint8_t)c; }
static void print(const char* s){ while(*s) putch(*s++); }

// CSR helpers
static inline uint32_t read_mstatus(){ uint32_t v; asm volatile("csrr %0,mstatus":"=r"(v)); return v; }
static inline void     write_mstatus(uint32_t v){ asm volatile("csrw mstatus,%0"::"r"(v)); }
static inline void     write_mtvec(uint32_t v){ asm volatile("csrw mtvec,%0"::"r"(v)); }
static inline void     write_mie(uint32_t v){ asm volatile("csrw mie,%0"::"r"(v)); }
static inline uint32_t read_mcycle(){ uint32_t v; asm volatile("csrr %0,mcycle":"=r"(v)); return v; }
static inline uint32_t read_minstret(){ uint32_t v; asm volatile("csrr %0,minstret":"=r"(v)); return v; }

// Atomic add using A extension (RV32A). Fallback to simple add if A not present.
static inline uint32_t atomic_add(volatile uint32_t* addr, uint32_t val){
    uint32_t old;
    // amoadd.w rd, rs2, (rs1) : old value loaded into rd; (rs1) += rs2
    asm volatile("amoadd.w %0, %2, %1" : "=r"(old), "+A"(*addr) : "r"(val));
    return old;
}

// Interrupt counters
volatile uint32_t irq_count = 0;
volatile uint32_t dma_done_count = 0;

// Trap handler (direct mode). Handles machine timer interrupt and could poll DMA.
void trap_handler(void){
    irq_count++;
    // Re-arm timer ~ later
    uint64_t now = ((uint64_t)MTIME_HI << 32) | MTIME_LO;
    uint64_t next = now + 8000ull; // longer period
    MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFu);
    MTIMECMP_HI = (uint32_t)(next >> 32);
}

__attribute__((naked)) void _start_trap(){
    asm volatile(
        "addi sp, sp, -32\n"
        "sw ra, 28(sp)\n"
        "call trap_handler\n"
        "lw ra, 28(sp)\n"
        "addi sp, sp, 32\n"
        "mret\n"
    );
}

static void setup_timer_irq(){
    write_mtvec((uint32_t)(uintptr_t)_start_trap); // direct mode
    write_mie(1u << 7);    // MTIE
    write_mstatus(read_mstatus() | 0x8u); // MIE global
    uint64_t now = ((uint64_t)MTIME_HI << 32) | MTIME_LO;
    uint64_t next = now + 8000ull;
    MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFu);
    MTIMECMP_HI = (uint32_t)(next >> 32);
}

// Hex print helper
static void print_hex32(uint32_t v){ for(int sh=28; sh>=0; sh-=4){ uint8_t n=(v>>sh)&0xF; putch(n<10?'0'+n:'A'+n-10);} }
static void print_hex64(uint64_t v){ for(int sh=60; sh>=0; sh-=4){ uint8_t n=(v>>sh)&0xF; putch(n<10?'0'+n:'A'+n-10);} }

// Synthetic workload sizes
#define BUF_WORDS  4096
#define DMA_WORDS  2048

static uint32_t src_buf[DMA_WORDS];
static uint32_t dst_buf[DMA_WORDS];
static uint32_t work_buf[BUF_WORDS];

int main(){
    print("[ROBUST] start\n");
    setup_timer_irq();

    // Initialize buffers
    for(uint32_t i=0;i<DMA_WORDS;i++){ src_buf[i] = i * 0x01010101u + 0x55AA1234u; }
    for(uint32_t i=0;i<BUF_WORDS;i++){ work_buf[i] = (i<<1) ^ 0xA5A5A5A5u; }

    // Launch DMA transfer src_buf -> dst_buf
    DMA_SRC = (uint32_t)(uintptr_t)src_buf;
    DMA_DST = (uint32_t)(uintptr_t)dst_buf;
    DMA_LEN = DMA_WORDS * sizeof(uint32_t);
    DMA_CTRL = 1u; // start

    // Mixed compute while DMA in flight
    volatile uint32_t atomic_counter = 0;
    uint64_t accum = 0;
    for(uint32_t outer=0; outer < 800; ++outer){
        // Arithmetic mix (M extension ops)
        uint32_t a = work_buf[(outer*13) & (BUF_WORDS-1)];
        uint32_t b = work_buf[(outer*29) & (BUF_WORDS-1)];
        uint32_t mul = a * b + (b ? (a / (b|1)) : 0);
        accum += mul;

        // Atomic updates
        atomic_add(&atomic_counter, 1);

        // Memory walking (stress caches / bus): stride pattern
        for(uint32_t i=outer & 0x3; i<BUF_WORDS; i+=64){ work_buf[i] ^= (outer + i); }

        // Poll DMA completion occasionally
        if ((outer & 0x1F) == 0){
            if ((DMA_CTRL & 1u) == 0){ dma_done_count++; }
        }

        // Light barrier
        asm volatile("fence iorw, iorw");
    }

    // Ensure DMA finished
    while (DMA_CTRL & 1u) { /* spin */ }
    dma_done_count++; // final completion

    // Verify DMA correctness
    uint32_t mismatches = 0;
    for(uint32_t i=0;i<DMA_WORDS;i++){ if(dst_buf[i] != src_buf[i]){ mismatches++; } }

    // Gather performance counters
    uint32_t ccycle = read_mcycle();
    uint32_t cinstr = read_minstret();

    print("[ROBUST] accum="); print_hex64(accum); putch('\n');
    print("[ROBUST] atomic_count="); print_hex32(atomic_counter); putch('\n');
    print("[ROBUST] dma_done="); print_hex32(dma_done_count); putch('\n');
    print("[ROBUST] dma_mismatch="); print_hex32(mismatches); putch('\n');
    print("[ROBUST] irq_count="); print_hex32(irq_count); putch('\n');
    print("[ROBUST] mcycle="); print_hex32(ccycle); putch('\n');
    print("[ROBUST] minstret="); print_hex32(cinstr); putch('\n');

    print("[ROBUST] end\n");
    asm volatile("ecall"); // signal end
    return 0;
}
