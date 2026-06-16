#include "SSDCounters.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
   std::array<std::atomic<SSDCounters*>, MAX_CORES> SSDCounters::ssd_counters = {nullptr}; // Per-core storage
        atomic<u64> SSDCounters::ssd_counter = {0};

        SSDCounters& SSDCounters::myCounters()
{
   int core_id = sched_getcpu();  // Get the current core ID
   SSDCounters* expected = ssd_counters[core_id].load(std::memory_order_acquire);

   if (!expected) {
    SSDCounters* new_instance = new SSDCounters(core_id);
      if (!ssd_counters[core_id].compare_exchange_strong(expected, new_instance)) {
         // Another thread initialized it first, delete our instance
         delete new_instance;
      }
      expected = ssd_counters[core_id].load(std::memory_order_acquire);
   }

   return *expected;
}}