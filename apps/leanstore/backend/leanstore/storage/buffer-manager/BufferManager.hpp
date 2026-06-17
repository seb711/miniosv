#include "BufferFrame.hpp"
#pragma once
#include "DTRegistry.hpp"
#include "FreeList.hpp"
#include "Partition.hpp"
#include "Swip.hpp"
#include "Units.hpp"
// -------------------------------------------------------------------------------------
#include "PerfEvent.hpp"
// -------------------------------------------------------------------------------------
#include <libaio.h>
#include <sys/mman.h>

#include <cstring>
#include <ios>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
// -------------------------------------------------------------------------------------
namespace leanstore
{
class LeanStore;
namespace profiling
{
class BMTable;
}
namespace storage
{
// -------------------------------------------------------------------------------------
/*
 * Swizzle a page:
 * 1- bf_s_lock global bf_s_lock
 * 2- if it is in cooling stage:
 *    a- yes: bf_s_lock write (spin till you can), remove from stage, swizzle in
 *    b- no: set the state to IO, increment the counter, hold the mutex, p_read
 * 3- if it is in IOFlight:
 *    a- increment counter,
 */
class BufferManager
{
  private:
   friend class leanstore::LeanStore;
   friend class leanstore::profiling::BMTable;
   /*
    * BUFFER MANAGER MEMORY LAYOUT (bfs is the pointer to it)
    * ========================================================
    *
    * We have as many cooling_partition as pp_threads (therefore we 
    * always need to set the FLAGS_pp_threads constant)
    * 
    * Total Size = sizeof(BufferFrame) * (dram_pool_size + safety_pages)
    * dram_pool_size = (FLAGS_dram_gib * 1GiB) / sizeof(BufferFrame)
    *
    * Memory Layout (bfs array):
    * --------------------------
    * bfs[0]                 ┌─── BufferFrame 0 → cooling_partition[0]
    * bfs[1]                 ├─── BufferFrame 1 → cooling_partition[1]
    * bfs[2]                 ├─── BufferFrame 2 → cooling_partition[2]
    * ...                    │
    * bfs[N-1]               ├─── BufferFrame N-1 → cooling_partition[N-1]
    * bfs[N]                 ├─── BufferFrame N → cooling_partition[0] (wrap)
    * bfs[N+1]               ├─── BufferFrame N+1 → cooling_partition[1]
    * ...                    │
    * bfs[dram_pool_size-1]  ├─── Last BufferFrame
    * bfs[dram_pool_size]    ├─── SAFETY PAGES (overflow protection)
    * ...                    │
    * bfs[total-1]           └─── End
    *
    * Distribution (Round-Robin):
    * ---------------------------
    * BufferFrame[i] → cooling_partition[i % cooling_partitions_count]
    *
    * Example (4 partitions):
    * -----------------------
    * Index:  0    1    2    3    4    5    6    7    ...
    * Part:   P0   P1   P2   P3   P0   P1   P2   P3   ...
    *
    * Per Partition Content:
    * ----------------------
    * cooling_partition[i]:
    *   - dram_free_list: [bfs[i], bfs[i+N], bfs[i+2N], ...] where N = cooling_partitions_count
    *   - ~(dram_pool_size / cooling_partitions_count) frames per partition
    */
   BufferFrame* bfs;
   // -------------------------------------------------------------------------------------
   int ssd_fd;
   // -------------------------------------------------------------------------------------
   // Free  Pages
   const u8 safety_pages = 10;                 // we reserve these extra pages to prevent segfaults
   u64 dram_pool_size;                         // total number of dram buffer frames
   atomic<u64> ssd_freed_pages_counter = {0};  // used to track how many pages did we really allocate
   // -------------------------------------------------------------------------------------
   // For cooling and inflight io
   u64 partitions_mask;
   // -------------------------------------------------------------------------------------
  public:
   u64 io_partitions_count;
   u64 cooling_partitions_count;
   IoPartition* io_partitions;
   CoolingPartition* cooling_partitions;

  private:
   // -------------------------------------------------------------------------------------
   // Threads managements
  public:
   // IoPartition& randomIoPartition();
   s64 calculateCoolingDeficit(CoolingPartition& partition) const;
   s64 calculateFreeBufferDeficit(CoolingPartition& partition) const;
   std::function<bool()> getPageProviderPrio(int partition_id); 
   
   void pageProviderThread(u64 partition_begin, u64 partition_end);
   int pageProviderCycle(int partition_id);

   bool isValidCoolingCandidate(const BufferFrame& bf) const;
   bool hasOnlyEvictedChildren(BufferFrame& buffer_frame, OptimisticGuard& guard);
   bool tryPickHotChild(BufferFrame*& current_bf, OptimisticGuard& guard);
   void coolBufferFrame(BufferFrame& bf, CoolingPartition& partition, ParentSwipHandler& parent_handler, OptimisticGuard& guard);
   u64 unswizzleHotPages(CoolingPartition& partition, u64 required_count, int partition_id);

   bool hasConflictingIoFrame(const BufferFrame& bf);
   void initiateWriteBack(BufferFrame& bf, CoolingPartition& partition, OptimisticGuard& guard);
   static void handleWriteCompletion(mean::IoBaseRequest* request);
   void evictBufferFrame(CoolingPartition& partition, FreedBfsBatch& freed_batch, BufferFrame& bf, OptimisticGuard& guard);
   int processCoolingQueue(CoolingPartition& partition, u64 pages_to_process, FreedBfsBatch& freed_batch);

   void evictCompletedIoPages(CoolingPartition& partition, FreedBfsBatch& freed_batch);

   // -------------------------------------------------------------------------------------
   atomic<u64> bg_threads_counter = {0};
   atomic<bool> bg_threads_keep_running = {true};
   // -------------------------------------------------------------------------------------
   // Misc
   BufferFrame& randomBufferFrame();
   BufferFrame& partitionRandomBufferFrame(u64 partition, u64);
   u64 partitionRandomBufferFramePos(u64 partition, u64 max_partitions);
   CoolingPartition& randomCoolingPartition();
   CoolingPartition& getCoolingPartition(BufferFrame&);
   IoPartition& getIoPartition(PID);
   u64 getIoPartitionID(PID);

  public:
   // -------------------------------------------------------------------------------------
   BufferManager();
   ~BufferManager();
   // -------------------------------------------------------------------------------------
   BufferFrame& allocatePage();
   inline BufferFrame& tryFastResolveSwip(Guard& swip_guard, Swip<BufferFrame>& swip_value)
   {
      /*
      if (rand() % 8 == 0) {
         mean::task::yield();
      }
      //*/
      if (swip_value.isHOT()) {
         BufferFrame& bf = swip_value.bfRef();
         swip_guard.recheck();
         return bf;
      } else {
         return resolveSwip(swip_guard, swip_value);
      }
   }
   BufferFrame& resolveSwip(Guard& swip_guard, Swip<BufferFrame>& swip_value);
   void reclaimPage(BufferFrame& bf);
   // -------------------------------------------------------------------------------------
   void stopBackgroundThreads();
   /*
    * Life cycle of a fix:
    * 1- Check if the pid is swizzled, if yes then store the BufferFrame address
    * temporarily 2- if not, then posix_check if it exists in cooling stage
    * queue, yes? remove it from the queue and return the buffer frame 3- in
    * anycase, posix_check if the threshold is exceeded, yes ? unswizzle a random
    * BufferFrame (or its children if needed) then add it to the cooling stage.
    */
   // -------------------------------------------------------------------------------------
   void readPageSync(PID pid, u8* destination);
   void readPageAsync(PID pid, u8* destination, std::function<void()> callback);
   void fDataSync();
   // -------------------------------------------------------------------------------------
   void clearSSD();
   void restore();
   void writeAllBufferFrames();
   // -------------------------------------------------------------------------------------
   u64 getPoolSize() { return dram_pool_size; }
   DTRegistry& getDTRegistry() { return DTRegistry::global_dt_registry; }
   u64 consumedPages();
   BufferFrame& getContainingBufferFrame(const u8*);  // get the buffer frame containing the given ptr address
};  // namespace storage
// -------------------------------------------------------------------------------------
class BMC
{
  public:
   static BufferManager* global_bf;
};
}  // namespace storage
}  // namespace leanstore
// -------------------------------------------------------------------------------------
