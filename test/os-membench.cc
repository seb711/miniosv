/*
 * Memory-subsystem benchmark for the slim OSv kernel (C++).
 *
 * Exercises and times the allocator (core/mempool.cc) across the three paths it
 * uses internally, selected purely by allocation size:
 *
 *   - pool path     : objects <= page_size/4 (1024 B) come from the per-CPU
 *                     malloc_pools. This is the hot path for small objects.
 *   - page-range    : objects between ~1 page and huge_page_size come out of the
 *                     free_page_ranges allocator (malloc_large).
 *   - mapped path   : objects >= huge_page_size (2 MiB) are backed by map_anon().
 *
 * Unlike os-stress (which checks correctness under contention), this file is a
 * benchmark: every test reports throughput (operations/second and, where
 * meaningful, bytes/second) measured with CLOCK_MONOTONIC. It still validates
 * the bytes it writes so a fast-but-wrong allocator cannot look good, and it
 * brackets the whole run with memory::stats::free() to flag leaks.
 *
 * Patterns covered, with "a lot of them" (millions of small ops):
 *   - alloc/free churn (allocate one, free it) - pure allocator latency
 *   - batch alloc then batch free - LIFO and FIFO release orders
 *   - random-order free of a large live set - stresses page/pool free lists
 *   - mixed small+large+huge workload
 *   - realloc growth/shrink
 *   - calloc (zeroing) throughput + zero-fill verification
 *   - new[]/delete[] (C++ operator new path)
 *   - concurrent same-CPU alloc/free across many threads (pool contention)
 *   - cross-thread free: alloc on one thread, free on another
 *     (drives pool::free_different_cpu, the remote-free path)
 *   - contiguous physical allocations (alloc_phys_contiguous_aligned)
 *
 * Entry point os_membench_main(); returns non-zero if any correctness CHECK
 * failed. Tuning knobs are the *_N / *_THREADS constants below - turn them up
 * for a longer, heavier run.
 */
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <time.h>

#include <osv/mempool.hh>
#include <osv/contiguous_alloc.hh>
#include <osv/mmu-defs.hh>

static std::atomic<int> g_checks{0};
static std::atomic<int> g_fails{0};
static const char *g_section = "";

#define CHECK(cond) do { \
        g_checks.fetch_add(1); \
        if (!(cond)) { \
            g_fails.fetch_add(1); \
            printf("FAIL [%s] %s:%d: %s\n", g_section, __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static void section(const char *s) { g_section = s; printf("  - %s\n", s); }

/* ------------------------------------------------------------ timing */

static inline double now_sec()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Report ops/sec (and optional bytes/sec) for `ops` operations in `dt` secs. */
static void report(const char *what, uint64_t ops, double dt, uint64_t bytes = 0)
{
    double mops = dt > 0 ? (ops / dt) / 1e6 : 0.0;
    if (bytes) {
        double gibs = dt > 0 ? (bytes / dt) / (1024.0 * 1024.0 * 1024.0) : 0.0;
        printf("      %-44s %10llu ops  %7.3fs  %8.2f Mops/s  %7.2f GiB/s\n",
               what, (unsigned long long)ops, dt, mops, gibs);
    } else {
        printf("      %-44s %10llu ops  %7.3fs  %8.2f Mops/s\n",
               what, (unsigned long long)ops, dt, mops);
    }
}

/* A cheap, deterministic, per-thread PRNG (xorshift) - no global RNG lock. */
static inline uint32_t xs(uint32_t &s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

/* Touch the buffer so the allocator's pages are really faulted in and the
 * compiler cannot optimize the allocation away. Returns a checksum byte. */
static inline unsigned char scribble(void *p, size_t n, unsigned char seed)
{
    volatile unsigned char *m = (volatile unsigned char *)p;
    const size_t step = n >= 4096 ? 4096 : (n ? n : 1);
    unsigned char acc = 0;
    for (size_t i = 0; i < n; i += step) { m[i] = (unsigned char)(seed + i); acc ^= m[i]; }
    if (n) { m[n - 1] = seed; acc ^= m[n - 1]; }
    return acc;
}

/* ------------------------------------------------------------ single-thread churn */

/* Allocate one object and immediately free it, N times. Pure round-trip
 * latency for a given size class. */
static void bench_churn(const char *label, size_t sz, uint64_t n)
{
    volatile unsigned char sink = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) {
        void *p = malloc(sz);
        if (!p) { g_fails.fetch_add(1); break; }
        sink ^= scribble(p, sz, (unsigned char)i);
        free(p);
    }
    double dt = now_sec() - t0;
    report(label, n, dt, n * sz);
    (void)sink;
}

static void test_churn()
{
    section("churn: alloc one + free one (round-trip latency)");
    const uint64_t SMALL_N = 4'000'000;
    const uint64_t LARGE_N =   200'000;
    const uint64_t HUGE_N  =     2'000;
    bench_churn("pool   16 B", 16, SMALL_N);
    bench_churn("pool   64 B", 64, SMALL_N);
    bench_churn("pool  256 B", 256, SMALL_N);
    bench_churn("pool 1024 B", 1024, SMALL_N);
    bench_churn("page  8 KiB", 8 * 1024, LARGE_N);
    bench_churn("page 64 KiB", 64 * 1024, LARGE_N);
    bench_churn("page  1 MiB", 1024 * 1024, LARGE_N / 10);
    bench_churn("map   2 MiB", 2 * 1024 * 1024, HUGE_N);
    bench_churn("map   8 MiB", 8 * 1024 * 1024, HUGE_N);
}

/* ------------------------------------------------------------ batch alloc/free */

/* Allocate `count` objects (building a live set of count*sz bytes), then free
 * them in `lifo` (reverse) or FIFO (forward) order. Separates alloc cost from
 * free cost and exposes free-list ordering effects. */
static void bench_batch(const char *label, size_t sz, size_t count, bool lifo)
{
    std::vector<void *> ptrs(count, nullptr);
    volatile unsigned char sink = 0;

    double ta = now_sec();
    for (size_t i = 0; i < count; i++) {
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) { g_fails.fetch_add(1); count = i; break; }
        sink ^= scribble(ptrs[i], sz, (unsigned char)i);
    }
    double tab = now_sec() - ta;

    /* verify the data survived (no overlap / corruption) before freeing */
    bool ok = true;
    for (size_t i = 0; i < count; i++)
        if (((volatile unsigned char *)ptrs[i])[sz ? sz - 1 : 0] != (unsigned char)i) ok = false;
    CHECK(ok);

    double tf = now_sec();
    if (lifo)
        for (size_t i = count; i-- > 0;) free(ptrs[i]);
    else
        for (size_t i = 0; i < count; i++) free(ptrs[i]);
    double tfb = now_sec() - tf;

    char buf[96];
    snprintf(buf, sizeof(buf), "%s alloc", label);
    report(buf, count, tab, (uint64_t)count * sz);
    snprintf(buf, sizeof(buf), "%s free (%s)", label, lifo ? "LIFO" : "FIFO");
    report(buf, count, tfb);
    (void)sink;
}

static void test_batch()
{
    section("batch: fill a live set, then drain it");
    bench_batch("pool   64 B x1M", 64, 1'000'000, true);
    bench_batch("pool   64 B x1M", 64, 1'000'000, false);
    bench_batch("pool  512 B x256K", 512, 256'000, true);
    bench_batch("page  8 KiB x32K", 8 * 1024, 32'000, true);
    bench_batch("page 64 KiB x4K", 64 * 1024, 4'000, false);
}

/* ------------------------------------------------------------ random-order free */

/* Build a big live set, then free it in a shuffled order. Worst case for
 * free-list locality and page-range coalescing. */
static void test_random_free()
{
    section("random: free a large live set in shuffled order");
    const size_t COUNT = 800'000;
    const size_t SZ = 48;
    std::vector<void *> ptrs(COUNT, nullptr);
    std::vector<uint32_t> order(COUNT);
    volatile unsigned char sink = 0;

    for (size_t i = 0; i < COUNT; i++) {
        ptrs[i] = malloc(SZ);
        if (!ptrs[i]) { g_fails.fetch_add(1); break; }
        sink ^= scribble(ptrs[i], SZ, (unsigned char)i);
        order[i] = (uint32_t)i;
    }
    /* deterministic Fisher-Yates shuffle */
    uint32_t s = 0x9e3779b9u;
    for (size_t i = COUNT; i-- > 1;) {
        uint32_t j = xs(s) % (uint32_t)(i + 1);
        std::swap(order[i], order[j]);
    }

    double t0 = now_sec();
    for (size_t i = 0; i < COUNT; i++) free(ptrs[order[i]]);
    double dt = now_sec() - t0;
    report("random free 48 B x800K", COUNT, dt);
    (void)sink;
}

/* ------------------------------------------------------------ mixed workload */

/* A churn loop that draws a fresh size on every iteration spanning all three
 * allocator paths, keeping a bounded live set so memory pressure stays steady.
 * This is the closest thing here to a "real" allocation mix. */
static void test_mixed()
{
    section("mixed: small+large+huge, bounded live set");
    const size_t LIVE = 4096;          /* live slots */
    const uint64_t ITERS = 2'000'000;
    std::vector<void *> live(LIVE, nullptr);
    std::vector<size_t> live_sz(LIVE, 0);
    uint32_t s = 0x12345678u;
    uint64_t alloc_bytes = 0;
    volatile unsigned char sink = 0;

    double t0 = now_sec();
    for (uint64_t i = 0; i < ITERS; i++) {
        uint32_t slot = xs(s) % LIVE;
        if (live[slot]) { free(live[slot]); live[slot] = nullptr; }
        uint32_t r = xs(s) % 100;
        size_t sz;
        if (r < 80)      sz = 8  + (xs(s) % 1016);              /* 80% small  (pool) */
        else if (r < 98) sz = 4096 + (xs(s) % (256 * 1024));   /* 18% large  (page) */
        else             sz = 2 * 1024 * 1024 + (xs(s) % (1024 * 1024)); /* 2% huge */
        void *p = malloc(sz);
        if (!p) { g_fails.fetch_add(1); continue; }
        sink ^= scribble(p, sz, (unsigned char)i);
        live[slot] = p;
        live_sz[slot] = sz;
        alloc_bytes += sz;
    }
    for (size_t i = 0; i < LIVE; i++) if (live[i]) free(live[i]);
    double dt = now_sec() - t0;
    report("mixed 80/18/2 small/large/huge", ITERS, dt, alloc_bytes);
    (void)sink;
}

/* ------------------------------------------------------------ realloc */

static void test_realloc()
{
    section("realloc: grow + shrink an allocation repeatedly");
    const uint64_t ROUNDS = 200'000;
    volatile unsigned char sink = 0;
    double t0 = now_sec();
    uint64_t ops = 0;
    for (uint64_t r = 0; r < ROUNDS; r++) {
        void *p = malloc(16);
        size_t sz = 16;
        ops++;
        /* grow across the pool -> page boundary, then shrink back */
        for (size_t target : {64u, 1024u, 8192u, 65536u, 1024u, 64u}) {
            void *np = realloc(p, target);
            if (!np) { g_fails.fetch_add(1); break; }
            p = np; sz = target;
            sink ^= scribble(p, sz, (unsigned char)r);
            ops++;
        }
        free(p);
        ops++;
    }
    double dt = now_sec() - t0;
    report("realloc grow/shrink chain", ops, dt);
    (void)sink;
}

/* ------------------------------------------------------------ calloc */

static void test_calloc()
{
    section("calloc: zeroed allocations + zero-fill verification");
    const uint64_t N = 500'000;
    const size_t SZ = 512;
    uint64_t bad_zero = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < N; i++) {
        unsigned char *p = (unsigned char *)calloc(1, SZ);
        if (!p) { g_fails.fetch_add(1); break; }
        /* spot-check the memory really is zero */
        if (p[0] || p[SZ / 2] || p[SZ - 1]) bad_zero++;
        p[0] = 1;   /* dirty it so the next reuse must re-zero */
        free(p);
    }
    double dt = now_sec() - t0;
    CHECK(bad_zero == 0);
    report("calloc 512 B (zeroed)", N, dt, N * SZ);
}

/* ------------------------------------------------------------ operator new[] */

static void test_operator_new()
{
    section("new[]/delete[]: C++ array allocation path");
    const uint64_t N = 1'000'000;
    volatile unsigned char sink = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < N; i++) {
        auto *p = new unsigned char[200];
        p[0] = (unsigned char)i; p[199] = (unsigned char)i;
        sink ^= p[0] ^ p[199];
        delete[] p;
    }
    double dt = now_sec() - t0;
    report("new[]/delete[] 200 B", N, dt);
    (void)sink;
}

/* ------------------------------------------------------------ concurrent same-CPU */

/* Every thread allocates and frees on (mostly) its own CPU - this is the pool
 * fast path. Measures aggregate throughput and per-CPU pool scalability. */
static void test_concurrent_churn()
{
    section("concurrent: per-thread alloc/free (pool fast path)");
    const int NT = 16;
    const uint64_t PER = 1'000'000;
    const size_t SZ = 64;
    std::atomic<uint64_t> fails{0};
    std::vector<std::thread> ts;

    double t0 = now_sec();
    for (int t = 0; t < NT; t++)
        ts.emplace_back([&, t] {
            uint32_t s = 0x1000u + t;
            volatile unsigned char sink = 0;
            for (uint64_t i = 0; i < PER; i++) {
                void *p = malloc(SZ);
                if (!p) { fails.fetch_add(1); continue; }
                sink ^= scribble(p, SZ, (unsigned char)(xs(s)));
                free(p);
            }
            (void)sink;
        });
    for (auto &t : ts) t.join();
    double dt = now_sec() - t0;
    CHECK(fails.load() == 0);
    report("16 threads x 1M churn 64 B", (uint64_t)NT * PER, dt);
}

/* ------------------------------------------------------------ cross-thread free */

/* Producers allocate; consumers free what the producers made. This forces the
 * remote-free path (pool::free_different_cpu) where an object is returned to a
 * pool page owned by a different CPU. Uses a simple sharded hand-off queue. */
static void test_cross_thread_free()
{
    section("cross-thread: alloc on producers, free on consumers");
    const int PAIRS = 8;
    const uint64_t PER = 600'000;
    const size_t SZ = 96;
    std::atomic<uint64_t> fails{0};

    struct Shard {
        std::atomic<void *> slots[256];
        std::atomic<uint64_t> head{0};   /* producer reserves */
        std::atomic<uint64_t> tail{0};   /* consumer drains */
        std::atomic<bool> done{false};
    };
    std::vector<Shard *> shards;
    for (int i = 0; i < PAIRS; i++) shards.push_back(new Shard());

    std::vector<std::thread> ts;
    double t0 = now_sec();
    for (int p = 0; p < PAIRS; p++) {
        Shard *sh = shards[p];
        ts.emplace_back([&, sh, p] {           /* producer */
            uint32_t s = 0x2000u + p;
            for (uint64_t i = 0; i < PER; i++) {
                void *mem = malloc(SZ);
                if (!mem) { fails.fetch_add(1); continue; }
                scribble(mem, SZ, (unsigned char)xs(s));
                /* spin until a slot frees up (bounded ring of 256) */
                uint64_t idx = sh->head.load(std::memory_order_relaxed);
                while (idx - sh->tail.load(std::memory_order_acquire) >= 256)
                    std::this_thread::yield();
                sh->slots[idx & 255].store(mem, std::memory_order_release);
                sh->head.store(idx + 1, std::memory_order_release);
            }
            sh->done.store(true, std::memory_order_release);
        });
        ts.emplace_back([&, sh] {              /* consumer */
            uint64_t idx = 0;
            for (;;) {
                if (idx >= sh->head.load(std::memory_order_acquire)) {
                    if (sh->done.load(std::memory_order_acquire) &&
                        idx >= sh->head.load(std::memory_order_acquire)) break;
                    std::this_thread::yield();
                    continue;
                }
                void *mem = sh->slots[idx & 255].exchange(nullptr, std::memory_order_acquire);
                if (mem) { free(mem); idx++; sh->tail.store(idx, std::memory_order_release); }
            }
        });
    }
    for (auto &t : ts) t.join();
    double dt = now_sec() - t0;
    CHECK(fails.load() == 0);
    report("8 producer/consumer pairs 96 B", (uint64_t)PAIRS * PER, dt);
    for (auto *sh : shards) delete sh;
}

/* ------------------------------------------------------------ contiguous phys */

static void test_contiguous()
{
    section("contiguous: alloc_phys_contiguous_aligned churn");
    const uint64_t N = 50'000;
    const size_t SZ = 16 * 1024;
    const size_t ALIGN = 4096;
    uint64_t fails = 0;
    volatile unsigned char sink = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < N; i++) {
        void *p = memory::alloc_phys_contiguous_aligned(SZ, ALIGN);
        if (!p) { fails++; continue; }
        CHECK(((uintptr_t)p & (ALIGN - 1)) == 0);
        sink ^= scribble(p, SZ, (unsigned char)i);
        memory::free_phys_contiguous_aligned(p);
    }
    double dt = now_sec() - t0;
    CHECK(fails == 0);
    report("contiguous 16 KiB / 4 KiB align", N, dt, N * SZ);
    (void)sink;
}

/* ------------------------------------------------------------ driver */

int os_membench_main()
{
    printf("==== OSv memory-subsystem benchmark ====\n");

    size_t free_before = memory::stats::free();
    size_t total = memory::stats::total();
    printf("  memory: %zu KiB free / %zu KiB total at start\n",
           free_before / 1024, total / 1024);

    double whole = now_sec();

    test_churn();
    test_batch();
    test_random_free();
    test_mixed();
    test_realloc();
    test_calloc();
    test_operator_new();
    test_concurrent_churn();
    test_cross_thread_free();
    test_contiguous();

    double wall = now_sec() - whole;

    /* Leak check: free memory should return to roughly where it started.
     * Allow a small slack for pool pages kept warm in per-CPU caches. */
    size_t free_after = memory::stats::free();
    long delta = (long)free_before - (long)free_after;
    printf("  memory: %zu KiB free after (delta %ld KiB, %.1fs wall)\n",
           free_after / 1024, delta / 1024, wall);
    /* 16 MiB slack: per-CPU pools legitimately retain some warm pages. */
    CHECK(delta < 16 * 1024 * 1024);

    printf("\n==== os-membench: %d checks, %d failures ====\n",
           g_checks.load(), g_fails.load());
    if (g_fails.load() == 0)
        printf("RESULT: ALL MEMORY BENCHMARKS PASSED\n");
    else
        printf("RESULT: %d MEMORY BENCHMARK CHECK(S) FAILED\n", g_fails.load());
    return g_fails.load() ? 1 : 0;
}
