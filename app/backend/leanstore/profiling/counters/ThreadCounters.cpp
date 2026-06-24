#include "ThreadCounters.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
   std::array<std::atomic<ThreadCounters*>, MAX_CORES> ThreadCounters::thread_counters = {nullptr}; // Per-core storage
        atomic<u64> ThreadCounters::thread_counter = {0};

ThreadCounters& ThreadCounters::myCounters()
{
   int core_id = sched_getcpu();  // Get the current core ID
   ThreadCounters* expected = thread_counters[core_id].load(std::memory_order_acquire);

   if (!expected) {
      ThreadCounters* new_instance = new ThreadCounters(core_id);
      if (!thread_counters[core_id].compare_exchange_strong(expected, new_instance)) {
         // Another thread initialized it first, delete our instance
         delete new_instance;
      }
      expected = thread_counters[core_id].load(std::memory_order_acquire);
   }

   return *expected;
}}