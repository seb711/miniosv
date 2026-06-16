#pragma once
// -------------------------------------------------------------------------------------
#include "UniqueTaskExecutor.hpp"
#include "leanstore/utils/BlockedRange.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/io/IoInterface.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <iostream>
#include <memory>
#include <vector>
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
class UniqueTaskManager
{
   std::atomic<int> runningExecs = {0};
   std::vector<std::unique_ptr<UniqueTaskExecutor>> execs;
   std::atomic<int> exclusiveThreadCounter = {0};
   std::unordered_map<int, std::reference_wrapper<UniqueTaskExecutor>> exclusiveThreadsMap;
   std::vector<std::unique_ptr<ThreadWithJump>> exclusive_threads;
   std::unique_ptr<MessageHandlerManager> messageManager = nullptr;
   std::vector<DummyNIC*> nics; 


   int exclusiveThreads;
   int threadAffinityOffset;
   static constexpr u64 MAX_WORKER_THREADS = 256;
   s64 partitions_count = -1;
   BufferManager* buffer_manager = nullptr;
  public:
   leanstore::cr::Worker* workers[MAX_WORKER_THREADS]; // TODO
   // -------------------------------------------------------------------------------------
   ~UniqueTaskManager();
   // -------------------------------------------------------------------------------------
   // env
   // -------------------------------------------------------------------------------------
   void init(int workerThreads, int exclsuiveThreads, IoOptions ioOptions, int threadAffinityOffset = 0);
   void start(TaskFunction taskFun);
   void shutdown();
   void join();
   void sleepAll(float sleep);
   std::string printCountersHeader();
   std::string printCounters(int te_id);
   int workerCount();
   void adjustWorkerCount(int workerThreads);
   void registerPageProvider(void* bf_ptr, int partitions_count);
   void reflowPageProviderPartitions();
   // -------------------------------------------------------------------------------------
   // exec
   // -------------------------------------------------------------------------------------
   UniqueTaskExecutor* localExec();
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
   UniqueTask& this_task();
   void set_current_task_lock(YieldLock& lock);

   // -------------------------------------------------------------------------------------
   // -------------------------------------------------------------------------------------
   UniqueTaskExecutor& getExec(int id);
   int size() const;
  private:
   void sendTask(int to, TaskFunction taskFun);
   IoChannelCounterAggregator printAggregateExecs(std::ostream& ss, int fromExcecId, int toExecId, bool printDetailed);
};

extern UniqueTaskExecutor::UniqueTaskPtr originTask; 
extern std::atomic<uint8_t> parallel_threads; 
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
