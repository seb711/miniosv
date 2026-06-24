#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "leanstore/sync-primitives/JumpMU.hpp"

#include "../UniqueTask.hpp"
#include "boost/context/continuation_fcontext.hpp"

namespace mean
{

using TaskFunction = std::function<void()>;

// Simple free list memory pool
template <size_t BlockSize>
class MemoryPool
{
   std::vector<void*> free_list_;
   std::vector<void*> all_blocks_;
   size_t capacity_;

  public:
   MemoryPool(size_t capacity);
   ~MemoryPool();

   void* allocate();
   void deallocate(void* ptr);

   size_t available() const { return free_list_.size(); }
   size_t capacity() const { return capacity_; }
};

// Memory pool manager for UniqueTaskContext
class TaskContextPool
{
   MemoryPool<sizeof(UniqueTask)> task_pool_;

  public:
   explicit TaskContextPool(size_t capacity);

   UniqueTask* allocate(TaskFunction* fun);
   void deallocate(UniqueTask* task);

   size_t available() const;
   size_t capacity() const { return task_pool_.capacity(); }

   static constexpr size_t getStackSize() { return UniqueTaskContext::STACK_SIZE; }
};

/*
// Global pool management
void initTaskContextPool(size_t capacity);
void destroyTaskContextPool();

// Helper to create initialized context with boost make_fcontext
void initializeTaskContext(UniqueTaskContext& ctx, void* stack_base, size_t stack_size,
                          void (*fn)(boost::context::detail::transfer_t));
using UniqueTaskPtr = std::unique_ptr<UniqueTask, UniqueTaskDeleter>;

// Factory function to create a task with pre-allocated memory
UniqueTaskPtr createTask(TaskFunction fun, void (*entry_fn)(boost::context::detail::transfer_t));


*/
}  // namespace mean