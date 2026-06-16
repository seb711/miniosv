/*
 * time() for the llvm-libc build (LLVM_LIBC_PLAN.md, Phase 4). Only
 * built with conf_llvm_libc=1; musl's time/time.o owns it otherwise.
 * llvm-libc's time() is Linux-only (internal vDSO/syscall clock), so
 * OSv provides it directly on its own clock_gettime.
 */

#include <time.h>
#include <osv/export.h>

extern "C" OSV_LIBC_API
time_t time(time_t *tp)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (tp) {
        *tp = ts.tv_sec;
    }
    return ts.tv_sec;
}
