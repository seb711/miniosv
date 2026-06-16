#include "leanstore/sync-primitives/JumpMU.hpp"

#include "BufferFrame.hpp"
#include "BufferManager.hpp"
#include "Exceptions.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/storage/buffer-manager/DTRegistry.hpp"
#include "leanstore/storage/buffer-manager/Partition.hpp"
#include "leanstore/storage/buffer-manager/Swip.hpp"
#include "leanstore/utils/Parallelize.hpp"

#include <gflags/gflags.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <stdexcept>

#include "arch.hh"

namespace leanstore
{
namespace storage
{

// ============================================================================
// Helper Functions for Partition State
// ============================================================================

s64 BufferManager::calculateCoolingDeficit(CoolingPartition& partition) const
{
   return static_cast<s64>(partition.cooling_bfs_limit) - (partition.dram_free_list.counter + partition.cooling_bfs_counter);
}

s64 BufferManager::calculateFreeBufferDeficit(CoolingPartition& partition) const
{
   return static_cast<s64>(partition.free_bfs_limit) - partition.dram_free_list.counter;
}

// ============================================================================
// Main Page Provider Cycle
// ============================================================================

void BufferManager::pageProviderThread(u64 partition_begin, u64 partition_end)
{
   // Thread entry point - can be extended with initialization if needed
}

std::function<bool()> BufferManager::getPageProviderPrio(int partition_id)
{
   CoolingPartition& partition = cooling_partitions[partition_id];

   return [&partition, this]() { return calculateFreeBufferDeficit(partition) > 32; };
}

int BufferManager::pageProviderCycle(int partition_id)
{
   ensure(partition_id < static_cast<int>(cooling_partitions_count));
   CoolingPartition& partition = cooling_partitions[partition_id];

   // Phase 1:   Unswizzle hot pages and move them to cooling stage
   //            If there are less than 64 free pages or (to be) free pages
   if (calculateCoolingDeficit(partition) > 64) {
      assert(arch::irq_enabled());
      uint64_t unswizzledHotPages = unswizzleHotPages(partition, 64, partition_id);
   }

   // Phase 2: Process cooling queue and initiate I/O for dirty pages
   s64 pages_to_process = calculateFreeBufferDeficit(partition);
   int cooledDownPages = 0;
   if (pages_to_process > 0) {
      // mean::exec::ioChannel().poll();
      cooledDownPages = processCoolingQueue(partition, pages_to_process, partition.state.freed_bfs_batch);
   }

   // Phase 3: Complete I/O operations and evict clean pages
   evictCompletedIoPages(partition, partition.state.freed_bfs_batch);

   // Attach freed buffer frames to partition's free list
   if (partition.state.freed_bfs_batch.size()) {
      partition.pushFreeList();
   }

   return cooledDownPages;
}

// ============================================================================
// Phase 1: Unswizzle Hot Pages
// ============================================================================

bool BufferManager::isValidCoolingCandidate(const BufferFrame& bf) const
{
   return !bf.header.keep_in_memory && !bf.header.isWB && !bf.header.latch.isExclusivelyLatched() && bf.header.state == BufferFrame::STATE::HOT;
}

bool BufferManager::hasOnlyEvictedChildren(BufferFrame& buffer_frame, OptimisticGuard& guard)
{
   bool all_children_evicted = true;

   getDTRegistry().iterateChildrenSwips(buffer_frame.page.dt_id, buffer_frame, [&](Swip<BufferFrame>& swip) {
      all_children_evicted &= swip.isEVICTED();
      guard.recheck();
      return true;
   });

   return all_children_evicted;
}

bool BufferManager::tryPickHotChild(BufferFrame*& current_bf, OptimisticGuard& guard)
{
   bool found_hot_child = false;

   getDTRegistry().iterateChildrenSwips(current_bf->page.dt_id, *current_bf, [&](Swip<BufferFrame>& swip) {
      if (swip.isHOT()) {
         current_bf = &swip.bfRef();
         guard.recheck();
         found_hot_child = true;
         return false;  // Stop iteration
      }
      guard.recheck();
      return true;  // Continue iteration
   });

   return found_hot_child;
}

void BufferManager::coolBufferFrame(BufferFrame& bf, CoolingPartition& partition, ParentSwipHandler& parent_handler, OptimisticGuard& guard)
{
   ExclusiveUpgradeIfNeeded parent_guard(parent_handler.parent_guard);
   ExclusiveGuard exclusive_guard(guard);

   assert(bf.header.state == BufferFrame::STATE::HOT);
   assert(!bf.header.isWB);
   assert(parent_handler.parent_guard.version == parent_handler.parent_guard.latch->ref().load());
   assert(parent_handler.swip->bf == &bf);

   partition.cooling_queue.push_back(&bf);
   partition.cooling_bfs_counter++;
   ensure(partition.cooling_queue.size() == partition.cooling_bfs_counter);

   bf.header.state = BufferFrame::STATE::COOL;
   parent_handler.swip->cool();
}

// unswizzleHotPages only for the given partition; the partition
// is per PageProviderThread until now
u64 BufferManager::unswizzleHotPages(CoolingPartition& partition, u64 required_count, int partition_id)
{
   const int max_partitions = FLAGS_pp_threads;
   ensure(partition_id < max_partitions);

   BufferFrame* current_bf = &partitionRandomBufferFrame(partition_id, max_partitions);
   u64 cooled_count = 0;
   u64 failed_attempts = 0;
   const u64 max_failed_attempts = 10;

   while (failed_attempts < max_failed_attempts) {
      jumpmuTry()
      {
         while (cooled_count < required_count && failed_attempts < max_failed_attempts) {
            OptimisticGuard guard(current_bf->header.latch, true);

            // Check if this buffer frame is a valid cooling candidate
            if (!isValidCoolingCandidate(*current_bf)) {
               current_bf = &partitionRandomBufferFrame(partition_id, max_partitions);
               failed_attempts++;
               continue;
            }

            guard.recheck();

            // Try to pick a hot child instead of current frame
            // If we picked a Hot Child then we need to start with it
            // from the beginning
            if (tryPickHotChild(current_bf, guard)) {
               continue;
            }

            // Ensure all children are evicted before cooling parent
            // if not then we should also start from new with a repicked frame
            if (!hasOnlyEvictedChildren(*current_bf, guard)) {
               current_bf = &partitionRandomBufferFrame(partition_id, max_partitions);
               failed_attempts++;
               continue;
            }

            // Find parent and check space utilization
            DTID dt_id = current_bf->page.dt_id;
            guard.recheck();

            ParentSwipHandler parent_handler;
            getDTRegistry().findParent(dt_id, *current_bf, parent_handler);

            if (FLAGS_optimistic_parent_pointer && parent_handler.is_bf_updated) {
               guard.guard.version += 2;
            }

            assert(parent_handler.parent_guard.state == GUARD_STATE::OPTIMISTIC);
            assert(parent_handler.parent_guard.latch != reinterpret_cast<HybridLatch*>(0x99));

            guard.recheck();

            if (getDTRegistry().checkSpaceUtilization(current_bf->page.dt_id, *current_bf, guard, parent_handler)) {
               current_bf = &partitionRandomBufferFrame(partition_id, max_partitions);
               continue;
            }

            guard.recheck();

            // Cool the buffer frame
            coolBufferFrame(*current_bf, partition, parent_handler, guard);
            cooled_count++;

            // at the end pick a new candidate
            current_bf = &partitionRandomBufferFrame(partition_id, max_partitions);
         }

         failed_attempts = 0;
         jumpmu_break;
      }
      jumpmuCatch()
      {
         // pick a new candidate
         current_bf = &partitionRandomBufferFrame(partition_id, max_partitions);
      }
   }

   if (failed_attempts == max_failed_attempts) {
      WorkerCounters::myCounters().time_counter_3++;
   }

   return cooled_count;
}

// ============================================================================
// Phase 2: Process Cooling Queue
// ============================================================================

bool BufferManager::hasConflictingIoFrame(const BufferFrame& bf)
{
   IoPartition& io_partition = getIoPartition(bf.header.pid);

   if (!io_partition.io_mutex.try_lock()) {
      return true;
   }

   bool has_conflict = io_partition.io_ht.lookup(bf.header.pid).holder != nullptr;
   io_partition.io_mutex.unlock();

   return has_conflict;
}

void BufferManager::initiateWriteBack(BufferFrame& bf, CoolingPartition& partition, OptimisticGuard& guard)
{
   {
      ExclusiveGuard exclusive_guard(guard);
      assert(!bf.header.isWB);
      bf.header.isWB = true;
   }

   SharedGuard shared_guard(guard);
   PID write_pid = bf.header.pid;

   if (FLAGS_out_of_place) {
      write_pid = partition.nextPID();
   }

   mean::UserIoCallback callback;
   callback.user_data.val.ptr = &bf;
   callback.user_data2.val.u = bf.page.GSN;
   callback.user_data3.val.ptr = &partition;
   callback.callback = [](mean::IoBaseRequest* req) { handleWriteCompletion(req); };

   bf.page.magic_debugging_number = write_pid;
   bf.page.magic_debugging_number_end = write_pid;

   mean::exec::ioChannel().pushWrite(reinterpret_cast<char*>(&bf.page), PAGE_SIZE * write_pid, PAGE_SIZE, callback, true);
   PPCounters::myCounters().flushed_pages_counter++;

   partition.state.submitted++;
   partition.outstanding++;
   ensure(partition.outstanding <= static_cast<s64>(partition.io_queue.max_size));
}

void BufferManager::handleWriteCompletion(mean::IoBaseRequest* request)
{
   auto& written_bf = *request->user.user_data.as<BufferFrame*>();
   auto written_gsn = request->user.user_data2.val.u;
   auto& partition = *request->user.user_data3.as<CoolingPartition*>();

   partition.open_writeback_handles.push_back({&written_bf, written_gsn});
}

void BufferManager::evictBufferFrame(CoolingPartition& partition, FreedBfsBatch& freed_batch, BufferFrame& bf, OptimisticGuard& guard)
{
   DTID dt_id = bf.page.dt_id;
   guard.recheck();

   ParentSwipHandler parent_handler;
   getDTRegistry().findParent(dt_id, bf, parent_handler);

   if (FLAGS_optimistic_parent_pointer && parent_handler.is_bf_updated) {
      guard.guard.version += 2;
   }

   assert(parent_handler.parent_guard.state == GUARD_STATE::OPTIMISTIC);

   ExclusiveUpgradeIfNeeded parent_guard(parent_handler.parent_guard);
   guard.guard.toExclusive();

   assert(!bf.header.isWB);
   assert(bf.header.state == BufferFrame::STATE::IOCOLDDONE || bf.header.state == BufferFrame::STATE::IOPOPPED ||
          bf.header.state == BufferFrame::STATE::COOL);

   // Evict from parent
   parent_handler.swip->evict(bf.header.pid);
   parent_handler.parent_bf->page.GSN++;

   // Reset buffer frame
   bf.reset();
   ensure(bf.header.state == BufferFrame::STATE::FREE);
   bf.header.latch->fetch_add(LATCH_EXCLUSIVE_BIT, std::memory_order_release);
   bf.header.latch.mutex.unlock();

   freed_batch.add(bf);

   if (freed_batch.size() <= std::min<u64>(FLAGS_worker_threads, 128)) {
      partition.pushFreeList();
   }
}

int BufferManager::processCoolingQueue(CoolingPartition& partition, u64 pages_to_process, FreedBfsBatch& freed_batch)
{
   if (partition.state.debug_thread == -1) {
      partition.state.debug_thread = mean::exec::getId();
   }
   ensure(partition.state.debug_thread == mean::exec::getId());

   s64 pages_remaining = std::min(pages_to_process, partition.cooling_queue.size());
   int pages_processed = 0;

   while (pages_remaining > 0 && !mean::exec::ioChannel().writeStackFull() && partition.outstanding < static_cast<s64>(partition.io_queue.max_size)) {
      BufferFrame* bf_ptr;
      if (!partition.cooling_queue.try_pop(bf_ptr)) {
         break;
      }

      pages_remaining--;
      partition.cooling_bfs_counter--;
      ensure(partition.cooling_queue.size() == partition.cooling_bfs_counter);

      BufferFrame& bf = *bf_ptr;

      jumpmuTry()
      {
         OptimisticGuard guard(bf.header.latch, true);

         // Check if buffer frame is still in cooling state
         if (bf.header.state != BufferFrame::STATE::COOL) {
            jumpmu::jump();
         }

         // Skip if already in write-back
         if (bf.header.isWB) {
            jumpmu_break;
         }

         // Check for conflicting I/O operations
         if (hasConflictingIoFrame(bf)) {
            jumpmu::jump();
         }

         pages_remaining--;

         if (bf.isDirty()) {
            // Initiate write-back for dirty pages
            ensure(partition.outstanding >= 0);
            if (!mean::exec::ioChannel().writeStackFull() && partition.outstanding < static_cast<s64>(partition.io_queue.max_size)) {
               initiateWriteBack(bf, partition, guard);
               // pages_processed++;
            } else {
               // Re-queue if I/O resources unavailable
               partition.cooling_bfs_counter++;
               partition.cooling_queue.push_back(&bf);
               ensure(partition.cooling_queue.size() == partition.cooling_bfs_counter);
               jumpmu_break;
            }
         } else {
            // Evict clean pages immediately
            pages_processed++;
            __builtin_prefetch(bf.header.optimistic_parent_pointer.child.parent_bf, 0, 1);
            evictBufferFrame(partition, freed_batch, bf, guard);
         }
      }
      jumpmuCatch()
      {
         // Re-queue on failure if still in cooling state
         if (bf.header.state == BufferFrame::STATE::COOL) {
            partition.cooling_bfs_counter++;
            partition.cooling_queue.push_back(&bf);
            ensure(partition.cooling_queue.size() == partition.cooling_bfs_counter);
         }
      }
   }

   // ensure(mean::exec::ioChannel().submitMin() == 0);

   return pages_processed;
}

// ============================================================================
// Phase 3: Evict Completed I/O Pages
// ============================================================================

void BufferManager::evictCompletedIoPages(CoolingPartition& partition, FreedBfsBatch& freed_batch)
{
   std::pair<BufferFrame*, u64> written_bf;
   while (partition.open_writeback_handles.try_pop(written_bf)) {
      while (true) {
         jumpmuTry()
         {
            Guard guard(written_bf.first->header.latch);
            guard.toExclusive();

            assert(written_bf.first->header.isWB);
            assert(written_bf.first->header.lastWrittenGSN < written_bf.second);

            if (FLAGS_out_of_place) {
               PID old_pid = written_bf.first->header.pid;
               PID new_pid = written_bf.second;
               written_bf.first->header.pid = new_pid;
               partition.freePage(old_pid);
            }

            written_bf.first->header.lastWrittenGSN = written_bf.second;
            written_bf.first->header.isWB = false;

            guard.unlock();
            jumpmu_break;
         }
         jumpmuCatch()
         {
            // Retry on failure
         }
      }

      partition.state.done++;
      partition.io_queue.push_back(written_bf.first);
   }

   BufferFrame* bf_ptr;
   int pending_count = partition.io_queue.size();

   while (!partition.io_queue.empty() && pending_count-- > 0) {
      ensure(partition.io_queue.try_pop(bf_ptr), "Failed to pop from I/O queue");
      // ensure(partition.outstanding >= 0, "Outstanding count negative");
      // ensure(partition.outstanding <= static_cast<s64>(partition.io_queue.max_size), "Outstanding count exceeds maximum");

      partition.outstanding--;

      BufferFrame& bf = *bf_ptr;
      bool needs_requeue = false;

      jumpmuTry()
      {
         OptimisticGuard guard(bf.header.latch, true);

         // Check if buffer frame is in a valid state for eviction
         if (bf.header.state != BufferFrame::STATE::COOL && bf.header.state != BufferFrame::STATE::IOCOLDDONE &&
             bf.header.state != BufferFrame::STATE::IOLOST2) {
            needs_requeue = true;
            jumpmu::jump();
         }

         // Evict if clean and not in write-back
         if (!bf.header.isWB && !bf.isDirty()) {
            evictBufferFrame(partition, freed_batch, bf, guard);
         }
      }
      jumpmuCatch()
      {
         // Re-queue on failure
         if (!needs_requeue || bf.header.state == BufferFrame::STATE::IOCOLDDONE) {
            partition.outstanding++;
            partition.io_queue.push_back(bf_ptr);
            // ensure(partition.outstanding <= static_cast<s64>(partition.io_queue.max_size), "Outstanding count exceeds maximum after requeue");
         }
      }
   }
}

}  // namespace storage
}  // namespace leanstore