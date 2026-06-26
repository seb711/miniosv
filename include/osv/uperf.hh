#pragma once

// Disclaimer
// This header targets new amd and arm chips and does not provide backward
// compatibility Tested chips are: For x86
//    - Zen 4
//    - Zen 5
// For ARM
//    - Ampere-1a
//    - Neoverse V-1
//    - Neoverse V-2

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_TARGET_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_TARGET_ARM64
#else
#error Unsuported target architecture
#endif

#ifdef ARCH_TARGET_X86_64

inline uint32_t pmu_num_counters() {
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

inline void enable_pmu() {
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

namespace uperf {

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
  PMCSelect(std::initializer_list<PMC> pmcs) : pmcs(pmcs) {}

  PMC *acquire(PMClass pmClass, uint8_t retries = 0) {
    for (auto &pmc : pmcs) {
      bool expected = true;
      if (pmc.pmClass == pmClass &&
          pmc.free.compare_exchange_strong(expected, false))
        return &pmc;
    }
    if (retries == 6)
      return nullptr;
    ++retries;
    std::cout << "Couldn't acquire PMC of the requested class. Retrying ("
              << retries << "/6)" << std::endl;
    return acquire(pmClass, retries);
  }

  void release(PMC *pmc) { pmc->free.store(true); }

protected:
  std::vector<PMC> pmcs;
};

// Specification for core-local counters.
// This adjust the number of available counters at runtime,
// since AWS slices the number of counters per VM.
struct PMCSelectCore : PMCSelect {
  PMCSelectCore(std::initializer_list<PMC> pmcs) : PMCSelect(pmcs) {
    uint32_t act_ctrs = pmu_num_counters();
    uint32_t exp_ctrs{0};
    for (auto &c : this->pmcs) {
      exp_ctrs += c.pmClass == CORE ? 1 : 0;
    }
    if (act_ctrs < exp_ctrs) {
      std::cout << "Expected " << exp_ctrs << " hardware counters, but only "
                << act_ctrs << " are available." << std::endl
                << " Removing the last " << (exp_ctrs - act_ctrs)
                << " entries..." << std::endl;
      this->pmcs.erase(this->pmcs.begin() + act_ctrs, this->pmcs.end());
    }

    if (is_midr(midr_neoverseV1)) {
      std::cout << "Detected ARM Neoverse V1: Disabling counter 0 since it "
                   "doesn't work reliably on this CPU. You have "
                << act_ctrs - 1 << " counters available" << std::endl;
      this->pmcs.erase(this->pmcs.begin());
    }
  }
};

struct PMCEvent {
  uint64_t bitmap;
  PMClass pmClass;
  const char *name;
};

namespace Events {
#ifdef ARCH_TARGET_X86_64
// Cycle
constexpr PMCEvent CPU_CYCLES = {0x430076, CORE, "cpu-cycles"};
// Cycle where no operation is issued because of the frontend
constexpr PMCEvent STALL_FRONTEND = {0x4300A9, CORE, "stall-frontend"};
// Instruction architecturally executed
constexpr PMCEvent INSTRUCTIONS = {0x4300C0, CORE, "instructions"};
// Predictable branch speculatively executed
constexpr PMCEvent BRANCH_PREDICTION = {0x4300C2, CORE, "branch-predictions"};
// Mispredicted or not predicted branch speculatively executed
constexpr PMCEvent BRANCH_MISS = {0x4300C3, CORE, "branch-misses"};
// Cache miss on last on chip cache (often: L2)
constexpr PMCEvent L2_CACHE_MISS = {0x430964, CORE, "l2-cache-misses"};
// Cache access on last on chip cache (often: L2)
constexpr PMCEvent L2_CACHE = {0x430729, CORE, "l2-cache-accesses"};
// Number of TLB flushes
constexpr PMCEvent TLB_FLUSHES = {0x43FF78, CORE, "tlb-flushes"};
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
} // namespace Events

// ---------------------------------------
// High level measurement logic
// ---------------------------------------

struct Event {
  PMCEvent pmce;

  uint64_t before;
  uint64_t after;

  Event(PMCEvent pmce, PMCSelect &pmcs) : pmce(pmce), pmcs(pmcs) {}

  void start() {
    pmc = pmcs.acquire(pmce.pmClass);
    if (!pmc) {
      std::cerr << "[ERROR] All hardware counters are occupied. Event "
                << pmce.name << " will not be measured." << std::endl;
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

  uint64_t report() { return after - before; }

private:
  PMCSelect &pmcs;
  PMC *pmc;
};

// The uperf interface to the application:
// A number of counters that are started and stopped simultaneously.
// The default constructor sets popular counters and assumes no other
// performance measurements on the same core.
//
// To select different counters:
struct Collection {
  // The selection of hardware counters available to this collection.
  PMCSelect &pmcs = default_pmcs;

  // A vector of registered events. Must not be larger than the number of
  // available hardware counters.
  std::vector<Event> events;

  std::chrono::time_point<std::chrono::steady_clock> startTime;
  std::chrono::time_point<std::chrono::steady_clock> stopTime;

  // Constructor to use with default set of counters.
  // Common use case: On-core measurements, single Collection per core.
  Collection(bool set_default_counters = true) {
    enable_pmu();

    if (set_default_counters) {
      registerCounter(Events::CPU_CYCLES);
      registerCounter(Events::INSTRUCTIONS);
      registerCounter(Events::L2_CACHE_MISS);
      registerCounter(Events::BRANCH_MISS);
    }
  }

  // Constructor to use with custom set of counters.
  // Common use case: Share counters between multiple collections
  Collection(PMCSelect &pmcSelect) : pmcs(pmcSelect) { enable_pmu(); }

  void registerCounter(PMCEvent pmce) { events.push_back(Event(pmce, pmcs)); }
  void registerCounter(PMCEvent pmce, const char *name) {
    pmce.name = name;
    registerCounter(pmce);
  }
  void registerCounter(uint64_t bitmap, PMClass pmClass, const char *name) {
    registerCounter({bitmap, pmClass, name});
  }

  double getCounter(const char *name) {
    for (auto &event : events) {
      if (event.pmce.name == name)
        return event.report();
    }
    return -1;
  }

  double getDuration() {
    return std::chrono::duration<double>(stopTime - startTime).count();
  }

  // IPC is calculated from the instructions and cycle counter.
  // If one of them is not counted, this function returns NaN.
  double getIPC() {
    double res = getCounter(Events::INSTRUCTIONS.name) /
                 getCounter(Events::CPU_CYCLES.name);
    return res > 0 ? res : NAN;
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
    printCounter(headerOut, dataOut, name, stream.str(), addComma);
  }

  void printReport(std::ostream &out, uint64_t normalizationConstant = 1) {
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

    printCounter(headerOut, dataOut, "IPC", getIPC());
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

private:
  // By default, each collection operates on known core-local counters,
  // but selections can also be shared between cores to measure uncore counters
  // (e.g. L3 cache counters)
  PMCSelectCore default_pmcs{
#ifdef ARCH_TARGET_X86_64
      PMC{0xC0010200, 0xC0010201, CORE}, PMC{0xC0010202, 0xC0010203, CORE},
      PMC{0xC0010204, 0xC0010205, CORE}, PMC{0xC0010206, 0xC0010207, CORE},
      PMC{0xC0010208, 0xC0010209, CORE}, PMC{0xC001020A, 0xC001020B, CORE},
#elif defined ARCH_TARGET_ARM64
      // Armv8_pmu3 usually has 6 counters (ids 0-5)
      PMC{0, 0, CORE},
      PMC{1, 1, CORE},
      PMC{2, 2, CORE},
      PMC{3, 3, CORE},
      PMC{4, 4, CORE},
      PMC{5, 5, CORE},

      PMC{1u << 31, 1u << 31, CYCLES},
#endif
  };
};
} // namespace uperf
