#pragma once

#include "./helper/ContextPool.hpp"
#include "./helper/DummyNic.hpp"
#include "./helper/LatencyTracker.hpp"
#include "./helper/SystemState.hpp"
#include "Exceptions.hpp"
#include "UniqueBackgroundWork.hpp"
#include "UniqueTask.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/concurrency/utils/MessageHandler.hpp"
#include "leanstore/concurrency/utils/ThreadBase.hpp"
#include "leanstore/io/IoAbstraction.hpp"
#include "leanstore/profiling/counters/TaskExecutorCounters.hpp"
#include "leanstore/storage/btree/core/BTreeInterface.hpp"
#include "leanstore/storage/buffer-manager/BufferFrame.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"

#include "boost/context/continuation.hpp"
#include "boost/context/continuation_fcontext.hpp"

#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>

// #define USE_INTERRUPTS
// #define USE_PERIODIC_TIMER
// #define USE_WATCHDOG
#define INTERRUPT_TIME FLAGS_tmp

#if IS_PERIODIC
#define USE_INTERRUPTS
#define USE_PERIODIC_TIMER
#endif

#ifdef IS_WATCHDOG
#define USE_INTERRUPTS
#define USE_WATCHDOG
#endif

#ifdef IS_ADAPTIVE
#define USE_BACKGROUND_TASKS
#endif

namespace mean
{
class UniqueTaskExecutor : public ThreadBase
{
  public:
   using UniqueTaskPtr = UniqueTask*;

   // Constructor & Destructor
   UniqueTaskExecutor(MessageHandler& msg, IoChannel& io, DummyNIC& nic, int id);
   ~UniqueTaskExecutor();

   // Delete copy and move
   UniqueTaskExecutor(const UniqueTaskExecutor&) = delete;
   UniqueTaskExecutor(UniqueTaskExecutor&&) = delete;
   UniqueTaskExecutor& operator=(const UniqueTaskExecutor&) = delete;
   UniqueTaskExecutor& operator=(UniqueTaskExecutor&&) = delete;

   // Static Context Entry Points
   static void trampoline(boost::context::detail::transfer_t t);
   static boost::context::detail::transfer_t store_sink_on_yield(boost::context::detail::transfer_t t);
   static boost::context::detail::transfer_t store_task_on_yield(boost::context::detail::transfer_t t);

   // Task Management
   void pushTask(UniqueTaskPtr task);
   void pushTask(TaskFunction* fun);
   void moveReady(UniqueTaskPtr task);
   bool popTask(UniqueTaskPtr& task);
   int taskCount();

   // Page Provider
   void registerPageProvider(void*, u64 partition_id);
   void pageProviderCycle();

   // Messaging
   void sendMessage(int toId, MessageFunction fun, uintptr_t userData);

   // Context Management
   void updateCurrentSink(boost::context::detail::fcontext_t t)
   {
      // std::cout << "current sink set: " << std::hex << t << std::endl << std::hex;
      _currentSink = t;
   }
   boost::context::detail::fcontext_t getCurrentSink()
   {
      // std::cout << "current sink get: " << std::hex << _currentSink << std::endl << std::flush;
      // assert(false);
      ensure(_currentSink != nullptr);
      assert(_currentSink != nullptr);

      auto tmp = _currentSink;
      _currentSink = nullptr;
      return tmp;
   }
   void set_workload_function(TaskFunction fun) { workloadFunction = fun; }
   // -------------------------------------------------------------------------------------

   // Static Accessors
   static UniqueTaskExecutor& localExec();
   static UniqueTask& currentTask();
   static UniqueTaskPtr getCurrentTaskOwnership();
   static void yieldCurrentTask(TaskState ts);
   static void yieldRunningTask(UniqueTask* task, TaskState ts);
   void setupInterruptHandling();

   // Public Members
   TaskContextPool* g_task_context_pool;
   void* _sinkInterruptStack;
   UniqueTaskPtr _currentTask;  // Use brace initialization
   boost::context::detail::fcontext_t _currentSink = nullptr;

   std::atomic<float> sleep;
   IoChannel& ioChannel;
   DummyNIC& nic;
   TaskExecutorCounters counters;
   leanstore::cr::Worker* this_worker;

  private:
   // Watchdog Syncing Structure
   // Padded atomic timestamp that occupies exactly one cache line
   struct alignas(64) PaddedTimestamp {
      std::atomic<uint64_t> timestamp;

      // Padding to fill the rest of the cache line
      char padding[64 - sizeof(std::atomic<uint64_t>)];

      // Assignment operator from uint64_t
      PaddedTimestamp& operator=(uint64_t value)
      {
         timestamp.store(value, std::memory_order_release);
         return *this;
      }

      // Optional: conversion operator to read the value
      operator uint64_t() const { return timestamp.load(std::memory_order_acquire); }

      PaddedTimestamp() : timestamp(0) {}
   };

   // Now create your array
   static PaddedTimestamp timestamps[8];

   // Lifecycle

   // Thread Pool Management
   void initTaskContextPool(size_t capacity);
   void destroyTaskContextPool();
   void initializeTaskContext(UniqueTaskContext& ctx, void* stack_base, size_t stack_size, void (*fn)(boost::context::detail::transfer_t));

   // Task Creation
   UniqueTaskPtr createTask(TaskFunction* fun, void (*entry_fn)(boost::context::detail::transfer_t));

   // Main Execution Loop
   int process() override;
   void cycle();

   // Cycle Helper Methods
   void handleSleep();
   void pollMessages();
   void pollIo();
   void handleIoSubmission(u64 cycles, u64& delay_until_cycle);
   void submitIo();
   void handleDelayedIoSubmission(u64 cycles, u64& delay_until_cycle, int delay_amount);

  public:
   int pollWorkload();
   int pollWorkloadWithRate();
   int pollWorkloadWithoutRate();
   bool shouldCheckWatchdog();

   // Task Execution
   TaskState runCurrentTask();
   int runScheduledTasks();
   bool tryAcquireTaskLock();
   void handleTaskState(TaskState state);
   void handleDoneTask();
   void handleWaitingTask();
   void handleWaitIoTask();
   void handleReadyMemTask();
   void handleReadyLockTask();
   void handleReadyJumpLockTask();
   void handleReadyTask();
   void updateCycleCounters(int tasks_run, u64& cycles_nothing_run);

   // Background Work
   void setupBackgroundWork();
   void handleBackgroundWork();

   static std::atomic<int> interruptVector;
   static std::atomic<int> readyExecutors;
   static void setupInterruptVector();

   // Task Queues
   static const int MAX_TASKS = 1 << 10;
   leanstore::utils::RingBuffer<UniqueTaskPtr> tasks{MAX_TASKS};
   leanstore::utils::RingBuffer<UniqueTaskPtr> tasks_io_done{MAX_TASKS};

#ifndef NDEBUG
   std::unordered_map<UniqueTask*, std::tuple<TaskState, TimePoint, bool>> waiting_tasks;
#endif

   // Background Work
   uint64_t last_background_check = 0;
   std::array<uint8_t, 1 << 12> time_wheel;
   std::array<std::unique_ptr<UniqueBackgroundWork>, 8> background_work;
   std::uniform_int_distribution<> distr;
   std::mt19937 gen;

   // Workload
   TaskFunction workloadFunction;

   // Infrastructure
   MessageHandler& messageHandler;
   jumpmu::JumpMUContext defaultExecutorContext;
   boost::context::continuation main_process_context;

   // Task Counters
   std::atomic<u64> waitingTaskCount = {0};
   std::atomic<u64> waitIoTaskCount = {0};

   // Watchdog stuff
   int core_id;
   uint64_t last_watchdog_access = 0;

   // BG Optimization
   LatencyTracker latencyTracker;
   SystemState systemState;
   size_t current_optimize_task = 1;  // start with first active task
   uint32_t optimize_tick = 0;

  public:
   // Page Provider State
   s64 partition_id = -1;
   BufferManager* buffer_manager;
   s64 pp_required = 0;
   bool local_pause_seen = false;
};

// Thread-local State
extern std::array<bool, MAX_CORES> run_task;
extern thread_local std::atomic<uint64_t> last_timestamp;
}  // namespace mean