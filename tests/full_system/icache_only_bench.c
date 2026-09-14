/*
 * icache_only_bench.c — ISOLATES pure I$-refill latency, no D$ traffic at all.
 *
 * Large (>16KB) straight-line body of register-only ALU ops (no loads/stores),
 * unrolled and repeated, so I$ misses heavily while D$ stays essentially idle
 * (zero data memory accesses). This re-derives the flat per-line I$ miss
 * penalty cleanly, independent of any D$/if_empty interaction — the original
 * calibration benchmark that produced "~9.5 cyc/line" no longer exists in this
 * tree, so this rebuilds an equivalent isolating control.
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64f -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;

#define OUTER 100

/* Register-only op, no memory access at all. */
#define OP    a += b; b ^= c; c -= d; d += (a << 1); a ^= b; b += c;
#define OP4   OP OP OP OP
#define OP16  OP4 OP4 OP4 OP4
#define OP64  OP16 OP16 OP16 OP16
#define OP256 OP64 OP64 OP64 OP64
#define OP1024 OP256 OP256 OP256 OP256

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

    u64 a = 0x1111, b = 0x2222, c = 0x3333, d = 0x4444;
    for (u64 o = 0; o < OUTER; o++) {
        OP1024
    }
    asm volatile ("" : : "r"(a), "r"(b), "r"(c), "r"(d));

    u64 cycles, instret, br, mp, im, dm, ia, da;
    asm volatile ("csrr %0, mcycle"       : "=r"(cycles));
    asm volatile ("csrr %0, minstret"     : "=r"(instret));
    asm volatile ("csrr %0, mhpmcounter3" : "=r"(br));
    asm volatile ("csrr %0, mhpmcounter4" : "=r"(mp));
    asm volatile ("csrr %0, mhpmcounter5" : "=r"(im));
    asm volatile ("csrr %0, mhpmcounter6" : "=r"(dm));
    asm volatile ("csrr %0, mhpmcounter7" : "=r"(ia));
    asm volatile ("csrr %0, mhpmcounter8" : "=r"(da));
    u64 result = a ^ b ^ c ^ d;

    volatile u64 *h = (volatile u64 *)0x90000018ULL;
    *h = cycles; *h = instret; *h = br; *h = mp; *h = im; *h = dm; *h = ia; *h = da;
    *h = result;
    *h = 0xE000000CULL;
    *(volatile u64 *)0x80001000ULL = result;
}
