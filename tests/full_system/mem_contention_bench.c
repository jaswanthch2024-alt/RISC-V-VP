/*
 * mem_contention_bench.c — I$/D$ shared-port CONTENTION stress test.
 *
 * Unlike the register-only microbenchmarks (which are cache-resident and never
 * miss), this deliberately thrashes BOTH L1 caches at once so instruction-fetch
 * misses and data-load misses are in flight simultaneously — the only regime
 * where a shared memory-port arbiter changes timing.
 *
 *   - Data:  sweeps a 64 KB region with a 16 B stride (one D$ line each), and
 *            64 KB > 32 KB D$, so essentially every access is a D$ miss.
 *   - Code:  the read body is unrolled 1024x (~20 KB of straight-line loads),
 *            larger than the 16 KB I$, so the fetch stream also misses.
 *
 * Memory is WRITTEN before it is read so the co-sim's SlaveFromFile has every
 * address populated (read-before-write would assert).
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64f -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;

#define REGION_BASE 0x80200000ULL      /* data region, well past the code */
#define NLINES      4096               /* 4096 * 16 B = 64 KB working set  */
#define REGION_MASK ((NLINES * 16ULL) - 1)
#define OUTER       200                /* repeat the big body -> re-sweep  */

/* One strided volatile load; advance by a full D$ line (16 B) and wrap. */
#define RD    sum += *(volatile u64 *)(REGION_BASE + off); off = (off + 16) & REGION_MASK;
#define RD4   RD RD RD RD
#define RD16  RD4 RD4 RD4 RD4
#define RD64  RD16 RD16 RD16 RD16
#define RD256 RD64 RD64 RD64 RD64
#define RD1024 RD256 RD256 RD256 RD256

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

    /* --- write phase: populate the whole region so co-sim memory is mapped --- */
    for (u64 i = 0; i < NLINES; i++)
        *(volatile u64 *)(REGION_BASE + i * 16ULL) = i * 2654435761ULL;

    /* --- read phase: big unrolled sweep -> concurrent I$ + D$ misses --- */
    volatile u64 sum = 0;
    u64 off = 0;
    for (u64 o = 0; o < OUTER; o++) {
        RD1024
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
    u64 result = sum;

    volatile u64 *h = (volatile u64 *)0x90000018ULL;
    *h = cycles; *h = instret; *h = br; *h = mp; *h = im; *h = dm; *h = ia; *h = da;
    *h = result;
    *h = 0xE000000CULL;
    *(volatile u64 *)0x80001000ULL = result;   /* VP clean halt (HTIF) */
}
