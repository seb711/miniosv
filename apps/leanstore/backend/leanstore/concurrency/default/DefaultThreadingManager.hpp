#pragma once
// -------------------------------------------------------------------------------------
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/concurrency/batch/Task.hpp"
#include "leanstore/concurrency/utils/MessageHandler.hpp"
#include "leanstore/concurrency/utils/ThreadBase.hpp"
#include "leanstore/io/IoInterface.hpp"
#include "leanstore/utils/BlockedRange.hpp"
// -------------------------------------------------------------------------------------
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include "leanstore/sync-primitives/JumpMU.hpp"

#include <thread>
#include <vector>

#define USE_THREAD_POOL
#define USE_SAME_THREAD  // this flag is for measuring the performance if everything runs on the same thread

// -------------------------------------------------------------------------------------
namespace mean
{
struct ThreadData {
   // Your captured variables
   jumpmu::JumpMUContext ctx = {};
   uint64_t id = 0;
   uint64_t timestamp = 0;
   std::atomic<ThreadData*>* head_pointer;
   std::mutex* threadDataPoolMutex;
   std::condition_variable* threadDataPoolCV;
   std::atomic<bool>* cancelable = nullptr;
   std::function<void(u64, std::atomic<bool>& cancelable)>* fun = nullptr;

   ThreadData* next = nullptr;

   ThreadData(std::atomic<ThreadData*>* head_pointer, std::mutex* threadDataPoolMutex, std::condition_variable* threadDataPoolCV)
       : head_pointer(head_pointer), threadDataPoolMutex(threadDataPoolMutex), threadDataPoolCV(threadDataPoolCV) {};
   // Add other captured variables as needed
};
// -------------------------------------------------------------------------------------
class DefaultThreadingManager
{
#ifdef USE_THREAD_POOL
std::vector<std::vector<std::unique_ptr<ThreadWithJump>>> worker_threads_per_core;  // [core][thread_idx]
std::vector<std::unique_ptr<std::atomic<ThreadWithJump*>>> thread_pool_heads;  // One head per core
std::vector<std::unique_ptr<std::mutex>> threadPoolMutexes;  // One mutex per core
std::vector<std::unique_ptr<std::condition_variable>> threadPoolCVs;  // One CV per core

#else
   std::mutex threadDataPoolMutex;
   std::condition_variable threadDataPoolCV;
   std::vector<std::unique_ptr<ThreadData>> thread_data;
   std::atomic<ThreadData*> thread_data_pool_head = {nullptr};
#endif
   int total_threads_count;
   std::atomic<int> running_threads;
   std::vector<std::unique_ptr<ThreadWithJump>> exclusive_threads;
   int max_exclusive_threads;
   std::atomic<int> exclusiveThreadCounter = {0};
   static constexpr int MAX_WORKER_THREADS = 2048;

  public:
   leanstore::cr::Worker* workers[MAX_WORKER_THREADS];
   // -------------------------------------------------------------------------------------
   ~DefaultThreadingManager();
   // -------------------------------------------------------------------------------------
   // env
   // -------------------------------------------------------------------------------------
   void init(int workerThreads, [[maybe_unused]] int exclusvieThreads, IoOptions ioOptions, [[maybe_unused]] int threadAffinityOffset = 0);
   void start(TaskFunction taskFun);
   void shutdown();
   void join();
   std::string stats();
   void adjustWorkerCount(int workerThreads);
   void registerPageProvider(void* bf_ptr, int partitions_count);
   std::string printCountersHeader();
   std::string printCounters(int te_id);
   // -------------------------------------------------------------------------------------
   // exec
   // -------------------------------------------------------------------------------------
   int execId();
   IoChannel& execIoChannel();
   // -------------------------------------------------------------------------------------
   // task
   // -------------------------------------------------------------------------------------
   void registerExclusiveThread(std::string name, int t_i, TaskFunction fun);
   void parallelFor(BlockedRange range, TaskFunction fun, int tasks, s64 bbgranularity = -1, bool rate_active = false);
   void scheduleTaskSync(TaskFunction fun);
   void yield(TaskState ts);
   void blockingIo(IoRequestType type, char* data, s64 addr, u64 len);
   Task& this_task();
   void set_current_task_lock(YieldLock& lock);
   void sleepAll(float sleep) {};

   // -------------------------------------------------------------------------------------
   // int getFd();
   // -------------------------------------------------------------------------------------
   int workerCount();
};
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
