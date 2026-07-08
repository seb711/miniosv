#pragma once

// Disclaimer
// This header targets new amd, intel and arm chips and does not provide
// backward compatibility Tested chips are: For x86
//    - AMD Zen 4
//    - AMD Zen 5
//    - Intel Skylake
// For ARM
//    - Ampere-1a
//    - Neoverse V-1
//    - Neoverse V-2

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_TARGET_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_TARGET_ARM64
#else
#error Unsuported target architecture
#endif

namespace perf {

#ifdef ARCH_TARGET_X86_64

enum class CpuVendor { AMD, INTEL, UNKNOWN };

inline void cpuid(uint32_t leaf, uint32_t &eax, uint32_t &ebx, uint32_t &ecx,
                  uint32_t &edx) {
  asm volatile("cpuid"
               : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
               : "a"(leaf), "c"(0));
}

inline CpuVendor cpu_vendor() {
  uint32_t eax, ebx, ecx, edx;
  cpuid(0, eax, ebx, ecx, edx);
  char vendor[13];
  std::memcpy(vendor + 0, &ebx, sizeof(ebx));
  std::memcpy(vendor + 4, &edx, sizeof(edx));
  std::memcpy(vendor + 8, &ecx, sizeof(ecx));
  vendor[12] = '\0';
  if (std::memcmp(vendor, "GenuineIntel", 12) == 0)
    return CpuVendor::INTEL;
  if (std::memcmp(vendor, "AuthenticAMD", 12) == 0)
    return CpuVendor::AMD;
  return CpuVendor::UNKNOWN;
}

inline bool is_intel() { return cpu_vendor() == CpuVendor::INTEL; }
inline bool is_amd() { return cpu_vendor() == CpuVendor::AMD; }

inline uint32_t pmu_num_counters() {
  if (is_intel()) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x0A, eax, ebx, ecx, edx);
    return (eax >> 8) & 0xFF;
  }
  // TODO: This is currently a stub / best guess for amd processors
  return 6;
}

inline void msr_write(uint32_t msr, uint64_t value) {
  asm volatile("wrmsr"
               :
               : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

// Stub this until we need it (currently only required for ARM)
inline bool is_midr(uint32_t to_check) { return false; };

inline constexpr uint32_t intel_msr_perf_global_ctrl = 0x38Fu;
inline void enable_pmu() {
  if (pmu_num_counters() > 0)
    if (is_intel()) {
      uint32_t n = pmu_num_counters();
      uint64_t mask = (n >= 32) ? 0xFFFFFFFFull : ((1ull << n) - 1);
      msr_write(intel_msr_perf_global_ctrl, mask);
      return;
    }
  // TODO: This is currently a noop as the pmu is usually enabled by default
}

inline void msr_stop(int32_t counter, uint32_t evtSel) { msr_write(evtSel, 0); }

inline void msr_start_with_conf(uint32_t counter, uint32_t evtSel,
                                uint64_t value) {
  msr_write(evtSel, value);
}

inline void msr_write_counter(uint32_t msr, uint64_t value) {
  msr_write(msr, value);
}

inline uint64_t msr_read(uint32_t msr) {
  uint32_t low, high;
  asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

#elif defined ARCH_TARGET_ARM64

inline constexpr uint32_t midr_fixed_mask = 0xFF0F'FFF0u;

// To check how many hardware counters are available
inline uint32_t pmu_num_counters() {
  uint64_t pmcr;
  asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
  return (pmcr >> 11) & 0x1F;
}

// To check if we currently run on a specific CPU.
inline bool is_midr(uint32_t to_check) {
  uint64_t midr;
  __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
  return (static_cast<uint32_t>(midr) & midr_fixed_mask) == to_check;
}

// To check if the requested event is supported on this CPU
inline bool is_event_supported(uint64_t value) {
  uint64_t pmceid;
  uint64_t cmp;
  if (value < 0x20) {
    // The event is supported if pmceid0[value] = 1. (lower 32 bit of pmceid0)
    cmp = value;
    asm volatile("mrs %0, pmceid0_el0" : "=r"(pmceid));
  } else if (value < 0x40) {
    // The event is supported if pmceid1[value-0x20] = 1. (lower 32 bit of
    // pmceid1)
    cmp = value - 0x20;
    asm volatile("mrs %0, pmceid1_el0" : "=r"(pmceid));
  } else if (value < 0x4000) {
    // There is no pmceid3 in 64bit mode to validate these counters
    std::cout << "Warning: Requested event 0x" << std::hex << value
              << " could not be checked for compatibility" << std::endl;
    return true;
  } else if (value < 0x4020) {
    // The event is supported if pmceid0[value-0x4000] = 1. (upper 32 bit of
    // pmceid0)
    cmp = value - 0x4000;
    asm volatile("mrs %0, pmceid0_el0" : "=r"(pmceid));
  } else if (value < 0x4040) {
    // The event is supported if pmceid1[value-0x4020] = 1. (upper 32 bit of
    // pmceid1)
    cmp = value - 0x4020;
    asm volatile("mrs %0, pmceid1_el0" : "=r"(pmceid));
  } else {
    // There is no pmceid3 in 64bit mode to validate these counters
    std::cout << "Warning: Requested event 0x" << std::hex << value
              << " could not be checked for compatibility" << std::endl;
    return true;
  }

  if (((1ull << cmp) & pmceid) == 0) {
    std::cerr << "Requested event 0x" << std::hex << value
              << " is not supported." << std::endl;
    return false;
  }
  return true;
}

inline void enable_pmu() {
  uint64_t pmcr;

  // Clear all counters
  asm volatile("msr pmcntenclr_el0, %0\t\n"
               "isb" ::"r"((uint64_t)0xFFFFFFFF)
               : "memory");
  // 1. Read PMCR_EL0
  // - set bit[0]: E (Enable)
  // - set bit[1]: P (Reset event counters)
  // - set bit[2]: C (Reset cycle counter)
  // - clr bit[3]: D (Cycle counter divider)
  asm volatile("mrs %0, pmcr_el0\n\t"
               "orr %0, %0, #0b111\n\t"
               "and %0, %0, #~0b1000\n\t"
               "msr pmcr_el0, %0\n\t"
               "isb\n\t"
               : "=&r"(pmcr)
               :
               : "memory");
}

inline void msr_stop(int32_t counter, uint32_t evtSel) {
  // Disable counter
  asm volatile("msr pmcntenclr_el0, %0" : : "r"((uint64_t)evtSel));
  asm volatile("isb" ::: "memory");
}

inline void msr_write_counter(uint32_t counter, uint64_t value) {
  switch (counter) {
    // clang-format off
  case 0: asm volatile("msr pmevcntr0_el0, %0" : : "r"(value)); break;
  case 1: asm volatile("msr pmevcntr1_el0, %0" : : "r"(value)); break;
  case 2: asm volatile("msr pmevcntr2_el0, %0" : : "r"(value)); break;
  case 3: asm volatile("msr pmevcntr3_el0, %0" : : "r"(value)); break;
  case 4: asm volatile("msr pmevcntr4_el0, %0" : : "r"(value)); break;
  case 5: asm volatile("msr pmevcntr5_el0, %0" : : "r"(value)); break;
  case (1u << 31): asm volatile("msr pmccntr_el0, %0" : : "r"(value)); break;
    // clang-format on
  }
}

inline void msr_start_with_conf(uint32_t counter, uint32_t evtSel,
                                uint64_t value) {
  // Ensure the event is supported
  if (!is_event_supported(value))
    return;

  // Write event configuration into corresponding register
  switch (evtSel) {
    // clang-format off
  case 0: asm volatile("msr pmevtyper0_el0, %0" : : "r"(value)); break;
  case 1: asm volatile("msr pmevtyper1_el0, %0" : : "r"(value)); break;
  case 2: asm volatile("msr pmevtyper2_el0, %0" : : "r"(value)); break;
  case 3: asm volatile("msr pmevtyper3_el0, %0" : : "r"(value)); break;
  case 4: asm volatile("msr pmevtyper4_el0, %0" : : "r"(value)); break;
  case 5: asm volatile("msr pmevtyper5_el0, %0" : : "r"(value)); break;
    // clang-format on
  case (1u << 31):
    asm volatile("msr pmcntenset_el0, %0\n\t"
                 "isb" ::"r"((uint64_t)(1u << 31))
                 : "memory");
    return;
  }
  // Ensure pipeline is synchronized before we enable counter
  asm volatile("isb" ::: "memory");

  // enable counter at index counter
  asm volatile("msr pmcntenset_el0, %0" : : "r"(1ull << counter));

  // Ensure pipeline is synchronized before we start counting
  asm volatile("isb" ::: "memory");
}

inline uint64_t msr_read(uint32_t counter) {
  uint64_t value;
  switch (counter) {
    // clang-format off
  case 0: asm volatile("mrs %0, pmevcntr0_el0" : "=r"(value)); break;
  case 1: asm volatile("mrs %0, pmevcntr1_el0" : "=r"(value)); break;
  case 2: asm volatile("mrs %0, pmevcntr2_el0" : "=r"(value)); break;
  case 3: asm volatile("mrs %0, pmevcntr3_el0" : "=r"(value)); break;
  case 4: asm volatile("mrs %0, pmevcntr4_el0" : "=r"(value)); break;
  case 5: asm volatile("mrs %0, pmevcntr5_el0" : "=r"(value)); break;
  case (1u << 31): asm volatile("mrs %0, pmccntr_el0" : "=r"(value)); break;
    // clang-format on
  }
  return value;
}

#endif

// Implementer=0x41, Arch=0xF, Part=0xD46
inline constexpr uint32_t midr_neoverseV1 = 0x410F'D400u;

// ---------------------------------------
// Declaration of required data structures
// ---------------------------------------

// Some architecture differentiate between different classes of pmcs.
enum PMClass {
#ifdef ARCH_TARGET_X86_64
  // Specifies on-core AMD events
  CORE,
#elif defined ARCH_TARGET_ARM64
  // Specifies the designated cycles counter register
  CYCLES,
  // Specifies on-core ARM events
  CORE
#endif
};

// A Performance Measurement Counter (PMC) represents a physical counter and
// corresponding configuration registers.
struct PMC {
  // For the corresponding performance counter, this register specifies the
  // events counted, and controls other aspects of counter operation.
  uint32_t perfEvtSel;

  // Used to count specific processor events, or the duration of events,
  // as specified by the corresponding PerfEvtSel[n] register.
  uint32_t perfCtr;

  PMClass pmClass;

  // Whether or not this pmc is currently counting
  mutable std::atomic<bool> free{true};

  PMC(uint32_t perfEvtSel, uint32_t perfCtr, PMClass pmClass)
      : perfEvtSel(perfEvtSel), perfCtr(perfCtr), pmClass(pmClass), free(true) {
  }

  // atomic is not copyable, so define it manually
  PMC(PMC const &pmc)
      : perfEvtSel(pmc.perfEvtSel), perfCtr(pmc.perfCtr), pmClass(pmc.pmClass),
        free(true) {}

  PMC &operator=(PMC const &pmc) {
    perfEvtSel = pmc.perfEvtSel;
    perfCtr = pmc.perfCtr;
    pmClass = pmc.pmClass;
    free.store(true);
    return *this;
  }

  uint64_t probe() { return msr_read(perfCtr); }
  void start_with_conf(uint64_t value) {
    msr_write_counter(perfCtr, 0);
    msr_start_with_conf(perfCtr, perfEvtSel, value);
  }
  void stop() { msr_stop(perfCtr, perfEvtSel); }
};

struct PMCSelect {
  explicit PMCSelect(std::vector<PMC> pmcs) : pmcs(std::move(pmcs)) {}

  bool erase_counter(uint32_t perfEvtSel, uint32_t perfCtr, PMClass pmClass) {
    auto it = std::find_if(
        this->pmcs.begin(), this->pmcs.end(), [&](const auto &pmc) {
          return pmc.perfEvtSel == perfEvtSel && pmc.perfCtr == perfCtr &&
                 pmc.pmClass == pmClass;
        });
    if (it == this->pmcs.end())
      return false;
    this->pmcs.erase(it);
    return true;
  }

  bool erase_last_n_of_x(uint32_t n, PMClass x) {
    auto it = this->pmcs.end();
    while (n > 0 && it != this->pmcs.begin()) {
      --it;
      if (it->pmClass == x) {
        it = this->pmcs.erase(it);
        --n;
      }
    }
    return n == 0;
  }

  PMC *acquire(PMClass pmClass, uint8_t retries = 0) {
    for (auto &pmc : pmcs) {
      bool expected = true;
      if (pmc.pmClass == pmClass &&
          pmc.free.compare_exchange_strong(expected, false))
        return &pmc;
    }
    if (retries == 6)
      return nullptr;
    return acquire(pmClass, ++retries);
  }

  void release(PMC *pmc) { pmc->free.store(true); }

  uint32_t size() { return pmcs.size(); }

  uint32_t size_of_x(PMClass x) {
    uint32_t size{0};
    for (auto &pmc : pmcs) {
      if (pmc.pmClass == x)
        ++size;
    }
    return size;
  }

protected:
  std::vector<PMC> pmcs;
};

// Specification for core-local counters.
// This adjust the number of available counters at runtime,
// since AWS slices the number of counters per VM.
struct PMCSelectCore : PMCSelect {
  explicit PMCSelectCore(std::vector<PMC> pmcs) : PMCSelect(std::move(pmcs)) {
    uint32_t act_ctrs = pmu_num_counters();
    uint32_t exp_ctrs{0};
    for (auto &c : this->pmcs) {
      exp_ctrs += c.pmClass == CORE ? 1 : 0;
    }
    if (act_ctrs < exp_ctrs) {
      std::cout << "Expected " << exp_ctrs << " hardware counters, but only "
                << act_ctrs << " are available.\n Assuming the first "
                << act_ctrs << " counters to be valid." << std::endl;
      erase_last_n_of_x(exp_ctrs - act_ctrs, CORE);
    } else if (is_midr(midr_neoverseV1)) {
      // TODO: There is a nicer way to check this. We only want to disable on
      // neoverse v1 + kvm
      std::cout << "Detected ARM Neoverse V1: Disabling counter 0 since it "
                   "doesn't work reliably on this CPU. You have "
                << act_ctrs - 1 << " counters available" << std::endl;
      erase_counter(0, 0, CORE);
    }
  }

  PMCSelectCore(std::initializer_list<PMC> pmcs)
      : PMCSelectCore(std::vector<PMC>(pmcs)) {}
};

// Builds the default core-local counter list for this platform: the MSR
// addresses and PMC count are AMD- or Intel-specific on x86 (resolved at
// runtime via CPUID), and fixed for ARM (resolved from pmcr_el0 by
// PMCSelectCore itself).
inline std::vector<PMC> make_default_core_pmcs() {
  std::vector<PMC> pmcs;
#ifdef ARCH_TARGET_X86_64
  if (is_intel()) {
    uint32_t n = pmu_num_counters();
    for (uint32_t i = 0; i < n; ++i) {
      pmcs.emplace_back(0x186u + i, 0xC1u + i, CORE);
    }
  } else {
    // AMD "extended" core PMC range (Zen and later): 6 general-purpose
    // counters at MSRC001_0200h + 2n / MSRC001_0201h + 2n.
    for (uint32_t i = 0; i < 6; ++i) {
      pmcs.emplace_back(0xC0010200u + 2 * i, 0xC0010201u + 2 * i, CORE);
    }
  }
#elif defined ARCH_TARGET_ARM64
  // Armv8_pmu3 usually has 6 counters (ids 0-5)
  pmcs.emplace_back(0, 0, CORE);
  pmcs.emplace_back(1, 1, CORE);
  pmcs.emplace_back(2, 2, CORE);
  pmcs.emplace_back(3, 3, CORE);
  pmcs.emplace_back(4, 4, CORE);
  pmcs.emplace_back(5, 5, CORE);
  pmcs.emplace_back(1u << 31, 1u << 31, CYCLES);
#endif
  return pmcs;
}

struct PMCEvent {
  uint64_t bitmap;
  PMClass pmClass;
  const char *name;
};

namespace PERF_COUNT_HW {
#ifdef ARCH_TARGET_X86_64

namespace detail {
// PerfEvtSel-style encoding shared by both vendors:
// bit22=EN, bit17=OS, bit16=USR, bits8-15=UMask, bits0-7=Event Select.
constexpr uint64_t encode(uint8_t event, uint8_t umask) {
  return 0x430000ull | (static_cast<uint64_t>(umask) << 8) | event;
}

// clang-format off
constexpr PMCEvent AMD_CPU_CYCLES = {encode(0x76, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent AMD_STALL_FRONTEND = {encode(0xA9, 0x00), CORE, "stall-frontend"};
constexpr PMCEvent AMD_INSTRUCTIONS = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent AMD_BRANCH_PREDICTION = {encode(0xC2, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent AMD_BRANCH_MISS = {encode(0xC3, 0x00), CORE, "branch-misses"};
constexpr PMCEvent AMD_LLC_CACHE_MISS = {encode(0x64, 0x09), CORE, "llc-cache-misses"};
constexpr PMCEvent AMD_LLC_CACHE = {encode(0x29, 0x07), CORE, "llc-cache-accesses"};
constexpr PMCEvent AMD_TLB_FLUSHES = {encode(0x78, 0xFF), CORE, "tlb-flushes"};
constexpr PMCEvent INTEL_CPU_CYCLES = {encode(0x3C, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent INTEL_INSTRUCTIONS = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent INTEL_BRANCH_PREDICTION = {encode(0xC4, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent INTEL_BRANCH_MISS = {encode(0xC5, 0x00), CORE, "branch-misses"};
constexpr PMCEvent INTEL_LLC_CACHE = {encode(0x2E, 0x4F), CORE, "llc-cache-accesses"};
constexpr PMCEvent INTEL_LLC_CACHE_MISS = {encode(0x2E, 0x41), CORE, "llc-cache-misses"};
constexpr PMCEvent INTEL_STALL_FRONTEND = {encode(0x9C, 0x01), CORE, "stall-frontend"};
} // namespace detail
inline const PMCEvent CPU_CYCLES = is_intel() ? detail::INTEL_CPU_CYCLES : detail::AMD_CPU_CYCLES;
inline const PMCEvent INSTRUCTIONS = is_intel() ? detail::INTEL_INSTRUCTIONS : detail::AMD_INSTRUCTIONS;
inline const PMCEvent BRANCH_PREDICTION = is_intel() ? detail::INTEL_BRANCH_PREDICTION : detail::AMD_BRANCH_PREDICTION;
inline const PMCEvent BRANCH_MISS = is_intel() ? detail::INTEL_BRANCH_MISS : detail::AMD_BRANCH_MISS;
inline const PMCEvent L2_CACHE_MISS = is_intel() ? detail::INTEL_LLC_CACHE_MISS : detail::AMD_LLC_CACHE_MISS;
inline const PMCEvent L2_CACHE = is_intel() ? detail::INTEL_LLC_CACHE : detail::AMD_LLC_CACHE;
inline const PMCEvent STALL_FRONTEND = is_intel() ? detail::INTEL_STALL_FRONTEND : detail::AMD_STALL_FRONTEND;
// clang-format on

#elif defined ARCH_TARGET_ARM64
// Instruction architecturally executed, condition code check pass, software
// increment
constexpr PMCEvent SW_INCR = {0x0, CORE, "software-increase"};
// Attributable Level 1 instruction cache refill
constexpr PMCEvent L1I_CACHE_REFILL = {0x1, CORE, "l1i-cache-refill"};
// Attributable Level 1 instruction TLB refills
constexpr PMCEvent L1I_TLB_REFILL = {0x2, CORE, "l1i-tlb-refill"};
// Attributable Level 1 data cache refill
constexpr PMCEvent L1D_CACHE_REFILL = {0x3, CORE, "l1d-cache-refill"};
// Attributable Level 1 data cache access
constexpr PMCEvent L1D_CACHE = {0x4, CORE, "l1d-cache-access"};
// Attributable Level 1 data TLB refills
constexpr PMCEvent L1D_TLB_REFILL = {0x5, CORE, "l1d-tlb-refill"};
// Instruction architecturally executed, condition code check pass, load
constexpr PMCEvent LD_RETIRED = {0x6, CORE, "load-instructions"};
// Instruction architecturally executed, condition code check pass, store
constexpr PMCEvent ST_RETIRED = {0x7, CORE, "store-instructions"};
// Instruction architecturally executed
constexpr PMCEvent INSTRUCTIONS = {0x8, CORE, "instructions"};
// Exception Taken
constexpr PMCEvent EXC_TAKEN = {0x9, CORE, "exceptions-taken"};
// Instruction architecturally executed, condition code check pass,
// exception return
constexpr PMCEvent EXC_RETURN = {0xA, CORE, "exceptions-return"};
// Instruction architecturally executed, condition code check pass, software
// change of the PC
constexpr PMCEvent PC_WRITE_RETIRED = {0xC, CORE, "software-pc-writes"};
// Instruction architecturally executed, immediate branch
constexpr PMCEvent BR_IMMED_RETIRED = {0xD, CORE,
                                       "immediate-branch-instructions"};
// Instruction architecturally executed, condition code check pass,
// procedure return
constexpr PMCEvent BR_RETURN_RETIRED = {0xE, CORE,
                                        "procedure-return-instructions"};
// Instruction architecturally executed, condition code check pass,
// unaligned load or store
constexpr PMCEvent UNALIGNED_LDST_RETIRED = {0xF, CORE,
                                             "unaligned-loadstore-instruction"};
// Mispredicted or not predicted branch speculatively executed
constexpr PMCEvent BR_MIS_PRED = {0x10, CORE, "branch-misses-issued"};
// Cycle
constexpr PMCEvent CPU_CYCLES = {0x11, CYCLES, "cpu-cycles"};
// Predictable branch speculatively executed
constexpr PMCEvent BR_PRED = {0x12, CORE, "branch-predictions-issued"};
// Data memory access
constexpr PMCEvent MEM_ACCESS = {0x13, CORE, "memory-accesses"};
// Attributable Level 1 instruction cache access
constexpr PMCEvent L1I_CACHE = {0x14, CORE, "l1i-cache-accesses"};
// Attributable Level 1 data cache write-back
constexpr PMCEvent L1D_CACHE_WB = {0x15, CORE, "l1d-cache-writebacks"};
// Attributable Level 2 data cache access
constexpr PMCEvent L2D_CACHE = {0x16, CORE, "l2d-cache-accesses"};
// Attributable Level 2 data cache refill
constexpr PMCEvent L2D_CACHE_REFILL = {0x17, CORE, "l2d-cache-refills"};
// Attributable Level 2 data cache write-back
constexpr PMCEvent L2D_CACHE_WB = {0x18, CORE, "l2d-cache-writebacks"};
// Attributable Bus access
constexpr PMCEvent BUS_ACCESS = {0x19, CORE, "bus-accesses"};
// Local memory error
constexpr PMCEvent MEMORY_ERROR = {0x1A, CORE, "memory-errors"};
// Operation speculatively executed
constexpr PMCEvent INST_SPEC = {0x1B, CORE, "speculative-instructions"};
// Bus cycle
constexpr PMCEvent BUS_CYCLES = {0x1D, CORE, "bus-cycles"};
// For an odd numbered counter, increment when an overflow occurs on the
// preceding even-numbered counter on the same PE
constexpr PMCEvent CHAIN = {0x1E, CORE, "chain"};
// Attributable Level 1 data cache allocation without refill
constexpr PMCEvent L1D_CACHE_ALLOCATE = {0x1F, CORE, "l1d-cache-allocations"};
// Attributable Level 2 data cache allocation without refill
constexpr PMCEvent L2D_CACHE_ALLOCATE = {0x20, CORE, "l2d-cache-allocations"};
// Instruction architecturally executed, branch
constexpr PMCEvent BRANCH_PREDICTION = {0x21, CORE, "branch-predictions"};
// Instruction architecturally executed, mispredicted branch
constexpr PMCEvent BRANCH_MISS = {0x22, CORE, "branch-misses"};
// No operation issued because of the frontend
constexpr PMCEvent STALL_FRONTEND = {0x23, CORE, "frontend-stalls"};
// No operation issued because of the backend
constexpr PMCEvent STALL_BACKEND = {0x24, CORE, "backend-stalls"};
// Attributable Level 2 instruction cache access
constexpr PMCEvent L2I_CACHE = {0x27, CORE, "l2i-cache-accesses"};
// Attributable Level 2 instruction cache refill
constexpr PMCEvent L2I_CACHE_REFILL = {0x28, CORE, "l2i-cache-refills"};
// Attributable Level 3 data cache allocation without refill
constexpr PMCEvent L3D_CACHE_ALLOCATE = {0x29, CORE, "l3d-cache-allocations"};
// Attributable Level 3 data cache refill
constexpr PMCEvent L3D_CACHE_REFILL = {0x2A, CORE, "l3d-cache-refills"};
// Attributable Level 3 data cache access
constexpr PMCEvent L3D_CACHE = {0x2B, CORE, "l3d-cache-accesses"};
// Attributable Level 3 data cache access write-back
constexpr PMCEvent L3D_CACHE_WB = {0x2C, CORE, "l3d-cache-writebacks"};
// Last level data cache read
constexpr PMCEvent LL_CACHE = {0x36, CORE, "llc-cache-accesses"};
// Last level data cache read miss
constexpr PMCEvent LL_CACHE_MISS = {0x37, CORE, "llc-cache-misses"};
// Level 1 data cache read miss
constexpr PMCEvent L1D_CACHE_MISS = {0x39, CORE, "l1d-cache-misses"};
// Operation retired
constexpr PMCEvent OP_COMPLETE = {0x3A, CORE, "micro-operations-retired"};
// Operation speculated
constexpr PMCEvent OP_SPEC = {0x3B, CORE, "micro-operations-speculated"};
// No operation sent for execution
constexpr PMCEvent STALL = {0x3C, CORE, ""};
// No operation sent for execution on a slot because of the backend
constexpr PMCEvent STALL_OP_BACKEND = {0x3D, CORE, ""};
// No operation sent for execution on a slot because of the frontend
constexpr PMCEvent STALL_OP_FRONTEND = {0x3E, CORE, ""};
// No operation sent for execution on a slot
constexpr PMCEvent STALL_OP = {0x3F, CORE, ""};
// Level 2 data cache read miss
constexpr PMCEvent L2_CACHE_MISS = {0x4009, CORE, "l2-cache-misses"};

#endif
} // namespace PERF_COUNT_HW

// ---------------------------------------
// High level measurement logic
// ---------------------------------------

struct Event {
  PMCEvent pmce;

  uint64_t before;
  uint64_t after;

  bool valid = true;

  Event(PMCEvent pmce, PMCSelect &pmcs) : pmce(pmce), pmcs(pmcs) {}

  void start() {
    pmc = pmcs.acquire(pmce.pmClass);
    if (!pmc) {
      std::cerr << "[ERROR] All hardware counters are occupied ("
                << pmcs.size_of_x(pmce.pmClass) << "/"
                << pmcs.size_of_x(pmce.pmClass) << "). Event " << pmce.name
                << " will not be measured." << std::endl;
      valid = false;
      return;
    }
    pmc->start_with_conf(pmce.bitmap);
    before = pmc->probe();
  }

  void stop() {
    if (!pmc)
      return;
    after = pmc->probe();
    pmc->stop();
    pmcs.release(pmc);
  }

  uint64_t report() { return valid ? after - before : 0; }

private:
  PMCSelect &pmcs;
  PMC *pmc;
};

// Shim layer to match https://github.com/viktorleis/perfevent
struct PerfEvent {
private:
  // By default, each collection operates on known core-local counters,
  // but selections can also be shared between cores to measure uncore counters
  // (e.g. L3 cache counters)
  PMCSelectCore default_pmcs{make_default_core_pmcs()};

public:
  // The selection of hardware counters available to this collection.
  PMCSelect &pmcs = default_pmcs;
  // A vector of registered events. Must not be larger than the number of
  // available hardware counters.
  std::vector<Event> events;

  std::chrono::time_point<std::chrono::steady_clock> startTime;
  std::chrono::time_point<std::chrono::steady_clock> stopTime;

  // Constructor to use with default set of counters.
  // Common use case: On-core measurements, single Collection per core.
  PerfEvent(bool set_default_counters = true) {
    enable_pmu();

    if (set_default_counters) {
      registerCounter(PERF_COUNT_HW::CPU_CYCLES);
      registerCounter(PERF_COUNT_HW::INSTRUCTIONS);
      registerCounter(PERF_COUNT_HW::L2_CACHE_MISS);
      registerCounter(PERF_COUNT_HW::BRANCH_MISS);
    }
  }

  // Constructor to use with custom set of counters.
  // Common use case: Share counters between multiple collections
  PerfEvent(PMCSelect &pmcSelect) : pmcs(pmcSelect) { enable_pmu(); }

  void registerCounter(PMCEvent pmce) { events.push_back(Event(pmce, pmcs)); }

  void registerCounter(PMCEvent pmce, const char *name) {
    pmce.name = name;
    registerCounter(pmce);
  }

  void registerCounter(uint64_t bitmap, PMClass pmClass, const char *name) {
    registerCounter({bitmap, pmClass, name});
  }

  void startCounters() {
    for (auto &event : events) {
      event.start();
    }
    startTime = std::chrono::steady_clock::now();
  }

  void stopCounters() {
    stopTime = std::chrono::steady_clock::now();
    for (auto &event : events) {
      event.stop();
    }
  }

  double getDuration() {
    return std::chrono::duration<double>(stopTime - startTime).count();
  }

  size_t getDurationUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(stopTime -
                                                                 startTime)
        .count();
  }
  //
  // IPC is calculated from the instructions and cycle counter.
  // If one of them is not counted, this function returns NaN.
  double getIPC() {
    double res = getCounter(PERF_COUNT_HW::INSTRUCTIONS.name) /
                 getCounter(PERF_COUNT_HW::CPU_CYCLES.name);
    return res > 0 ? res : NAN;
  }

  double getCounter(const char *name) {
    for (auto &event : events) {
      if (event.pmce.name == name)
        return event.report();
    }
    return -1;
  }

  static void printCounter(std::ostream &headerOut, std::ostream &dataOut,
                           std::string name, std::string counterValue,
                           bool addComma = true) {
    auto width = std::max(name.length(), counterValue.length());
    headerOut << std::setw(static_cast<int>(width)) << name
              << (addComma ? "," : "") << " ";
    dataOut << std::setw(static_cast<int>(width)) << counterValue
            << (addComma ? "," : "") << " ";
  }

  template <typename T>
  static void printCounter(std::ostream &headerOut, std::ostream &dataOut,
                           std::string name, T counterValue,
                           bool addComma = true) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << counterValue;
    PerfEvent::printCounter(headerOut, dataOut, name, stream.str(), addComma);
  }

  void printReport(std::ostream &out, uint64_t normalizationConstant) {
    std::stringstream header;
    std::stringstream data;
    printReport(header, data, normalizationConstant);
    out << header.str() << std::endl;
    out << data.str() << std::endl;
  }

  void printReport(std::ostream &headerOut, std::ostream &dataOut,
                   uint64_t normalizationConstant) {
    if (!events.size())
      return;

    printCounter(headerOut, dataOut, "duration", getDuration());
    for (unsigned i = 0; i < events.size(); i++) {
      printCounter(headerOut, dataOut, events[i].pmce.name,
                   events[i].report() /
                       static_cast<double>(normalizationConstant));
    }

    printCounter(headerOut, dataOut, "scale", normalizationConstant);

    // derived metrics
    printCounter(headerOut, dataOut, "IPC", getIPC());
  }

  template <typename T>
  static void printCounterVertical(std::ostream &infoOut, std::string name,
                                   T counterValue, int eNameWidth) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << counterValue;
    infoOut << std::setw(eNameWidth) << std::left << name << " : "
            << stream.str() << std::endl;
  }

  void printReportVertical(std::ostream &out, uint64_t normalizationConstant) {
    std::stringstream info;
    printReportVerticalUtil(info, normalizationConstant);
    out << info.str() << std::endl;
  }

  void printReportVerticalUtil(std::ostream &infoOut,
                               uint64_t normalizationConstant) {
    if (!events.size())
      return;

    // get width of the widest event name. Minimum width is the one of 'scale'
    int eNameWidth = 5;
    for (unsigned i = 0; i < events.size(); i++) {
      eNameWidth = std::max(
          static_cast<int>(std::char_traits<char>::length(events[i].pmce.name)),
          eNameWidth);
    }

    printCounterVertical(infoOut, "duration", getDuration(), eNameWidth);
    // print all metrics
    for (unsigned i = 0; i < events.size(); i++) {
      printCounterVertical(infoOut, events[i].pmce.name,
                           events[i].report() /
                               static_cast<double>(normalizationConstant),
                           eNameWidth);
    }

    printCounterVertical(infoOut, "scale", normalizationConstant, eNameWidth);

    // derived metrics
    printCounterVertical(infoOut, "IPC", getIPC(), eNameWidth);
  }
};

struct BenchmarkParameters {

  void setParam(const std::string &name, const std::string &value) {
    params[name] = value;
  }

  void setParam(const std::string &name, const char *value) {
    params[name] = value;
  }

  template <typename T> void setParam(const std::string &name, T value) {
    setParam(name, std::to_string(value));
  }

  void printParams(std::ostream &header, std::ostream &data) {
    for (auto &p : params) {
      PerfEvent::printCounter(header, data, p.first, p.second);
    }
  }

  BenchmarkParameters(std::string name = "") {
    if (name.length())
      setParam("name", name);
  }

private:
  std::map<std::string, std::string> params;
};

struct PerfRef {
  union {
    PerfEvent instance;
    PerfEvent *pointer;
  };
  bool has_instance;

  PerfRef() : instance(), has_instance(true) {}
  PerfRef(PerfEvent *ptr) : pointer(ptr), has_instance(false) {}
  PerfRef(const PerfRef &) = delete;

  ~PerfRef() {
    if (has_instance)
      instance.~PerfEvent();
  }

  PerfEvent *operator->() { return has_instance ? &instance : pointer; }
};

struct PerfEventBlock {
  PerfRef e;
  uint64_t scale;
  BenchmarkParameters parameters;
  bool printHeader;

  PerfEventBlock(uint64_t scale = 1, BenchmarkParameters params = {},
                 bool printHeader = true)
      : scale(scale), parameters(params), printHeader(printHeader) {
    e->startCounters();
  }

  PerfEventBlock(PerfEvent &perf, uint64_t scale = 1,
                 BenchmarkParameters params = {}, bool printHeader = true)
      : e(&perf), scale(scale), parameters(params), printHeader(printHeader) {
    e->startCounters();
  }

  ~PerfEventBlock() {
    e->stopCounters();
    std::stringstream header;
    std::stringstream data;
    parameters.printParams(header, data);
    PerfEvent::printCounter(header, data, "time sec", e->getDuration());
    PerfEvent::printCounter(header, data, "micros", e->getDurationUs());
    PerfEvent::printCounter(header, data, "millis",
                            static_cast<double>(e->getDurationUs()) / 1000);
    e->printReport(header, data, scale);
    if (printHeader)
      std::cout << header.str() << std::endl;
    std::cout << data.str() << std::endl;
  }
};

}; // namespace perf
