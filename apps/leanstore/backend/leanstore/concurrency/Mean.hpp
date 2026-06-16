#pragma once
// -------------------------------------------------------------------------------------
#include "leanstore/utils/BlockedRange.hpp"
#include "leanstore/concurrency/batch/Task.hpp"
#include "leanstore/concurrency/utils/YieldLock.hpp"
#include "leanstore/io/IoInterface.hpp"
// -------------------------------------------------------------------------------------
#include <functional>
#include <string>
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
#if !defined(MEAN_USE_TASKING) && !defined(MEAN_USE_THREADING) && !defined(MEAN_USE_JOBBING) && !defined(MEAN_USE_DEFAULT_THREADING)
#define MEAN_USE_TASKING
#endif

#if defined(MEAN_USE_THREADING) || defined(MEAN_USE_DEFAULT_THREADING)
using mmutex = std::mutex;
#elif defined(MEAN_USE_JOBBING) && defined(NDEBUG)
using mmutex = lockfree::mutex;
#elif defined(MEAN_USE_JOBBING)
using mmutex = lockfree::mutex; // DebugLock;
#else
using mmutex = YieldLock;
#endif

using TaskFunction = std::function<void()>;  // std::add_pointer_t<void()>;
// -------------------------------------------------------------------------------------
namespace env
{
void init(int workerThreads, int exclusiveThreads, IoOptions ioOptions, int threadAffinityOffset = 0);
// ExecEnv& instance();
void start(TaskFunction fun);
void shutdown();
void join();
void sleepAll(float sleep);
int workerCount();
void adjustWorkerCount(int workerThreads);
void registerPageProvider(void* bm_ptr, u64 partitions_count);
std::string printCountersHeader();
std::string printCounters(int te_id);
template <typename impl>
impl& implementation();  // for internal use only
}  // namespace env
// -------------------------------------------------------------------------------------
namespace exec
{
// void* exec();
IoChannel& ioChannel();
int getId();
}  // namespace exec
// -------------------------------------------------------------------------------------
namespace task
{
void registerExclusiveThread(std::string name, int id, TaskFunction fun);
void parallelFor(BlockedRange bb, TaskFunction fun, int tasks, s64 granularity = -1, bool rate_active=false);
void scheduleTaskSync(TaskFunction fun);
// -------------------------------------------------------------------------------------
void yield(TaskState ts = TaskState::Ready);
// True if the calling thread is currently running inside a coroutine Task.
// The page-provider cycle runs directly in the executor loop (no Task), so it
// must not yield; callers use this to fall back to inline polling instead.
bool inTask();
void read(char* data, s64 addr, u64 len);
void write(char* data, s64 addr, u64 len);
void set_current_task_lock(YieldLock& lock);
}  // namespace task
// -------------------------------------------------------------------------------------
}  // namespace mean
