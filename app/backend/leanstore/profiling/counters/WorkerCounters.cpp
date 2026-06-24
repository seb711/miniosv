#include "WorkerCounters.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
   std::array<std::atomic<WorkerCounters*>, MAX_CORES> WorkerCounters::worker_counters = {nullptr}; // Per-core storage
   std::array<std::atomic<u64>, MAX_CORES> WorkerCounters::variable_for_workloads = {}; // Per-core storage

        atomic<u64> WorkerCounters::workers_counter = {0};

WorkerCounters& WorkerCounters::myCounters()
{
   int core_id = 1; // sched_getcpu();  // Get the current core ID
   WorkerCounters* expected = worker_counters[core_id].load(std::memory_order_relaxed);

   if (expected == nullptr) {
      WorkerCounters* new_instance = new WorkerCounters(core_id);
      if (!worker_counters[core_id].compare_exchange_strong(expected, new_instance)) {
         // Another thread initialized it first, delete our instance
         delete new_instance;
      }
      expected = worker_counters[core_id].load(std::memory_order_relaxed);
   }

   return *expected;
}
}  // namespace leanstore
