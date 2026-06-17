/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <assert.h>
#include "clock.hh"

// clock.hh now pulls in <chrono> (for the folded-in clock_event types), which
// transitively declares the C clock() function from <time.h>. That function
// shadows the clock class at namespace scope, so the bare type must be written
// as "class clock" in these definitions.
class clock* clock::_c;

clock::~clock()
{
}

void clock::register_clock(class clock* c)
{
    assert(!_c);
    _c = c;
}

class clock* clock::get()
{
    return _c;
}

// ---- clock_event (formerly clockevent.cc) --------------------------------

clock_event_driver* clock_event;

clock_event_driver::~clock_event_driver()
{
}

void clock_event_driver::set_callback(clock_event_callback* cb)
{
    _callback = cb;
}

clock_event_callback* clock_event_driver::callback() const
{
    return _callback;
}

clock_event_callback::~clock_event_callback()
{
}

// ---- pv_based_clock (formerly clock-common.cc) ---------------------------
//
// Included here rather than at the top of the file: clock-common.hh pulls in
// <osv/sched.hh> and thus <time.h>, whose C clock() function would otherwise
// shadow the clock class in the clock:: definitions above.
#include "clock-common.hh"

pv_based_clock::pv_based_clock()
    : _smp_init(false)
    , _boot_systemtime_init_counter(sched::cpus.size())
    , cpu_notifier([&] { setup_cpu(); })
{
}

void pv_based_clock::setup_cpu()
{
    init_on_cpu();

    // We need to do the following once, after all CPUs ran their
    // init_init_on_cpu(), so any CPU calling uptime() will see not only
    // _boot_systemtime set, but also a functional system_time(). Until
    // all CPUs are set up, all of the will see zero uptime().
    if (_boot_systemtime_init_counter.fetch_sub(1, std::memory_order_relaxed)
            == 1) {
        _boot_systemtime = system_time();
        _smp_init.store(true, std::memory_order_release);
    }
}

s64 pv_based_clock::time()
{
    auto r = wall_clock_boot();
    // FIXME: during early boot, while _smp_init is still false, we don't
    // add system_time() so we return the host's boot time instead of the
    // current time. When _smp_init becomes true, the clock jumps forward
    // to the correct current time.
    // This happens due to problems in init order dependencies (the clock
    // depends on the scheduler, for percpu initialization, and vice-versa,
    // for idle thread initialization).
    if (_smp_init.load(std::memory_order_acquire)) {
        r += system_time();
    }
    return r;
}

s64 pv_based_clock::uptime()
{
    if (_smp_init.load(std::memory_order_acquire)) {
        return system_time() - _boot_systemtime;
    } else {
        return 0;
    }
}

s64 pv_based_clock::boot_time()
{
    // The following is time()-uptime():
    auto r = wall_clock_boot();
    if (_smp_init.load(std::memory_order_acquire)) {
        r += _boot_systemtime;
    }
    return r;
}
