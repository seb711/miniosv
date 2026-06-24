// -------------------------------------------------------------------------------------
// Configuration values (formerly gflags). This is the single place to edit them.
//
// Values marked "[was --flag at runtime]" used to be overridden by the fake argv
// that app.cc passed to gflags::ParseCommandLineFlags(); those overrides are now
// baked in here so the build behaves identically without gflags.
// -------------------------------------------------------------------------------------
#include "Config.hpp"
// -------------------------------------------------------------------------------------
const std::string FLAGS_free_pages_list_path = "leanstore_free_pages";
// -------------------------------------------------------------------------------------
const double FLAGS_dram_gib = 1;
const double FLAGS_ssd_gib = 1700;
const u32 FLAGS_cool_pct = 10;            // Start cooling pages when <= x% are free
const u32 FLAGS_free_pct = 1;
const u32 FLAGS_partition_bits = 3;       // [was --partition_bits=3]
const u32 FLAGS_pp_threads = 1;           // [was --pp_threads=1]
// -------------------------------------------------------------------------------------
const std::string FLAGS_csv_path = "./log";
const bool FLAGS_csv_truncate = false;
const std::string FLAGS_ssd_path = "/dev/vda";   // [was --ssd_path=/dev/vda] OSv NVMe block device
const u32 FLAGS_async_batch_size = 256;
const bool FLAGS_trunc = false;           // Truncate file
const u32 FLAGS_falloc = 0;               // Preallocate GiB
// -------------------------------------------------------------------------------------
const bool FLAGS_print_debug = true;
const bool FLAGS_print_tx_console = true;
const u32 FLAGS_print_debug_interval_s = 1;
const bool FLAGS_print_obj_stats = false;
const bool FLAGS_profiling = false;
// -------------------------------------------------------------------------------------
const u32 FLAGS_worker_threads = 1;       // [was --worker_threads=1]
const u32 FLAGS_worker_tasks = 16;        // [was --worker_tasks=16]
const bool FLAGS_nopp = true;             // [was --nopp]
const bool FLAGS_pin_threads = false;     // Responsibility of the driver
const bool FLAGS_smt = true;              // Simultaneous multithreading
// -------------------------------------------------------------------------------------
const bool FLAGS_root = false;
// -------------------------------------------------------------------------------------
const u64 FLAGS_backoff_strategy = 0;
// -------------------------------------------------------------------------------------
const std::string FLAGS_zipf_path = "/bulk/zipf";
const double FLAGS_zipf_factor = 0.0;
const double FLAGS_target_gib = 2;        // [was --target_gib=2]
const u64 FLAGS_run_for_seconds = 60;     // [was --run_for_seconds=60]
const u64 FLAGS_warmup_for_seconds = 10;
// -------------------------------------------------------------------------------------
const bool FLAGS_contention_split = true; // [was --contention_split=1]
const u64 FLAGS_cm_update_on = 7;         // as exponent of 2
const u64 FLAGS_cm_period = 14;           // as exponent of 2
const u64 FLAGS_cm_slowpath_threshold = 1;
// -------------------------------------------------------------------------------------
const bool FLAGS_xmerge = true;           // [was --xmerge=1]
const u64 FLAGS_xmerge_k = 5;
const double FLAGS_xmerge_target_pct = 80;
// -------------------------------------------------------------------------------------
const u64 FLAGS_backoff = 512;
// -------------------------------------------------------------------------------------
const u64 FLAGS_x = 512;
const u64 FLAGS_y = 100;
const double FLAGS_d = 0.0;
// -------------------------------------------------------------------------------------
const bool FLAGS_bulk_insert = false;
// -------------------------------------------------------------------------------------
const s64 FLAGS_trace_dt_id = -1;         // Print a stack trace for page reads for this DT ID
const s64 FLAGS_trace_trigger_probability = 100;
const std::string FLAGS_tag = "";         // appended to each csv line
// -------------------------------------------------------------------------------------
const bool FLAGS_out_of_place = false;
const bool FLAGS_optimistic_parent_pointer = true;  // [was --optimistic_parent_pointer=1]
// -------------------------------------------------------------------------------------
const bool FLAGS_wal = false;
const u64 FLAGS_wal_offset_gib = 1;
const bool FLAGS_wal_io_hack = false;     // Does not really write logs on SSD
const bool FLAGS_wal_fsync = false;
// -------------------------------------------------------------------------------------
const bool FLAGS_si = false;
const u64 FLAGS_si_refresh_rate = 0;
const bool FLAGS_vw = false;              // BTree with SI using versions in WAL
const bool FLAGS_vw_todo = false;
const bool FLAGS_vi = false;              // BTree with SI using in-place version
// -------------------------------------------------------------------------------------
const std::string FLAGS_ioengine = "osv";   // [was --ioengine=osv]
const bool FLAGS_io_uring_poll_mode = true;  // enables IORING_SETUP_IOPOLL
const s64 FLAGS_io_uring_share_wq = 0;       // enables IORING_SETUP_ATTACH_WQ with a single worker
const bool FLAGS_raid5 = false;              // enable RAID 5
// -------------------------------------------------------------------------------------
const bool FLAGS_persist = false;
const u64 FLAGS_tx_rate = 0;
const u64 FLAGS_tmp = 0;
// -------------------------------------------------------------------------------------
const u64 FLAGS_worker_per_threads = 100;
// -------------------------------------------------------------------------------------
const bool FLAGS_disable_cross_cores_ut = false;
// -------------------------------------------------------------------------------------
const u32 FLAGS_ycsb_read_ratio = 100;    // [was --ycsb_read_ratio=100] percentage of reads
