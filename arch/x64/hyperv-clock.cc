/*
 * Copyright (C) 2026 miniosv contributors
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

//
// Hyper-V (Microsoft hypervisor) time source.
//
// x86 OSv normally uses kvmclock, which only exists under KVM. On Hyper-V
// hosts - notably Azure Gen2 VMs - kvmclock::probe() fails, so no clock would
// register and the first scheduler tick dereferences a null clock::get(). This
// driver fills that gap, the same way the aarch64 arm_clock does: it reads a
// monotonic hardware counter for uptime() and advances the UEFI GetTime()
// wall-clock base captured by the boot stub for time().
//
// The counter is the partition reference time, read from the synthetic MSR
// HV_X64_MSR_TIME_REF_COUNT in fixed units of 100 ns. It is partition-wide and
// monotonic, so uptime() reads identically on every CPU - no per-CPU setup is
// needed (unlike kvmclock). This is a plain rdmsr each read rather than the
// reference-TSC page; it is simpler and always available, at the cost of an
// MSR read per query, which is fine for OSv's timekeeping.
//

#include "drivers/clock.hh"
#include "processor.hh"
#include <osv/types.h>
#include <osv/prio.hh>

// Hyper-V CPUID leaves and the synthetic timer-count MSR (per the Microsoft
// Hypervisor Top-Level Functional Specification).
#define HV_CPUID_BASE           0x40000000  // EAX = highest Hyper-V leaf
#define HV_CPUID_INTERFACE      0x40000001  // EAX = "Hv#1" if Microsoft interface
#define HV_CPUID_FEATURES       0x40000003  // EAX bit 1 = ref-counter MSR
#define HV_INTERFACE_SIGNATURE  0x31237648  // "Hv#1"
#define HV_FEATURE_REF_COUNTER  (1u << 1)
#define HV_X64_MSR_TIME_REF_COUNT 0x40000020

static constexpr u64 NANO_PER_100NS = 100;

class hypervclock : public clock {
public:
    hypervclock();
    static bool probe();
    virtual s64 uptime() override __attribute__((no_instrument_function));
    virtual s64 time() override __attribute__((no_instrument_function));
    virtual s64 boot_time() override __attribute__((no_instrument_function));
private:
    s64 _boot_time_in_ns;
};

hypervclock::hypervclock()
{
    // Wall-clock base from the UEFI firmware (GetTime), captured by the UEFI
    // stub - Hyper-V exposes a wall clock too, but the firmware base plus the
    // monotonic reference counter is sufficient and matches the aarch64 path.
    extern u64 osv_boot_unixtime_ns;
    _boot_time_in_ns = osv_boot_unixtime_ns;
}

bool hypervclock::probe()
{
    // Prefer kvmclock when the KVM paravirtual clock is present: a host can
    // expose both KVM and Hyper-V enlightenments (e.g. QEMU's hv-* options on
    // KVM), and only one clock may register. Real Hyper-V hosts (Azure) have no
    // KVM clocksource, so this defers to kvmclock only when it would actually
    // work.
    if (processor::features().kvm_clocksource2 ||
        processor::features().kvm_clocksource) {
        return false;
    }
    // Identify the Microsoft hypervisor by its interface signature "Hv#1" in
    // leaf 0x40000001 EAX. This is the defined interface identifier and, unlike
    // the vendor string in leaf 0x40000000, is not affected by a spoofed
    // hv-vendor-id. Guard the feature-leaf read with the advertised max leaf.
    if (processor::cpuid(HV_CPUID_BASE).a < HV_CPUID_FEATURES) {
        return false;
    }
    if (processor::cpuid(HV_CPUID_INTERFACE).a != HV_INTERFACE_SIGNATURE) {
        return false;
    }
    // Require the partition reference counter (the TIME_REF_COUNT MSR).
    return processor::cpuid(HV_CPUID_FEATURES).a & HV_FEATURE_REF_COUNTER;
}

s64 hypervclock::uptime()
{
    // The reference counter starts at partition creation and counts in 100 ns
    // units; it is monotonic and consistent across all CPUs.
    return processor::rdmsr(HV_X64_MSR_TIME_REF_COUNT) * NANO_PER_100NS;
}

s64 hypervclock::time()
{
    return _boot_time_in_ns + uptime();
}

s64 hypervclock::boot_time()
{
    return _boot_time_in_ns;
}

static __attribute__((constructor(init_prio::clock))) void setup_hypervclock()
{
    if (hypervclock::probe()) {
        clock::register_clock(new hypervclock);
    }
}
