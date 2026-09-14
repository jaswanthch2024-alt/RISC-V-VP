/*
 * mem_store_bench.c — STORE-heavy stress test, to check whether the VP's
 * zero-cost store-drain model (store_write_penalty=0) under-counts vs CVA6's
 * write-through D-cache (every store -> memory transaction).
 *
 * Same structure as mem_contention_bench but the unrolled body STORES instead
 * of loads: 1024x unrolled (~code > I$) sweeping a 64 KB region (> D$) with a
 * 16 B stride, OUTER passes. If the VP runs faster than the RTL here, store
 * traffic is under-modelled and a store-drain model (AXI or a penalty) would
 * improve accuracy.
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64f -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;

#define REGION_BASE 0x80200000ULL
#define NLINES      4096               /* 64 KB working set */
#define REGION_MASK ((NLINES * 16ULL) - 1)
#define OUTER       20

#define WR    *(volatile u64 *)(REGION_BASE + off) = v; v += 0x9E3779B97F4A7C15ULL; off = (off + 16) & REGION_MASK;
#define WR4   WR WR WR WR
#define WR16  WR4 WR4 WR4 WR4
#define WR64  WR16 WR16 WR16 WR16
#define WR256 WR64 WR64 WR64 WR64
#define WR1024 WR256 WR256 WR256 WR256

void main(void) {
    asm volatile (".option norelax\n\tla gp, __global_pointer$");
    asm volatile ("csrw mhpmevent3,  %0" :: "r"(0x09));
    asm volatile ("csrw mhpmevent4,  %0" :: "r"(0x0A));
    asm volatile ("csrw mhpmevent5,  %0" :: "r"(0x01));
    asm volatile ("csrw mhpmevent6,  %0" :: "r"(0x02));
    asm volatile ("csrw mhpmevent7,  %0" :: "r"(0x10));
    asm volatile ("csrw mhpmevent8,  %0" :: "r"(0x11));
    asm volatile ("csrw mhpmcounter3, zero"); asm volatile ("csrw mhpmcounter4, zero");
    asm volatile ("csrw mhpmcounter5, zero"); asm volatile ("csrw mhpmcounter6, zero");
    asm volatile ("csrw mhpmcounter7, zero"); asm volatile ("csrw mhpmcounter8, zero");

    /* populate the region once so co-sim memory is mapped */
    for (u64 i = 0; i < NLINES; i++)
        *(volatile u64 *)(REGION_BASE + i * 16ULL) = i;

    /* store-sweep: big unrolled body writing the region -> heavy write traffic */
    u64 off = 0, v = 0x1234567;
    for (u64 o = 0; o < OUTER; o++) {
        WR1024
    }

    u64 cycles, instret, br, mp, im, dm, ia, da;
    asm volatile ("csrr %0, mcycle"       : "=r"(cycles));
    asm volatile ("csrr %0, minstret"     : "=r"(instret));
    asm volatile ("csrr %0, mhpmcounter3" : "=r"(br));
    asm volatile ("csrr %0, mhpmcounter4" : "=r"(mp));
    asm volatile ("csrr %0, mhpmcounter5" : "=r"(im));
    asm volatile ("csrr %0, mhpmcounter6" : "=r"(dm));
    asm volatile ("csrr %0, mhpmcounter7" : "=r"(ia));
    asm volatile ("csrr %0, mhpmcounter8" : "=r"(da));
    u64 result = v ^ off;

    volatile u64 *h = (volatile u64 *)0x90000018ULL;
    *h = cycles; *h = instret; *h = br; *h = mp; *h = im; *h = dm; *h = ia; *h = da;
    *h = result;
    *h = 0xE000000CULL;
    *(volatile u64 *)0x80001000ULL = result;
}
