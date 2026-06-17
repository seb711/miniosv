#pragma once
#include "Units.hpp"
// -------------------------------------------------------------------------------------
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/utils/Hist.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <unordered_map>
// -------------------------------------------------------------------------------------
namespace leanstore
{
struct ThreadCounters {
   atomic<u64> t_id = mean::exec::getId();
   // -------------------------------------------------------------------------------------
   atomic<u64> tx = 0; 
   atomic<u64> tx_abort = 0; 
   atomic<u64> exec_cycles = 0; 
   atomic<u64> exec_cycl_max_dur = 0; 
   atomic<u64> exec_cycl_max_poll_us= 0; 
   atomic<u64> exec_cycl_max_subm_us= 0; 
   atomic<u64> exec_cycl_max_task_us= 0; 
   atomic<u64> exec_tasks_run = 0; 
   atomic<u64> exec_no_tasks_run = 0; 
   atomic<u64> exec_tasks_st_comp = 0; 
   atomic<u64> exec_tasks_st_wait = 0; 
   atomic<u64> exec_tasks_st_wait_io = 0; 
   atomic<u64> exec_tasks_st_ready_mem = 0; 
   atomic<u64> exec_tasks_st_ready_lck = 0; 
   atomic<u64> exec_tasks_st_ready_lckskip = 0; 
   atomic<u64> exec_tasks_st_ready_jumplck = 0; 
   atomic<u64> exec_tasks_st_ready = 0; 
   atomic<u64> pp_p1_picked = 0; 
   atomic<u64> pp_p23_evicted = 0; 
   atomic<u64> pp_p2_iopushed = 0; 
   // -------------------------------------------------------------------------------------
   explicit ThreadCounters(int core) : core_id(core), ti_id(thread_counter++) {}
   // -------------------------------------------------------------------------------------
   static std::atomic<uint64_t> thread_counter;
   static std::array<std::atomic<ThreadCounters*>, MAX_CORES> thread_counters; // Per-core storage
   static std::mutex thread_counters_mut; // Fallback mutex
   static ThreadCounters& myCounters(); 

   int core_id;
   int ti_id;
};
}  // namespace leanstore
// -------------------------------------------------------------------------------------
