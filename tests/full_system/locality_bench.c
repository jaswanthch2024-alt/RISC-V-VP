/*
 * locality_bench.c — a FAIR LRU-vs-LFSR test: genuine temporal locality, not
 * an adversarial sequential scan.
 *
 * Unlike mem_contention_bench (a large sweep >> cache size, LRU's textbook
 * WORST case), this models realistic code: a small HOT region reused on
 * every iteration (protected by real recency), interleaved with occasional
 * COLD sweeps through a much larger region. This is exactly the pattern
 * LRU's "recently used = reused soon" assumption is designed for -- LRU
 * should correctly keep the hot region resident while random (LFSR) has no
 * such protection and can evict hot lines purely by chance.
 *
 *   - Hot region:  4 KB  (comfortably fits in the 32 KB D$ alone)
 *   - Cold region: 96 KB (>> D$, creates real eviction pressure)
 *   - Per outer iteration: sweep ALL of cold once, but touch hot HOT_REVISITS
 *     times (heavy reuse) -- so hot data is "recently used" far more often
 *     than any single cold line.
 *
 * Compile: -fPIC -march=rv64g -mabi=lp64f -O1 -fno-builtin, linked at 0x80000000.
 */
typedef unsigned long long u64;

#define HOT_BASE    0x80200000ULL
#define HOT_LINES   256                 /* 256*16B = 4 KB hot region */
#define HOT_MASK    ((HOT_LINES * 16ULL) - 1)
#define COLD_BASE   0x80300000ULL
#define COLD_LINES  6144                /* 6144*16B = 96 KB cold region */
#define OUTER       40
#define HOT_REVISITS 32                 /* hot touches per outer pass */

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

    /* populate both regions so co-sim memory is mapped */
    for (u64 i = 0; i < HOT_LINES; i++)
        *(volatile u64 *)(HOT_BASE + i * 16ULL) = i;
    for (u64 i = 0; i < COLD_LINES; i++)
        *(volatile u64 *)(COLD_BASE + i * 16ULL) = i * 2654435761ULL;

    /* Interleave hot touches THROUGHOUT the cold sweep (not a burst-then-
     * sweep) so the hot line's recency stays fresh under LRU while the cold
     * sweep is in progress -- this is what actually lets LRU's protection
     * work. One hot touch every COLD_PER_HOT cold accesses. */
    #define COLD_PER_HOT 4
    volatile u64 sum = 0;
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
