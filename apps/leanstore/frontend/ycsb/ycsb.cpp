#include "Time.hpp"
#include "Units.hpp"
#include "leanstore/BTreeAdapter.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/profiling/counters/ThreadCounters.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/Files.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
// -------------------------------------------------------------------------------------
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/io/IoInterface.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
// -------------------------------------------------------------------------------------
#include <iostream>
#include <set>
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
using namespace leanstore;
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using YCSBKey = u64;
using YCSBPayload = BytesPayload<120>;
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
double calculateMTPS(chrono::high_resolution_clock::time_point begin, chrono::high_resolution_clock::time_point end, u64 factor)
{
   double tps = ((factor * 1.0 / (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0)));
   return (tps / 1000000.0);
}
// -------------------------------------------------------------------------------------
void run_ycsb() {
   // -------------------------------------------------------------------------------------
   chrono::high_resolution_clock::time_point begin, end;
   // -------------------------------------------------------------------------------------
   // LeanStore DB
   LeanStore db;
   unique_ptr<BTreeInterface<YCSBKey, YCSBPayload>> adapter;
   mean::task::scheduleTaskSync([&]() {
         auto& vs_btree = db.registerBTreeLL("ycsb", "y");
         adapter.reset(new BTreeVSAdapter<YCSBKey, YCSBPayload>(vs_btree));
         db.registerConfigEntry("ycsb_read_ratio", FLAGS_ycsb_read_ratio);
         db.registerConfigEntry("ycsb_target_gib", FLAGS_target_gib);
   });
   // -------------------------------------------------------------------------------------
   auto& table = *adapter;
   const u64 ycsb_tuple_count = (FLAGS_ycsb_tuple_count)
                                    ? FLAGS_ycsb_tuple_count
                                    : FLAGS_target_gib * 1024 * 1024 * 1024 * 1.0 / 2.0 / (sizeof(YCSBKey) + sizeof(YCSBPayload));
   // Insert values
   {
      db.startProfilingThread();
      const u64 n = ycsb_tuple_count;
      cout << "-------------------------------------------------------------------------------------" << endl;
      cout << "Inserting values" << endl;
      begin = chrono::high_resolution_clock::now();

      // Atomic counter for block coordination
      std::atomic<u64> current_block_start{0};
      const u64 block_size = 1 << 18; // Adjust granularity as needed
      const u64 total_items = n;

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
               YCSBPayload payload;
               utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
               auto& key = t;
               table.insert(key, payload);
               
               YCSBPayload result; /// FIXME remove this check
               table.lookup(t, result);
               ensure(result == payload);
            }
         }
      };

      BlockedRange bb(0, (u64)1);
      ensure((bool)((bb.end - bb.begin) >= 1));
      mean::task::parallelFor(bb, ycsb_insert_fun, FLAGS_worker_tasks, false);
#else
#ifdef NEW_JUMPMU
            jumpmu::thread_local_jumpmu.pid = -2; 
#else
      // jumpmu::thread_local_jumpmu_ctx = new jumpmu::JumpMUContext(); 
      jumpmu::thread_local_jumpmu_ctx->pid = -2; 
#endif 

      // auto ycsb_insert_fun = [&](u64 t_i, std::atomic<bool>&) {
      for (int i = 0; i < n; i++) {
         // vector<u64> keys(range.size());
         // std::iota(keys.begin(), keys.end(), range.begin());
         // std::random_shuffle(keys.begin(), keys.end());
         YCSBPayload payload;
         utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
         auto& key = i;
         table.insert(key, payload);
         YCSBPayload result; /// FIXME remove this check
         // table.lookup(i, result);
         // ensure(result == payload);

         // mean::task::yield();
      }
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
   // Scan
   // -------------------------------------------------------------------------------------
   cout << "-------------------------------------------------------------------------------------" << endl;
   cout << "~Transactions" << endl;
   atomic<bool> keep_running = {true};
   atomic<u64> running_threads_counter = {0};
   {
      auto start = mean::getSeconds();

      auto ycsb_tx = [&](){
         running_threads_counter++;

            auto before = mean::readTSC();
            YCSBKey key = zipf_random->rand();
 
            assert(key < ycsb_tuple_count);
            YCSBPayload result;
            if (FLAGS_ycsb_read_ratio == 100 || utils::RandomGenerator::getRandU64(0, 100) < FLAGS_ycsb_read_ratio) {
               table.lookup(key, result);
            } else {
               YCSBPayload payload;
               utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
               table.update(key, payload);
            }
           auto now = mean::readTSC();
#ifdef NEW_JUMPMU
           auto timeDiff = mean::tscDifferenceUs(now, jumpmu::thread_local_jumpmu.tx_start_time);
#else
           auto timeDiff = mean::tscDifferenceUs(now, jumpmu::thread_local_jumpmu_ctx->tx_start_time);
#endif           // trace_finish_transaction(i); 
           auto timeDiffIncWait = mean::tscDifferenceUs(now, before);
           WorkerCounters::myCounters().total_tx_time += timeDiffIncWait;
           WorkerCounters::myCounters().tx_latency_hist.increaseSlot(timeDiffIncWait);
           WorkerCounters::myCounters().tx_latency_hist_incwait.increaseSlot(timeDiff);
           // if (timeDiffIncWait < 10000000) {
           //   WorkerCounters::myCounters().total_tx_time_inc_wait += timeDiffIncWait;
           // }
           // WorkerCounters::myCounters().tx_latency_hist_incwait.increaseSlot(timeDiffIncWait);
           WorkerCounters::myCounters().tx++;
           // ThreadCounters::myCounters().tx++;
           mean::task::yield();
            running_threads_counter--;
      };
      BlockedRange bb(0, (u64)1000000000000ul);
      auto startTsc = mean::readTSC();
      auto startTP = mean::getTimePoint();
      mean::task::parallelFor(bb, ycsb_tx, FLAGS_worker_tasks, 100000, true);
      auto diffTSC = mean::tscDifferenceNs(mean::readTSC(), startTsc) / 1e9;
      auto diffTP = mean::timePointDifference(mean::getTimePoint(), startTP) / 1e9;
      std::cout << "done: time: " << diffTP << " tsc: " << diffTSC << std::endl;
   }
   mean::env::shutdown();
   cout << "-------------------------------------------------------------------------------------" << endl;
   // -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
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
   ioOptions.iodepth = (FLAGS_async_batch_size + FLAGS_worker_tasks)*2; // hacky, how to take into account for remotes 
   // -------------------------------------------------------------------------------------
   if (FLAGS_nopp) {
      ioOptions.channelCount = FLAGS_worker_threads;
      mean::env::init(
         FLAGS_worker_threads, //std::min(std::thread::hardware_concurrency(), FLAGS_tpcc_warehouse_count),
         0/*FLAGS_pp_threads*/, ioOptions);
   } else {
      ioOptions.channelCount = 3; // FLAGS_worker_threads + FLAGS_pp_threads;
      mean::env::init(FLAGS_worker_threads, FLAGS_pp_threads, ioOptions);
   }
   mean::env::start(run_ycsb);
   // -------------------------------------------------------------------------------------
   mean::env::join();
   // -------------------------------------------------------------------------------------
   return 0;
}
