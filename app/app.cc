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
#include <osv/power.hh>
#include <osv/uperf.hh>

constexpr uint64_t n = 1ul << 20;

extern "C" void osv_app_main() {
  PerfEvent e;
  e.startCounters();

  for (uint64_t i{0}; i < n; ++i)
    asm volatile("" ::: "memory");

  e.stopCounters();
  e.printReport(std::cout, n);
  osv::poweroff();
}
