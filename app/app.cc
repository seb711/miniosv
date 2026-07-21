// A multicore smoke test for miniOSv.
//
// Prints how many CPUs the kernel recognized, then pins one CPU-bound worker
// thread to every CPU and lets them all spin on a heavy integer-mixing loop
// for 30 seconds. Each worker counts the work units it completed; afterwards we
// stop the workers and print a per-CPU breakdown. If every CPU reports a
// similar, non-zero count, then all cores ran compute in parallel — which is
// the thing we want to confirm the kernel and scheduler can actually do.

#include <osv/sched.hh>
#include <osv/power.hh>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// One heavy work unit: a tight, data-dependent integer-mixing loop. It is
// data-dependent (each step feeds the next) so the optimizer cannot fold it
// away or hoist it out of the worker's loop. Returns the final state so the
// caller can keep it observable.
uint64_t burn(uint64_t state)
{
    for (int k = 0; k < 200000; k++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        state ^= state >> 29;
    }
    return state;
}

} // namespace

extern "C" void osv_app_main()
{
    const unsigned ncpus = sched::cpus.size();
    printf("smoke: kernel recognized %u CPU(s)\n", ncpus);

    std::atomic<bool> stop{false};
    // One counter per CPU. Sized once and never resized, so element references
    // captured by the workers stay valid for the whole run.
    std::vector<std::atomic<uint64_t>> work_units(ncpus);
    std::vector<sched::thread*> workers;
    workers.reserve(ncpus);

    printf("smoke: starting %u worker(s), one pinned per CPU\n", ncpus);
    for (unsigned i = 0; i < ncpus; i++) {
        work_units[i].store(0, std::memory_order_relaxed);
        char name[16];
        snprintf(name, sizeof(name), "smoke-%u", i);
        auto* t = sched::thread::make(
            [&stop, &work_units, i] {
                uint64_t state = 0x9e3779b97f4a7c15ULL ^ (uint64_t(i) + 1);
                uint64_t units = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    state = burn(state);
                    units++;
                }
                work_units[i].store(units, std::memory_order_relaxed);
                // Keep `state` live so the compiler can't drop the burn loop.
                asm volatile("" :: "r"(state) : "memory");
            },
            sched::thread::attr().pin(sched::cpus[i]).name(name));
        workers.push_back(t);
        t->start();
    }

    printf("smoke: running for 10 seconds...\n");
    sched::thread::sleep(std::chrono::seconds(10));

    printf("smoke: stopping workers\n");
    stop.store(true, std::memory_order_relaxed);
    for (auto* t : workers) {
        t->join();
        delete t;
    }

    uint64_t total = 0, min_u = UINT64_MAX, max_u = 0;
    for (unsigned i = 0; i < ncpus; i++) {
        uint64_t u = work_units[i].load(std::memory_order_relaxed);
        total += u;
        if (u < min_u) min_u = u;
        if (u > max_u) max_u = u;
        printf("smoke: cpu %3u -> %llu work units\n", i, (unsigned long long)u);
    }
    printf("smoke: total %llu work units across %u CPU(s) in 30s "
           "(min %llu, max %llu)\n",
           (unsigned long long)total, ncpus,
           (unsigned long long)min_u, (unsigned long long)max_u);
    printf("smoke: done, exiting\n");

    osv::poweroff();
}
