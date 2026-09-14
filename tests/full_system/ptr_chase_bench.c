/*
 * ptr_chase_bench.c — isolates the LOAD->LOAD-ADDRESS (gather/pointer-chase)
 * dependency latency, with no other confounders.
 *
 * Each iteration does exactly one load whose ADDRESS is the result of the
 * previous load:  idx = arr[idx];  -> a serial dependent-load chain. The array
 * is 4 KB (D$-resident, so ~all hits: this measures the dependency latency, not
 * miss latency), and the stride (257, coprime to 512) defeats naive prefetch.
 *
 * Hypothesis under test: the VP charges 1 cycle for this (load_hit_penalty),
 * but CVA6 charges 2 (load result must route to address generation for the next
 * load). If so, RTL cycles ~= 1.5x VP cycles for the chase.
 *
 * Control: long_test3 (load->ALU) already matches the RTL at 1 cycle (BUGS §B5),
 * so any divergence here is specific to the load->address pattern.
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64f -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;

#define ARR   ((volatile u64*)0x80280000ULL)  /* scratch, write-then-read */
#define SIZE  512        /* 512 * 8 = 4 KB, D$-resident */
#define STRIDE 257       /* coprime to 512 -> single cycle visiting all, no prefetch */
#define N     400000     /* chase iterations */

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

    /* write phase: build the chase permutation (populates co-sim memory) */
    for (u64 j = 0; j < SIZE; j++)
        ARR[j] = (j + STRIDE) % SIZE;

    /* pointer chase: pure load -> load-address dependency, all D$ hits */
    u64 idx = 0;
    for (u64 i = 0; i < N; i++)
        idx = ARR[idx];

    u64 cycles, instret, br, mp, im, dm, ia, da;
    asm volatile ("csrr %0, mcycle"       : "=r"(cycles));
    asm volatile ("csrr %0, minstret"     : "=r"(instret));
    asm volatile ("csrr %0, mhpmcounter3" : "=r"(br));
    asm volatile ("csrr %0, mhpmcounter4" : "=r"(mp));
    asm volatile ("csrr %0, mhpmcounter5" : "=r"(im));
    asm volatile ("csrr %0, mhpmcounter6" : "=r"(dm));
    asm volatile ("csrr %0, mhpmcounter7" : "=r"(ia));
    asm volatile ("csrr %0, mhpmcounter8" : "=r"(da));
    u64 result = idx;

    volatile u64 *h = (volatile u64 *)0x90000018ULL;
    *h = cycles; *h = instret; *h = br; *h = mp; *h = im; *h = dm; *h = ia; *h = da;
    *h = result;
    *h = 0xE000000CULL;
    *(volatile u64 *)0x80001000ULL = result;
}
