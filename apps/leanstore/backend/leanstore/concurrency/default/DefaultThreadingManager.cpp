// -------------------------------------------------------------------------------------
#include "DefaultThreadingManager.hpp"
#include "leanstore/concurrency/batch/Task.hpp"
#include "leanstore/concurrency/utils/SharedConfig.hpp"
#include "leanstore/io/IoInterface.hpp"
// LibaioImpl not available in OSv kernel build
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"
// -------------------------------------------------------------------------------------
#include <pthread.h>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include "leanstore/sync-primitives/JumpMU.hpp"

#include <osv/leanstore_debug.hh>
#include <sstream>
#include <stdexcept>
#include <string>

// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
DefaultThreadingManager::~DefaultThreadingManager()
{
   shutdown();
}
// -------------------------------------------------------------------------------------
// env
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::init(int workers_count, int exclusiveThreads, IoOptions ioOptions, [[maybe_unused]] int threadAffinityOffset)
{
#ifdef USE_SAME_THREAD
   exclusiveThreads = workers_count;
#endif

#ifdef USE_THREAD_POOL
   total_threads_count = (workers_count * FLAGS_worker_per_threads) + exclusiveThreads;
#else
   total_threads_count = exclusiveThreads;
#endif
   max_exclusive_threads = exclusiveThreads;
   ensure(max_exclusive_threads > 0, "in threading mode there must be at least one pp thread. Be sure to not use --nopp flag.");
   IoInterface::initInstance(ioOptions);
   ensure(total_threads_count < MAX_WORKER_THREADS);

   // Initialize exclusive threads (unchanged)
   for (int t_i = 0; t_i < exclusiveThreads; t_i++) {
      auto thread = std::make_unique<ThreadWithJump>(
          [&, t_i]() {
             std::string name = std::to_string(t_i);
             leanstore::CPUCounters::registerThread(name, false);
             workers[t_i] = new leanstore::cr::Worker(t_i, workers, workers_count);
             leanstore::cr::Worker::tls_ptr = workers[t_i];
             running_threads++;
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
             running_threads--;
          },
          "w_" + std::to_string(t_i), t_i);
      thread->setCpuAffinityBeforeStart(t_i);
      thread->setNameBeforeStart("exclusive_" + std::to_string(t_i));
      exclusive_threads.push_back(std::move(thread));
      exclusive_threads.back()->start();
   }

#ifdef USE_THREAD_POOL
   // Initialize per-core data structures
   worker_threads_per_core.resize(workers_count);

   // Create mutexes and CVs using unique_ptr
   for (int w_i = 0; w_i < workers_count; w_i++) {
      thread_pool_heads.push_back(std::make_unique<std::atomic<ThreadWithJump*>>(nullptr));
      threadPoolMutexes.push_back(std::make_unique<std::mutex>());
      threadPoolCVs.push_back(std::make_unique<std::condition_variable>());
   }

   // Create thread pool for each core
   for (int w_i = 0; w_i < workers_count; w_i++) {
      for (size_t t_i = 0; t_i < FLAGS_worker_per_threads; t_i++) {
         auto thread = std::make_unique<ThreadWithJump>(
             [&, w_i, t_i]() {
                std::string name = "core_" + std::to_string(w_i) + "_thread_" + std::to_string(t_i);
                leanstore::CPUCounters::registerThread(name, false);

                workers[w_i] = new leanstore::cr::Worker(w_i, workers, workers_count);
                leanstore::cr::Worker::tls_ptr = workers[w_i];

                running_threads++;
                while (ThreadBase::this_thread().keepRunning()) {
                   auto* this_thread = static_cast<ThreadWithJump*>(&ThreadBase::this_thread());
                   auto& meta = this_thread->meta;
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

                   // Return to the pool for this specific core
                   ThreadWithJump* prev_top;
                   do {
                      prev_top = thread_pool_heads[w_i].get()->load();
                      this_thread->next = prev_top;
                   } while (!thread_pool_heads[w_i]->compare_exchange_weak(prev_top, this_thread));

                   if (prev_top == nullptr) {
                      std::unique_lock<std::mutex> lock(*threadPoolMutexes[w_i]);
                      threadPoolCVs[w_i]->notify_one();
                   }
                }
                running_threads--;
             },
             "w_" + std::to_string(w_i) + "_" + std::to_string(t_i), w_i * FLAGS_worker_per_threads + t_i);

         // Set CPU affinity for this thread to its corresponding core
#ifdef USE_SAME_THREAD
         thread->setCpuAffinityBeforeStart(w_i);
#else
         thread->setCpuAffinityBeforeStart(w_i + exclusiveThreads);
#endif
         thread->setNameBeforeStart("worker_core_" + std::to_string(w_i) + "_thread_" + std::to_string(t_i));

         worker_threads_per_core[w_i].push_back(std::move(thread));
         worker_threads_per_core[w_i].back()->start();

         // Add to the pool head for this core
         auto* thread_head = worker_threads_per_core[w_i].back().get();
         thread_head->next = thread_pool_heads[w_i]->load();
         thread_pool_heads[w_i]->store(thread_head);
      }
   }
#else
   // Non-thread-pool version unchanged
   for (size_t t_i = 0; t_i < FLAGS_worker_per_threads; t_i++) {
      auto thread_data_obj = std::make_unique<ThreadData>(&thread_data_pool_head, &threadDataPoolMutex, &threadDataPoolCV);
      thread_data.push_back(std::move(thread_data_obj));

      auto* thread_data_head = thread_data.back().get();
      thread_data_head->next = thread_data_pool_head.load();
      thread_data_pool_head.store(thread_data_head);
   }
#endif
   while (running_threads < total_threads_count) {
   }
}

// -------------------------------------------------------------------------------------
void DefaultThreadingManager::start(TaskFunction taskFun)
{
   // all_threads[max_exclusive_threads]->sendTask(taskFun);
   taskFun();
}
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::shutdown()
{
   for (auto& exe : exclusive_threads) {
      exe->shutdown();
   }
   IoInterface::instance().~RaidEnvironment();
}
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::join()
{
   for (auto& exe : exclusive_threads) {
      exe->join();
   }
}
// -------------------------------------------------------------------------------------
std::string DefaultThreadingManager::stats()
{
   /*
   std::stringstream ss;
   for (auto& exe: execs) {
           ss << exe->id()  << ": "<<  exe->getName() << " ";
           exe->counters.printCounters(ss);
           exe->counters.reset();
           ss << "\t";
           exe->ioChannel.printCounters(ss);
           ss << std::endl;
   }
   ss << std::endl;
   return ss.str();
   */
   return "";
}
void DefaultThreadingManager::adjustWorkerCount(int workerThreads) {}
std::string DefaultThreadingManager::printCountersHeader()
{
   return "a";
}
std::string DefaultThreadingManager::printCounters(int te_id)
{
   return "a";
}
// -------------------------------------------------------------------------------------
// exec
// -------------------------------------------------------------------------------------
int DefaultThreadingManager::execId()
{
   return 0;
}
// -------------------------------------------------------------------------------------
IoChannel& DefaultThreadingManager::execIoChannel()
{
   // int this_id = 0;
   // TODO check if in exclusive thread
#ifdef LEANSTORE_INCLUDE_OSV
   return IoInterface::instance().getIoChannel(leanstore_osv_debug::get_cpu_id());
#else
   return IoInterface::instance().getIoChannel(sched_getcpu());
#endif
}
// -------------------------------------------------------------------------------------
// task
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::registerExclusiveThread(std::string name, int, TaskFunction taskFun)
{
   int id = exclusiveThreadCounter++;
   auto& ex = *exclusive_threads[id];
   ex.setNameBeforeStart(name);
   ex.sendTask(taskFun);
}
void DefaultThreadingManager::registerPageProvider(void* bf_ptr, int partitions_count)
{
   auto buffer_manager = static_cast<leanstore::storage::BufferManager*>(bf_ptr);
   for (int t_i = 0; t_i < partitions_count; t_i++) {
      printf("register pp thread\n");
      registerExclusiveThread("pp", t_i, [buffer_manager, t_i, this]() {
         auto& iochannel = execIoChannel();
         while (true) {
            buffer_manager->pageProviderCycle(t_i);
            iochannel.submit();
            iochannel.poll();
            std::this_thread::yield();
         }
      });
   }
}

#ifndef USE_THREAD_POOL
void* threadFunction(void* arg)
{
   ThreadData* data = static_cast<ThreadData*>(arg);

   // Thread starts here already pinned to core 1
   jumpmu::thread_local_jumpmu_ctx = &data->ctx;
   jumpmu::thread_local_jumpmu_ctx->tx_start_time = data->timestamp;
   // Execute the function for this specific id
   (*data->fun)(data->id, *data->cancelable);

   // Return token to bucket
   {
      ThreadData* prev_top;
      do {
         prev_top = data->head_pointer->load();
         data->next = prev_top;
      } while (!(*data->head_pointer).compare_exchange_weak(prev_top, data));

      if (prev_top == nullptr) {
         std::unique_lock<std::mutex> lock(*data->threadDataPoolMutex);
         (*data->threadDataPoolCV).notify_one();
      }
   }

   return nullptr;
};
#endif

void DefaultThreadingManager::parallelFor(BlockedRange bb, TaskFunction fun, const int tasks, s64 bbgranularity, bool rate_active)
{
   int num_cores = FLAGS_worker_threads;

#ifdef LEANSTORE_INCLUDE_OSV
   volatile SharedConfig* config_ = SharedConfig::get_config();
   ensure(config_);
#else
   volatile SharedConfig* config_ = SharedConfig::get_config_from_file("/dev/shm/myshm");
   ensure(config_);
#endif

   // Divide work among cores
   std::vector<std::thread> workload_generators;

   for (int core_id = 0; core_id < num_cores; core_id++) {
      // Create workload generation thread for this core
      workload_generators.emplace_back([=, &fun]() {
         jumpmu::thread_local_jumpmu_ctx = new jumpmu::JumpMUContext();

         // Set CPU affinity for this workload generator to its core
         cpu_set_t cpuset;
         CPU_ZERO(&cpuset);
#ifdef USE_SAME_THREAD
         CPU_SET(core_id, &cpuset);
#else
         CPU_SET(core_id + max_exclusive_threads, &cpuset);
#endif
         pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

         // Set scheduling parameters
#ifndef LEANSTORE_INCLUDE_OSV
         pthread_t thread = pthread_self();
         struct sched_param param;
         param.sched_priority = 10;
         int ressched = pthread_setschedparam(thread, SCHED_FIFO, &param);
         if (ressched != 0) {
            perror("pthread_setschedparam failed");
         }
#else
         leanstore_osv_debug::set_priority(0.1);
#endif

         // Rate limiting setup
         int timeCheck = 0;
         auto startTime = mean::getSeconds();
         uint64_t prev_config_version = 0;

         auto nextStartTime = mean::readTSC();

         const float rate = FLAGS_tx_rate / (FLAGS_worker_threads);
         // std::random_device is disabled in OSv's slim libc++; seed deterministically.
         std::mt19937 gen(0xC0FFEE);
         std::exponential_distribution<> expDist(rate);

         uint64_t cycles = 0;

         // Process work assigned to this core
         while (true) {
            // if (cycles++ % 64 == 0 and prev_config_version < config_->version) {
            //    expDist = std::exponential_distribution<double>(config_->freq / (FLAGS_worker_threads));
            //    prev_config_version = config_->version;
            // }
            if ((timeCheck++ % 64 == 0 && mean::getSeconds() - startTime > FLAGS_run_for_seconds)) {
               break;
            }
#ifdef NEW_JUMPMU

#ifdef USE_THREAD_POOL
            // Wait for available thread in this core's pool
            if (thread_pool_heads[core_id]->load() == nullptr) {
               std::unique_lock<std::mutex> lock(*threadPoolMutexes[core_id]);
               threadPoolCVs[core_id]->wait(lock, [=] { return thread_pool_heads[core_id]->load() != nullptr; });
            }

            ThreadWithJump* old_top = thread_pool_heads[core_id]->load();

            while (old_top && !thread_pool_heads[core_id]->compare_exchange_weak(old_top, old_top->next)) {
               // CAS failed, retry
            }

            assert(old_top);
            assert(old_top->meta.job_set == false);
            auto txStartTime = jumpmu::thread_local_jumpmu.tx_start_time;

            old_top->sendTask([=, &fun]() {
               jumpmu::thread_local_jumpmu.tx_start_time = txStartTime;
               fun();
            });
#endif

            // Rate limiting
            while (true) {
               auto now = mean::readTSC();
               if (FLAGS_tx_rate == 0 || !rate_active)
                  break;
               if (now >= nextStartTime) {
                  if (mean::tscDifferenceS(now, jumpmu::thread_local_jumpmu.tx_start_time) > 1) {
                     nextStartTime = now;
                     std::cout << "reset start time on core " << core_id << std::endl;
                  }
                  auto d = expDist(gen);
                  jumpmu::thread_local_jumpmu.tx_start_time = nextStartTime;
                  nextStartTime += mean::nsToTSC(d * 1e9);
                  break;
               }
            }
#endif
         }
      });
   }

   // Wait for all workload generators to complete
   for (auto& wg_thread : workload_generators) {
      wg_thread.join();
   }
}
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::scheduleTaskSync(TaskFunction fun)
{
   jumpmu::thread_local_jumpmu_ctx = new jumpmu::JumpMUContext();
   fun();
   delete jumpmu::thread_local_jumpmu_ctx;
}
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::yield([[maybe_unused]] TaskState ts)
{
// do nothing?
#ifdef IS_LINUX
   std::this_thread::yield();
#else
// leanstore_osv_debug::yield();
#endif
}
// -------------------------------------------------------------------------------------
void DefaultThreadingManager::blockingIo(IoRequestType type, char* data, s64 addr, u64 len)
{
   // this is only relevant for the OSv runs / in Linux we can only use
   // pread because all the waits are not possible in Linux
   leanstore_osv_debug::Waiter waiter{};

   UserIoCallback cb;
   cb.callback = [](IoBaseRequest* req) {
      leanstore_osv_debug::Waiter* waiter = (leanstore_osv_debug::Waiter*)(req->user.user_data.val.ptr);
      {
         waiter->wake();
      }
   };
   cb.user_data.val.ptr = &waiter;

   assert(type == IoRequestType::Read);
   execIoChannel().push(type, data, addr, len, cb);

   auto start = mean::readTSC();
   {
      waiter.wait();
   }
}
Task& DefaultThreadingManager::this_task()
{
   throw std::logic_error("cannot be called when running with threads");
}
void DefaultThreadingManager::set_current_task_lock(YieldLock& lock)
{
   return;
}
// -------------------------------------------------------------------------------------
// other
// -------------------------------------------------------------------------------------
int DefaultThreadingManager::workerCount()
{
   return total_threads_count - max_exclusive_threads;
}
// -------------------------------------------------------------------------------------
}  // namespace mean
