#pragma once
#include "BufferFrame.hpp"
#include "FreeList.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/storage/buffer-manager/FreeList.hpp"
#include "leanstore/utils/Misc.hpp"
#include "leanstore/utils/PreallocationStack.hpp"
#include "leanstore/utils/RingBuffer.hpp"
#include "leanstore/utils/RingBufferMPSC.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
#include <mutex>
#include <unordered_set>
#include <vector>
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
// -------------------------------------------------------------------------------------
struct IOFrame {
   enum class STATE : u8 {
      READING = 0,
      READY = 1,
      TO_DELETE = 2,
      UNDEFINED = 3  // for debugging
   };
   mean::mmutex mutex;
   STATE state = STATE::UNDEFINED;
   BufferFrame* bf = nullptr;
   // -------------------------------------------------------------------------------------
   // Everything in CIOFrame is protected by partition lock
   // except the following counter which is decremented outside to determine
   // whether it is time to remove it
   atomic<s64> readers_counter = 0;
};
// -------------------------------------------------------------------------------------
struct HashTable {
   struct Entry {
      u64 key;
      Entry* next;
      IOFrame value;
      Entry(u64 key);
      Entry();
   };
   // -------------------------------------------------------------------------------------
   struct Handler {
      Entry** holder;
      operator bool() const { return holder != nullptr; }
      IOFrame& frame() const
      {
         assert(holder != nullptr);
         return *reinterpret_cast<IOFrame*>(&((*holder)->value));
      }
   };
   // -------------------------------------------------------------------------------------
   u64 mask;
   Entry** entries;
   utils::PreallocationStack<Entry> alloc_stack;
   // -------------------------------------------------------------------------------------
   u64 hashKey(u64 k);
   IOFrame& insert(u64 key);
   Handler lookup(u64 key);
   void remove(Handler& handler);
   void remove(u64 key);
   bool has(u64 key);  // for debugging
   HashTable(u64 size_in_bits);

   HashTable(const HashTable&) = delete;
   HashTable& operator=(const HashTable&) = delete;
   HashTable(HashTable&&) = delete;
   HashTable& operator=(HashTable&&) = delete;
};
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
struct FreedBfsBatch {
   BufferFrame *freed_bfs_batch_head = nullptr, *freed_bfs_batch_tail = nullptr;
   u64 freed_bfs_counter = 0;
   // -------------------------------------------------------------------------------------
   void reset()
   {
      freed_bfs_batch_head = nullptr;
      freed_bfs_batch_tail = nullptr;
      freed_bfs_counter = 0;
   }
   // -------------------------------------------------------------------------------------
   u64 size() { return freed_bfs_counter; }
   // -------------------------------------------------------------------------------------
   void add(BufferFrame& bf)
   {
      bf.header.next_free_bf = freed_bfs_batch_head;
      if (freed_bfs_batch_head == nullptr) {
         freed_bfs_batch_tail = &bf;
      }
      freed_bfs_batch_head = &bf;
      freed_bfs_counter++;
      // -------------------------------------------------------------------------------------
   }
};
struct CoolingPartition {
   enum class PPState {
      Phase1,
      Phase3poll,
      Phase3pollDone,
   };
   struct PPStateData {
      PPState state = PPState::Phase1;
      decltype(std::chrono::high_resolution_clock::now()) poll_begin, poll_end;
      decltype(std::chrono::high_resolution_clock::now()) phase_3_begin, phase_3_end;
      int submitted = 0;
      int done = 0;
      int debug_thread = -1;
      FreedBfsBatch freed_bfs_batch;
   } state;
   // -------------------------------------------------------------------------------------
   // -------------------------------------------------------------------------------------
   // mean::SpinLock cooling_mutex;
   utils::RingBuffer<BufferFrame*> cooling_queue;
   utils::RingBuffer<BufferFrame*> io_queue;

   struct WriteBackHandle {
      BufferFrame* written_bf; 
      u64 out_of_place_addr; 
      u64 written_gsn; 
   }; 
   utils::RingBuffer<std::pair<BufferFrame*, u64>> open_writeback_handles; 
   // -------------------------------------------------------------------------------------
   atomic<u64> cooling_bfs_counter = 0;
   const u64 free_bfs_limit;
   const u64 cooling_bfs_limit;
   FreeList dram_free_list;
   s64 outstanding = 0;
   // -------------------------------------------------------------------------------------
   const u64 pid_distance;
   // Fixed-size ring buffer for freed PIDs instead of lock-free queue
   utils::RingBufferMPSC<PID> freed_pids_queue;
   atomic<u64> next_pid;
   atomic<u64> freed_pids_count;  // Track count for statistics
   // -------------------------------------------------------------------------------------
   CoolingPartition(u64 first_pid, u64 pid_distance, u64 free_bfs_limit, u64 cooling_bfs_limit, u64 max_outsanding_ios)
       : cooling_queue(cooling_bfs_limit * 2),  // FIXME
         io_queue(max_outsanding_ios),
         open_writeback_handles(max_outsanding_ios),
         free_bfs_limit(free_bfs_limit),
         cooling_bfs_limit(cooling_bfs_limit),
         pid_distance(pid_distance),
         freed_pids_queue(FLAGS_ssd_gib * 1024 * 1024 * 1024 / PAGE_SIZE / pid_distance * 0.25),  // Fixed capacity ring buffer
         freed_pids_count(0)
   {
      next_pid = first_pid;
   }
   // -------------------------------------------------------------------------------------
   // SSD Pages
   // -------------------------------------------------------------------------------------
   inline PID nextPID()
   {
      PID pid;
      // Try to pop a freed PID from the ring buffer
      if (freed_pids_queue.try_pop(pid)) {
         freed_pids_count.fetch_sub(1, std::memory_order_relaxed);
         return pid;
      } else {
         // No freed PIDs available, allocate a new one
         pid = next_pid.fetch_add(pid_distance, std::memory_order_relaxed);
         ensure((pid * PAGE_SIZE / 1024 / 1024 / 1024) <= FLAGS_ssd_gib);
         return pid;
      }
   }

   void freePage(PID pid)
   {
      // Push the freed PID to the ring buffer
      // If full, we could either: drop (leak the PID), spin-wait, or fall back to allocation
      // Here we spin-wait since PIDs are a limited resource
      freed_pids_queue.push_back(pid);
         // Ring buffer is full, wait for space
         // In production, might want to add backpressure or fallback logic
      freed_pids_count.fetch_add(1, std::memory_order_relaxed);
   }

   u64 allocatedPages() { return next_pid.load(std::memory_order_relaxed) / pid_distance; }

   u64 freedPages() { return freed_pids_count.load(std::memory_order_relaxed); }
   // -------------------------------------------------------------------------------------
   void pushFreeList()
   {
      FreedBfsBatch& b = state.freed_bfs_batch;
      dram_free_list.batchPush(b.freed_bfs_batch_head, b.freed_bfs_batch_tail, b.freed_bfs_counter);
      b.reset();
   }
   // -------------------------------------------------------------------------------------
};
struct IoPartition {
   // -------------------------------------------------------------------------------------
   mean::mmutex io_mutex;
   HashTable io_ht;
   IoPartition(u64 first_pid, u64 pid_distance, u64 free_bfs_limit, u64 cooling_bfs_limit);
   // -------------------------------------------------------------------------------------
   IoPartition(u64 max_outsanding_ios) : io_ht(utils::getBitsNeeded(max_outsanding_ios)) {}
   ~IoPartition();
   // -------------------------------------------------------------------------------------
};
}  // namespace storage
}  // namespace leanstore
// -------------------------------------------------------------------------------------