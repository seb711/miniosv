#include "LeanStore.hpp"
#include <emmintrin.h>

#include "Time.hpp"
#include "leanstore/io/IoChannel.hpp"
#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/profiling/counters/PPCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/profiling/tables/BMTable.hpp"
#include "leanstore/profiling/tables/CPUTable.hpp"
#include "leanstore/profiling/tables/CRTable.hpp"
#include "leanstore/profiling/tables/DTTable.hpp"
#include "leanstore/profiling/tables/SSDTable.hpp"
#include "leanstore/profiling/tables/ThreadTable.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"
#include "leanstore/storage/buffer-manager/Swip.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/ThreadLocalAggregator.hpp"
// -------------------------------------------------------------------------------------
#include "leanstore/Config.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "tabulate/table.hpp"
// -------------------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
// -------------------------------------------------------------------------------------
#include <osv/sched.hh>
// -------------------------------------------------------------------------------------
using namespace tabulate;
using leanstore::utils::threadlocal::sum;
namespace leanstore
{
// -------------------------------------------------------------------------------------
LeanStore::LeanStore()
{
   // Set the default logger to file logger
   // -------------------------------------------------------------------------------------
   buffer_manager = make_unique<storage::BufferManager>();
   BMC::global_bf = buffer_manager.get();
   DTRegistry::global_dt_registry.registerDatastructureType(0, storage::btree::BTreeLL::getMeta());
   DTRegistry::global_dt_registry.registerDatastructureType(1, storage::btree::BTreeVW::getMeta());
   DTRegistry::global_dt_registry.registerDatastructureType(2, storage::btree::BTreeVI::getMeta());
   // -------------------------------------------------------------------------------------
   // u64 end_of_block_device;
   // if (FLAGS_wal_offset_gib == 0) {
   //    end_of_block_device = mean::IoInterface::instance().storageSize();
   // } else {
   //    end_of_block_device = FLAGS_wal_offset_gib * 1024 * 1024 * 1024;
   // }
   // cr_manager = make_unique<cr::CRManager>(ssd_fd, end_of_block_device);
   // cr::CRManager::global = cr_manager.get();
}  // namespace leanstore
// -------------------------------------------------------------------------------------
LeanStore::~LeanStore()
{
   bg_threads_keep_running = false;
   while (bg_threads_counter) {
      MYPAUSE();
   }
   //  close(ssd_fd);
}
// -------------------------------------------------------------------------------------
void LeanStore::printObjStats() {
   int sample_size = 1000;

   std::map<DTID, int> leafTypes;
   std::map<DTID, int> innerTypes;
   std::map<int, int> states;
   for (auto& t: DTRegistry::global_dt_registry.dt_instances_ht) {
      leafTypes[t.first] = 0;
      innerTypes[t.first] = 0;
   }
   for (int i = 0; i < (int)BufferFrame::STATE::COUNT; i++) {
      leafTypes[i] = 0;
   }

   for (int i = 0; i < sample_size; i++) {
      auto& bf =  buffer_manager->randomBufferFrame();
      // this cast is a bit... hacky
      auto& c_node = *reinterpret_cast<storage::btree::BTreeNode*>(bf.page.dt);
      if (c_node.is_leaf) {
         leafTypes[bf.page.dt_id]++;
      } else {
         innerTypes[bf.page.dt_id]++;
      }
      states[(int)bf.header.state]++;
   }
   std::cout << "";
   for (auto& h: innerTypes) {
      int percent = (int)((float)h.second / sample_size * 100);
      for (int i = 0; i < percent; i++) {
         std::cout << (char)std::toupper(std::get<2>(DTRegistry::global_dt_registry.dt_instances_ht[h.first])[0]);
      }
   }
   std::cout << "|";
   for (auto& h: leafTypes) {
      int percent = (int)((float)h.second / sample_size * 100);
      for (int i = 0; i < percent; i++) {
         std::cout << std::get<3>(DTRegistry::global_dt_registry.dt_instances_ht[h.first]) ;
      }
   }
   std::cout << std::endl;
   for (int s = 0; s < (int)BufferFrame::STATE::COUNT; s++) {
      int percent = (int)((float)states[s] / sample_size * 100);
      for (int i = 0; i < percent; i++) {
         std::cout << s;
      }
   }
   std::cout << std::endl;
}
// -------------------------------------------------------------------------------------
void LeanStore::startProfilingThread()
{
   // Pin the profiling thread to its dedicated core *at creation* via
   // sched::thread attr().pin(), instead of starting it on CPU0 and re-pinning
   // at runtime with pthread_setaffinity_np. A runtime self-migration only moves
   // the thread's logical CPU; it keeps physically running on CPU0, so its 1s
   // sleep timer is armed in the target core's timer_list while CPU0's LAPIC is
   // what fires -- the timer is never serviced and the thread never wakes.
#ifdef MEAN_USE_JOBBING
   const int profiling_cpu = 2;
#else
   const int profiling_cpu = FLAGS_worker_threads + 1;
#endif
   ensure(profiling_cpu >= 0 && (size_t)profiling_cpu < sched::cpus.size());
   auto profiling_body = [this]() {
      // posix_check(pthread_setname_np(pthread_self(), "profiling") == 0);
      // -------------------------------------------------------------------------------------
      profiling::BMTable bm_table(*buffer_manager.get());
      profiling::DTTable dt_table(*buffer_manager.get());
      profiling::CPUTable cpu_table;
      profiling::CRTable cr_table;
      profiling::SSDTable ssd_table;
      profiling::ThreadTable thread_table;
      std::vector<profiling::ProfilingTable*> tables = {&configs_table, &bm_table, &dt_table, &cpu_table, &cr_table, &ssd_table, &thread_table};
      // -------------------------------------------------------------------------------------
      // CSV-to-file output removed: OSv's libc++ is built without <fstream>.
      for (u64 t_i = 0; t_i < tables.size(); t_i++) {
         tables[t_i]->open();
      }
      // -------------------------------------------------------------------------------------
      config_hash = configs_table.hash();
      // -------------------------------------------------------------------------------------
      u64 seconds = 0;
      auto lastTime = mean::getTimePoint();
      auto lastTimePrint = mean::getTimePoint();
      while (bg_threads_keep_running) {
         for (u64 t_i = 0; t_i < tables.size(); t_i++) {
            tables[t_i]->next();
            if (tables[t_i]->size() == 0)
               continue;
            // -------------------------------------------------------------------------------------
            // CSV-to-file output removed (no <fstream> on OSv).
            // TODO: Websocket, CLI
         }
         // -------------------------------------------------------------------------------------
         const u64 tx = std::stoi(cr_table.get("0", "tx"));
         // Global Stats
         global_stats.accumulated_tx_counter += tx;
         // -------------------------------------------------------------------------------------
         // Console
         // -------------------------------------------------------------------------------------
         const double instr_per_tx = cpu_table.workers_agg_events["instr"] / tx;
         const double cycles_per_tx = cpu_table.workers_agg_events["cycle"] / tx;
         const double l1_per_tx = cpu_table.workers_agg_events["L1-miss"] / tx;
         // using RowType = std::vector<variant<std::string, const char*, Table>>;
         if (FLAGS_print_tx_console) {

            
            
            tabulate::Table table;
                        table.add_row({"t", "wt", "RT [AVG]", "RT [P99]","TX P [M]", "LAT [AVG]", "LAT [P99]" ,"LTX P [M]", "TXS P [M]", "C0", "C1", "C2", "C3", "W MiB", "R MiB"});
            table.add_row({std::to_string(seconds),
                           std::to_string(mean::timePointDifferenceMs(mean::getTimePoint(), lastTimePrint)/(float)1000),
                           cr_table.get("0", "tx_avg_runtime_us"), cr_table.get("0", "tx_p99_runtime_us"), std::to_string(stol(cr_table.get("0", "tx"))/(float)1000/1000),cr_table.get("0", "ltx_latency_us"), cr_table.get("0", "tx_latency_us_99pi"),std::to_string(stol(cr_table.get("0", "ltx"))/(float)1000/1000), std::to_string(stol(cr_table.get("0", "setup_tx"))/(float)1000/1000),cr_table.get("0", "time_counter_0"), cr_table.get("0", "time_counter_1"), cr_table.get("0", "time_counter_2") , cr_table.get("0", "time_counter_3"),
                           bm_table.get("0", "w_mib"), bm_table.get("0", "r_mib")});
            lastTimePrint = mean::getTimePoint();
            // -------------------------------------------------------------------------------------
            table.format().width(10);
            table.column(0).format().width(6);
            table.column(1).format().width(10);
            table.column(2).format().width(10);
            // -------------------------------------------------------------------------------------
            auto print_table = [](tabulate::Table& table, std::function<bool(u64)> predicate) {
               std::stringstream ss;
               table.print(ss);
               string str = ss.str();
               u64 line_n = 0;
               for (u64 i = 0; i < str.size(); i++) {
                  if (str[i] == '\n') {
                     line_n++;
                  }
                  if (predicate(line_n)) {
                     cout << str[i];
                  }
               }
            };
            if (seconds == 0) {
               print_table(table, [](u64 line_n) { return (line_n < 3) || (line_n == 4); });
            } else {
               print_table(table, [](u64 line_n) { return line_n == 4; });
            }
            // -------------------------------------------------------------------------------------
            if (FLAGS_print_obj_stats) {
               std::cout << std::endl;
               printObjStats();
            }
         }
         auto now = mean::getTimePoint();
         auto diff = mean::timePointDifferenceUs(now, lastTime);
         // diff is unsigned: if an iteration overruns the 1s period (or the
         // monotonic clock reads backwards, making diff huge), `1000*1000 - diff`
         // would underflow into a near-2^64 value and sleep_for() would park this
         // thread for thousands of years. Clamp so we never sleep more than 1s.
         const u64 period_us = 1000 * 1000;
         const u64 sleep_us = diff < period_us ? period_us - diff : 0;
         std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
         seconds += 1;
         std::locale::global(std::locale::classic());
         lastTime = mean::getTimePoint();
      }
      bg_threads_counter--;
   };
   bg_threads_counter++;
   // Detached, pinned-at-creation OSv thread (self-reclaims on exit).
   sched::thread::make(profiling_body,
                       sched::thread::attr().name("profiling")
                           .pin(sched::cpus[profiling_cpu]).detached())
       ->start();
}
// -------------------------------------------------------------------------------------
storage::btree::BTreeLL& LeanStore::registerBTreeLL(string name, string short_name)
{
   assert(btrees_ll.find(name) == btrees_ll.end());
   auto& btree = btrees_ll[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(0, reinterpret_cast<void*>(&btree), name, short_name);
   auto& bf = buffer_manager->allocatePage();
   Guard guard(bf.header.latch, GUARD_STATE::EXCLUSIVE);
   bf.header.keep_in_memory = true;
   bf.page.dt_id = dtid;
   guard.unlock();
   btree.create(dtid, &bf);
   return btree;
}

// -------------------------------------------------------------------------------------
storage::btree::BTreeVW& LeanStore::registerBTreeVW(string name, string short_name)
{
   assert(btrees_vw.find(name) == btrees_vw.end());
   auto& btree = btrees_vw[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(1, reinterpret_cast<void*>(&btree), name, short_name);
   auto& bf = buffer_manager->allocatePage();
   Guard guard(bf.header.latch, GUARD_STATE::EXCLUSIVE);
   bf.header.keep_in_memory = true;
   bf.page.dt_id = dtid;
   guard.unlock();
   btree.create(dtid, &bf);
   return btree;
}
// -------------------------------------------------------------------------------------
storage::btree::BTreeVI& LeanStore::registerBTreeVI(string name, string short_name)
{
   assert(btrees_vi.find(name) == btrees_vi.end());
   auto& btree = btrees_vi[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(2, reinterpret_cast<void*>(&btree), name, short_name);
   auto& bf = buffer_manager->allocatePage();
   Guard guard(bf.header.latch, GUARD_STATE::EXCLUSIVE);
   bf.header.keep_in_memory = true;
   bf.page.dt_id = dtid;
   guard.unlock();
   btree.create(dtid, &bf);
   return btree;
}
// -------------------------------------------------------------------------------------
u64 LeanStore::getConfigHash()
{
   return config_hash;
}
// -------------------------------------------------------------------------------------
LeanStore::GlobalStats LeanStore::getGlobalStats()
{
   return global_stats;
}
// -------------------------------------------------------------------------------------
void LeanStore::persist()
{
   // TODO
}
// -------------------------------------------------------------------------------------
void LeanStore::restore()
{
   // TODO
}
// -------------------------------------------------------------------------------------
}  // namespace leanstore
// -------------------------------------------------------------------------------------
