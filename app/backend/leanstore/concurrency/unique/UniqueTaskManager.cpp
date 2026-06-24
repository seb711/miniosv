// -------------------------------------------------------------------------------------
#include "UniqueTaskManager.hpp"
#include "leanstore/concurrency/batch/Task.hpp"
#include "leanstore/io/IoInterface.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
// -------------------------------------------------------------------------------------
#include <osv/clock.hh>
#include "arch.hh"
#include "exceptions.hh"
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
// -------------------------------------------------------------------------------------
namespace mean
{
UniqueTaskExecutor::UniqueTaskPtr originTask;
std::atomic<uint8_t> parallel_threads;

// -------------------------------------------------------------------------------------
UniqueTaskManager::~UniqueTaskManager()
{
   shutdown();
}
// -------------------------------------------------------------------------------------
// env
// -------------------------------------------------------------------------------------
void UniqueTaskManager::init(int workerThreads, int exclusiveThreads, IoOptions ioOptions, int threadAffinityOffset)
{
   std::cout << "USE UNIQUE TASKING" << std::endl;
   if (ioOptions.engine == "auto") {
      if (ioOptions.path.find("traddr") != std::string::npos) {
         ioOptions.engine = "spdk";
      } else {
         ioOptions.engine = "libaio";
      }
   }
   this->exclusiveThreads = exclusiveThreads;
   this->threadAffinityOffset = threadAffinityOffset;
   // -------------------------------------------------------------------------------------
   // all exclusive threds get an exclusibe channel
   // worker threads must share with a message handler
   messageManager = std::make_unique<MessageHandlerManager>(workerThreads + exclusiveThreads);
   IoInterface::initInstance(ioOptions);
   // exclusive
   for (int t_i = 1; t_i <= exclusiveThreads; t_i++) {
      auto thread = std::make_unique<ThreadWithJump>(
          [&, t_i]() {
             // this is the setup code so to say

             // -------------------------------------------------------------------------------------
             std::string name = std::to_string(t_i);
             leanstore::cr::Worker::tls_ptr = workers[t_i];
             // HERE WE WANT TO SETUP ALL THE IMPORTANT THREAD SPECIFIC STUFF

             // -------------------------------------------------------------------------------------
             while (ThreadBase::this_thread().keepRunning()) {
                auto& meta = static_cast<ThreadWithJump*>(&ThreadBase::this_thread())->meta;
                std::unique_lock<std::mutex> guard(meta.mutex);
                meta.cv.wait(guard, [&]() { return ThreadBase::this_thread().keepRunning() == false || meta.job_set; });
                if (!ThreadBase::this_thread().keepRunning()) {
                   break;
                }
                meta.wt_ready = false;
                meta.task();
                meta.wt_ready = true;
                meta.job_done = true;
                meta.job_set = false;
                meta.cv.notify_one();
             }
          },
          "w_" + std::to_string(t_i), t_i);
      thread->setCpuAffinityBeforeStart(t_i);
      thread->setNameBeforeStart("exclusive_" + std::to_string(t_i));
      exclusive_threads.push_back(std::move(thread));
      exclusive_threads.back()->start();
   }

   // workers
   adjustWorkerCount(workerThreads);
}
int UniqueTaskManager::workerCount()
{
   return runningExecs;
}
// -------------------------------------------------------------------------------------
void UniqueTaskManager::adjustWorkerCount(int workerThreads)
{
   if (runningExecs > workerThreads) {
      // shut them down
      for (int i = workerThreads; i < runningExecs; i++) {
         execs[i]->stop();
         execs[i]->join();
      }
      execs.resize(workerThreads);
   } else {
      int ioChannels = IoInterface::instance().channelCount();
      // ioChannels = 1;
      assert(ioChannels > exclusiveThreads);
      for (int i = runningExecs; i < workerThreads; i++) {
         nics.push_back(new DummyNIC(FLAGS_tx_rate / FLAGS_worker_threads));
         int id = i + exclusiveThreads;
         ensure(id < ioChannels);
         // physical channel
         execs.push_back(std::make_unique<UniqueTaskExecutor>(messageManager->getMessageHandler(id),
                                                              IoInterface::instance().getIoChannel(i - runningExecs), *nics.back(), id));
         execs.back()->setCpuAffinityBeforeStart(id + threadAffinityOffset);
         workers[id] = new leanstore::cr::Worker(id, workers, workerThreads + exclusiveThreads);
         execs.back()->this_worker = workers[id];
      }
   }
   runningExecs = workerThreads;
   // reflowPageProviderPartitions();
}
void UniqueTaskManager::reflowPageProviderPartitions()
{
   if (partitions_count <= 0) {
      return;
   }
   // ensure(runningExecs == (int)buffer_manager->cooling_partitions_count);
   for (int t_i = 0; t_i < runningExecs; t_i++) {
      if (!FLAGS_nopp) {
         exclusiveThreads = buffer_manager->cooling_partitions_count;
      }
      if (t_i < (int)buffer_manager->cooling_partitions_count) {
         sendTask(t_i, [=]() { UniqueTaskExecutor::localExec().registerPageProvider(buffer_manager, t_i); });
      }
   }
}
// -------------------------------------------------------------------------------------
void UniqueTaskManager::start(TaskFunction taskFun)
{
   for (auto& exe : execs) {
      exe->start();
      printf("start thread\n");
   }
   for (auto& exe : execs) {
      while (!exe->ready()) {
      }
   }
   printf("everybody is ready\n");
   auto taskf = new TaskFunction(taskFun);
   messageManager->dbgSendMessage(
       exclusiveThreads, exclusiveThreads,
       [](void*, uintptr_t task) {
          auto t = reinterpret_cast<TaskFunction*>(task);
          // ensure(TaskManager::instance().exclusiveThreads.find(this_task::exec().id()) == TaskManager::instance().exclusiveThreads.end());
          UniqueTaskExecutor::localExec().pushTask(t);
       },
       reinterpret_cast<uint64_t>(taskf));
}
// -------------------------------------------------------------------------------------
void UniqueTaskManager::shutdown()
{
   for (auto& exe : execs) {
      exe->stop();
   }
   for (auto* nic : nics) {
      delete nic;
   }
   IoInterface::instance().~RaidEnvironment();
   std::cout << "delete here? " << std::endl;
}
// -------------------------------------------------------------------------------------
void UniqueTaskManager::join()
{
   for (auto& exe : execs) {
      exe->join();
   }
}
void UniqueTaskManager::sleepAll(float sleep)
{
   for (auto& exe : execs) {
      exe->sleep = sleep;
   }
}
// -------------------------------------------------------------------------------------
IoChannelCounterAggregator UniqueTaskManager::printAggregateExecs(std::ostream& ss, int fromExcecId, int toExecId, bool printDetailed)
{
   IoChannelCounterAggregator aggr;
   for (int i = fromExcecId; i < toExecId; i++) {
      UniqueTaskExecutor& exe = *execs[i];
      aggr.aggregate(exe.ioChannel.counters);
      if (printDetailed) {
         ss << exe.id() << ": " << exe.getName() << " ";
         exe.counters.printCounters(ss);
         ss << "\t";
         exe.ioChannel.printCounters(ss);
         ss << std::endl;
      }
      exe.counters.reset();
      exe.ioChannel.counters.reset();
   }
   return aggr;
}
std::string UniqueTaskManager::printCountersHeader()
{
   std::stringstream ss;
   /*
   bool printDetailed = true;
   IoChannelCounterAggregator aggr = printAggregateExecs(ss, 0, exclusiveThreadCounter, printDetailed);
   ss << "e[" << exclusiveThreadCounter << "]: aggr: ";
   aggr.print(ss);
   ss << std::endl;
   aggr = printAggregateExecs(ss, exclusiveThreadCounter, execs.size(), printDetailed);
   ss << "w[" << execs.size() - exclusiveThreadCounter << "]: aggr: ";
   aggr.print(ss);
   ss << std::endl;
   */
   execs[0]->counters.printCountersHeader(ss);
   ss << ",";
   execs[0]->ioChannel.counters.printCountersHeader(ss);
   return ss.str();
}
std::string UniqueTaskManager::printCounters(int te_id)
{
   std::stringstream ss;
   execs[te_id]->counters.printCounters(ss);
   execs[te_id]->counters.reset();
   ss << ",";
   execs[te_id]->ioChannel.counters.printCounters(ss);
   execs[te_id]->ioChannel.counters.reset();
   return ss.str();
}
// -------------------------------------------------------------------------------------
// exec
// -------------------------------------------------------------------------------------
UniqueTaskExecutor* UniqueTaskManager::localExec()
{
   return &UniqueTaskExecutor::localExec();
}
int UniqueTaskManager::execId()
{
   return UniqueTaskExecutor::localExec().id();
}
// -------------------------------------------------------------------------------------
IoChannel& UniqueTaskManager::execIoChannel()
{
   return UniqueTaskExecutor::localExec().ioChannel;
}
// -------------------------------------------------------------------------------------
// task
// -------------------------------------------------------------------------------------
void UniqueTaskManager::registerExclusiveThread(std::string name, int, TaskFunction fun)
{
   int id = exclusiveThreadCounter++;
   auto& ex = *exclusive_threads[id];
   ex.setNameBeforeStart(name);
   ex.sendTask(fun);
}
void UniqueTaskManager::registerPageProvider(void* bf_ptr, int partitions_count)
{
   this->buffer_manager = static_cast<BufferManager*>(bf_ptr);
   this->partitions_count = partitions_count;
   reflowPageProviderPartitions();
}
// -------------------------------------------------------------------------------------
/*
 * Simple parallel for implementation, runs the function over all threads with a number of tasks.
 * The default granularity is ((end-start)/threads/tasks/some factor)
 * If a single cycle through the loop is very short, bbgranularity should be set accordingly higher.
 * must be called from within a task
 */
void UniqueTaskManager::parallelFor(BlockedRange bb, TaskFunction fun, const int tasks, s64 bbgranularity, bool rate_active)
{
   // we also need to setup the interupt here -> because before that we do not know the origin task
   // and we need the origin tasks because we assume that this task is always runnable

   // so first we should setup an interrupt that when fired just swaps context with the origin task and
   // and yields from there...

   // when using the boost execution lib i guess we can just use the yield call acutally because this should just work

   // so we basically only have to install and rewire the interrupt handler and then we are good to go
   // -> but we should in the end rewire the interrupt handler so nothing breaks
   // Disable interrupts while setting up

   ensure(tasks > 0);
   const int threads = workerCount();
   int originExecId = UniqueTaskExecutor::localExec().id();
   originTask = UniqueTaskExecutor::localExec().getCurrentTaskOwnership();

   // std::cout << "threads: " << threads << " tasks: " << tasks << " granularity: " << bbgranularity << std::endl;
   const unsigned int totalTasks = threads * tasks;
   std::atomic<u64> doneTasks = {0};
   std::atomic<bool> cancleable = {false};
   int startedTasks = 0;

   // what we want to do here:
   // 1. setup the necessary fields on the executors on each thread
   // 2. this means that we first set the DummyNICs on all threads or we at least say to use them
   // 3. inserts cannot be parallel inserted -> this is not the main workload unf

   // we create here a workload nic (-> basically a workload generator but that is timing aware)
   // therefore we need a rate and a function

   parallel_threads = threads;

   std::atomic<uint8_t> done_tasks = {0};

   for (int thr = 0; thr < parallel_threads; thr++) {
      done_tasks++;
      sendTask(thr + exclusiveThreads, [&done_tasks, &rate_active, &fun]() {
         // here setup the local executor
         // std::cout << "\n\n\nyeah idk what we will do here\n\n\n" << std::endl;

         // setup the workload function

         // std::cout << "turn on nic " << std::hex << &UniqueTaskExecutor::localExec().nic << std::endl;
         if (rate_active) {
            // problem with the interrupt handling is that you can only set it up in a
            // running environment and not before that -> otherwise the interrupt handler go
            // crazy (-> probably would need some work on the interrupts)
            UniqueTaskExecutor::localExec().setupInterruptHandling();
            UniqueTaskExecutor::localExec().set_workload_function(fun);
            UniqueTaskExecutor::localExec().nic.turn_on();
         } else {
            // run the function directly
            fun();

#if defined(USE_INTERRUPTS) && !defined(USE_PERIODIC_TIMER)
            clock_event->disable();
#endif
            if (--done_tasks == 0) {
               UniqueTaskExecutor::localExec().moveReady(std::move(originTask));
            }
         }
      });
   }
   // yield, and push to waitingTasks

   // clock_event->disable();
   // leanstore_osv_debug::disable_scheduler();

   UniqueTaskExecutor::localExec().yieldRunningTask(originTask, TaskState::Waiting);
   // UniqueTaskExecutor::localExec().yieldCurrentTask(TaskState::Waiting);
   std::cout << "finished" << std::endl;
}
void UniqueTaskManager::scheduleTaskSync(TaskFunction fun)
{
   fun();
}
// -------------------------------------------------------------------------------------
void UniqueTaskManager::yield(TaskState ts)
{
   UniqueTaskExecutor::yieldCurrentTask(ts);
}
// -------------------------------------------------------------------------------------
void UniqueTaskManager::blockingIo(IoRequestType type, char* data, s64 addr, u64 len)
{
   UserIoCallback cb;
   cb.callback = [](IoBaseRequest* req) {
      auto this_task = reinterpret_cast<UniqueTask*>(req->user.user_data2.val.ptr);
      auto this_executor = reinterpret_cast<UniqueTaskExecutor*>(req->user.user_data3.val.ptr);
      // std::cout << "completion addr: " << req->addr <<  std::endl << std::flush;
      assert(this_executor->id() == req->user.user_data.val.s);  //

      UniqueTaskExecutor::UniqueTaskPtr restored(this_task);

      this_executor->moveReady(std::move(restored));
      // TODO maybe push Task to top.
   };
   cb.user_data.val.s = UniqueTaskExecutor::localExec().id();
   cb.user_data3.val.ptr = &UniqueTaskExecutor::localExec();
   cb.user_data2.val.ptr = UniqueTaskExecutor::localExec().getCurrentTaskOwnership();

   UniqueTaskExecutor::localExec().ioChannel.push(type, data, addr, len, cb);
   // UniqueTaskExecutor::localExec().ioChannel.submit();
   UniqueTaskExecutor::yieldRunningTask((UniqueTask*)cb.user_data2.val.ptr, TaskState::WaitIo);
}
// -------------------------------------------------------------------------------------
UniqueTask& UniqueTaskManager::this_task()
{
   return UniqueTaskExecutor::localExec().currentTask();
}
void UniqueTaskManager::set_current_task_lock(YieldLock& lock)
{
   UniqueTaskExecutor::localExec().currentTask().lock = &lock;
}
// -------------------------------------------------------------------------------------
// other
// -------------------------------------------------------------------------------------
UniqueTaskExecutor& UniqueTaskManager::getExec(int id)
{
   return *execs[id];
}
void UniqueTaskManager::sendTask(int to, TaskFunction taskFun)
{
   printf("sending task to %i\n", to);
   auto taskFun1 = new TaskFunction(taskFun);
   UniqueTaskExecutor::localExec().sendMessage(
       to,
       [](void*, uintptr_t taskFun) {
          auto t = reinterpret_cast<TaskFunction*>(taskFun);
          // ensure(TaskManager::instance().exclusiveThreads.find(this_task::exec().id()) == TaskManager::instance().exclusiveThreads.end());
          // createTask(taskFun1);
          UniqueTaskExecutor::localExec().pushTask(t);
       },
       reinterpret_cast<uint64_t>(taskFun1));
}
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
int UniqueTaskManager::size() const
{
   return execs.size();
}
// -------------------------------------------------------------------------------------
}  // namespace mean
