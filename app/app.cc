/*
 * LeanStore YCSB benchmark for OSv.
 *
 * Fixed configuration:
 *   MEAN_TYPE   = MEAN_USE_TASKING
 *   dram_gib    = 1.0
 *   target_gib  = 1.0
 *   threads     = 1  (worker_threads + worker_tasks)
 *   io engine   = osv
 *   ssd_path    = /dev/vda  (first NVMe device exposed by OSv NVMe driver)
 */

#include "leanstore/BTreeAdapter.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/Files.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/io/IoInterface.hpp"
#include "Time.hpp"
#include "Units.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <set>
#include <cstdio>

// YCSB-specific flags not declared in leanstore/Config.hpp
DEFINE_uint32(ycsb_read_ratio, 100, "percentage of read operations");
DEFINE_uint64(ycsb_tuple_count, 0, "number of tuples; 0 = derived from target_gib");
DEFINE_uint32(ycsb_payload_size, 120, "tuple payload size in bytes");

using namespace leanstore;

using YCSBKey = u64;
using YCSBPayload = BytesPayload<120>;

static double calculateMTPS(chrono::high_resolution_clock::time_point begin,
                             chrono::high_resolution_clock::time_point end,
                             u64 factor)
{
    double tps = (factor * 1.0 /
                  (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0));
    return tps / 1000000.0;
}

static void run_ycsb()
{
    chrono::high_resolution_clock::time_point begin, end;

    LeanStore db;
    unique_ptr<BTreeInterface<YCSBKey, YCSBPayload>> adapter;
    mean::task::scheduleTaskSync([&]() {
        auto& vs_btree = db.registerBTreeLL("ycsb", "y");
        adapter.reset(new BTreeVSAdapter<YCSBKey, YCSBPayload>(vs_btree));
        db.registerConfigEntry("ycsb_read_ratio", FLAGS_ycsb_read_ratio);
        db.registerConfigEntry("ycsb_target_gib", FLAGS_target_gib);
    });

    auto& table = *adapter;
    const u64 ycsb_tuple_count =
        FLAGS_target_gib * 1024.0 * 1024.0 * 1024.0 / 2.0 /
        (sizeof(YCSBKey) + sizeof(YCSBPayload));

    // Insert phase
    {
        db.startProfilingThread();
        const u64 n = ycsb_tuple_count;
        printf("Inserting %lu tuples\n", n);
        begin = chrono::high_resolution_clock::now();

        std::atomic<u64> current_block_start{0};
        const u64 block_size = 1 << 18;

        auto ycsb_insert_fun = [&]() {
            while (true) {
                u64 block_start = current_block_start.fetch_add(block_size, std::memory_order_relaxed);
                if (block_start >= n) break;
                u64 block_end = std::min(block_start + block_size, n);
                for (u64 t = block_start; t < block_end; t++) {
                    YCSBPayload payload;
                    utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
                    table.insert(t, payload);
                    YCSBPayload result;
                    table.lookup(t, result);
                    ensure(result == payload);
                }
            }
        };

        BlockedRange bb(0, (u64)1);
        mean::task::parallelFor(bb, ycsb_insert_fun, FLAGS_worker_tasks, false);

        end = chrono::high_resolution_clock::now();
        printf("Insert time: %.3f s  (%.2f M tps)\n",
               chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0,
               calculateMTPS(begin, end, n));
        const u64 written_pages = db.getBufferManager().consumedPages();
        printf("Inserted: %lu pages (%lu MiB)\n",
               written_pages, written_pages * PAGE_SIZE / 1024 / 1024);
    }

    // Transaction phase
    auto zipf_random = std::make_unique<utils::ScrambledZipfGenerator>(
        0, ycsb_tuple_count, FLAGS_zipf_factor);

    printf("Starting YCSB transactions (read_ratio=%u, run_for=%lu s)\n",
           FLAGS_ycsb_read_ratio, FLAGS_run_for_seconds);

    {
        auto start = mean::getSeconds();

        auto ycsb_tx = [&]() {
            auto before = mean::readTSC();
            YCSBKey key = zipf_random->rand();
            assert(key < ycsb_tuple_count);
            YCSBPayload result;
            if (FLAGS_ycsb_read_ratio == 100 ||
                utils::RandomGenerator::getRandU64(0, 100) < FLAGS_ycsb_read_ratio) {
                table.lookup(key, result);
            } else {
                YCSBPayload payload;
                utils::RandomGenerator::getRandString(
                    reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
                table.update(key, payload);
            }
            auto now = mean::readTSC();
            auto timeDiff = mean::tscDifferenceUs(now, before);
            WorkerCounters::myCounters().total_tx_time += timeDiff;
            WorkerCounters::myCounters().tx_latency_hist.increaseSlot(timeDiff);
            WorkerCounters::myCounters().tx++;
            mean::task::yield();
        };

        BlockedRange bb(0, (u64)1000000000000ul);
        auto startTsc = mean::readTSC();
        auto startTP = mean::getTimePoint();
        mean::task::parallelFor(bb, ycsb_tx, FLAGS_worker_tasks, 100000, true);
        auto diffTP = mean::timePointDifference(mean::getTimePoint(), startTP) / 1e9;
        printf("Done: %.3f s\n", diffTP);
    }

    mean::env::shutdown();
}

extern "C" void osv_main_app()
{
    // Synthesize a fake argv with the hardcoded configuration so that all
    // FLAGS_* variables are initialised by gflags before leanstore starts.
    const char* args[] = {
        "leanstore-ycsb",
        "--ssd_path=/dev/vda",   // OSv NVMe block device (first drive)
        "--ioengine=osv",
        "--dram_gib=1",
        "--target_gib=2",
        "--worker_threads=1",
        "--worker_tasks=16",
        "--pp_threads=1",
        "--nopp",
        "--ycsb_read_ratio=100",
        "--run_for_seconds=60",
        "--partition_bits=3",
        "--optimistic_parent_pointer=1",
        "--xmerge=1",
        "--contention_split=1"
    	, nullptr
    };
    int argc = 0;
    while (args[argc]) argc++;

    // gflags needs a non-const argv
    char** argv = const_cast<char**>(args);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    using namespace mean;
    IoOptions ioOptions("osv", "/dev/vda");
    ioOptions.write_back_buffer_size = PAGE_SIZE;
    ioOptions.engine = "osv";
    ioOptions.iodepth = 2;  // 1 worker + small batch
    ioOptions.channelCount = 1;

    mean::env::init(1 /*workerThreads*/, 0 /*pp_threads*/, ioOptions);
    mean::env::start(run_ycsb);
    mean::env::join();
}

// The OSv kernel calls osv_app_main() at boot.
extern "C" void osv_app_main()
{
    osv_main_app();
}
