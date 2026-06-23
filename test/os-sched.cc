/*
 * Scheduler / thread-lifecycle test suite for the slim OSv kernel (C++).
 *
 * This suite deliberately concentrates on the thread *lifecycle* and the
 * machinery the scheduler uses to create, run, and tear down threads -- the
 * code paths exercised by thread::make()/start()/complete()/destroy()/join()/
 * detach() and the per-thread resource pools (thread object storage, the
 * embedded TLS/TCB, and the stack). It is the regression net for the work that
 * makes detached-thread reclamation happen without a dedicated reaper thread,
 * so it leans hard on:
 *
 *   - detached threads (which self-reclaim): create floods, churn, and races;
 *   - thread teardown under churn: storage/stack/TLS pools recycled heavily;
 *   - TLS isolation: each thread must see its own __thread storage even as
 *     slots (and their embedded TCBs) are recycled;
 *   - the join/detach/exit corners: detach-while-running, detach-after-exit,
 *     pthread_exit() mid-function, join of an already-finished thread;
 *   - stack reuse correctness: a pooled stack handed to a new thread must work
 *     under deep use.
 *
 * Each CHECK keeps running on failure; os_sched_main() returns non-zero if
 * anything failed. Worker threads never call CHECK directly -- they report via
 * atomics/return values and the main thread asserts after joining/draining.
 */
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstdint>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
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

// Spin/sleep until 'val' reaches 'target' or we exceed 'timeout_ms'. Returns
// true on success. Used to wait for detached threads (which cannot be joined).
static bool wait_for(std::atomic<int>& val, int target, int timeout_ms = 10000)
{
    const int step_us = 200;
    long waited_us = 0;
    while (val.load(std::memory_order_acquire) != target) {
        if (waited_us > (long)timeout_ms * 1000) {
            return false;
        }
        struct timespec ts{0, step_us * 1000};
        nanosleep(&ts, nullptr);
        waited_us += step_us;
    }
    return true;
}

static void small_sleep(int us)
{
    struct timespec ts{0, (long)us * 1000};
    nanosleep(&ts, nullptr);
}

/* ===================================================== basic create/join */

static void test_create_join_sum()
{
    section("lifecycle: create + join, every worker runs exactly once");
    const int NT = 64;
    std::atomic<long> sum{0};
    std::atomic<int> ran{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&, i] { sum.fetch_add(i); ran.fetch_add(1); });
    for (auto& t : ts) t.join();
    CHECK(ran.load() == NT);
    CHECK(sum.load() == (long)NT * (NT - 1) / 2);
}

/* ===================================================== detached threads */

static void test_detached_all_run()
{
    section("detached: many detached threads each run exactly once");
    const int NT = 200;
    std::atomic<int> done{0};
    std::atomic<long> sum{0};
    for (int i = 0; i < NT; i++) {
        std::thread t([&, i] { sum.fetch_add(i); done.fetch_add(1); });
        t.detach();
    }
    CHECK(wait_for(done, NT));
    CHECK(done.load() == NT);
    CHECK(sum.load() == (long)NT * (NT - 1) / 2);
}

static void test_detached_churn()
{
    section("detached: sustained create/exit churn (pool recycling)");
    // Many rounds of detached threads, bounded live set, to recycle the thread
    // object / TLS / stack pools many times over.
    const int ROUNDS = 40, BATCH = 32;
    std::atomic<int> done{0};
    for (int r = 0; r < ROUNDS; r++) {
        std::atomic<int> round_done{0};
        for (int i = 0; i < BATCH; i++) {
            std::thread t([&] { round_done.fetch_add(1); done.fetch_add(1); });
            t.detach();
        }
        // Throttle so the live set stays bounded and dead threads get reclaimed.
        CHECK(wait_for(round_done, BATCH));
    }
    CHECK(done.load() == ROUNDS * BATCH);
}

static void test_detached_immediate_exit()
{
    section("detached: threads that exit almost immediately");
    const int NT = 256;
    std::atomic<int> done{0};
    for (int i = 0; i < NT; i++) {
        std::thread t([&] { done.fetch_add(1, std::memory_order_relaxed); });
        t.detach();
    }
    CHECK(wait_for(done, NT));
}

/* ===================================================== TLS isolation */

static __thread long tls_value;
static __thread long tls_array[8];

static void test_tls_isolation()
{
    section("tls: each thread sees private __thread storage under churn");
    const int NT = 64;
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++) {
        ts.emplace_back([&, i] {
            tls_value = i * 7 + 1;
            for (int k = 0; k < 8; k++) tls_array[k] = (long)i * 100 + k;
            // Yield repeatedly so the scheduler interleaves us with peers; our
            // TLS must be unchanged across context switches.
            for (int spin = 0; spin < 50; spin++) {
                sched_yield();
                if (tls_value != i * 7 + 1) { bad.fetch_add(1); break; }
                for (int k = 0; k < 8; k++)
                    if (tls_array[k] != (long)i * 100 + k) { bad.fetch_add(1); break; }
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(bad.load() == 0);
}

static void test_tls_after_recycle()
{
    section("tls: recycled thread slots start with zero-initialized TLS");
    // A fresh thread must observe its __thread vars zero-initialized even when
    // running on a recycled storage slot whose previous owner wrote to them.
    const int ROUNDS = 64;
    std::atomic<int> nonzero{0};
    for (int r = 0; r < ROUNDS; r++) {
        std::thread t([&] {
            if (tls_value != 0) nonzero.fetch_add(1);
            for (int k = 0; k < 8; k++) if (tls_array[k] != 0) nonzero.fetch_add(1);
            tls_value = 0xdead;          // dirty the slot for the next reuse
            for (int k = 0; k < 8; k++) tls_array[k] = 0xbeef;
        });
        t.join();   // serialize so the next thread likely reuses this slot
    }
    CHECK(nonzero.load() == 0);
}

/* ===================================================== stack reuse */

static long recurse_sum(int depth)
{
    // Touch a chunk of stack at each level so the stack is genuinely used down
    // toward its base (where the pool's freelist node lived while cached).
    volatile char buf[256];
    for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)(depth + i);
    long s = buf[depth & 0xff];
    if (depth <= 0) return s;
    return s + recurse_sum(depth - 1);
}

static void test_stack_reuse_deep()
{
    section("stack: pooled stacks are reusable under deep recursion");
    const int ROUNDS = 40;
    std::atomic<int> bad{0};
    for (int r = 0; r < ROUNDS; r++) {
        std::thread t([&] {
            long a = recurse_sum(300);
            long b = recurse_sum(300);
            if (a != b) bad.fetch_add(1);   // deterministic within a run
        });
        t.join();
    }
    CHECK(bad.load() == 0);
}

/* ===================================================== pthread exit/detach */

static void* exit_midway(void* arg)
{
    long v = (long)(intptr_t)arg;
    pthread_exit((void*)(intptr_t)(v * 3 + 1));
    return (void*)0;   // unreachable
}

static void test_pthread_exit_join()
{
    section("pthread: pthread_exit() mid-function, value seen by join");
    const int NT = 48;
    int bad = 0;
    for (int i = 0; i < NT; i++) {
        pthread_t th;
        CHECK(pthread_create(&th, nullptr, exit_midway, (void*)(intptr_t)i) == 0);
        void* ret = nullptr;
        CHECK(pthread_join(th, &ret) == 0);
        if ((long)(intptr_t)ret != (long)i * 3 + 1) bad++;
    }
    CHECK(bad == 0);
}

static std::atomic<int> g_pthread_detached_done{0};
static void* detached_worker(void*)
{
    small_sleep(50);
    g_pthread_detached_done.fetch_add(1);
    return nullptr;
}

static void test_pthread_detach_attr()
{
    section("pthread: PTHREAD_CREATE_DETACHED attribute");
    const int NT = 64;
    g_pthread_detached_done.store(0);
    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    for (int i = 0; i < NT; i++) {
        pthread_t th;
        CHECK(pthread_create(&th, &attr, detached_worker, nullptr) == 0);
    }
    pthread_attr_destroy(&attr);
    CHECK(wait_for(g_pthread_detached_done, NT));
}

static void test_pthread_detach_running()
{
    section("pthread: pthread_detach() of a still-running thread");
    const int NT = 64;
    g_pthread_detached_done.store(0);
    for (int i = 0; i < NT; i++) {
        pthread_t th;
        CHECK(pthread_create(&th, nullptr, detached_worker, nullptr) == 0);
        // Detach while it is (probably) still sleeping in detached_worker.
        CHECK(pthread_detach(th) == 0);
    }
    CHECK(wait_for(g_pthread_detached_done, NT));
}

static void* quick_worker(void*)
{
    g_pthread_detached_done.fetch_add(1);
    return nullptr;
}

static void test_pthread_detach_after_exit()
{
    section("pthread: detach-after-completion race (detach a finished thread)");
    // Create a thread, let it certainly finish, then detach it. This drives the
    // "completed before detach()" handshake arm: nobody joins, so detach() must
    // be what triggers reclamation.
    const int NT = 64;
    g_pthread_detached_done.store(0);
    for (int i = 0; i < NT; i++) {
        pthread_t th;
        CHECK(pthread_create(&th, nullptr, quick_worker, nullptr) == 0);
        small_sleep(200);                 // let it run to completion
        CHECK(pthread_detach(th) == 0);   // detach the (already finished) thread
    }
    CHECK(wait_for(g_pthread_detached_done, NT));
}

/* ===================================================== mixed / races */

static void test_mixed_join_detach()
{
    section("mixed: interleaved joinable and detached threads");
    const int NT = 128;
    std::atomic<int> detached_done{0};
    std::atomic<long> joined_sum{0};
    std::vector<std::thread> joinable;
    int detached_count = 0;
    for (int i = 0; i < NT; i++) {
        if (i & 1) {
            std::thread t([&, i] { detached_done.fetch_add(1); (void)i; });
            t.detach();
            detached_count++;
        } else {
            joinable.emplace_back([&, i] { joined_sum.fetch_add(i); });
        }
    }
    for (auto& t : joinable) t.join();
    CHECK(wait_for(detached_done, detached_count));
    long expect = 0;
    for (int i = 0; i < NT; i += 2) expect += i;
    CHECK(joined_sum.load() == expect);
}

static void test_join_finished_thread()
{
    section("join: joining a thread that has already finished");
    const int NT = 64;
    std::atomic<int> ran{0};
    for (int i = 0; i < NT; i++) {
        std::thread t([&] { ran.fetch_add(1); });
        small_sleep(100);   // very likely finished before we join
        t.join();           // must not hang or crash
    }
    CHECK(ran.load() == NT);
}

static void test_threads_spawning_threads()
{
    section("nesting: threads that themselves create and join threads");
    const int OUTER = 16, INNER = 8;
    std::atomic<long> sum{0};
    std::vector<std::thread> outer;
    for (int i = 0; i < OUTER; i++) {
        outer.emplace_back([&, i] {
            std::vector<std::thread> inner;
            for (int j = 0; j < INNER; j++)
                inner.emplace_back([&, i, j] { sum.fetch_add((long)i * INNER + j); });
            for (auto& t : inner) t.join();
        });
    }
    for (auto& t : outer) t.join();
    long expect = 0;
    for (int i = 0; i < OUTER; i++) for (int j = 0; j < INNER; j++) expect += (long)i * INNER + j;
    CHECK(sum.load() == expect);
}

static void test_concurrent_detach_storm()
{
    section("stress: concurrent producers each spawning detached threads");
    // Several producer threads concurrently create detached threads, stressing
    // thread-id allocation, the pools, and the registry from multiple CPUs.
    const int PRODUCERS = 8, PER = 64;
    std::atomic<int> done{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; p++) {
        producers.emplace_back([&] {
            for (int i = 0; i < PER; i++) {
                std::thread t([&] { done.fetch_add(1, std::memory_order_relaxed); });
                t.detach();
                if ((i & 7) == 0) sched_yield();
            }
        });
    }
    for (auto& t : producers) t.join();
    CHECK(wait_for(done, PRODUCERS * PER));
}

/* ===================================================== scheduling behavior */

static void test_yield_progress()
{
    section("sched: sched_yield lets peers make progress (no starvation)");
    const int NT = 8;
    std::atomic<bool> stop{false};
    std::vector<std::atomic<long>> progress(NT);
    for (auto& p : progress) p.store(0);
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&, i] {
            while (!stop.load(std::memory_order_relaxed)) {
                progress[i].fetch_add(1, std::memory_order_relaxed);
                sched_yield();
            }
        });
    small_sleep(50000);   // 50ms
    stop.store(true);
    for (auto& t : ts) t.join();
    int made_progress = 0;
    for (auto& p : progress) if (p.load() > 0) made_progress++;
    CHECK(made_progress == NT);   // every thread got scheduled at least once
}

static void test_sleep_wakes()
{
    section("sched: timed sleeps complete and wake the sleeper");
    const int NT = 16;
    std::atomic<int> woke{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { small_sleep(5000); woke.fetch_add(1); });
    for (auto& t : ts) t.join();
    CHECK(woke.load() == NT);
}

static void test_high_thread_count()
{
    section("scale: large number of sequential short-lived threads");
    // Exercises thread-id allocation/recycling and pool high-water behavior
    // over many creations without a large live set.
    const int N = 1000;
    std::atomic<int> ran{0};
    for (int i = 0; i < N; i++) {
        std::thread t([&] { ran.fetch_add(1, std::memory_order_relaxed); });
        t.join();
    }
    CHECK(ran.load() == N);
}

/* ===================================================== entry point */

int os_sched_main()
{
    printf("==== os-sched: scheduler / thread-lifecycle tests ====\n");
    g_checks.store(0);
    g_fails.store(0);

    test_create_join_sum();
    test_detached_all_run();
    test_detached_churn();
    test_detached_immediate_exit();
    test_tls_isolation();
    test_tls_after_recycle();
    test_stack_reuse_deep();
    test_pthread_exit_join();
    test_pthread_detach_attr();
    test_pthread_detach_running();
    test_pthread_detach_after_exit();
    test_mixed_join_detach();
    test_join_finished_thread();
    test_threads_spawning_threads();
    test_concurrent_detach_storm();
    test_yield_progress();
    test_sleep_wakes();
    test_high_thread_count();

    int checks = g_checks.load(), fails = g_fails.load();
    printf("==== os-sched: %d checks, %d failures ====\n", checks, fails);
    printf("RESULT: %s\n", fails ? "SCHED TESTS FAILED" : "ALL SCHED TESTS PASSED");
    return fails ? 1 : 0;
}
