#include "UniqueTaskExecutor.hpp"
#include "Exceptions.hpp"
#include "Time.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/concurrency/unique/UniqueTask.hpp"
#include "leanstore/concurrency/unique/UniqueTaskManager.hpp"
#include "leanstore/concurrency/utils/MessageHandler.hpp"
#include "leanstore/concurrency/utils/ThreadBase.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/profiling/counters/ThreadCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/RandomGenerator.hpp"

#include <boost/context/preallocated.hpp>
#include "boost/context/continuation.hpp"
#include "leanstore/io/impl/OsvImpl.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <thread>

#ifdef USE_INTERRUPTS
// drivers/clockevent.hh (and the clock_event device) only exist on the
// interrupt-driven timer path, which is disabled in this build. It is also gone
// from slim OSv, so only pull it when that path is actually compiled.
#include <drivers/clockevent.hh>
#endif
#include <osv/clock.hh>
#include <osv/sched.hh>
#ifdef __x86_64__
#include "arch/x64/apic.hh"
#endif
#include "arch.hh"
#include "exceptions.hh"
#include "processor.hh"

#define TIMING_WHEEL_SLOT_SIZE 64  // us

namespace mean
{

// ============================================================================
// Thread-Local Variables
// ============================================================================

thread_local uint64_t opentasks = 0;
thread_local TaskContextPool* tl_task_context_pool;
std::array<bool, MAX_CORES> run_task = {false};

UniqueTaskExecutor::PaddedTimestamp UniqueTaskExecutor::timestamps[8];

std::atomic<int> UniqueTaskExecutor::interruptVector = {-1};
std::atomic<int> UniqueTaskExecutor::readyExecutors = {0};

// ============================================================================
// Context Switch Callbacks (CRITICAL - DO NOT MODIFY)
// ============================================================================
// These fcontext callbacks are used during context switches and must
// maintain their exact signature and behavior for boost::context

boost::context::detail::transfer_t UniqueTaskExecutor::store_sink_on_yield(boost::context::detail::transfer_t t)
{
   auto* self = reinterpret_cast<UniqueTaskExecutor*>(t.data);
   self->updateCurrentSink(t.fctx);
   return {nullptr, nullptr};
}

boost::context::detail::transfer_t UniqueTaskExecutor::store_task_on_yield(boost::context::detail::transfer_t t)
{
   auto* self = reinterpret_cast<UniqueTask*>(t.data);
   self->context.this_task_context = t.fctx;
   return {nullptr, nullptr};
}

// ============================================================================
// Task Context Management
// ============================================================================

void UniqueTaskDeleter::operator()(UniqueTask* task) const
{
   opentasks--;

   if (task) {
      UniqueTaskExecutor::localExec().g_task_context_pool->deallocate(task);
   }
   delete task;
}

void UniqueTaskExecutor::initTaskContextPool(size_t capacity)
{
   if (g_task_context_pool) {
      throw std::runtime_error("Pool already initialized");
   }
   g_task_context_pool = new TaskContextPool(capacity);
}

void UniqueTaskExecutor::destroyTaskContextPool()
{
   delete g_task_context_pool;
   g_task_context_pool = nullptr;
}

void UniqueTaskExecutor::initializeTaskContext(UniqueTaskContext& ctx,
                                               void* stack_base,
                                               size_t stack_size,
                                               void (*fn)(boost::context::detail::transfer_t))
{
   ctx.this_task_context = boost::context::detail::make_fcontext(static_cast<char*>(stack_base) + stack_size, stack_size, fn);
}

UniqueTaskExecutor::UniqueTaskPtr UniqueTaskExecutor::createTask(TaskFunction* fun, void (*entry_fn)(boost::context::detail::transfer_t))
{
   if (!g_task_context_pool) {
      throw std::runtime_error("Pool not initialized");
   }

   // auto task = new UniqueTask(fun);
   auto task = g_task_context_pool->allocate(fun);
   initializeTaskContext(task->context, task->context.stack, TaskContextPool::getStackSize(), entry_fn);

   opentasks++;
   return UniqueTaskPtr(task);
}

// ============================================================================
// Task Trampoline (CRITICAL - DO NOT MODIFY)
// ============================================================================
// Entry point for new tasks - handles initialization and cleanup

void UniqueTaskExecutor::trampoline(boost::context::detail::transfer_t t)
{
   auto& self = UniqueTaskExecutor::localExec();
   uint64_t arg = reinterpret_cast<uint64_t>(t.data);
   self.updateCurrentSink(t.fctx);

#ifdef USE_INTERRUPTS
   arch::irq_enable();
#if !defined(USE_PERIODIC_TIMER) && !defined(USE_WATCHDOG)
   clock_event->set(std::chrono::nanoseconds(INTERRUPT_TIME));
#endif
#endif

   (*self._currentTask->fun)();

#ifdef USE_WATCHDOG
   timestamps[self.core_id] = 0;
#endif

   {
#if defined(USE_INTERRUPTS)
      arch::irq_disable(); 
      sched::current_cpu->arch.set_ist_entry(3, (char*)localExec()._sinkInterruptStack, UniqueTaskContext::INTERRUPT_STACK_SIZE);
#endif
      self._currentTask->state = TaskState::Done;
      // self.latencyTracker.record(mean::tscDifferenceUs(mean::readTSC(), jumpmu::thread_local_jumpmu_ctx->tx_start_time), DESIRED_P99);
      jumpmu::thread_local_jumpmu_ctx = &self.defaultExecutorContext;
      boost::context::detail::jump_fcontext(self.getCurrentSink(), nullptr);
   }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

UniqueTaskExecutor::UniqueTaskExecutor(MessageHandler& msg, IoChannel& io_channel, DummyNIC& nic, int executor_id)
    : ThreadBase("te_", executor_id), ioChannel(io_channel), nic(nic), messageHandler(msg), systemState()
{
   core_id = executor_id;
   jumpmu::thread_local_jumpmu_ctx = &defaultExecutorContext;
   initTaskContextPool(FLAGS_worker_tasks);
   tl_task_context_pool = g_task_context_pool;

   // setupInterruptHandling();

   printf("create unique task executor %i\n", executor_id);
   _sinkInterruptStack = static_cast<char*>(aligned_alloc(UniqueTaskContext::INTERRUPT_STACK_SIZE, 64));
}

UniqueTaskExecutor::~UniqueTaskExecutor()
{
   destroyTaskContextPool();
}

void UniqueTaskExecutor::setupInterruptVector()
{
   // this is the solution for now. i guess you could
   // think about a better solution but

   // 1. rebuild the apic table to make it core local (-> probably the best solution); but this would mean that we copy the interrupt table for each
   // smp core
   // 2. make one vector per core (problem: this cannot scale infinitely because the interrupt table can only be 256 entries (-> but this would be
   // most probably enough))
   // 3. somehow setup the vector in the threading manager and not the executor (-> but this makes single-responsiblity a bit blurry)

#ifdef USE_INTERRUPTS
   int exp = -1;
   if (interruptVector.compare_exchange_strong(exp, 1)) {
      arch::irq_disable();

      auto timer_handler = []() {
         if (!run_task[sched::current_cpu->id]) return; 

         if (!sched::thread::current()->is_app()) return;  

         ensure(jumpmu::thread_local_jumpmu_ctx->CANARY == 0xFEFE and jumpmu::thread_local_jumpmu_ctx->CANARY2 == 0xbaba);

         if (jumpmu::thread_local_jumpmu_ctx->lock_counter > 0) {
            // leanstore::WorkerCounters::myCounters().time_counter_2++;
            return;
         }

         ensure(!arch::irq_enabled());

         auto& self = UniqueTaskExecutor::localExec();

         if (self._currentTask != nullptr && self._currentSink != nullptr) {

            if (!self._currentTask->context.interrupted) {
               self._currentTask->context.interrupted = true; 
               return; 
            }

#ifdef USE_WATCHDOG
            timestamps[self.core_id] = 0;
#endif
            sched::current_cpu->arch.set_ist_entry(3, (char*)UniqueTaskExecutor::localExec()._sinkInterruptStack, UniqueTaskContext::INTERRUPT_STACK_SIZE);

            assert(self._currentSink != nullptr);
            assert(self._currentTask != nullptr);

            // leanstore_osv_debug::trace_interrupted(&self._currentTask, jumpmu::thread_local_jumpmu_ctx->lock_counter);
            if (self._currentTask == nullptr) {
               abort(); 
            }

            self._currentTask->state = TaskState::Preempted;  

            boost::context::detail::ontop_fcontext(self.getCurrentSink(), (void*)self._currentTask, UniqueTaskExecutor::store_task_on_yield);

#if !defined(USE_PERIODIC_TIMER) && !defined(USE_WATCHDOG)
            clock_event->set(std::chrono::nanoseconds(INTERRUPT_TIME));
#endif
            return;
         }
      };

      auto vector = idt.register_handler(timer_handler);
      interruptVector.store(vector);
   }

   while (interruptVector.load() <= 1) {
   }
#endif
}

void UniqueTaskExecutor::setupInterruptHandling()
{
#ifdef USE_INTERRUPTS
   UniqueTaskExecutor::setupInterruptVector();

   clock_event->reset_vector(interruptVector.load());
#ifdef USE_PERIODIC_TIMER
   // printf("setup clock\n");
   clock_event->set_periodic(true);
   clock_event->set(std::chrono::nanoseconds(INTERRUPT_TIME));
#endif

#if defined(USE_WATCHDOG) && !defined(USE_PERIODIC_TIMER)
   // this block is a bit odd and we could probably do better than this
   clock_event->set_periodic(true);
   clock_event->disable();
#endif
   arch::irq_enable();
#endif
}

// ============================================================================
// Task Execution (CRITICAL - Handle with care)
// ============================================================================

TaskState UniqueTaskExecutor::runCurrentTask()
{
   auto task = _currentTask;
   jumpmu::thread_local_jumpmu_ctx = &_currentTask->context.jumpmuctx;

#ifdef USE_INTERRUPTS
   arch::irq_disable();
   run_task[sched::current_cpu->id] = true;
#ifdef USE_WATCHDOG
   timestamps[this->core_id] = processor::rdtsc();
#endif
   sched::current_cpu->arch.set_ist_entry(3, (char*)task->context.interrupt_stack, UniqueTaskContext::INTERRUPT_STACK_SIZE);
#endif

   assert(_currentTask->context.isStackValid()); 

   if (!_currentTask->context.init) {
      _currentTask->context.init = true;
      boost::context::detail::jump_fcontext(_currentTask->context.this_task_context, (void*)_currentTask->arg);
   } else {
      boost::context::detail::ontop_fcontext(_currentTask->context.this_task_context, this, this->store_sink_on_yield);
   }

#ifdef USE_INTERRUPTS
   run_task[sched::current_cpu->id] = false;
#ifdef USE_WATCHDOG
   timestamps[this->core_id] = 0;
#endif
   arch::irq_enable();
#endif
   jumpmu::thread_local_jumpmu_ctx = &defaultExecutorContext;

   return task->getState();
}

// ============================================================================
// Task Yielding (CRITICAL - DO NOT MODIFY)
// ============================================================================

void UniqueTaskExecutor::yieldCurrentTask(TaskState state)
{
   assert(arch::irq_enabled());

#ifdef USE_INTERRUPTS
   arch::irq_disable();
#if !defined(USE_PERIODIC_TIMER) && !defined(USE_WATCHDOG)
   clock_event->disable();
#endif
#endif

   UniqueTask& task = currentTask();
   task.state = state;

   auto& local_executor = UniqueTaskExecutor::localExec();
   auto sink_fcontext = local_executor.getCurrentSink();

#ifdef USE_INTERRUPTS
   sched::current_cpu->arch.set_ist_entry(3, (char*)UniqueTaskExecutor::localExec()._sinkInterruptStack, UniqueTaskContext::INTERRUPT_STACK_SIZE);
#endif

   boost::context::detail::ontop_fcontext(sink_fcontext, (void*)&task, local_executor.store_task_on_yield);

#if defined(USE_INTERRUPTS)
#if !defined(USE_PERIODIC_TIMER) && !defined(USE_WATCHDOG)
   clock_event->set(std::chrono::nanoseconds(INTERRUPT_TIME));
#endif
   arch::irq_enable();
#endif
}

void UniqueTaskExecutor::yieldRunningTask(UniqueTask* task, TaskState state)
{
   assert(arch::irq_enabled());
   auto& local_executor = UniqueTaskExecutor::localExec();

#ifdef USE_INTERRUPTS
   arch::irq_disable();
#if !defined(USE_PERIODIC_TIMER) && !defined(USE_WATCHDOG)
   clock_event->disable();
#endif
#endif

#ifdef USE_WATCHDOG
   timestamps[local_executor.core_id] = 0;
#endif

   task->state = state;

   auto sink_fcontext = local_executor.getCurrentSink();

#ifdef USE_INTERRUPTS
   sched::current_cpu->arch.set_ist_entry(3, (char*)UniqueTaskExecutor::localExec()._sinkInterruptStack, UniqueTaskContext::INTERRUPT_STACK_SIZE);
#endif

   boost::context::detail::ontop_fcontext(sink_fcontext, (void*)task, local_executor.store_task_on_yield);

#if defined(USE_INTERRUPTS)
#if !defined(USE_PERIODIC_TIMER) && !defined(USE_WATCHDOG)
   clock_event->set(std::chrono::nanoseconds(INTERRUPT_TIME));
#endif
   arch::irq_enable();
#endif
}

// ============================================================================
// Background Work
// ============================================================================
void UniqueTaskExecutor::setupBackgroundWork()
{
   std::unique_ptr<UniqueBackgroundWorkMeta> meta1 = std::make_unique<UniqueBackgroundWorkMeta>();
   auto* meta1_ptr = meta1.get();
   std::function<void(void)>* io_poller_fn = new std::function<void(void)>([this, meta1_ptr]() {
      while (true) {
         auto work_done = ioChannel.poll();
         meta1_ptr->last_work = work_done;
         yieldCurrentTask(TaskState::Ready);
      }
   });
   UniqueTaskPtr bgctx1 = createTask(io_poller_fn, UniqueTaskExecutor::trampoline);
   background_work[1] = std::make_unique<UniqueBackgroundWork>(std::move(bgctx1), TaskState::WaitIo, std::move(meta1));

   // IO SUBMITTER - linked to WaitIo
   std::unique_ptr<UniqueBackgroundWorkMeta> meta2 = std::make_unique<UniqueBackgroundWorkMeta>();
   auto* meta2_ptr = meta2.get();
   std::function<void(void)>* io_submitter_fn = new std::function<void(void)>([this, meta2_ptr]() {
      while (true) {
         auto submitted = ioChannel.submit();
         meta2_ptr->last_work = submitted;
         yieldCurrentTask(TaskState::Ready);
      }
   });
   UniqueTaskPtr bgctx2 = createTask(io_submitter_fn, UniqueTaskExecutor::trampoline);
   background_work[2] = std::make_unique<UniqueBackgroundWork>(std::move(bgctx2), TaskState::WaitIo, std::move(meta2));

   // PAGE PROVIDER - linked to ReadyNoFreePages (placeholder)
   // std::function<uint64_t(void)> page_provider_fn = [this]() -> uint64_t {
   //    return providePages();
   // };
   // background_work[3] = std::make_unique<UniqueBackgroundWork>(page_provider_fn, TaskState::ReadyNoFreePages);

   // NIC - no linked state
   // std::function<uint64_t(void)> dummy_nic_handler_fn = [this]() -> uint64_t { return pollWorkload(); };
   // background_work[4] = std::make_unique<UniqueBackgroundWork>(dummy_nic_handler_fn);

   for (size_t i = 0; i < background_work.size(); ++i) {
      if (background_work[i]) {
         time_wheel[0] |= (1 << i);
      }
   }
}

// handleBackgroundWork (updated)
void UniqueTaskExecutor::handleBackgroundWork()
{
   // Tier 1: Check system state and run linked background tasks
   for (size_t task_id = 0; task_id < background_work.size(); ++task_id) {
      if (!background_work[task_id] || !background_work[task_id]->has_linked_state)
         continue;

      auto* task = background_work[task_id].get();
      if (systemState.get(task->linked_state) > 0) {
         _currentTask = std::move(task->bg_task);
         runCurrentTask();
         task->bg_task = std::move(_currentTask);
         // task->meta->max_work = std::max(task->meta->max_work, task->meta->last_work);
      }
   }

   // Tier 2: Timing wheel tasks
   uint64_t current_tsc = mean::readTSC() / 1000;
   uint64_t current_time_us = current_tsc / 4;
   size_t current_position = (current_time_us / TIMING_WHEEL_SLOT_SIZE) % time_wheel.size();

   uint64_t tiles_to_advance = current_position >= last_background_check ? current_position - last_background_check
                                                                         : time_wheel.size() - last_background_check + current_position;

   uint8_t pending_work = 0;
   for (uint64_t i = 0; i < tiles_to_advance; i++) {
      size_t pos = (last_background_check + i) % time_wheel.size();
      pending_work |= time_wheel[pos];
      time_wheel[pos] = 0;
   }

   last_background_check = current_position;
   if (pending_work == 0)
      return;

   for (size_t task_id = 0; task_id < background_work.size(); ++task_id) {
      if (!(pending_work & (1 << task_id)) || !background_work[task_id])
         continue;

      auto* task = background_work[task_id].get();

      uint64_t start_time = mean::readTSC();
      _currentTask = task->bg_task;
      runCurrentTask();
      task->bg_task = _currentTask;
      uint64_t end_time = mean::readTSC();

      // task->meta->max_work = std::max(task->meta->max_work, task->meta->last_work);
      auto done_work = task->meta->last_work;
      task->meta->timestamp = current_time_us;

      double work_ratio = static_cast<double>(done_work) / task->meta->max_work;
      uint16_t target_frequency = task->meta->cfrequency;

      if (work_ratio > 0.9) {
         target_frequency = static_cast<uint16_t>(task->meta->cfrequency * 0.8);
         if (target_frequency < TIMING_WHEEL_SLOT_SIZE)
            target_frequency = TIMING_WHEEL_SLOT_SIZE;
      } else if (work_ratio < 0.1) {
         target_frequency = static_cast<uint16_t>(task->meta->cfrequency * 1.2);
         if (target_frequency > 5000)
            target_frequency = 5000;
      }

      constexpr double alpha = 0.8;
      task->meta->cfrequency = static_cast<uint16_t>(alpha * target_frequency + (1 - alpha) * task->meta->cfrequency);
      uint64_t tiles_ahead = task->meta->cfrequency / TIMING_WHEEL_SLOT_SIZE;
      size_t next_run_position = (current_position + tiles_ahead) % time_wheel.size();
      time_wheel[next_run_position] |= (1 << task_id);
   }
}
// ============================================================================
// Main Execution Loop
// ============================================================================

int UniqueTaskExecutor::process()
{
   ensure(tasks.size() == 0);
   leanstore::cr::Worker::tls_ptr = this_worker;
   leanstore::CPUCounters::registerThread(std::to_string(id()), false);
   jumpmu::thread_local_jumpmu_ctx = &defaultExecutorContext;
#ifdef USE_BACKGROUND_TASKS
   setupBackgroundWork();
   time_wheel[0] = 0b00001110;  // THIS IS SUPER IMPORTANT AS IT INITS THE FIRST RUN
                                // last_background_check = mean::readTSC();
#endif
   cycle();
   return 0;
}

void UniqueTaskExecutor::cycle()
{
   UniqueTaskExecutor::readyExecutors++;
   u64 cycles = 0;

   random_generator.seed(mean::exec::getId());
   int start = mean::getSeconds();
   int timeCheck = 0;

   while (_keep_running) {
      if (timeCheck++ % 64 == 0 && mean::getSeconds() - start > FLAGS_run_for_seconds) {
         if (--parallel_threads == 0) {
            _currentTask = std::move(originTask);
            runCurrentTask();
         }
      }

      cycles++;
      if (cycles % 1 << 13 == 0) {
         pollMessages();
      }
      if (cycles % 64 == 0) {
         pollWorkload();
      }
#ifndef USE_BACKGROUND_TASKS
      handleSleep();
      if (cycles % 32 == 0) {
         pageProviderCycle();
      }

      submitIo();

      if (cycles % 64 == 0) {
         pollIo();
      }
#else
      handleBackgroundWork();
#endif

#if defined(USE_INTERRUPTS) && defined(USE_WATCHDOG)
      // here you need to check if another thread needs a nudge
      // WE DO NOT SPIN UP ANOTHER THREAD BUT JUST USE THE CURRENT INFRA
      // thread_i controls thread_i+1

      // here fore we first want to check when we have last accesses the other timestamp (because maybe it is not needed yet)
      // if bigger then we want to check the timestamp (cache inval)
      // and send an interrupt if needed (-> that's it)
      if (shouldCheckWatchdog()) {
         uint64_t current_timestamp = mean::readTSC();
         int watchdog_core = (this->core_id + 1) % FLAGS_worker_threads;
         if (UniqueTaskExecutor::timestamps[watchdog_core] > 0 &&
             (current_timestamp - UniqueTaskExecutor::timestamps[watchdog_core]) > INTERRUPT_TIME * 4) {
            processor::apic->ipi(sched::cpus[watchdog_core]->arch.apic_id, interruptVector);
         }
         last_watchdog_access = current_timestamp;
      }
#endif

      int tasks_run = runScheduledTasks();
   }
}

void UniqueTaskExecutor::handleSleep()
{
   if (sleep != 0) {
      float sleep_time = sleep.exchange(0);
      std::cout << "sleep for: " << sleep_time << std::endl;
      std::this_thread::sleep_for(std::chrono::nanoseconds(static_cast<uint64_t>(sleep_time * 1e9)));
   }
}
void UniqueTaskExecutor::pollMessages()
{
   messageHandler.poll(this);
   counters.msgPollCalled++;
}

void UniqueTaskExecutor::pollIo()
{
   ioChannel.poll();
}

void UniqueTaskExecutor::handleIoSubmission(u64 cycles, u64& delay_until_cycle)
{
#ifdef USE_BACKGROUND_TASKS
   constexpr int delay_submit = 0;
#else
   constexpr int delay_submit = 64;
#endif

   if (delay_submit == 0) {
      submitIo();
   } else {
      handleDelayedIoSubmission(cycles, delay_until_cycle, delay_submit);
   }
}

void UniqueTaskExecutor::submitIo()
{
   int submitted = ioChannel.submit();
}

void UniqueTaskExecutor::handleDelayedIoSubmission(u64 cycles, u64& delay_until_cycle, int delay_amount)
{
   if (ioChannel.submitable() > 0) {
      if (delay_until_cycle < cycles) {
         delay_until_cycle = cycles + delay_amount;
      } else if (delay_until_cycle == cycles) {
         submitIo();
      }
   }
}

int UniqueTaskExecutor::pollWorkload()
{
   if (!nic.active)
      return 0;
   if (FLAGS_tx_rate > 0) {
      return pollWorkloadWithRate();
   } else {
      return pollWorkloadWithoutRate();
   }
}

int UniqueTaskExecutor::pollWorkloadWithRate()
{
   nic.sync(); 
   size_t requests = nic.size();

   size_t task_to_do = std::min(requests, FLAGS_worker_tasks - opentasks);

   for (size_t i = 0; i < task_to_do; i++) {
      UniqueTaskPtr task = createTask(&workloadFunction, trampoline);
      task->context.jumpmuctx.tx_start_time = nic.get(i)->timestamp;
      tasks.push_back(std::move(task));
   }

   nic.consume(task_to_do);

   // Drop any requests we couldn't handle — don't let backlog grow
   // if (requests > task_to_do) {
   //    leanstore::WorkerCounters::myCounters().time_counter_2 += requests - task_to_do;
   //    nic.consume(requests - task_to_do);
   // }

   return task_to_do;
}

int UniqueTaskExecutor::pollWorkloadWithoutRate()
{
   if (FLAGS_worker_tasks - opentasks > 16) {
      size_t new_tasks = FLAGS_worker_tasks - opentasks;
      for (size_t i = 0; i < new_tasks; i++) {
         this->pushTask(&workloadFunction);
      }
      return new_tasks;
   }
   return 0;
}

bool UniqueTaskExecutor::shouldCheckWatchdog()
{
   if (interruptVector > 1 && (UniqueTaskExecutor::readyExecutors == FLAGS_worker_threads) &&
       ((mean::readTSC() - last_watchdog_access) > INTERRUPT_TIME * 4)) {  // oh man we should really normalize this
      return true;
   }
   return false;
}

int UniqueTaskExecutor::runScheduledTasks()
{
#ifdef USE_BACKGROUND_TASKS
   int max_tasks_per_cycle = std::min((unsigned long)16, tasks.size() + tasks_io_done.size());
#else
   const int max_tasks_per_cycle = 1;  // std::min((unsigned long)16, tasks.size() + tasks_io_done.size());
#endif
   int tasks_run = 0;

   while (tasks_run < max_tasks_per_cycle && popTask(_currentTask)) {
      counters.tasksRun++;

      if (!tryAcquireTaskLock()) {
         tasks_run++;
         // continue;
         break;
      }

      tasks_run++;
      TaskState state = runCurrentTask();
      handleTaskState(state);
      if (state == TaskState::Done) {
         _currentTask->~UniqueTask(); 
         g_task_context_pool->deallocate(_currentTask);
      }
   }

   leanstore::WorkerCounters::myCounters().time_counter_3++;
   leanstore::WorkerCounters::myCounters().total_time_sum_3 += tasks_run;

   return tasks_run;
}

bool UniqueTaskExecutor::popTask(UniqueTaskPtr& task)
{
   if (tasks_io_done.try_pop(task) || tasks.try_pop(task)) {
      systemState.decrement(task->state);
      return true;
   }
   return false;
}

void UniqueTaskExecutor::handleTaskState(TaskState state)
{
   switch (state) {
      case TaskState::Done:
         counters.tasksCompleted++;
         opentasks--;
         leanstore::ThreadCounters::myCounters().exec_tasks_st_comp++;
         break;

      case TaskState::Waiting:
         systemState.increment(state);
         leanstore::ThreadCounters::myCounters().exec_tasks_st_wait++;
         counters.tasksWaiting++;
         waitingTaskCount++;
         break;

      case TaskState::WaitIo:
         systemState.increment(state);
         leanstore::ThreadCounters::myCounters().exec_tasks_st_wait_io++;
         counters.tasksWaiting++;
         waitingTaskCount++;
         waitIoTaskCount++;
         break;

      case TaskState::Ready:
      case TaskState::ReadyNoFreePages:
      case TaskState::ReadyLock:
      case TaskState::ReadyJumpLock:
      case TaskState::Preempted: 
         systemState.increment(state);
         tasks.push_back(std::move(_currentTask));
         counters.tasksReady++;

         if (state == TaskState::Ready)
            leanstore::ThreadCounters::myCounters().exec_tasks_st_ready++;
         else if (state == TaskState::ReadyNoFreePages)
            leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_mem++;
         else if (state == TaskState::ReadyLock)
            leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_lck++;
         else
            leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_jumplck++;
         break;

      default:
         throw std::logic_error("Invalid task state");
   }

   leanstore::ThreadCounters::myCounters().exec_tasks_run++;
}

bool UniqueTaskExecutor::tryAcquireTaskLock()
{
   if (_currentTask->state == TaskState::ReadyLock) {
      if (!_currentTask->lock->try_lock()) {
         systemState.increment(TaskState::ReadyLock);
         tasks.push_back(_currentTask);
         leanstore::WorkerCounters::myCounters().time_counter_0++;
         leanstore::ThreadCounters::myCounters().exec_tasks_st_ready_lckskip++;
         return false;
      }
      _currentTask->lock->unlock();
   }
   return true;
}

// ============================================================================
// Page Provider Integration
// ============================================================================

void UniqueTaskExecutor::registerPageProvider(void* buffer_manager_ptr, u64 partition)
{
   printf("register page provider %lu\n", partition);

   this->partition_id = partition;
   this->buffer_manager = static_cast<BufferManager*>(buffer_manager_ptr);

   ensure(buffer_manager->cooling_partitions_count > partition);
   buffer_manager->cooling_partitions[partition].state.debug_thread = mean::exec::getId();

   // PAGE PROVIDER
   std::unique_ptr<UniqueBackgroundWorkMeta> meta3 = std::make_unique<UniqueBackgroundWorkMeta>();
   auto meta3_ptr = meta3.get();
   std::function<void(void)>* page_provider_fn1 = new std::function<void(void)>([this, meta3_ptr]() {
      CoolingPartition& partition = buffer_manager->cooling_partitions[partition_id];

      while (true) {
         if (buffer_manager && partition_id >= 0) {

            // Phase 1:   Unswizzle hot pages and move them to cooling stage
            //            If there are less than 64 free pages or (to be) free pages
            uint64_t unswizzledHotPages = 0;
            if (buffer_manager->calculateCoolingDeficit(partition) > 64) {
               assert(arch::irq_enabled());
               unswizzledHotPages = buffer_manager->unswizzleHotPages(partition, 64, partition_id);
            }
            meta3_ptr->last_work = unswizzledHotPages;
         }
         yieldCurrentTask(TaskState::Ready);
      }
      return 0;
   });
   UniqueTaskPtr bgctx3 = createTask(page_provider_fn1, UniqueTaskExecutor::trampoline);
   std::unique_ptr<UniqueBackgroundWork> page_provider_bg =
       std::make_unique<UniqueBackgroundWork>(std::move(bgctx3), TaskState::ReadyNoCoolPages, std::move(meta3));
   background_work[3] = std::move(page_provider_bg);

   std::unique_ptr<UniqueBackgroundWorkMeta> meta4 = std::make_unique<UniqueBackgroundWorkMeta>();
   auto meta4_ptr = meta4.get();
   std::function<void(void)>* page_provider_fn2 = new std::function<void(void)>([this, meta4_ptr]() {
      CoolingPartition& partition = buffer_manager->cooling_partitions[partition_id];

      while (true) {

         if (buffer_manager && partition_id >= 0) {
            if (partition.cooling_bfs_counter == 0) {
               yieldCurrentTask(TaskState::ReadyNoCoolPages);
            }

            s64 pages_to_process = buffer_manager->calculateFreeBufferDeficit(partition);
            int nonDirtyEvictions = 0;

            if (pages_to_process > 0) {
               nonDirtyEvictions = buffer_manager->processCoolingQueue(partition, pages_to_process, partition.state.freed_bfs_batch);
            } else {
               yieldCurrentTask(TaskState::ReadyNoCoolPages);
            }

            // Phase 3: Complete I/O operations and evict clean pages
            buffer_manager->evictCompletedIoPages(partition, partition.state.freed_bfs_batch);

            // Attach freed buffer frames to partition's free list
            u64 freed = partition.state.freed_bfs_batch.size();
            if (freed) {
               partition.pushFreeList();
            }


            meta4_ptr->last_work = freed + nonDirtyEvictions;
         }
         yieldCurrentTask(TaskState::Ready);
      }
      return 0;
   });
   UniqueTaskPtr bgctx4 = createTask(page_provider_fn2, UniqueTaskExecutor::trampoline);
   std::unique_ptr<UniqueBackgroundWork> page_provider_bg2 =
       std::make_unique<UniqueBackgroundWork>(std::move(bgctx4), TaskState::ReadyNoCoolPages, std::move(meta4));
   background_work[4] = std::move(page_provider_bg2);

   time_wheel[0] |= (1 << 3) | (1 << 4);
}

void UniqueTaskExecutor::pageProviderCycle()
{
   if (buffer_manager && partition_id >= 0) {
      buffer_manager->pageProviderCycle(partition_id);
   }
}

// ============================================================================
// Task Management
// ============================================================================

void UniqueTaskExecutor::pushTask(UniqueTaskPtr task)
{
   tasks.push_back(task);
}

void UniqueTaskExecutor::pushTask(TaskFunction* fun)
{
   UniqueTaskPtr task = createTask(fun, trampoline);
   tasks.push_back(task);
}

void UniqueTaskExecutor::moveReady(UniqueTaskPtr task)
{
   task->state = TaskState::Ready;
   waitingTaskCount--;
   waitIoTaskCount--;
   tasks_io_done.push_back(std::move(task));
}

int UniqueTaskExecutor::taskCount()
{
   return tasks.size();
}

// ============================================================================
// Messaging
// ============================================================================

void UniqueTaskExecutor::sendMessage(int to_id, MessageFunction fun, uintptr_t user_data)
{
   messageHandler.sendMessage(to_id, fun, user_data);
}

// ============================================================================
// Static Access Methods
// ============================================================================

UniqueTaskExecutor& UniqueTaskExecutor::localExec()
{
   return static_cast<UniqueTaskExecutor&>(ThreadBase::this_thread());
}

UniqueTask& UniqueTaskExecutor::currentTask()
{
   auto task = localExec()._currentTask;
   if (!task) {abort(); }
   return *task;
}

UniqueTaskExecutor::UniqueTaskPtr UniqueTaskExecutor::getCurrentTaskOwnership()
{
   return localExec()._currentTask;
}

}  // namespace mean