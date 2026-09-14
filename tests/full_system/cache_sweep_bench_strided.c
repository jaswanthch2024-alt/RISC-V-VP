/*
 * cache_sweep_bench_strided.c -- second, deliberately DIFFERENT real trace
 * source for tools/cache_sweep/, to check whether the line-width finding
 * from cache_sweep_bench.c (sequential stride) generalizes or was an
 * artifact of that specific access pattern.
 *
 * Same footprint as cache_sweep_bench.c (2 MB cold region, 4 KB hot region,
 * same interleaving) so capacity comparisons stay apples-to-apples -- the
 * ONLY thing that changes is how the cold region is walked: a large odd
 * stride (in line units) instead of +1 line each step. Since COLD_LINES is
 * a power of 2, any odd stride is coprime to it, so this still visits every
 * line exactly once per pass (same "does it fit" capacity story as the
 * sequential version) -- but consecutive accesses land far apart in memory,
 * defeating within-line spatial locality on purpose. If line width's huge
 * advantage was really about spatial locality (as claimed), it should be
 * much weaker or gone here; if it still dominates, that claim was wrong.
 *
 * Compile (same recipe as cache_sweep_bench.c):
 *   riscv64-linux-gnu-gcc -march=rv64gc -mabi=lp64 -mcmodel=medany -O2 \
 *     -nostdlib -static -Wl,--entry=_start -Wl,--gc-sections \
 *     -Wl,-Ttext=0x80000000 cache_sweep_bench_strided.c -o cache_sweep_bench_strided.elf
 *   riscv64-linux-gnu-objcopy -O ihex cache_sweep_bench_strided.elf cache_sweep_bench_strided.hex
 */
typedef unsigned long long u64;

#define HOT_BASE     0x80200000ULL
#define HOT_LINES    256
#define HOT_MASK     ((HOT_LINES * 16ULL) - 1)
#define COLD_BASE    0x80300000ULL
#define COLD_LINES   131072             /* 2 MB, same as cache_sweep_bench.c */
#define COLD_MASK    (COLD_LINES - 1)   /* power of 2 -- any odd stride is coprime */
#define STRIDE_LINES 12345ULL           /* arbitrary odd stride -> full scrambled pass */
#define OUTER        3
#define COLD_PER_HOT 4

asm (
    ".global _start\n"
    "_start:\n"
    "  li sp, 0x80600000\n"
    "  call main\n"
    "1: j 1b\n"
);

void main(void) {
    asm volatile (".option norelax\n\tla gp, __global_pointer$");

    for (u64 i = 0; i < HOT_LINES; i++)
        *(volatile u64 *)(HOT_BASE + i * 16ULL) = i;
    for (u64 i = 0; i < COLD_LINES; i++)
        *(volatile u64 *)(COLD_BASE + i * 16ULL) = i * 2654435761ULL;

    /* NOT volatile -- see cache_sweep_bench.c: loads are already forced via
     * the volatile pointer casts below, so sum can stay in a register. */
    u64 sum = 0;
    u64 hot_off = 0;
    u64 cold_line = 0; /* line index, not byte offset -- stride applied in line units */
    for (u64 o = 0; o < OUTER; o++) {
        for (u64 c = 0; c < COLD_LINES; c++) {
            sum += *(volatile u64 *)(COLD_BASE + cold_line * 16ULL);
            cold_line = (cold_line + STRIDE_LINES) & COLD_MASK;
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
