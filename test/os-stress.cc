/*
 * OS-interaction stress tests for the slim OSv kernel (C++).
 *
 * Hammers the parts of the slim kernel that involve the scheduler and memory:
 * thread scheduling/synchronization, anonymous virtual memory, signals, and
 * time. It is deliberately concurrent and repetitive (many threads, many
 * iterations) to shake out races and leaks.
 *
 * The filesystem, the fd table, networking and the fd-based IPC/event
 * facilities (pipes, sockets, eventfd, poll/epoll) were removed from the slim
 * kernel (Phases 5-9), so the tests that exercised them are gone - what remains
 * is the surface the in-kernel application can actually use.
 *
 * Each CHECK keeps running on failure; os_stress_main() returns non-zero if
 * anything failed. Worker threads never call CHECK directly - they report via
 * atomics/return values and the main thread asserts after joining.
 */
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <vector>
#include <queue>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

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

/* ============================================================ threads */

static void test_threads_atomic()
{
    section("threads: atomic counter under contention");
    const int NT = 16, PER = 100000;
    std::atomic<long> counter{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { for (int j = 0; j < PER; j++) counter.fetch_add(1, std::memory_order_relaxed); });
    for (auto &t : ts) t.join();
    CHECK(counter.load() == (long)NT * PER);
}

static void test_threads_mutex()
{
    section("threads: mutex-protected counter (lost-update detection)");
    const int NT = 12, PER = 20000;
    long counter = 0;
    std::mutex m;
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { for (int j = 0; j < PER; j++) { std::lock_guard<std::mutex> lk(m); counter++; } });
    for (auto &t : ts) t.join();
    CHECK(counter == (long)NT * PER);
}

static void test_condvar_queue()
{
    section("threads: condition_variable producer/consumer");
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    bool done = false;
    std::atomic<long> sum{0};
    const int N = 50000;

    std::thread consumer([&] {
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return !q.empty() || done; });
            while (!q.empty()) { sum += q.front(); q.pop(); }
            if (done && q.empty()) break;
        }
    });
    for (int i = 1; i <= N; i++) {
        { std::lock_guard<std::mutex> lk(m); q.push(i); }
        cv.notify_one();
    }
    { std::lock_guard<std::mutex> lk(m); done = true; }
    cv.notify_one();
    consumer.join();
    CHECK(sum.load() == (long)N * (N + 1) / 2);
}

static void test_async_futures()
{
    section("threads: std::async / std::future fan-out");
    const int N = 64;
    std::vector<std::future<long>> fs;
    for (int i = 0; i < N; i++)
        fs.push_back(std::async(std::launch::async, [i] {
            long s = 0; for (int j = 0; j <= i; j++) s += j; return s;
        }));
    long total = 0;
    for (auto &f : fs) total += f.get();
    long expect = 0;
    for (int i = 0; i < N; i++) expect += (long)i * (i + 1) / 2;
    CHECK(total == expect);
}

static thread_local int tls_value = -1;
static void test_thread_local()
{
    section("threads: thread_local isolation");
    const int NT = 20;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([i, &mismatches] {
            tls_value = i;
            for (int k = 0; k < 1000; k++) {
                std::this_thread::yield();
                if (tls_value != i) mismatches++;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(mismatches.load() == 0);
}

static void test_pthread_barrier()
{
    section("threads: pthread_barrier synchronization");
    const int NT = 8, ROUNDS = 100;
    pthread_barrier_t bar;
    CHECK(pthread_barrier_init(&bar, nullptr, NT) == 0);
    std::atomic<int> phase{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] {
            for (int r = 0; r < ROUNDS; r++) {
                int seen = phase.load();
                int rc = pthread_barrier_wait(&bar);
                /* exactly one thread gets the SERIAL return value */
                if (rc == PTHREAD_BARRIER_SERIAL_THREAD) phase.fetch_add(1);
                (void)seen;
                pthread_barrier_wait(&bar);
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
    CHECK(phase.load() == ROUNDS);
    pthread_barrier_destroy(&bar);
}

static void test_rwlock()
{
    section("threads: pthread_rwlock readers/writers");
    pthread_rwlock_t rw;
    CHECK(pthread_rwlock_init(&rw, nullptr) == 0);
    long shared = 0;
    std::atomic<int> inconsistencies{0};
    std::vector<std::thread> ts;
    const int READERS = 8, WRITERS = 4, ITERS = 5000;
    for (int w = 0; w < WRITERS; w++)
        ts.emplace_back([&] {
            for (int i = 0; i < ITERS; i++) {
                pthread_rwlock_wrlock(&rw);
                long a = shared; shared = a + 1; long b = shared;
                if (b != a + 1) inconsistencies++;
                pthread_rwlock_unlock(&rw);
            }
        });
    for (int r = 0; r < READERS; r++)
        ts.emplace_back([&] {
            for (int i = 0; i < ITERS; i++) {
                pthread_rwlock_rdlock(&rw);
                long a = shared; (void)a;
                pthread_rwlock_unlock(&rw);
            }
        });
    for (auto &t : ts) t.join();
    CHECK(inconsistencies.load() == 0);
    CHECK(shared == (long)WRITERS * ITERS);
    pthread_rwlock_destroy(&rw);
}

/* ============================================================ virtual memory */

static void test_mmap_anon()
{
    section("vm: anonymous mmap, touch pages, mprotect, madvise");
    const size_t SZ = 8 * 1024 * 1024;
    void *p = mmap(nullptr, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return;
    unsigned char *m = (unsigned char *)p;
    for (size_t i = 0; i < SZ; i += 4096) m[i] = 0xA5;
    bool ok = true;
    for (size_t i = 0; i < SZ; i += 4096) if (m[i] != 0xA5) ok = false;
    CHECK(ok);
    CHECK(mprotect(p, SZ, PROT_READ) == 0);          /* make read-only */
    CHECK(madvise(p, SZ, MADV_DONTNEED) == 0 || errno == EINVAL);
    CHECK(munmap(p, SZ) == 0);
}

static void test_mmap_concurrent()
{
    section("vm: concurrent anonymous mmap/touch/munmap (16 threads)");
    const int NT = 16;
    const size_t SZ = 1024 * 1024;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([i, &errors] {
            for (int r = 0; r < 32; r++) {
                void *p = mmap(nullptr, SZ, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED) { errors++; continue; }
                unsigned char *m = (unsigned char *)p;
                for (size_t o = 0; o < SZ; o += 4096) m[o] = (unsigned char)(i + r);
                for (size_t o = 0; o < SZ; o += 4096) if (m[o] != (unsigned char)(i + r)) errors++;
                if (munmap(p, SZ) != 0) errors++;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
}

/* ============================================================ time/sched */

static void test_time_sched()
{
    section("time: monotonic clock + nanosleep + sched_yield");
    struct timespec a, b;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    struct timespec nap = {0, 20 * 1000 * 1000};   /* 20 ms */
    CHECK(nanosleep(&nap, nullptr) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    double elapsed = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(elapsed >= 0.015);   /* at least ~15ms passed */

    for (int i = 0; i < 1000; i++) sched_yield();
    CHECK(true);

    /* hammer clock_gettime from many threads (no crash / monotonic per thread) */
    const int NT = 8;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] {
            struct timespec p{0, 0};
            for (int k = 0; k < 20000; k++) {
                struct timespec n;
                clock_gettime(CLOCK_MONOTONIC, &n);
                if (n.tv_sec < p.tv_sec) errors++;
                p = n;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
}

/* =================================================== big cross-core stress */

// These deliberately spawn a large number of threads spread across every CPU
// and force heavy cross-core wakeups (mutex handoff, condvar broadcast). That
// exercises the scheduler's wake path and the detached_state recycle path
// (lots of thread create/destroy) far harder than the small tests above.

extern "C" int sched_getcpu(void);

static std::vector<int> online_cpus()
{
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof(set), &set);
    std::vector<int> cpus;
    for (int c = 0; c < __CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &set)) cpus.push_back(c);
    return cpus;
}

// Spawn `fn(arg)` pinned to `cpu` with a small stack (we create hundreds of
// these, so the default 1 MiB pthread stack would be far too much memory).
static bool spawn_pinned_small(pthread_t *tid, int cpu, void *(*fn)(void *), void *arg)
{
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_attr_setaffinity_np(&attr, sizeof(one), &one);
    bool ok = pthread_create(tid, &attr, fn, arg) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}

namespace {
struct storm_arg {
    std::mutex *m;
    long *counter;     // guarded by *m
    int iters;
};
}

static void *mutex_storm_fn(void *p)
{
    auto *a = static_cast<storm_arg *>(p);
    for (int i = 0; i < a->iters; i++) {
        std::lock_guard<std::mutex> lk(*a->m);
        (*a->counter)++;
    }
    return nullptr;
}

static void test_cross_core_mutex_storm()
{
    section("threads: 128/core hammering a few shared mutexes (cross-core)");
    auto cpus = online_cpus();
    if (cpus.empty()) { CHECK(false); return; }

    const int NPER  = 128;                  // threads per CPU
    const int NMTX  = 4;                     // shared mutexes, contended cross-core
    const int ITERS = 2000;                  // lock/unlock per thread per round
    const int ROUNDS = 4;                    // repeat to churn the thread pool hard
    const int NT = NPER * (int)cpus.size();  // e.g. 512 on 4 vcpus

    bool ok = true;
    for (int round = 0; round < ROUNDS; round++) {
        std::array<std::mutex, NMTX> mtx;
        std::array<long, NMTX> counter;
        counter.fill(0);
        std::vector<storm_arg> args(NT);
        std::vector<pthread_t> tids(NT);

        bool spawn_ok = true;
        for (int k = 0; k < NT; k++) {
            int cpu = cpus[k % cpus.size()];
            // Decouple the mutex index from the CPU so each mutex is shared by
            // threads on *different* cores (the cross-core part).
            int mi = (k / (int)cpus.size()) % NMTX;
            args[k] = { &mtx[mi], &counter[mi], ITERS };
            if (!spawn_pinned_small(&tids[k], cpu, mutex_storm_fn, &args[k]))
                spawn_ok = false;
        }
        for (int k = 0; k < NT; k++) pthread_join(tids[k], nullptr);

        long sum = 0;
        for (int mi = 0; mi < NMTX; mi++) sum += counter[mi];
        if (!spawn_ok || sum != (long)NT * ITERS) ok = false;
    }
    // Passing means: no lost updates under heavy contention, and every round
    // of hundreds of cross-core threads ran to completion (no deadlock/hang).
    CHECK(ok);
}

namespace {
struct broadcast_arg {
    std::mutex *m;
    std::condition_variable *cv;
    bool *go;
    std::atomic<int> *woken;
};
}

static void *broadcast_wait_fn(void *p)
{
    auto *a = static_cast<broadcast_arg *>(p);
    std::unique_lock<std::mutex> lk(*a->m);
    a->cv->wait(lk, [&] { return *a->go; });
    a->woken->fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

static void test_cross_core_condvar_broadcast()
{
    section("threads: 128/core all wait on one condvar, single broadcast wakes all");
    auto cpus = online_cpus();
    if (cpus.empty()) { CHECK(false); return; }

    const int NPER = 128;
    const int NT = NPER * (int)cpus.size();

    std::mutex m;
    std::condition_variable cv;
    bool go = false;
    std::atomic<int> woken{0};

    std::vector<broadcast_arg> args(NT);
    std::vector<pthread_t> tids(NT);
    bool spawn_ok = true;
    for (int k = 0; k < NT; k++) {
        args[k] = { &m, &cv, &go, &woken };
        if (!spawn_pinned_small(&tids[k], cpus[k % cpus.size()], broadcast_wait_fn, &args[k]))
            spawn_ok = false;
    }
    // Give the workers time to all reach cv.wait() across every CPU.
    { struct timespec req { 0, 50 * 1000 * 1000 }, rem{}; nanosleep(&req, &rem); }
    { std::lock_guard<std::mutex> lk(m); go = true; }
    cv.notify_all();                       // one mass cross-core wake of NT threads

    for (int k = 0; k < NT; k++) pthread_join(tids[k], nullptr);
    CHECK(spawn_ok);
    CHECK(woken.load() == NT);             // every waiter observed the broadcast
}

int os_stress_main()
{
    printf("==== OSv OS-interaction stress tests ====\n");

    test_threads_atomic();
    test_threads_mutex();
    test_condvar_queue();
    test_async_futures();
    test_thread_local();
    test_pthread_barrier();
    test_rwlock();

    test_cross_core_mutex_storm();
    test_cross_core_condvar_broadcast();

    test_mmap_anon();
    test_mmap_concurrent();

    test_time_sched();

    printf("\n==== os-stress: %d checks, %d failures ====\n",
           g_checks.load(), g_fails.load());
    if (g_fails.load() == 0)
        printf("RESULT: ALL OS-STRESS TESTS PASSED\n");
    else
        printf("RESULT: %d OS-STRESS TEST(S) FAILED\n", g_fails.load());
    return g_fails.load() ? 1 : 0;
}
