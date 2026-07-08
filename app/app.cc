// The OSv application.
//
// Unlike upstream OSv, the application is not a separate ELF shared object
// loaded at runtime from a filesystem image. It is compiled and statically
// linked directly into the kernel image (app.o in loader.elf). The kernel
// calls osv_app_main() once, after early initialization, on a dedicated thread.
//
// This is a minimal "hello world" placeholder: it prints a greeting and powers
// the machine off so a run exits cleanly. The conformance and stress suites
// that used to live here now build with `make app=tests` (see test/).

#include <cstdint>
#include <iostream>
#include <osv/power.hh>
#include <osv/perf.hh>

extern "C" void osv_app_main()
{
    printf("Hello, world from OSv!\n");

    // Small perf demo: measure a busy loop with the default hardware counters.
    perf::PerfEvent perf;
    perf.startCounters();
    for (volatile int i = 0; i < 1'000'000; i = i + 1);
    perf.stopCounters();
    perf.printReport(std::cout, 1);

    // Do not power off: keep the machine running so the boot output stays
    // visible on the (cloud) serial console instead of the instance stopping
    // the moment it finishes booting. The empty asm keeps the compiler from
    // optimizing this infinite loop away.
    while (true) {
        asm volatile("" ::: "memory");
    }
}
