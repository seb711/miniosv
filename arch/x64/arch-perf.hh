#pragma once

// x86 PMU back-end for osv/perf.hh.
// Targets AMD Zen 4/5 and Intel Skylake and newer.

#include <cstdint>
#include <cstring>

#include "osv/perf.hh"
#include "processor.hh"

namespace perf {

enum class CpuVendor { AMD, INTEL, UNKNOWN };

inline CpuVendor cpu_vendor() {
  processor::cpuid_result r = processor::cpuid(0);
  char v[13];
  std::memcpy(v + 0, &r.b, sizeof(r.b));
  std::memcpy(v + 4, &r.d, sizeof(r.d));
  std::memcpy(v + 8, &r.c, sizeof(r.c));
  v[12] = '\0';
  if (std::memcmp(v, "GenuineIntel", 12) == 0)
    return CpuVendor::INTEL;
  if (std::memcmp(v, "AuthenticAMD", 12) == 0)
    return CpuVendor::AMD;
  return CpuVendor::UNKNOWN;
}

inline bool is_intel() { return cpu_vendor() == CpuVendor::INTEL; }
inline bool is_amd() { return cpu_vendor() == CpuVendor::AMD; }

inline uint32_t pmu_num_counters() {
  if (is_intel()) {
    // Intel: CPUID leaf 0x0A EAX[15:8] = number of general-purpose PMCs.
    processor::cpuid_result r = processor::cpuid(0x0A);
    return (r.a >> 8) & 0xFF;
  }
  // AMD: prefer PerfMonV2 (CPUID Fn8000_0022h) which reports the counter
  // count directly. This is architectural from Zen 4 onwards.
  processor::cpuid_result ext_max = processor::cpuid(0x80000000);
  if (ext_max.a >= 0x80000022u) {
    processor::cpuid_result r = processor::cpuid(0x80000022);
    if (r.a & 0x1u)
      return r.b & 0xFu;
  }
  // Fall back: PerfCtrExtCore (CPUID Fn8000_0001h_ECX[23]) gates the extended
  // 6-counter MSR range; legacy AMDs expose 4.
  processor::cpuid_result r = processor::cpuid(0x80000001);
  return (r.c & (1u << 23)) ? 6 : 4;
}

// x86 has no MIDR register; the shim keeps architecture-neutral callers free
// of ifdefs.
inline bool is_midr(uint32_t) { return false; }

// KVM-guest probing is ARM-only in the front-end (used to work around a
// Neoverse V1 + KVM PMU quirk). Returning false on x86 keeps the callsite
// arch-neutral.
inline bool is_kvm_guest() { return false; }

inline constexpr uint32_t intel_msr_perf_global_ctrl = 0x38Fu;

inline void enable_pmu() {
  if (!is_intel()) {
    // AMD: PMU is enabled by default.
    return;
  }
  uint32_t n = pmu_num_counters();
  if (n == 0)
    return;
  uint64_t mask = (n >= 32) ? 0xFFFF'FFFFull : ((1ull << n) - 1);
  processor::wrmsr(intel_msr_perf_global_ctrl, mask);
}

inline void pmc_stop(uint32_t evt_sel) { processor::wrmsr(evt_sel, 0); }

inline void pmc_write_counter(uint32_t ctr, uint64_t value) {
  processor::wrmsr(ctr, value);
}

inline void pmc_start_with_conf(uint32_t /*ctr*/, uint32_t evt_sel,
                                uint64_t value) {
  processor::wrmsr(evt_sel, value);
}

inline uint64_t pmc_read(uint32_t ctr) { return processor::rdmsr(ctr); }

namespace PERF_COUNT_HW {
namespace detail {
// PerfEvtSel-style encoding shared by both vendors:
// bit22=EN, bit17=OS, bit16=USR, bits8-15=UMask, bits0-7=Event Select.
constexpr uint64_t encode(uint8_t event, uint8_t umask) {
  return 0x430000ull | (static_cast<uint64_t>(umask) << 8) | event;
}

using enum PMClass;

// clang-format off
constexpr PMCEvent AMD_CPU_CYCLES         = {encode(0x76, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent AMD_STALL_FRONTEND     = {encode(0xA9, 0x00), CORE, "stall-frontend"};
constexpr PMCEvent AMD_INSTRUCTIONS       = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent AMD_BRANCH_PREDICTION  = {encode(0xC2, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent AMD_BRANCH_MISS        = {encode(0xC3, 0x00), CORE, "branch-misses"};
constexpr PMCEvent AMD_LLC_CACHE_MISS     = {encode(0x64, 0x09), CORE, "llc-cache-misses"};
constexpr PMCEvent AMD_LLC_CACHE          = {encode(0x29, 0x07), CORE, "llc-cache-accesses"};
constexpr PMCEvent AMD_TLB_FLUSHES        = {encode(0x78, 0xFF), CORE, "tlb-flushes"};
constexpr PMCEvent INTEL_CPU_CYCLES        = {encode(0x3C, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent INTEL_INSTRUCTIONS      = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent INTEL_BRANCH_PREDICTION = {encode(0xC4, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent INTEL_BRANCH_MISS       = {encode(0xC5, 0x00), CORE, "branch-misses"};
constexpr PMCEvent INTEL_LLC_CACHE         = {encode(0x2E, 0x4F), CORE, "llc-cache-accesses"};
constexpr PMCEvent INTEL_LLC_CACHE_MISS    = {encode(0x2E, 0x41), CORE, "llc-cache-misses"};
constexpr PMCEvent INTEL_STALL_FRONTEND    = {encode(0x9C, 0x01), CORE, "stall-frontend"};
// clang-format on
} // namespace detail

inline const PMCEvent CPU_CYCLES =
    is_intel() ? detail::INTEL_CPU_CYCLES : detail::AMD_CPU_CYCLES;
inline const PMCEvent INSTRUCTIONS =
    is_intel() ? detail::INTEL_INSTRUCTIONS : detail::AMD_INSTRUCTIONS;
inline const PMCEvent BRANCH_PREDICTION = is_intel()
                                              ? detail::INTEL_BRANCH_PREDICTION
                                              : detail::AMD_BRANCH_PREDICTION;
inline const PMCEvent BRANCH_MISS =
    is_intel() ? detail::INTEL_BRANCH_MISS : detail::AMD_BRANCH_MISS;
inline const PMCEvent L2_CACHE_MISS =
    is_intel() ? detail::INTEL_LLC_CACHE_MISS : detail::AMD_LLC_CACHE_MISS;
inline const PMCEvent L2_CACHE =
    is_intel() ? detail::INTEL_LLC_CACHE : detail::AMD_LLC_CACHE;
inline const PMCEvent STALL_FRONTEND =
    is_intel() ? detail::INTEL_STALL_FRONTEND : detail::AMD_STALL_FRONTEND;
} // namespace PERF_COUNT_HW

} // namespace perf
