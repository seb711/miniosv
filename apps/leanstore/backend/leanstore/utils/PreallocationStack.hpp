#pragma once
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace leanstore
{
namespace utils
{
template <typename TEntry>
class PreallocationStack
{
   static constexpr uint64_t CANARY = 0xDEADBEEFCAFEBABEULL;

   uint64_t canary1_ = CANARY;
   std::vector<TEntry> preallocated_entries;
   std::vector<TEntry*> unused_stack;
   int unused_stack_pos;
   int capacity_;
   uint64_t canary_ = CANARY;

   bool ownsPointer(TEntry* e) const { return e >= preallocated_entries.data() && e < preallocated_entries.data() + capacity_; }

  public:
   PreallocationStack(int size) : capacity_(size)
   {
      assert(size > 0 && "PreallocationStack: capacity must be positive");
      preallocated_entries = std::vector<TEntry>(size);
      unused_stack.resize(size);
      for (int i = 0; i < size; i++) {
         unused_stack[i] = &preallocated_entries[i];
      }
      unused_stack_pos = size;
      assert(canary_ == CANARY && "PreallocationStack: memory corruption during construction");
   }

   TEntry* pop()
   {
      assert(canary_ == CANARY && "PreallocationStack: memory corruption detected");
      assert(unused_stack_pos >= 0 && "PreallocationStack: stack position underflow");

      if (unused_stack_pos > 0) {
         TEntry* entry = unused_stack[--unused_stack_pos];
         assert(entry != nullptr && "PreallocationStack: null entry in stack");
         assert(ownsPointer(entry) && "PreallocationStack: popped entry outside owned range");
         return entry;
      } else {
         throw std::logic_error("PreallocationStack: exhausted (capacity=" + std::to_string(capacity_) + ")");
      }
   }

   void ret(TEntry* e)
   {
      assert(canary_ == CANARY && "PreallocationStack: memory corruption detected");
      assert(e != nullptr && "PreallocationStack: returning null pointer");
      assert(ownsPointer(e) && "PreallocationStack: returning pointer outside owned range");
      assert(unused_stack_pos >= 0 && "PreallocationStack: stack position negative");
      assert(unused_stack_pos < capacity_ && "PreallocationStack: double-free or returning more than allocated");

      // Debug: check for double-return
#ifndef NDEBUG
      for (int i = 0; i < unused_stack_pos; i++) {
         assert(unused_stack[i] != e && "PreallocationStack: double return detected");
      }
#endif

      unused_stack[unused_stack_pos++] = e;
   }

   int available() const { return unused_stack_pos; }
   int capacity() const { return capacity_; }
   bool empty() const { return unused_stack_pos == 0; }
   bool full() const { return unused_stack_pos == capacity_; }

   void assertIntegrity() const
   {
      assert(canary_ == CANARY && "PreallocationStack: canary corrupted");
      assert(unused_stack_pos >= 0 && unused_stack_pos <= capacity_);
      assert(static_cast<int>(preallocated_entries.size()) == capacity_);
      assert(static_cast<int>(unused_stack.size()) == capacity_);
#ifndef NDEBUG
      for (int i = 0; i < unused_stack_pos; i++) {
         assert(unused_stack[i] != nullptr && "PreallocationStack: null in free list");
         assert(ownsPointer(unused_stack[i]) && "PreallocationStack: corrupt pointer in free list");
      }
#endif
   }

   PreallocationStack(const PreallocationStack&) = delete;
   PreallocationStack& operator=(const PreallocationStack&) = delete;
   PreallocationStack(PreallocationStack&&) = delete;
   PreallocationStack& operator=(PreallocationStack&&) = delete;
};
}  // namespace utils
}  // namespace leanstore