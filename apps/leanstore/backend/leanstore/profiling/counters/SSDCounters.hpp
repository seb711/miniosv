#pragma once
#include "Units.hpp"
// -------------------------------------------------------------------------------------

#include "PerfEvent.hpp"
#include "leanstore/utils/Hist.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <unordered_map>
// -------------------------------------------------------------------------------------
namespace leanstore
{
struct SSDCounters {
   static constexpr u64 max_ssds = 20;
   atomic<u64> t_id = 9999;                // used by tpcc
   // -------------------------------------------------------------------------------------
   // Space and contention management
   atomic<u64> pushed[max_ssds] = {0};
   atomic<u64> polled[max_ssds] = {0};
   atomic<s64> outstandingx_max[max_ssds] = {0};
   atomic<s64> outstandingx_min[max_ssds] = {0};
   atomic<u64> read_latncy50p[max_ssds] = {0};
   atomic<u64> read_latncy99p[max_ssds] = {0};
   atomic<u64> read_latncy99p9[max_ssds] = {0};
   atomic<u64> read_latncy_max[max_ssds] = {0};
   atomic<u64> write_latncy50p[max_ssds] = {0};
   atomic<u64> write_latncy99p[max_ssds] = {0};
   atomic<u64> write_latncy99p9[max_ssds] = {0};
   atomic<u64> writes[max_ssds] = {0};
   atomic<u64> reads[max_ssds] = {0};
   // -------------------------------------------------------------------------------------
   explicit SSDCounters(int core) : core_id(core), ti_id(ssd_counter++) {}
   // -------------------------------------------------------------------------------------
   static std::atomic<uint64_t> ssd_counter;
   static std::array<std::atomic<SSDCounters*>, MAX_CORES> ssd_counters; // Per-core storage
   static std::mutex ssd_counters_mut; // Fallback mutex
   static SSDCounters& myCounters(); 

   int core_id;
   int ti_id;
};
}  // namespace leanstore
// -------------------------------------------------------------------------------------
