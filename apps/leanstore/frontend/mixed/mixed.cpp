#include "Time.hpp"
#include "adapter.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/profiling/counters/ThreadCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/Misc.hpp"
#include "leanstore/utils/Parallelize.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
#include "leanstore/utils/ZipfGenerator.hpp"
#include "schema.hpp"
#include "types.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
// -------------------------------------------------------------------------------------
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/io/IoInterface.hpp"

#include "PerfEvent.hpp"
// -------------------------------------------------------------------------------------
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>
// -------------------------------------------------------------------------------------
DEFINE_uint32(ycsb_read_ratio, 100, "");
DEFINE_uint64(ycsb_tuple_count, 0, "");
DEFINE_uint32(ycsb_payload_size, 100, "tuple size in bytes");
DEFINE_uint32(ycsb_warmup_rounds, 0, "");
DEFINE_uint32(ycsb_tx_rounds, 1, "");
DEFINE_uint32(ycsb_tx_count, 0, "default = tuples");
DEFINE_bool(verify, false, "");
DEFINE_bool(ycsb_scan, false, "");
DEFINE_bool(ycsb_tx, true, "");
DEFINE_bool(ycsb_count_unique_lookup_keys, true, "");
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore;
// -------------------------------------------------------------------------------------
LeanStoreAdapter<item_t> kv_store;
// yeah, dirty include...

#include "mixed_workload.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
double calculateMTPS(chrono::high_resolution_clock::time_point begin, chrono::high_resolution_clock::time_point end, u64 factor)
{
   double tps = ((factor * 1.0 / (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0)));
   return (tps / 1000000.0);
}
// -------------------------------------------------------------------------------------
void run_ycsb()
{
   // -------------------------------------------------------------------------------------
   chrono::high_resolution_clock::time_point begin, end;
   // -------------------------------------------------------------------------------------
   // LeanStore DB
   LeanStore db;

   mean::task::scheduleTaskSync([&]() { kv_store = LeanStoreAdapter<item_t>(db, "ycsb", "y"); });

   Workload wl(kv_store);

   db.registerConfigEntry("ycsb_read_ratio", FLAGS_ycsb_read_ratio);
   db.registerConfigEntry("ycsb_target_gib", FLAGS_target_gib);
   // -------------------------------------------------------------------------------------
   const u64 ycsb_tuple_count = (FLAGS_ycsb_tuple_count)
                                    ? FLAGS_ycsb_tuple_count
                                    : FLAGS_target_gib * 1024 * 1024 * 1024 * 1.0 / 2.0 / (sizeof(uint64_t) + sizeof(BytesPayload<120>));
   // Insert values
   {
      db.startProfilingThread();
      const u64 n = ycsb_tuple_count;
      std::cout << "-------------------------------------------------------------------------------------" << endl;
      cout << "Inserting " << n << " values" << endl;
      begin = chrono::high_resolution_clock::now();

      std::atomic<u64> current_block_start{0};
      const u64 block_size = 1 << 18;  // Adjust granularity as needed
      const u64 total_items = n;

// #ifdef MEAN_USE_TASKING
#if defined(MEAN_USE_TASKING) || defined(MEAN_USE_UNIQUE_TASKING)
      auto ycsb_insert_fun = [&]() {
         while (true) {
            // Atomically fetch the next block to work on
            u64 block_start = current_block_start.fetch_add(block_size, std::memory_order_relaxed);

            // Check if we're done
            if (block_start >= total_items) {
               break;
            }

            // Calculate actual block end (handle last block being smaller)
            u64 block_end = std::min(block_start + block_size, total_items);

            // Process this block
            for (u64 t = block_start; t < block_end; t++) {
               wl.insert(t);
            }
         }
      };
      BlockedRange bb(0, (u64)1);
      mean::task::parallelFor(bb, ycsb_insert_fun, FLAGS_worker_tasks, false);
      wl.current_idx = n - 1; 
      wl.highest_inserted = n; 
#else
#ifndef NEW_JUMPMU
      // jumpmu::thread_local_jumpmu_ctx = new jumpmu::JumpMUContext();
#endif
      for (uint64_t i = 0; i < n; i++) {
         wl.insert(i);
      }
      wl.current_idx = n - 1; 
      wl.highest_inserted = n; 
#endif
      end = chrono::high_resolution_clock::now();
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      cout << calculateMTPS(begin, end, n) << " M tps" << endl;
      // -------------------------------------------------------------------------------------
      const u64 written_pages = db.getBufferManager().consumedPages();
      const u64 mib = written_pages * PAGE_SIZE / 1024 / 1024;
      cout << "Inserted volume: (pages, MiB) = (" << written_pages << ", " << mib << ")" << endl;
      cout << "-------------------------------------------------------------------------------------" << endl;
   }
   // -------------------------------------------------------------------------------------
   auto zipf_random = std::make_unique<utils::ScrambledZipfGenerator>(0, ycsb_tuple_count, FLAGS_zipf_factor);
   cout << setprecision(4);
   // -------------------------------------------------------------------------------------
   // -------------------------------------------------------------------------------------
   cout << "-------------------------------------------------------------------------------------" << endl;
   cout << "~Transactions" << endl;
   atomic<bool> keep_running = {true};
   atomic<u64> running_threads_counter = {0};
   {
      auto start = mean::getSeconds();
      auto ycsb_tx = [&]() {
         running_threads_counter++;

         auto before = mean::readTSC();
         uint64_t key = zipf_random->rand();

         int txtype = wl.tx();
         auto now = mean::readTSC();
         // auto timeDiff = mean::tscDifferenceUs(now, before);

#ifdef NEW_JUMPMU
         auto timeDiffIncWait = mean::tscDifferenceUs(now, jumpmu::thread_local_jumpmu.tx_start_time);
#else
         auto timeDiffIncWait = mean::tscDifferenceUs(now, jumpmu::thread_local_jumpmu_ctx->tx_start_time);
#endif
         if (txtype == 0) {
            WorkerCounters::myCounters().total_tx_time += timeDiffIncWait;
            WorkerCounters::myCounters().tx_latency_hist.increaseSlot(timeDiffIncWait);
            WorkerCounters::myCounters().tx++;
         } else {
            WorkerCounters::myCounters().total_ltx_time += timeDiffIncWait;                      // / 1000
            WorkerCounters::myCounters().tx_latency_hist_incwait.increaseSlot(timeDiffIncWait);  // / 1000
            WorkerCounters::myCounters().ltx++;
         }
         running_threads_counter--;
         mean::task::yield();
      };
      BlockedRange bb(0, (u64)1000000000000ul);
      auto startTsc = mean::readTSC();
      auto startTP = mean::getTimePoint();
      mean::task::parallelFor(bb, ycsb_tx, FLAGS_worker_tasks, 100000, true);
      auto diffTSC = mean::tscDifferenceNs(mean::readTSC(), startTsc) / 1e9;
      auto diffTP = mean::timePointDifference(mean::getTimePoint(), startTP) / 1e9;
      std::cout << "done: time: " << diffTP << " tsc: " << diffTSC << std::endl;
   }
#ifndef NEW_JUMPMU
   delete jumpmu::thread_local_jumpmu_ctx;
#endif
   mean::env::shutdown();
   cout << "-------------------------------------------------------------------------------------" << endl;
   // -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
   std::cout << "run process1" << std::endl;

   gflags::SetUsageMessage("Leanstore Frontend");
   gflags::ParseCommandLineFlags(&argc, &argv, true);
   // -------------------------------------------------------------------------------------
   // -------------------------------------------------------------------------------------
   using namespace mean;
   IoOptions ioOptions("auto", FLAGS_ssd_path);
   ioOptions.write_back_buffer_size = PAGE_SIZE;
   ioOptions.engine = FLAGS_ioengine;
   ioOptions.ioUringPollMode = FLAGS_io_uring_poll_mode;
   ioOptions.ioUringShareWq = FLAGS_io_uring_share_wq;
   ioOptions.raid5 = FLAGS_raid5;
#ifndef MEAN_USE_TASKING
   ioOptions.iodepth = 2048 + 512;
#else
   ioOptions.iodepth = (FLAGS_async_batch_size + FLAGS_worker_tasks) * 2;  // hacky, how to take into account for remotes
#endif  // -------------------------------------------------------------------------------------
   if (FLAGS_nopp) {
      ioOptions.channelCount = FLAGS_worker_threads;
      mean::env::init(FLAGS_worker_threads,  // std::min(std::thread::hardware_concurrency(), FLAGS_tpcc_warehouse_count),
                      0 /*FLAGS_pp_threads*/, ioOptions);
   } else {
      std::cout << "init" << std::endl;
#ifdef MEAN_USE_JOBBING
      ioOptions.channelCount = 1;  // FLAGS_worker_threads + FLAGS_pp_threads;
#else
      ioOptions.channelCount = FLAGS_worker_threads + FLAGS_pp_threads;
#endif
      mean::env::init(FLAGS_worker_threads, FLAGS_pp_threads, ioOptions);
      std::cout << "init finished" << std::endl;
   }
   std::cout << "run process2" << std::endl;
   mean::env::start(run_ycsb);
   // -------------------------------------------------------------------------------------
   mean::env::join();
   // -------------------------------------------------------------------------------------
   return 0;
}
