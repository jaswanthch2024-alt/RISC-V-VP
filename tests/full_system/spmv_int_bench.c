/*
 * spmv_int_bench.c — SpMV MEMORY pattern with integer arithmetic.
 *
 * Same arrays and the same irregular gather x[idx[k]] as spmv_bare.c, so the
 * D-cache access pattern (and therefore miss counts / replacement behaviour) is
 * IDENTICAL — but the double FMADDs are replaced by integer multiply-add on the
 * raw 64-bit words. This isolates the cache/memory question (the actual subject
 * of the SpMV divergence) from the double-precision FPU, which makes the RTL
 * co-sim run at normal speed instead of choking.
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64f -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;
#include "spmv_dataset.h"     /* R, NNZ, val[], idx[], x[], ptr[] */

#define OUTER 20
#define Y_BASE 0x80280000ULL

/* read the raw 64-bit word at a double's address (same load address/size) */
static inline u64 rd64(const double* p) { return *(const u64*)p; }

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

    volatile u64* y = (volatile u64*)Y_BASE;
    for (int o = 0; o < OUTER; o++) {
        for (int i = 0; i < R; i++) {
            u64 acc = 0;
            for (int k = ptr[i]; k < ptr[i+1]; k++)
                acc += rd64(&val[k]) * rd64(&x[idx[k]]);   /* same gather, int mul */
            y[i] = acc;
        }
    }

    u64 sum = 0;
    for (int i = 0; i < R; i++) sum += y[i];

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
    *(volatile u64 *)0x80001000ULL = result;
}
