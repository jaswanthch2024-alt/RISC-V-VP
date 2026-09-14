/*
 * cache_sweep_bench.c -- real, executed RISC-V trace source for the
 * tools/cache_sweep/ offline geometry-sweep tool.
 *
 * Same hot/cold interleaved design as locality_bench.c (a small, frequently
 * reused HOT region interleaved with sweeps of a much larger COLD region --
 * genuine temporal locality, not an adversarial one-shot scan), but the COLD
 * region is scaled up to 2 MB so it stays meaningfully larger than every
 * config in the cache-geometry sweep (largest single-dimension test point is
 * 512 KB, ways=128) -- the earlier synthetic sweep already showed that a
 * footprint too close to (or smaller than) the swept range makes every
 * config saturate identically and destroys the signal.
 *
 * This program's job is only to generate a REAL D$ address trace (via
 * VP_DCACHE_TRACE=<path>, see CPU_P64_6_Cycle.h/.cpp) for offline replay --
 * its own on-VP timing/IPC numbers are not the point and are not compared
 * against anything.
 *
 * Compile (validated with riscv64-linux-gnu-gcc 7.5.0, GNU ld):
 *   riscv64-linux-gnu-gcc -march=rv64gc -mabi=lp64 -mcmodel=medany -O2 \
 *     -nostdlib -static -Wl,--entry=main -Wl,--gc-sections \
 *     -Wl,-Ttext=0x80000000 cache_sweep_bench.c -o cache_sweep_bench.elf
 *   riscv64-linux-gnu-objcopy -O ihex cache_sweep_bench.elf cache_sweep_bench.hex
 */
typedef unsigned long long u64;

#define HOT_BASE    0x80200000ULL
#define HOT_LINES   256                 /* 256*16B = 4 KB hot region */
#define HOT_MASK    ((HOT_LINES * 16ULL) - 1)
#define COLD_BASE   0x80300000ULL
#define COLD_LINES  131072              /* 131072*16B = 2 MB cold region */
#define OUTER       3                   /* full cold sweeps */
#define COLD_PER_HOT 4                  /* one hot touch per 4 cold accesses */

/* _start sets sp BEFORE main() is ever entered -- main()'s own compiler-
 * generated prologue (e.g. `addi sp,sp,-16` for local spills) would
 * otherwise run first and compute its frame from whatever garbage is in sp
 * at reset, corrupting the D$ trace with bogus stack addresses (found via
 * disassembly: `sd zero,8(sp)` spilling `sum`, with sp never initialized).
 * Stack placed at 0x80600000 -- above both HOT (0x80200000) and COLD
 * (0x80300000-0x804FFFF0), no overlap. */
asm (
    ".global _start\n"
    "_start:\n"
    "  li sp, 0x80600000\n"
    "  call main\n"
    "1: j 1b\n"
);

void main(void) {
    asm volatile (".option norelax\n\tla gp, __global_pointer$");

    /* populate both regions so co-sim memory is mapped */
    for (u64 i = 0; i < HOT_LINES; i++)
        *(volatile u64 *)(HOT_BASE + i * 16ULL) = i;
    for (u64 i = 0; i < COLD_LINES; i++)
        *(volatile u64 *)(COLD_BASE + i * 16ULL) = i * 2654435761ULL;

    /* Interleave hot touches THROUGHOUT the cold sweep (not burst-then-sweep)
     * so hot recency stays fresh under LRU-style policies while the cold
     * sweep is in progress -- same interleaving rationale as locality_bench. */
    /* NOT volatile: the loads themselves are already forced by the
     * volatile-pointer casts on HOT_BASE/COLD_BASE below, so sum can stay
     * in a register across the whole loop -- making it volatile too would
     * force a spurious stack write on every single accumulation, which
     * showed up as ~half of the captured D$ trace being stack traffic
     * (0x805fxxxx) instead of the intended hot/cold access pattern. */
    u64 sum = 0;
    u64 hot_off = 0, cold_off = 0;
    for (u64 o = 0; o < OUTER; o++) {
        for (u64 c = 0; c < COLD_LINES; c++) {
            sum += *(volatile u64 *)(COLD_BASE + cold_off);
            cold_off = (cold_off + 16) % (COLD_LINES * 16ULL);
            if ((c % COLD_PER_HOT) == 0) {
                sum += *(volatile u64 *)(HOT_BASE + hot_off);
                hot_off = (hot_off + 16) & HOT_MASK;
            }
        }
    }

    u64 result = sum;
    volatile u64 *h = (volatile u64 *)0x90000018ULL;
    *h = result;
    *h = 0xE000000CULL;
    *(volatile u64 *)0x80001000ULL = result;
}
