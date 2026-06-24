// -------------------------------------------------------------------------------------
#include "YieldLock.hpp"
#include "leanstore/concurrency/batch/Task.hpp"
// -------------------------------------------------------------------------------------
#include "Units.hpp"
#include "leanstore/concurrency/Mean.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <mutex>
#include "arch.hh"
#include "leanstore/concurrency/unique/UniqueTaskExecutor.hpp"
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
bool YieldLock::try_lock()
{
   bool b = !_lock.test_and_set(std::memory_order_acquire);
   if (b) {
      // DEBUG: we won the flag -> nobody else may currently believe they hold it. The
      // unlocker decrements dbg_held *before* clearing the flag, so when we observe the
      // flag free and win it, dbg_held must already be 0.
      int prev = dbg_held.fetch_add(1, std::memory_order_acq_rel);
      if (prev != 0) {
         printf("DBG LOCK-EXCL: YieldLock %p acquired but dbg_held was %d (mutual exclusion broken); "
                "acquirer ctx=%p previous holder=%p\n",
                (void*)this, prev, (void*)jumpmu::thread_local_jumpmu_ctx, dbg_holder.load());
         abort();
      }
      dbg_holder.store((void*)jumpmu::thread_local_jumpmu_ctx, std::memory_order_release);
#ifdef MEAN_USE_UNIQUE_TASKING
      jumpmu::thread_local_jumpmu_ctx->lock_counter++;
      _owner = (uintptr_t)mean::UniqueTaskExecutor::localExec()._currentTask;  // mean::exec::getId();
#else
      _owner = mean::exec::getId();
#endif
   } else {
   }
   return b;
}
void YieldLock::lock()
{
// this implementation is super unsafe -> you have to be sure that you have the lock when you
// resume after locking here -> not documented...
#ifdef MEAN_USE_UNIQUE_TASKING
   while (!try_lock()) {
#else
   if (!try_lock()) {
#endif
      _waiting++;
      // if (_waiting > 10)
      //   abort();
      /*
       */
      // auto& this_task = mean::task::this_task();
      // this_task.lock = this;
      mean::task::set_current_task_lock(*this);
      mean::task::yield(mean::TaskState::ReadyLock);
      // must be locked at this point
      _waiting--;
#ifndef MEAN_USE_UNIQUE_TASKING
      // DEBUG: the batch path uses `if (!try_lock())`, not `while` -> it assumes the
      // scheduler handed the lock off before resuming us. Verify we actually hold it;
      // if dbg_held < 1 the handoff is broken and we're about to enter a critical
      // section without the lock.
      if (dbg_held.load(std::memory_order_acquire) < 1) {
         printf("DBG LOCK-HANDOFF: YieldLock %p resumed from ReadyLock but dbg_held=%d (lock NOT held!)\n",
                (void*)this, dbg_held.load());
         abort();
      }
#endif
   }

#ifdef MEAN_USE_UNIQUE_TASKING
   assert(jumpmu::thread_local_jumpmu_ctx->lock_counter > 0);
#endif
}
void YieldLock::unlock()
{
   // DEBUG: verify we actually held it, and drop dbg_held *before* releasing the flag
   // so the next acquirer never observes a stale held-count.
   int prev = dbg_held.fetch_sub(1, std::memory_order_acq_rel);
   if (prev != 1) {
      printf("DBG LOCK-UNDER: YieldLock %p unlock with dbg_held=%d (double unlock / not held); holder=%p ctx=%p\n",
             (void*)this, prev, dbg_holder.load(), (void*)jumpmu::thread_local_jumpmu_ctx);
      abort();
   }
   dbg_holder.store(nullptr, std::memory_order_release);
   // if (_owner >= 0) {
   _owner = -1;
   _lock.clear(std::memory_order_release);
#ifdef MEAN_USE_UNIQUE_TASKING
   jumpmu::thread_local_jumpmu_ctx->lock_counter--;
   assert(jumpmu::thread_local_jumpmu_ctx->lock_counter >= 0);
#endif
   // }
}
}  // namespace mean
// -------------------------------------------------------------------------------------
