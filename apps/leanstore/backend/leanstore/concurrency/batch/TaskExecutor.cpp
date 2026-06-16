// -------------------------------------------------------------------------------------
#include "TaskExecutor.hpp"
#include "Task.hpp"
// -------------------------------------------------------------------------------------
#include "Exceptions.hpp"
#include "Time.hpp"
#include "leanstore/concurrency/utils/MessageHandler.hpp"
#include "leanstore/concurrency/utils/ThreadBase.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/profiling/counters/ThreadCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
// -------------------------------------------------------------------------------------
#include "boost/context/continuation.hpp"
// -------------------------------------------------------------------------------------
#include <boost/context/preallocated.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
#if true //MACRO_COUNTERS_ALL
#define DEBUG_TASK_COUNTERS_BLOCK(X) X
#else
#define DEBUG_TASK_COUNTERS_BLOCK(X)
#endif
//std::atomic<bool> TaskExecutor::pause = true;
//std::atomic<int> TaskExecutor::pause_seen = 0;
// -------------------------------------------------------------------------------------
TaskExecutor::TaskExecutor(MessageHandler& msg, IoChannel& ioChannel, int id)
    : ThreadBase("te_" /*+ std::to_string(id)*/, id), messageHandler(msg), ioChannel(ioChannel)
{
   jumpmu::thread_local_jumpmu_ctx = &defaultExecutorContext;
}
TaskExecutor::~TaskExecutor() {}
// -------------------------------------------------------------------------------------
void TaskExecutor::runUserThread(Task& task)
{
   // std::cout << "runUserThread swap oucp: " << std::hex <<  current_uctx << " ucp: " << &th->context  << std::dec <<  std::endl << std::flush;
   _currentTask = &task;
   jumpmu::thread_local_jumpmu_ctx = &task.jumpctx;
   // -------------------------------------------------------------------------------------
    // -------------------------------------------------------------------------------------
   if (!task.context.init) {
      // std::cout << "xx runUserThread init " << std::endl;
      task.context.init = true;
      task.context.this_task_context =
          boost::context::callcc(std::allocator_arg, boost::context::fixedsize_stack(64 * 1024), [&task](boost::context::continuation&& sink) {
             // task.context.this_task_context = boost::context::callcc([&task](boost::context::continuation&& sink_process_context){
             // std::cout << "callcc lamda " <<std::endl << std::flush;
             task.context.sink_process_context = &sink;
             task.fun();
             task.state = TaskState::Done;
             // std::cout << "callcc lamda end" <<std::endl << std::flush;
             return std::move(sink);
          });
      // std::cout << "xx runUserThread init done  " << std::endl;
   } else {
      ////std::cout << "xx runUserThread resume " << std::endl;
      task.context.this_task_context = task.context.this_task_context.resume();
   }
   _currentTask = nullptr;
   jumpmu::thread_local_jumpmu_ctx = &defaultExecutorContext;
   // std::cout << "rundUserThread swap end " <<  std::endl << std::flush;
}
// -------------------------------------------------------------------------------------
void TaskExecutor::yieldCurrentTask(TaskState ts)
{
   // std::cout << "yield" << std::endl << std::flush;
   Task& task = currentTask();
   task.state = ts;
   // cycle(); TODO directly run scheduler and jump to next context
   // this would return to process task.
   *task.context.sink_process_context = task.context.sink_process_context->resume();
   // Careful: function will continue here only when the task is being resumed
   // std::cout << "continue" << std::endl << std::flush;
}
// -------------------------------------------------------------------------------------
int TaskExecutor::process()
{
   ensure(tasks.size() == 0);
   // std::cout << "boost fixedsize_tack size: " << boost::context::fixedsize_stack::traits_type::default_size() << std::endl;
   cycle();
   // std::cout << "# task exec end return " << id() << " #" << std::endl << std::flush;
   return 0;
}
bool TaskExecutor::popTask(Task*& task) {
   if (tasks_io_done.try_pop(task)) {
      return true;
   }
   return tasks.try_pop(task);
}
void TaskExecutor::cycle()
{
   // -------------------------------------------------------------------------------------
   u64 cycles = 0;
   u64 cyclesNothingRun = 0;
   const u64 sleepIfNothingRunForCycles = 100000;
   u64 delaySubmitUntilCycle = 0;
   auto counterUpdateTime = getSeconds();
   random_generator.seed(mean::exec::getId());

   while (_keep_running) {

      if (sleep != 0) {
         float s = sleep.exchange(0);
         std::cout << "sleep for: " << s << std::endl;
         std::this_thread::sleep_for(std::chrono::nanoseconds((uint64_t)(s*1e9)));
      }
      cycles++;
      // good for ycsb 60 thr: p: 2, pp: 32, d: 0
      constexpr int everyPoll = 64;
      constexpr int everyPP = 32;
      constexpr int delaySubmit = 0;
      // run poll Routines
      // TODO for runs with >>6 60 threads, this hast to be changed
      if (cycles % (8*1024) == 0 || cyclesNothingRun > sleepIfNothingRunForCycles) {
         messageHandler.poll(this);
         counters.msgPollCalled++;
      }
      if (cycles % everyPP  == 0) {
         pageProviderCycle();
      }
      if (cycles % everyPoll == 0) {
         leanstore::ThreadCounters::myCounters().exec_cycles += everyPoll;
         counters.cycles = cycles;
         ioChannel.poll();
         if (cycles % (8*1024)) {
            auto now = getSeconds();
            if (now - counterUpdateTime > 0.99999) {
               counterUpdateTime = now;
               /*COUNTERS_BLOCK()*/ { ioChannel.counters.updateLeanStoreCounters(); }
               /*COUNTERS_BLOCK()*/ { ioChannel.counters.reset(); }
            }
         }
      }
      // -------------------------------------------------------------------------------------
      // schedule next task
      const int maxTasksRun = 1;
      int tasksRun = 0;
      //*
      Task* task;
      // WE HAVE HERE A INDIVIUAL SCHEDULING DECISION -> IO TASKS GET WORKED THROUGH FIRST
      while (tasksRun < maxTasksRun && popTask(task)) { // pop after maxTaskRun check 
         //std::cout << this->getName() << " runTask: " << task << " task.size: " << tasks.size() << std::endl;
         counters.tasksRun++;
         //DEBUG_TASK_COUNTERS_BLOCK(auto start = readTSC();)
         if (task->state == TaskState::ReadyLock) {
            if (!task->lock->try_lock()) {
               tasks.push_back(task);
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_lckskip++; }
               break;
            }
         }
         tasksRun++;
         // DEBUG: a task pulled from a run queue must be runnable, never parked on IO
         // or waiting. If a WaitIo task is being run, it would resume before its read
         // completed and operate on stale/garbage page data.
         if (task->state == TaskState::WaitIo || task->state == TaskState::Waiting) {
            printf("DBG RUN-BLOCKED: about to run task %p in parked state %d\n", (void*)task, (int)task->state);
            abort();
         }
         if (task->dbg_outstanding_ios.load() != 0) {
            printf("DBG RUN-IO: about to run task %p with outstanding_ios=%d (IO not done)\n",
                   (void*)task, task->dbg_outstanding_ios.load());
            abort();
         }
         runUserThread(*task);
         //DEBUG_TASK_COUNTERS_BLOCK(auto time = readTSC() - start; counters.taskDurationNet += time;)
         switch (task->state) {
            case TaskState::Done:
               counters.tasksCompleted++;
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_comp++; }
               //memset((void*)task, 0, sizeof(Task));
               //delete task; //FIXME
               break;
            case TaskState::Waiting:
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_wait++; }
               counters.tasksWaiting++;
               waitingTaskCount++;
               break;
            case TaskState::WaitIo:
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_wait_io++; }
               counters.tasksWaiting++;
               waitingTaskCount++;
               waitIoTaskCount++;
#ifndef NDEBUG
               waiting_tasks[task] = std::tuple(TaskState::WaitIo, getTimePoint(), false);
#endif
               break;
            case TaskState::ReadyNoFreePages:
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_mem++; }
               counters.tasksReady++;
               leanstore::WorkerCounters::myCounters().time_counter_2++; 
               tasks.push_back(task);
               break;
            case TaskState::ReadyLock:
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_lck++; }
               counters.tasksReady++;
               tasks.push_back(task);
               break;
            case TaskState::ReadyJumpLock:
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_jumplck++; }
               counters.tasksReady++;
               tasks.push_back(task);
               break;
            case TaskState::Ready:
               COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_st_ready++; }
               counters.tasksReady++;
               tasks.push_back(task);
               break;
            case TaskState::WaitSubmit:
               // OsvImpl's submit loop yields this when the I/O submit queue is
               // full; re-queue so it retries next cycle (osv builds with
               // IS_PERIODIC undefined, so this path is live here).
               counters.tasksReady++;
               tasks.push_back(task);
               break;
            default:
               throw std::logic_error("should never happen");
         }
         COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_tasks_run++; }
      }
#ifndef NDEBUG
      auto now = getTimePoint();
      for (auto& tt: waiting_tasks) {
         auto diff = timePointDifferenceMs(now, std::get<1>(tt.second));
         if (diff > 1000 && !std::get<2>(tt.second)) {
            std::get<2>(tt.second) = true;
            std::cout << "io is stuck " << tt.first << " diff: " << std::dec << diff << std::endl;
            //raise(SIGINT);
         }
      }
#endif
      if (tasksRun == 0) {
         COUNTERS_BLOCK() { leanstore::ThreadCounters::myCounters().exec_no_tasks_run++; }
         cyclesNothingRun++;
      } else {
         cyclesNothingRun = 0;
      }
   }
}

// -------------------------------------------------------------------------------------
void TaskExecutor::pushPoller(TaskFunction poller)
{
   pollers.push_back(std::make_unique<Task>(poller));
}
void TaskExecutor::registerPageProvider(void* bm_ptr, u64 partition_id) {
   this->partition_id = partition_id;
   this->buffer_manager = static_cast<BufferManager*>(bm_ptr);
   ensure(buffer_manager->cooling_partitions_count > partition_id);
   buffer_manager->cooling_partitions[partition_id].state.debug_thread = mean::exec::getId();
}
void TaskExecutor::pageProviderCycle() {
   if (buffer_manager && partition_id >= 0) {
      buffer_manager->pageProviderCycle(partition_id);
   }
}
void TaskExecutor::pushTask(Task* task)
{
   tasks.push_back(task);
}
void TaskExecutor::pushTask(TaskFunction fun)
{
   auto task = new Task(fun);
   tasks.push_back(task);
}
void TaskExecutor::moveReady(Task* task)
{
   // DEBUG: only a parked task (WaitIo from blockingIo, or Waiting from a message)
   // should ever be woken, and any IO it parked on must have completed first.
   if (task->state != TaskState::WaitIo && task->state != TaskState::Waiting) {
      printf("DBG WAKE-STATE: moveReady on task %p in non-parked state %d\n", (void*)task, (int)task->state);
      abort();
   }
   if (task->dbg_outstanding_ios.load() != 0) {
      printf("DBG WAKE-IO: moveReady on task %p with outstanding_ios=%d (IO not done)\n",
             (void*)task, task->dbg_outstanding_ios.load());
      abort();
   }
   task->state = TaskState::Ready;
#ifndef NDEBUG
   waiting_tasks.erase(task);
#endif
   waitingTaskCount--;
   waitIoTaskCount--;
   tasks_io_done.push_back(task);
}
int TaskExecutor::taskCount()
{
   return tasks.size();
}
void TaskExecutor::sendMessage(int toId, MessageFunction fun, uintptr_t userData)
{
   messageHandler.sendMessage(toId, fun, userData);
}
TaskExecutor& TaskExecutor::localExec()
{
   return static_cast<TaskExecutor&>(ThreadBase::this_thread());
}
Task& TaskExecutor::currentTask()
{
   auto t = localExec()._currentTask;
   ensure(t);
   return *t;
}
// -------------------------------------------------------------------------------------
// True only while a coroutine Task is running on this executor. The page-provider
// cycle runs inline in cycle() with _currentTask == nullptr, so this returns false
// there and callers must not yield.
bool TaskExecutor::inTask()
{
   return localExec()._currentTask != nullptr;
}
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------