#include "CRTable.hpp"
#include <limits>

#include "leanstore/Config.hpp"
#include "leanstore/profiling/counters/CRCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/ThreadLocalAggregator.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using leanstore::utils::threadlocal::sum;
namespace leanstore
{
namespace profiling
{
// -------------------------------------------------------------------------------------
std::string CRTable::getName()
{
   return "cr";
}
// -------------------------------------------------------------------------------------
void CRTable::open()
{
   columns.emplace("key", [&](Column& out) { out << 0; });
   columns.emplace("wal_reserve_blocked", [&](Column& col) { col << (sum(CRCounters::cr_counters, &CRCounters::wal_reserve_blocked)); });
   columns.emplace("wal_reserve_immediate", [&](Column& col) { col << (sum(CRCounters::cr_counters, &CRCounters::wal_reserve_immediate)); });
   columns.emplace("gct_phase_1_pct", [&](Column& col) { col << 100.0 * p1 / total; });
   columns.emplace("gct_phase_2_pct", [&](Column& col) { col << 100.0 * p2 / total; });
   columns.emplace("gct_write_pct", [&](Column& col) { col << 100.0 * write / total; });
   columns.emplace("gct_committed_tx", [&](Column& col) { col << sum(CRCounters::cr_counters, &CRCounters::gct_committed_tx); });
   columns.emplace("gct_rounds", [&](Column& col) { col << sum(CRCounters::cr_counters, &CRCounters::gct_rounds); });
   columns.emplace("tx", [&](Column& col) { col << local_tx; });
   columns.emplace("ltx", [&](Column& col) { col << local_ltx; });
   columns.emplace("setup_tx", [&](Column& col) { col << local_setup_tx; });
   columns.emplace("tx_rate", [&](Column& col) { col << FLAGS_tx_rate; });
   columns.emplace("tx_abort", [](Column& col) { col << sum(WorkerCounters::worker_counters, &WorkerCounters::tx_abort); });
   // -------------------------------------------------------------------------------------
   columns.emplace("tx_avg_runtime_us",
                   [&](Column& col) { col << (local_tx > 0 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_tx_time) / local_tx : 0); });
    columns.emplace("ltx_latency_us",
                    [&](Column& col) { col << (local_ltx > 0 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_ltx_time) / local_ltx : 0); });
   
    columns.emplace("tx_setup_latency_us",
                    [&](Column& col) { col << (local_setup_tx > 0 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_setup_tx_time) / local_setup_tx : 0); });

    columns.emplace("tx_wait_latency_us",
        [&](Column& col) { col << (local_thread_tx > 0 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_wait_tx_time) / local_thread_tx : 0); });
    
    //  columns.emplace("time_counter_0",
    //      [&](Column& col) { col << local_time_counter_0 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_time_sum_0) / local_time_counter_0 : 0); });

     columns.emplace("time_counter_0",
       [&](Column& col) { col << local_time_counter_0; });
    
    columns.emplace("time_counter_1",
    //     [&](Column& col) { col << (local_time_counter_1 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_time_sum_1) / local_time_counter_1 : 0); });
       [&](Column& col) { col << local_time_counter_1; });
          
    
     columns.emplace("time_counter_2",
     //   [&](Column& col) { col << (local_time_counter_2 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_time_sum_2) / local_time_counter_2 : 0); });
      [&](Column& col) { col << local_time_counter_2; });
    
    columns.emplace("time_counter_3",
        [&](Column& col) { col << (local_time_counter_3 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_time_sum_3) / local_time_counter_3 : 0); });
              // [&](Column& col) { col << local_time_counter_3; });

                            
                   // -------------------------------------------------------------------------------------
   columns.emplace("tx_latency_us_10p", [&](Column& col) { col << local_tx_lat10p_us; });
   columns.emplace("tx_latency_us_25p", [&](Column& col) { col << local_tx_lat25p_us; });
   columns.emplace("tx_latency_us_50p", [&](Column& col) { col << local_tx_lat50p_us; });
   columns.emplace("tx_latency_us_95p", [&](Column& col) { col << local_tx_lat95p_us; });
   columns.emplace("tx_latency_us_99p", [&](Column& col) { col << local_tx_lat99p_us; });
   columns.emplace("tx_latency_us_99p9", [&](Column& col) { col << local_tx_lat99p9_us; });
   columns.emplace("tx_latency_us_99p99", [&](Column& col) { col << local_tx_lat99p99_us; });
   // -------------------------------------------------------------------------------------
   columns.emplace("tx_latency_us_inc_wait", [&](Column& col) {
      col << (local_tx > 0 ? sum(WorkerCounters::worker_counters, &WorkerCounters::total_tx_time_inc_wait) / local_tx : 0);
   });
   
   
   // avg runtime
   columns.emplace("tx_p99_runtime_us", [&](Column& col) { col << local_tx_lat99p_us; });

   columns.emplace("tx_latency_us_10pi", [&](Column& col) { col << local_tx_lat10pi_us; });
   columns.emplace("tx_latency_us_25pi", [&](Column& col) { col << local_tx_lat25pi_us; });
   columns.emplace("tx_latency_us_50pi", [&](Column& col) { col << local_tx_lat50pi_us; });
   columns.emplace("tx_latency_us_95pi", [&](Column& col) { col << local_tx_lat95pi_us; });
   columns.emplace("tx_latency_us_99pi", [&](Column& col) { col << local_tx_lat99pi_us; });
   columns.emplace("tx_latency_us_99pi9", [&](Column& col) { col << local_tx_lat99pi9_us; });
   columns.emplace("tx_latency_us_99pi99", [&](Column& col) { col << local_tx_lat99pi99_us; });
   // -------------------------------------------------------------------------------------
   columns.emplace("ssd_read_latency_us_50p", [&](Column& col) { col << local_ssd_read_lat50p_us; });
   columns.emplace("ssd_read_latency_us_99p", [&](Column& col) { col << local_ssd_read_lat99p_us; });
   columns.emplace("ssd_read_latency_us_99p9", [&](Column& col) { col << local_ssd_read_lat99p9_us; });
   columns.emplace("ssd_read_latency_us_99p99", [&](Column& col) { col << local_ssd_read_lat99p99_us; });
   // -------------------------------------------------------------------------------------
   columns.emplace("ssd_write_latency_us_50p", [&](Column& col) { col << local_ssd_write_lat50p_us; });
   columns.emplace("ssd_write_latency_us_99p", [&](Column& col) { col << local_ssd_write_lat99p_us; });
   columns.emplace("ssd_write_latency_us_99p9", [&](Column& col) { col << local_ssd_write_lat99p9_us; });
   columns.emplace("ssd_write_latency_us_99p99", [&](Column& col) { col << local_ssd_write_lat99p99_us; });
   // -------------------------------------------------------------------------------------
   columns.emplace("wal_read_gib", [&](Column& col) {
      col << (sum(WorkerCounters::worker_counters, &WorkerCounters::wal_read_bytes) * 1.0) / 1024.0 / 1024.0 / 1024.0;
   });
   columns.emplace("wal_write_gib",
                   [&](Column& col) { col << (sum(CRCounters::cr_counters, &CRCounters::gct_write_bytes) * 1.0) / 1024.0 / 1024.0 / 1024.0; });
   columns.emplace("wal_miss_pct", [&](Column& col) { col << wal_miss_pct; });
   columns.emplace("wal_hit_pct", [&](Column& col) { col << wal_hit_pct; });
   columns.emplace("wal_miss", [&](Column& col) { col << wal_miss; });
   columns.emplace("wal_hit", [&](Column& col) { col << wal_hits; });
   columns.emplace("wal_total", [&](Column& col) { col << wal_total; });
}
// -------------------------------------------------------------------------------------
template <typename CountersClass, typename FieldAccessor>
u64 getPercentileOfField(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, FieldAccessor field_accessor, float percentile)
{
   u64 max = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
        if (counters[t]) {
            max = std::max(max, field_accessor(*counters[t]).getPercentile(percentile));
        }
   }
   return max;
   /*j
   int s = worker_counters.size();
    std::vector<u64> med(s);

    int idx = 0;
    for (typename Container::iterator i = worker_counters.begin(); idx < s; ++i) {
        med.at(idx++) = field_accessor(*i).getPercentile(percentile);
    }

    auto m = med.begin() + med.size() / 2;
    std::nth_element(med.begin(), m, med.end());

    return *m;
    */
}
// -------------------------------------------------------------------------------------
void CRTable::next()
{
   wal_hits = sum(WorkerCounters::worker_counters, &WorkerCounters::wal_buffer_hit);
   wal_miss = sum(WorkerCounters::worker_counters, &WorkerCounters::wal_buffer_miss);
   wal_total = wal_hits + wal_miss;
   wal_hit_pct = wal_hits * 1.0 / wal_total;
   wal_miss_pct = wal_miss * 1.0 / wal_total;
   // -------------------------------------------------------------------------------------
   p1 = sum(CRCounters::cr_counters, &CRCounters::gct_phase_1_ms);
   p2 = sum(CRCounters::cr_counters, &CRCounters::gct_phase_2_ms);
   write = sum(CRCounters::cr_counters, &CRCounters::gct_write_ms);
   total = p1 + p2 + write;

   local_tx = sum(WorkerCounters::worker_counters, &WorkerCounters::tx);
   local_ltx = sum(WorkerCounters::worker_counters, &WorkerCounters::ltx);
   local_setup_tx = sum(WorkerCounters::worker_counters, &WorkerCounters::setup_tx);
   local_thread_tx = sum(WorkerCounters::worker_counters, &WorkerCounters::wait_tx);
   local_time_counter_0 = sum(WorkerCounters::worker_counters, &WorkerCounters::time_counter_0);
   local_time_counter_1 = sum(WorkerCounters::worker_counters, &WorkerCounters::time_counter_1);
   local_time_counter_2 = sum(WorkerCounters::worker_counters, &WorkerCounters::time_counter_2);
   local_time_counter_3 = sum(WorkerCounters::worker_counters, &WorkerCounters::time_counter_3);
   local_tx_lat99p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 99.0);
    local_tx_lat99pi_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 99.0);
   /* 
   // lat10p = getPercentileOfField(WorkerCounters::worker_counters, [](const WorkerCounters &wc) -> const auto& { return wc.tx_latency_hist; }, 10);
   local_tx_lat10p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 10);
   local_tx_lat25p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 25);
   local_tx_lat50p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 50);
   local_tx_lat95p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 95);
   local_tx_lat99p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 99);
   local_tx_lat99p9_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 99.9);
   local_tx_lat99p99_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist); }, 99.99);

   local_tx_lat10pi_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 10);
   local_tx_lat25pi_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 25);
   local_tx_lat50pi_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 50);
   local_tx_lat95pi_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 95);
   local_tx_lat99pi_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 99);
   local_tx_lat99pi9_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 99.9);
   local_tx_lat99pi99_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.tx_latency_hist_incwait); }, 99.99);

   local_ssd_read_lat50p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_read_latency); }, 50);
   local_ssd_read_lat99p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_read_latency); }, 99);
   local_ssd_read_lat99p9_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_read_latency); }, 99.9);
   local_ssd_read_lat99p99_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_read_latency); }, 99.99);

   local_ssd_write_lat50p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_write_latency); }, 50);
   local_ssd_write_lat99p_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_write_latency); }, 99);
   local_ssd_write_lat99p9_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_write_latency); }, 99.9);
   local_ssd_write_lat99p99_us = getPercentileOfField(
       WorkerCounters::worker_counters,
       [](const WorkerCounters& wc) -> auto& { return const_cast<Hist<int, long unsigned int>&>(wc.ssd_write_latency); }, 99.99);

   // for (typename decltype(WorkerCounters::worker_counters)::iterator i = WorkerCounters::worker_counters.begin(); i !=
   // WorkerCounters::worker_counters.end(); ++i) {*/
   int counters = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (WorkerCounters::worker_counters[t].load()) {
         WorkerCounters::worker_counters[t].load()->tx_latency_hist.resetData();
         WorkerCounters::worker_counters[t].load()->tx_latency_hist_incwait.resetData();
         WorkerCounters::worker_counters[t].load()->ssd_read_latency.resetData();
         WorkerCounters::worker_counters[t].load()->ssd_write_latency.resetData();
         counters++;
      }
   } 
 

   clear();
   for (auto& c : columns) {
      c.second.generator(c.second);
   }
}
// -------------------------------------------------------------------------------------
}  // namespace profiling
}  // namespace leanstore
