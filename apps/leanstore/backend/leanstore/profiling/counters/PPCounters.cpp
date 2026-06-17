#include "PPCounters.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
   std::array<std::atomic<PPCounters*>, MAX_CORES> PPCounters::pp_counters = {nullptr}; // Per-core storage
        atomic<u64> PPCounters::pp_counter = {0};

PPCounters& PPCounters::myCounters()
{
   int core_id = sched_getcpu();  // Get the current core ID
   PPCounters* expected = pp_counters[core_id].load(std::memory_order_acquire);

   if (!expected) {
      PPCounters* new_instance = new PPCounters(core_id);
      if (!pp_counters[core_id].compare_exchange_strong(expected, new_instance)) {
         // Another thread initialized it first, delete our instance
         delete new_instance;
      }
      expected = pp_counters[core_id].load(std::memory_order_acquire);
   }

   return *expected;
}}