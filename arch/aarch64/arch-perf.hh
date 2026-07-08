#pragma once

// ARMv8-A PMU back-end for osv/perf.hh.
// Targets Ampere-1a, Neoverse V1 and Neoverse V2.

#include "osv/perf.hh"
#include <cstdint>
#include <iostream>

namespace perf {

inline constexpr uint32_t midr_fixed_mask = 0xFF0F'FFF0u;

// Number of hardware event counters (PMCR_EL0.N, bits 15:11).
inline uint32_t pmu_num_counters() {
  uint64_t pmcr;
  asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
  return (pmcr >> 11) & 0x1F;
}

// Whether we are running on a specific CPU implementation (matched via
// MIDR_EL1).
inline bool is_midr(uint32_t to_check) {
  uint64_t midr;
  asm volatile("mrs %0, midr_el1" : "=r"(midr));
  return (static_cast<uint32_t>(midr) & midr_fixed_mask) == to_check;
}

// Whether OSv is running as a KVM guest. Queries SMCCC Vendor Hypervisor UID
// (function ID 0x8600FF01); KVM's UUID is defined in the Linux kernel at
// arch/arm64/kvm/hypercalls.c (ARM_SMCCC_VENDOR_HYP_UID_KVM_REG_{0..3}).
inline bool is_kvm_guest() {
  register uint64_t x0 asm("x0") = 0x8600FF01ull;
  register uint64_t x1 asm("x1");
  register uint64_t x2 asm("x2");
  register uint64_t x3 asm("x3");
  asm volatile("hvc #0" : "+r"(x0), "=r"(x1), "=r"(x2), "=r"(x3));
  return x0 == 0xb66fb428ull && x1 == 0xe911c52eull &&
         x2 == 0x564bcaa9ull && x3 == 0x743a004dull;
}

// Whether the requested event is supported on this CPU (per PMCEID{0,1}_EL0).
inline bool is_event_supported(uint64_t value) {
  uint64_t pmceid;
  uint64_t cmp;
  if (value < 0x20) {
    // Supported if pmceid0[value] = 1 (lower 32 bits of PMCEID0_EL0).
    cmp = value;
    asm volatile("mrs %0, pmceid0_el0" : "=r"(pmceid));
  } else if (value < 0x40) {
    // Supported if pmceid1[value-0x20] = 1 (lower 32 bits of PMCEID1_EL0).
    cmp = value - 0x20;
    asm volatile("mrs %0, pmceid1_el0" : "=r"(pmceid));
  } else if (value < 0x4000) {
    // No PMCEID3 in 64-bit mode to validate these events against.
    std::cout << "Warning: Requested event 0x" << std::hex << value
              << " could not be checked for compatibility" << std::endl;
    return true;
  } else if (value < 0x4020) {
    // Supported if pmceid0[value-0x4000] = 1 (upper 32 bits of PMCEID0_EL0).
    cmp = value - 0x4000;
    asm volatile("mrs %0, pmceid0_el0" : "=r"(pmceid));
  } else if (value < 0x4040) {
    // Supported if pmceid1[value-0x4020] = 1 (upper 32 bits of PMCEID1_EL0).
    cmp = value - 0x4020;
    asm volatile("mrs %0, pmceid1_el0" : "=r"(pmceid));
  } else {
    // No PMCEID3 in 64-bit mode to validate these events against.
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

  // Clear all counter enables.
  asm volatile("msr pmcntenclr_el0, %0\n\t"
               "isb" ::"r"((uint64_t)0xFFFFFFFF)
               : "memory");
  // PMCR_EL0: set E (enable), P (reset event counters), C (reset cycle
  // counter); clear D (cycle counter divider).
  asm volatile("mrs %0, pmcr_el0\n\t"
               "orr %0, %0, #0b111\n\t"
               "and %0, %0, #~0b1000\n\t"
               "msr pmcr_el0, %0\n\t"
               "isb\n\t"
               : "=&r"(pmcr)
               :
               : "memory");
}

inline void pmc_stop(uint32_t counter_mask) {
  asm volatile("msr pmcntenclr_el0, %0" : : "r"((uint64_t)counter_mask));
  asm volatile("isb" ::: "memory");
}

inline void pmc_write_counter(uint32_t counter, uint64_t value) {
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

inline void pmc_start_with_conf(uint32_t counter, uint32_t evt_sel,
                                uint64_t value) {
  if (!is_event_supported(value))
    return;

  // Write event configuration into corresponding pmevtyperN_el0.
  switch (evt_sel) {
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
  // Synchronise pipeline before enabling the counter.
  asm volatile("isb" ::: "memory");

  asm volatile("msr pmcntenset_el0, %0" : : "r"(1ull << counter));

  // Synchronise pipeline before counting starts.
  asm volatile("isb" ::: "memory");
}

inline uint64_t pmc_read(uint32_t counter) {
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

namespace PERF_COUNT_HW {
using enum PMClass;

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
} // namespace PERF_COUNT_HW

} // namespace perf
