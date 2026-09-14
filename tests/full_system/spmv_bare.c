/*
 * spmv_bare.c — bare-metal double-precision SpMV (riscv-tests kernel) wired to
 * the VP+co-sim HPM/halt harness, to find the ROOT CAUSE of the paper's 9.3%
 * SpMV residual now that the LRU->LFSR replacement bug (§B8) is fixed.
 *
 * Working set: val[2399]*8 + idx[2399]*4 + x[500]*8 ~= 34 KB, just over the
 * 32 KB D$, with the irregular gather x[idx[k]] -> reuse-with-eviction, the
 * exact pattern where LRU vs random (replacement policy) diverges.
 *
 * Output y is written to a scratch DRAM region (write-then-read, so the co-sim
 * SlaveFromFile has it mapped). Repeated OUTER passes to amplify D$ pressure.
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64d -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;
#include "spmv_dataset.h"     /* defines R, NNZ, val[], idx[], x[], ptr[] */

#define OUTER 1
#define Y_BASE 0x80280000ULL  /* scratch output region (write-then-read) */

static void spmv_once(double* y) {
    for (int i = 0; i < R; i++) {
        int k;
        double yi0 = 0, yi1 = 0, yi2 = 0, yi3 = 0;
        for (k = ptr[i]; k < ptr[i+1]-3; k += 4) {
            yi0 += val[k+0]*x[idx[k+0]];
            yi1 += val[k+1]*x[idx[k+1]];
            yi2 += val[k+2]*x[idx[k+2]];
            yi3 += val[k+3]*x[idx[k+3]];
        }
        for ( ; k < ptr[i+1]; k++)
            yi0 += val[k]*x[idx[k]];
        y[i] = (yi0+yi1)+(yi2+yi3);
    }
}

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

    double* y = (double*)Y_BASE;
    for (int o = 0; o < OUTER; o++)
        spmv_once(y);

    /* checksum y (write-then-read; keeps the result live) */
    double sum = 0;
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
    u64 result = (u64)sum;

    volatile u64 *h = (volatile u64 *)0x90000018ULL;
    *h = cycles; *h = instret; *h = br; *h = mp; *h = im; *h = dm; *h = ia; *h = da;
    *h = result;
    *h = 0xE000000CULL;
    *(volatile u64 *)0x80001000ULL = result;
}
