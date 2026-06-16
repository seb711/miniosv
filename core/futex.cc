/*
 * Copyright (C) 2013-2014 Cloudius Systems, Ltd.
 * Copyright (C) 2018-2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

// The last remnant of the Linux syscall emulation that used to live in
// linux.cc: there is no SYSCALL entry path and no syscall table anymore,
// but a handful of statically linked consumers still need these functions:
//  - core/sched.cc calls futex() directly for robust-list/clear-child-tid
//    wakeups,
//  - libstdc++.a (guard.o, futex.o) and libgcc.a call the libc syscall()
//    function with SYS_futex for __cxa_guard_* contention and atomic waits,
//  - leanstore's task managers call gettid().
// Everything else answers ENOSYS.

#include <osv/debug.hh>
#include <osv/sched.hh>
#include <osv/mutex.h>
#include <osv/waitqueue.hh>
#include <osv/export.h>

#include <syscall.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

#include <unordered_map>

extern "C" OSV_LIBC_API long gettid()
{
    // During early boot (before the scheduler starts its first thread) there is
    // no current thread yet, but libc++abi's __cxa_guard_acquire - used to guard
    // function-local static initialization, e.g. in C++ global constructors run
    // from premain() - calls gettid() via syscall(SYS_gettid) to tag the
    // initializing thread. Return a sentinel rather than dereferencing null.
    auto* t = sched::thread::current();
    return t ? t->id() : 0;
}

// We don't expect applications to use the Linux futex() system call (it is
// normally only used to implement higher-level synchronization mechanisms),
// but unfortunately gcc's C++ runtime uses a subset of futex in the
// __cxa__guard_* functions, which safeguard the concurrent initialization
// of function-scope static objects. We only implement here this subset.
// The __cxa_guard_* functions only call futex in the rare case of contention,
// in fact so rarely that OSv existed for a year before anyone noticed futex
// was missing. So the performance of this implementation is not critical.
static std::unordered_map<void*, waitqueue> queues;
static mutex queues_mutex;

#define FUTEX_BITSET_MATCH_ANY  0xffffffff

int futex(int *uaddr, int op, int val, const struct timespec *timeout,
        int *uaddr2, uint32_t val3)
{
    switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT_BITSET:
        if (val3 != FUTEX_BITSET_MATCH_ANY) {
            abort("Unimplemented futex() operation %d\n", op);
        }

    case FUTEX_WAIT:
        WITH_LOCK(queues_mutex) {
            if (*uaddr == val) {
                waitqueue &q = queues[uaddr];
                if (timeout) {
                    sched::timer tmr(*sched::thread::current());
                    if ((op & FUTEX_CMD_MASK) == FUTEX_WAIT_BITSET) {
                        // If FUTEX_WAIT_BITSET we need to interpret timeout as an absolute
                        // time point. If futex operation FUTEX_CLOCK_REALTIME is set we will use
                        // real-time clock otherwise we will use monotonic clock
                        if (op & FUTEX_CLOCK_REALTIME) {
                            tmr.set(osv::clock::wall::time_point(std::chrono::seconds(timeout->tv_sec) +
                                                                 std::chrono::nanoseconds(timeout->tv_nsec)));
                        } else {
                            tmr.set(osv::clock::uptime::time_point(std::chrono::seconds(timeout->tv_sec) +
                                                                   std::chrono::nanoseconds(timeout->tv_nsec)));
                        }
                    } else {
                        tmr.set(std::chrono::seconds(timeout->tv_sec) +
                                std::chrono::nanoseconds(timeout->tv_nsec));
                    }
                    sched::thread::wait_for(queues_mutex, tmr, q);
                    // FIXME: testing if tmr was expired isn't quite right -
                    // we could have had both a wakeup and timer expiration
                    // racing. It would be more correct to check if we were
                    // waken by a FUTEX_WAKE. But how?
                    if (tmr.expired()) {
                        errno = ETIMEDOUT;
                        return -1;
                    }
                } else {
                    q.wait(queues_mutex);
                }
                return 0;
            } else {
                errno = EWOULDBLOCK;
                return -1;
            }
        }
    case FUTEX_WAKE:
        if(val < 0) {
            errno = EINVAL;
            return -1;
        }

        WITH_LOCK(queues_mutex) {
            auto i = queues.find(uaddr);
            if (i != queues.end()) {
                int waken = 0;
                while( (val > waken) && !(i->second.empty()) ) {
                    i->second.wake_one(queues_mutex);
                    waken++;
                }
                if(i->second.empty()) {
                    queues.erase(i);
                }
                return waken;
            }
        }
        return 0;
    default:
        abort("Unimplemented futex() operation %d\n", op);
    }
}

extern "C" OSV_LIBC_API long syscall(long number, ...)
{
    switch (number) {
    case SYS_futex: {
        va_list args;
        va_start(args, number);
        auto uaddr = va_arg(args, int*);
        auto op = va_arg(args, int);
        auto val = va_arg(args, int);
        auto timeout = va_arg(args, const struct timespec*);
        auto uaddr2 = va_arg(args, int*);
        auto val3 = va_arg(args, uint32_t);
        va_end(args);
        return futex(uaddr, op, val, timeout, uaddr2, val3);
    }
    case SYS_gettid:
        return gettid();
    }
    errno = ENOSYS;
    return -1;
}
