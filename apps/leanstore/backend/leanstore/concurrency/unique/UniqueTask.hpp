#pragma once
// -------------------------------------------------------------------------------------
#include "leanstore/concurrency/batch/Task.hpp"
#include "leanstore/concurrency/utils/MessageHandler.hpp"
#include "leanstore/concurrency/utils/YieldLock.hpp"
#include "leanstore/io/IoAbstraction.hpp"
#include "leanstore/sync-primitives/JumpMU.hpp"
// -------------------------------------------------------------------------------------
#include "boost/context/continuation.hpp"
#include "boost/context/continuation_fcontext.hpp"
// -------------------------------------------------------------------------------------
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
// -------------------------------------------------------------------------------------
namespace mean
{
class UniqueTaskExecutor;
// -------------------------------------------------------------------------------------
struct UniqueTaskContext {
   bool init = false;
   bool wait = false;
   bool interrupted = false; 
   s64 worker_id = -1;
   static constexpr size_t INTERRUPT_STACK_SIZE = 1 << 15; 
   static constexpr size_t STACK_SIZE = 1 << 19;  // or whatever yours is
   static constexpr uint64_t STACK_GUARD = 0xDEADBEEFCAFEBABEULL;

   // Stack grows downward, so guard goes at the bottom
   uint64_t stack_guard_bottom = STACK_GUARD;
   alignas(64) uint8_t stack[STACK_SIZE];
   uint64_t stack_guard_top = STACK_GUARD;
   alignas(64) uint8_t interrupt_stack[INTERRUPT_STACK_SIZE];

   boost::context::detail::fcontext_t this_task_context;
   jumpmu::JumpMUContext jumpmuctx;

   bool isStackValid() const { return stack_guard_bottom == STACK_GUARD && stack_guard_top == STACK_GUARD; }
};
// -------------------------------------------------------------------------------------
class UniqueTask
{
  public:
   UniqueTaskContext context;

  private:
   TaskFunction* fun;
   uint64_t arg;
   TaskState state = TaskState::Ready;
   static void trampoline(boost::context::detail::transfer_t t);
   friend UniqueTaskExecutor;
   friend class UniqueTaskDeleter;

  public:
   YieldLock* lock;
   UniqueTask(TaskFunction* fun) : fun(fun) {}
   ~UniqueTask();
   TaskState getState();
};
// -------------------------------------------------------------------------------------
class UniqueTaskDeleter
{
   friend UniqueTask;

  public:
   UniqueTaskDeleter() noexcept = default;
   UniqueTaskDeleter(const UniqueTaskDeleter&) noexcept = default;
   UniqueTaskDeleter(UniqueTaskDeleter&&) noexcept = default;
   UniqueTaskDeleter& operator=(const UniqueTaskDeleter&) noexcept = default;
   UniqueTaskDeleter& operator=(UniqueTaskDeleter&&) noexcept = default;

   void operator()(UniqueTask* task) const;
};
// -------------------------------------------------------------------------------------
}  // namespace mean