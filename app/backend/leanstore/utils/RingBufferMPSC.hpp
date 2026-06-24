#pragma once

#include "leanstore/concurrency/utils/YieldLock.hpp"

#include <mutex>
#include <stdexcept>
#include <vector>
#include <atomic>
#include "Units.hpp"

#if true
#define DEBUG_COUNTER(x) x
#else
#define DEBUG_COUNTER(x)
#endif

namespace leanstore {
namespace utils {
template<typename TValue>
class RingBufferMPSC {
   std::vector<TValue> values;
   TValue *const vec_first;
   TValue *const vec_last;
   alignas(64) mean::SpinLock _write_mutex; 
   std::atomic<TValue*> _write_ptr;
   alignas(64) mean::SpinLock _read_mutex;  // Added for multi-consumer
   std::atomic<TValue*> _read_ptr;
   DEBUG_COUNTER(
         std::atomic<u64> inserted{0};
         std::atomic<u64> erased{0};
   )
public:
   static constexpr int POP_MAX = 32;
   const u64 max_size;
   
   RingBufferMPSC(int max_size) : values(max_size+1), // +1 as one always stays empty. 
      vec_first(&values[0]), vec_last(&values[max_size]), 
      _write_ptr(&values[0]), _read_ptr(&values[0]), max_size(max_size)  {
   }
   RingBufferMPSC(RingBufferMPSC const&) = delete;
   
   TValue& push_back(const TValue& value) {
      std::lock_guard<mean::SpinLock> write_lock(_write_mutex);
      TValue*const current = _write_ptr.load(std::memory_order_relaxed); // only accessed under _write_mutex
      TValue* next = current + 1;
      if (next > vec_last) { // overflow
         next = vec_first;
      }
      if (next == _read_ptr.load(std::memory_order_acquire)) { // full
         throw std::logic_error("full");
      }
      DEBUG_COUNTER() {inserted.fetch_add(1, std::memory_order_relaxed);}
      *current = value; // write value here
      _write_ptr.store(next, std::memory_order_release); // move forward
      return *current; 
   }
   
   bool try_pop(TValue& ret) {
      std::lock_guard<mean::SpinLock> read_lock(_read_mutex);  // Lock for multi-consumer
      TValue*const current_read = _read_ptr.load(std::memory_order_relaxed); // protected by _read_mutex
      if (current_read == _write_ptr.load(std::memory_order_acquire)) {
         return false;
      }
      DEBUG_COUNTER() {erased.fetch_add(1, std::memory_order_relaxed);}
      TValue* next = current_read + 1;
      if (next > vec_last) { // overflow
         next = vec_first;
      }
      ret = *current_read;
      _read_ptr.store(next, std::memory_order_release); // make it visible to producers
      return true;
   }
   
   int pop_multiple(std::array<TValue, POP_MAX>& pop_into, int pop_max) {
      std::lock_guard<mean::SpinLock> read_lock(_read_mutex);  // Lock for multi-consumer
      TValue* current_read = _read_ptr.load(std::memory_order_relaxed); // protected by _read_mutex
      TValue*const current_write = _write_ptr.load(std::memory_order_acquire);
      int popped = 0;
      while (current_read != current_write && popped < POP_MAX && popped < pop_max) {
         DEBUG_COUNTER() {erased.fetch_add(1, std::memory_order_relaxed);}
         pop_into[popped++] = *current_read;
         current_read++;
         if (current_read > vec_last) { // overflow
            current_read = vec_first;
         }
      }
      if (popped > 0) {
         _read_ptr.store(current_read, std::memory_order_release); // make it visible to producers
      }
      return popped;
   }
   
   u64 size() const {
      TValue* current_write = _write_ptr.load(std::memory_order_acquire);
      TValue* current_read = _read_ptr.load(std::memory_order_acquire);
      
      if (current_write >= current_read) {
         return current_write - current_read;
      } else {
         // Wrapped around
         return (vec_last - current_read + 1) + (current_write - vec_first);
      }
   }
   
   bool empty() const {
      return _read_ptr.load(std::memory_order_acquire) == _write_ptr.load(std::memory_order_acquire);
   }
};
} //namespace utils
} //namespace leanstore