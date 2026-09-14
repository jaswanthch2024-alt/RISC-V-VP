// SPDX-License-Identifier: GPL-3.0-or-later
// cache_sweep -- standalone D$ geometry design-space exploration tool.
//
// Sweeps SETS, WAYS, and LINE_BYTES independently (one dimension at a time,
// others held at the RTL-matched baseline: 256 sets, 8 ways, 16B line =
// 32KB) against two purpose-built stress sections, using RuntimeCache (a
// verified duplicate of the production riscv_tlm::Cache, see the self-check
// below). No RTL comparison -- these geometries are not validated against
// any CVA6 config, this is pure VP-internal exploration.
//
// "real" column (capacity/sets/line-width stress): a genuine D$ address
// trace from cache_sweep_bench.c actually compiled and executed on the VP
// (hot/cold interleaved access, 2 MB cold region -- comfortably bigger than
// every swept config, largest is 512 KB) -- not a synthetic pattern. An
// earlier synthetic-scan version of this section was replaced after
// concluding it could only prove "does the cache behave the way theory
// predicts," not "does this matter for real code."
//
// Section 2 (associativity/ways stress): a deliberate same-set conflict --
// more distinct tags than the largest WAYS tested (64) all mapping to one
// fixed set, cycled repeatedly. Total cache size cannot fix this pattern;
// only WAYS can. This isolates associativity from the "bigger cache wins"
// confound that a generic scan can't separate.
//
// "real" (replaces the earlier synthetic Section 1): a genuine D$ address
// trace captured from an actual compiled/executed RISC-V program
// (systemc/samples/cache_sweep_bench.c, run through the real VP with
// VP_DCACHE_TRACE=<path>), replayed identically against every swept config.
// Section 2 stays synthetic on purpose -- there's no natural way to get a
// real program to touch an engineered N-way same-set conflict on demand,
// and isolating associativity from capacity requires exactly that kind of
// deliberate construction.
#include "RuntimeCache.h"
#include "../../inc/Cache.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <unordered_map>

// ---- Real trace: load once, replay against every config -----------------
// Trace format: one lowercase-hex address per line (matches
// trace_dcache_access() in inc/CPU_P64_6_Cycle.h, VP_DCACHE_TRACE output).
static std::vector<uint64_t> load_trace(const char *path) {
    std::vector<uint64_t> trace;
    std::FILE *f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "Could not open trace file: %s\n", path);
        std::exit(1);
    }
    char line[64];
    while (std::fgets(line, sizeof(line), f)) {
        trace.push_back(std::strtoull(line, nullptr, 16));
    }
    std::fclose(f);
    return trace;
}

static void run_real_trace(RuntimeCache &c, const std::vector<uint64_t> &trace) {
    for (uint64_t addr : trace) c.access(addr);
}

static int log2_of(std::size_t n); // forward decl, defined below with Section 2

// ---- Belady's OPT: offline-optimal, not a real/implementable policy -----
// A ceiling, not a competitor: uses perfect knowledge of the FUTURE access
// sequence to always evict the line that won't be needed again soonest (or
// ever). No real cache can do this -- the value is knowing how much headroom
// *any* practical policy (LFSR/LRU/MRU) is leaving on the table.
//
// Exact per-set decomposition: lines in different sets never compete for the
// same physical ways, so OPT on a set-associative cache is exactly the sum of
// independent fully-associative-within-WAYS OPT on each set's own
// sub-sequence of accesses. Standard textbook algorithm (Belady 1966):
// on a miss with a full set, evict whichever cached tag's next reuse is
// furthest in the future (or never reused again).
static void run_opt(std::size_t sets_n, std::size_t ways, std::size_t line_bytes,
                     const std::vector<uint64_t> &trace, uint64_t &hits, uint64_t &misses) {
    const int offset_bits = log2_of(line_bytes);
    const int tag_shift = offset_bits + log2_of(sets_n);
    const std::size_t n = trace.size();

    std::vector<uint64_t> tags(n);
    std::vector<std::vector<std::size_t>> per_set_indices(sets_n);
    for (std::size_t i = 0; i < n; i++) {
        uint64_t addr = trace[i];
        std::size_t idx = static_cast<std::size_t>((addr >> offset_bits) & (sets_n - 1));
        tags[i] = addr >> tag_shift;
        per_set_indices[idx].push_back(i);
    }

    hits = 0; misses = 0;
    for (std::size_t s = 0; s < sets_n; s++) {
        auto &seq = per_set_indices[s]; // global trace indices touching this set, in order
        const std::size_t m = seq.size();
        // nextUse[p] = position (in seq) of the next occurrence of the same
        // tag after p, or SIZE_MAX if never reused again in this set.
        std::vector<std::size_t> nextUse(m, SIZE_MAX);
        std::unordered_map<uint64_t, std::size_t> lastPos;
        for (std::size_t p = m; p-- > 0; ) {
            uint64_t tag = tags[seq[p]];
            auto it = lastPos.find(tag);
            if (it != lastPos.end()) nextUse[p] = it->second;
            lastPos[tag] = p;
        }
        // Simulate: cached_last_pos maps a currently-cached tag to the seq
        // position it was last accessed at (<=current p); since no
        // occurrence happened between that position and now (else it would
        // have been updated), nextUse[] at that position is still correct
        // for "next occurrence after now."
        std::unordered_map<uint64_t, std::size_t> cached_last_pos;
        cached_last_pos.reserve(ways * 2);
        for (std::size_t p = 0; p < m; p++) {
            uint64_t tag = tags[seq[p]];
            auto it = cached_last_pos.find(tag);
            if (it != cached_last_pos.end()) {
                hits++;
                it->second = p;
                continue;
            }
            misses++;
            if (cached_last_pos.size() >= ways) {
                uint64_t victim_tag = 0;
                std::size_t worst = 0;
                bool first = true;
                for (auto &kv : cached_last_pos) {
                    std::size_t nu = nextUse[kv.second];
                    if (first || nu > worst) { worst = nu; victim_tag = kv.first; first = false; }
                }
                cached_last_pos.erase(victim_tag);
            }
            cached_last_pos[tag] = p;
        }
    }
}

// ---- Section 2: deliberate same-set conflict -----------------------------
// conflict_tags must exceed the largest swept WAYS value (64) so the
// pattern still forces misses even at max associativity.
static int log2_of(std::size_t n) {
    int r = 0;
    while (n > 1) { n >>= 1; r++; }
    return r;
}

static void run_section2(RuntimeCache &c, int conflict_tags, int reps) {
    // addr = tag << tag_shift places the tag in the tag field and zeroes
    // every bit below it (offset + index bits), so every such address maps
    // to set index 0 regardless of this config's SETS/LINE_BYTES -- tag_shift
    // is recomputed per-config from its own sets()/line_bytes(), mirroring
    // RuntimeCache's internal formula (log2(line_bytes) + log2(sets)).
    const int tag_shift = log2_of(c.line_bytes()) + log2_of(c.sets());
    for (int r = 0; r < reps; r++) {
        for (int t = 1; t <= conflict_tags; t++) {
            uint64_t addr = (static_cast<uint64_t>(t) << tag_shift) + 0x2000'0000ULL;
            c.access(addr);
        }
    }
}

struct Config { std::string dim; std::size_t sets, ways, line; };

static void self_check() {
    // Cross-check RuntimeCache against the real production Cache<> template
    // at the exact default geometry, on an identical access sequence --
    // this is the correctness proof that the duplicate model is faithful.
    riscv_tlm::Cache<256, 8, 16> real;
    RuntimeCache dup(256, 8, 16);

    uint64_t mismatches = 0;
    for (uint64_t i = 0; i < 500000; i++) {
        // A mixed synthetic sequence: sequential sweep with occasional
        // revisits, enough to exercise both hit and miss/eviction paths.
        uint64_t addr = (i * 37) % (200 * 1024) + ((i % 13 == 0) ? 0 : (i * 16));
        bool hr = real.access(addr);
        bool hd = dup.access(addr);
        if (hr != hd) mismatches++;
    }
    if (mismatches != 0 || real.get_hits() != dup.get_hits() ||
        real.get_misses() != dup.get_misses()) {
        std::fprintf(stderr,
            "SELF-CHECK FAILED: RuntimeCache diverges from production Cache<> "
            "(%llu per-access mismatches, real hits=%llu/dup hits=%llu, "
            "real misses=%llu/dup misses=%llu) -- duplicate model is NOT "
            "faithful, do not trust sweep results until fixed.\n",
            (unsigned long long)mismatches,
            (unsigned long long)real.get_hits(), (unsigned long long)dup.get_hits(),
            (unsigned long long)real.get_misses(), (unsigned long long)dup.get_misses());
        std::exit(1);
    }
    std::printf("Self-check PASSED: RuntimeCache bit-identical to production Cache<256,8,16> "
                "over 500000 accesses (%llu hits, %llu misses).\n\n",
                (unsigned long long)real.get_hits(), (unsigned long long)real.get_misses());
}

int main() {
    self_check();

    const std::size_t BASE_SETS = 256, BASE_WAYS = 8, BASE_LINE = 16;
    const int SECTION2_CONFLICT_TAGS = 80; // > largest WAYS tested (64)
    const int SECTION2_REPS = 5000;

    std::vector<uint64_t> real_trace = load_trace("dcache_trace.txt");
    std::printf("Loaded real trace 1 (sequential): %zu D$ accesses (cache_sweep_bench.c)\n",
                real_trace.size());
    std::vector<uint64_t> strided_trace = load_trace("dcache_trace_strided.txt");
    std::printf("Loaded real trace 2 (strided/scrambled): %zu D$ accesses "
                "(cache_sweep_bench_strided.c -- same footprint, same hot/cold "
                "structure, cold region walked with a large odd stride instead "
                "of sequentially, to check whether the line-width finding from "
                "trace 1 generalizes or was an artifact of sequential access)\n\n",
                strided_trace.size());

    std::vector<Config> configs = {
        {"baseline", BASE_SETS, BASE_WAYS, BASE_LINE},
        {"sets",  128, BASE_WAYS, BASE_LINE},
        {"sets",  512, BASE_WAYS, BASE_LINE},
        {"sets", 1024, BASE_WAYS, BASE_LINE},
        {"sets", 2048, BASE_WAYS, BASE_LINE},
        {"sets", 4096, BASE_WAYS, BASE_LINE},   // 512KB -- past the 2MB-footprint knee search
        {"sets", 8192, BASE_WAYS, BASE_LINE},   // 1MB
        {"sets",16384, BASE_WAYS, BASE_LINE},   // 2MB -- footprint-equal point
        {"sets",32768, BASE_WAYS, BASE_LINE},   // 4MB -- past the footprint
        {"sets",65536, BASE_WAYS, BASE_LINE},   // 8MB -- confirm plateau after the knee
        {"ways", BASE_SETS,  4, BASE_LINE},
        {"ways", BASE_SETS, 16, BASE_LINE},
        {"ways", BASE_SETS, 32, BASE_LINE},
        {"ways", BASE_SETS, 64, BASE_LINE},
        {"ways", BASE_SETS,128, BASE_LINE},     // past SECTION2_CONFLICT_TAGS=80, confirm plateau
        {"ways", BASE_SETS,256, BASE_LINE},     // 1MB
        {"ways", BASE_SETS,512, BASE_LINE},     // 2MB -- footprint-equal point
        {"ways", BASE_SETS,1024,BASE_LINE},     // 4MB -- past the footprint
        {"ways", BASE_SETS,2048,BASE_LINE},     // 8MB -- confirm plateau after the knee
        {"line", BASE_SETS, BASE_WAYS,  32},
        {"line", BASE_SETS, BASE_WAYS,  64},
        {"line", BASE_SETS, BASE_WAYS, 128},
        {"line", BASE_SETS, BASE_WAYS, 256},   // 512KB
        {"line", BASE_SETS, BASE_WAYS, 512},   // 1MB
        {"line", BASE_SETS, BASE_WAYS,1024},   // 2MB -- same footprint-equal point as sets/ways
        {"line", BASE_SETS, BASE_WAYS,2048},   // 4MB -- past the footprint, confirm/find plateau
    };

    std::printf("%-10s %6s %5s %6s %10s | %9s %9s %8s | %9s\n",
                "dim", "sets", "ways", "line", "size(B)",
                "seq_hit%", "strd_hit%", "delta", "sec2_hit%");
    std::printf("%s\n", std::string(95, '-').c_str());

    for (auto &cfg : configs) {
        RuntimeCache c1(cfg.sets, cfg.ways, cfg.line);
        run_real_trace(c1, real_trace);

        RuntimeCache c1b(cfg.sets, cfg.ways, cfg.line);
        run_real_trace(c1b, strided_trace);

        RuntimeCache c2(cfg.sets, cfg.ways, cfg.line);
        run_section2(c2, SECTION2_CONFLICT_TAGS, SECTION2_REPS);

        double seq_pct = c1.hit_rate() * 100.0;
        double strd_pct = c1b.hit_rate() * 100.0;
        std::printf("%-10s %6zu %5zu %6zu %10zu | %8.2f%% %8.2f%% %+7.2f%% | %8.2f%%\n",
            cfg.dim.c_str(), cfg.sets, cfg.ways, cfg.line, c1.size_bytes(),
            seq_pct, strd_pct, strd_pct - seq_pct, c2.hit_rate() * 100.0);
    }

    // ---- Policy comparison: LFSR vs LRU vs MRU vs OPT (the ceiling) -------
    // Separate table on purpose -- this is a different question (which
    // eviction policy, not which geometry) and doesn't belong crammed into
    // the geometry table's columns. OPT is not a real/implementable policy;
    // it's the theoretical best any policy could achieve, included to show
    // how much headroom LFSR/LRU/MRU are each leaving on the table.
    std::printf("\n\nPolicy comparison (LFSR vs LRU vs MRU vs Belady's OPT ceiling)\n");
    std::printf("%-10s %6s %5s %6s | %9s %9s %9s %9s\n",
                "dim", "sets", "ways", "line", "LFSR%", "LRU%", "MRU%", "OPT%");
    std::printf("%s\n", std::string(80, '-').c_str());

    std::vector<Config> policy_configs = {
        {"baseline", BASE_SETS, BASE_WAYS, BASE_LINE},
        {"sets_2MB", 16384, BASE_WAYS, BASE_LINE},
        {"line_512", BASE_SETS, BASE_WAYS, 512},
    };
    for (auto &trace_pair : { std::make_pair("sequential", &real_trace),
                              std::make_pair("strided", &strided_trace) }) {
        std::printf("\n-- %s trace --\n", trace_pair.first);
        for (auto &cfg : policy_configs) {
            RuntimeCache c_lfsr(cfg.sets, cfg.ways, cfg.line);
            c_lfsr.set_policy(RuntimeCache::Policy::LFSR);
            run_real_trace(c_lfsr, *trace_pair.second);

            RuntimeCache c_lru(cfg.sets, cfg.ways, cfg.line);
            c_lru.set_policy(RuntimeCache::Policy::LRU);
            run_real_trace(c_lru, *trace_pair.second);

            RuntimeCache c_mru(cfg.sets, cfg.ways, cfg.line);
            c_mru.set_policy(RuntimeCache::Policy::MRU);
            run_real_trace(c_mru, *trace_pair.second);

            uint64_t opt_hits, opt_misses;
            run_opt(cfg.sets, cfg.ways, cfg.line, *trace_pair.second, opt_hits, opt_misses);
            double opt_pct = 100.0 * opt_hits / (opt_hits + opt_misses);

            std::printf("%-10s %6zu %5zu %6zu | %8.2f%% %8.2f%% %8.2f%% %8.2f%%\n",
                cfg.dim.c_str(), cfg.sets, cfg.ways, cfg.line,
                c_lfsr.hit_rate() * 100.0, c_lru.hit_rate() * 100.0,
                c_mru.hit_rate() * 100.0, opt_pct);
        }
    }

    return 0;
}
