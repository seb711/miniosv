// -------------------------------------------------------------------------------------
#include "Mean.hpp"
#include <libaio.h>
// -------------------------------------------------------------------------------------
#include "batch/TaskManager.hpp"
#include "default/DefaultThreadingManager.hpp"
// -------------------------------------------------------------------------------------
#include <mutex>
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
// defaults set in hpp
#ifdef IS_LINUX
#pragma message ("Compiling with IS_LINUX enabled")
#endif

#ifdef MEAN_USE_DEFAULT_THREADING
#pragma message ("Compiling with MEAN_USE_THREADING enabled")
using ExecEnv = DefaultThreadingManager;
#elif defined(MEAN_USE_TASKING)
#pragma message ("Compiling with MEAN_USE_TASKING enabled")
using ExecEnv = TaskManager;
#elif defined(MEAN_USE_JOBBING)
#pragma message ("Compiling with MEAN_USE_JOBBING enabled")
using ExecEnv = OsvJobManager;
#endif
// -------------------------------------------------------------------------------------
namespace env
{
ExecEnv _instance;
void init(int workerThreads, int exclusiveThreads, mean::IoOptions ioOptions, int threadAffinityOffset)
{
   _instance.init(workerThreads, exclusiveThreads, ioOptions, threadAffinityOffset);
}
// -------------------------------------------------------------------------------------
void start(TaskFunction fun)
{
   _instance.start(fun);
}
void shutdown()
{
   _instance.shutdown();
}
void join()
{
   _instance.join();
}
void sleepAll(float sleep)
{
   _instance.sleepAll(sleep);
}
int workerCount() {
   return _instance.workerCount();
}
void adjustWorkerCount(int workerThreads) {
   _instance.adjustWorkerCount(workerThreads);
}
void registerPageProvider(void* bm_ptr, u64 partitions_count) {
   env::_instance.registerPageProvider(bm_ptr, partitions_count);
}
std::string printCountersHeader()
{
   return _instance.printCountersHeader();
}
std::string printCounters(int te_id)
{
   return _instance.printCounters(te_id);
}
/*
int fd() {
        return _instance.getFd();
}
*/
template <>
ExecEnv& implementation<ExecEnv>()
{
   return _instance;
}
}  // namespace env
namespace exec
{
/*
void* exec() {
        return env::_instance.localExec();
}
*/
int getId()
{
   return env::_instance.execId();
}
IoChannel& ioChannel()
{
   return env::_instance.execIoChannel();
}
}  // namespace exec
namespace task
{
void registerExclusiveThread(std::string name, int id, TaskFunction fun)
{
   env::_instance.registerExclusiveThread(name, id, fun);
}
void parallelFor(BlockedRange bb, TaskFunction fun, int tasks, s64 granularity, bool rate_active)
{
   env::_instance.parallelFor(bb, fun, tasks, granularity, rate_active);
}
void scheduleTaskSync(TaskFunction fun)
{
   env::_instance.scheduleTaskSync(fun);
}
void yield(TaskState ts)
{
   env::_instance.yield(ts);
}
bool inTask()
{
   return env::_instance.inTask();
}
void read(char* data, s64 addr, u64 len)
{
   env::_instance.blockingIo(IoRequestType::Read, data, addr, len);
}
void write(char* data, s64 addr, u64 len)
{
   env::_instance.blockingIo(IoRequestType::Write, data, addr, len);
}
void set_current_task_lock(YieldLock& lock) {
   return env::_instance.set_current_task_lock(lock);
}
}  // namespace task
}  // namespace mean
