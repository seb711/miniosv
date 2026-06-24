#pragma once
#include "Units.hpp"
#include <string>
// -------------------------------------------------------------------------------------
// Compile-time configuration constants (formerly gflags command-line flags).
//
// These replace the old google-gflags machinery. The values live in Config.cpp;
// edit them there and rebuild. Because the definitions are in a .cpp translation
// unit (not inlined into every includer), changing a value only recompiles
// Config.cpp and relinks - the rest of the tree is untouched.
//
// The historical `FLAGS_<name>` spelling is kept so existing call sites compile
// unchanged.
// -------------------------------------------------------------------------------------
extern const std::string FLAGS_free_pages_list_path;
// -------------------------------------------------------------------------------------
extern const double FLAGS_dram_gib;
extern const double FLAGS_ssd_gib;
extern const u32 FLAGS_cool_pct;
extern const u32 FLAGS_free_pct;
extern const u32 FLAGS_partition_bits;
extern const u32 FLAGS_pp_threads;
// -------------------------------------------------------------------------------------
extern const std::string FLAGS_csv_path;
extern const bool FLAGS_csv_truncate;
extern const std::string FLAGS_ssd_path;
extern const u32 FLAGS_async_batch_size;
extern const bool FLAGS_trunc;
extern const u32 FLAGS_falloc;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_print_debug;
extern const bool FLAGS_print_tx_console;
extern const u32 FLAGS_print_debug_interval_s;
extern const bool FLAGS_print_obj_stats;
extern const bool FLAGS_profiling;
// -------------------------------------------------------------------------------------
extern const u32 FLAGS_worker_threads;
extern const u32 FLAGS_worker_tasks;
extern const bool FLAGS_nopp;
extern const bool FLAGS_pin_threads;
extern const bool FLAGS_smt;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_root;
// -------------------------------------------------------------------------------------
extern const u64 FLAGS_backoff_strategy;
// -------------------------------------------------------------------------------------
extern const std::string FLAGS_zipf_path;
extern const double FLAGS_zipf_factor;
extern const double FLAGS_target_gib;
extern const u64 FLAGS_run_for_seconds;
extern const u64 FLAGS_warmup_for_seconds;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_contention_split;
extern const u64 FLAGS_cm_update_on;
extern const u64 FLAGS_cm_period;
extern const u64 FLAGS_cm_slowpath_threshold;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_xmerge;
extern const u64 FLAGS_xmerge_k;
extern const double FLAGS_xmerge_target_pct;
// -------------------------------------------------------------------------------------
extern const u64 FLAGS_backoff;
// -------------------------------------------------------------------------------------
extern const u64 FLAGS_x;
extern const u64 FLAGS_y;
extern const double FLAGS_d;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_bulk_insert;
// -------------------------------------------------------------------------------------
extern const s64 FLAGS_trace_dt_id;
extern const s64 FLAGS_trace_trigger_probability;
extern const std::string FLAGS_tag;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_out_of_place;
extern const bool FLAGS_optimistic_parent_pointer;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_wal;
extern const u64 FLAGS_wal_offset_gib;
extern const bool FLAGS_wal_io_hack;
extern const bool FLAGS_wal_fsync;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_si;
extern const u64 FLAGS_si_refresh_rate;
extern const bool FLAGS_vw;
extern const bool FLAGS_vw_todo;
extern const bool FLAGS_vi;
// -------------------------------------------------------------------------------------
extern const std::string FLAGS_ioengine;
extern const bool FLAGS_io_uring_poll_mode;
extern const s64 FLAGS_io_uring_share_wq;
extern const bool FLAGS_raid5;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_persist;
extern const u64 FLAGS_tx_rate;
extern const u64 FLAGS_tmp;
// -------------------------------------------------------------------------------------
extern const u64 FLAGS_worker_per_threads;
// -------------------------------------------------------------------------------------
extern const bool FLAGS_disable_cross_cores_ut;
// -------------------------------------------------------------------------------------
// YCSB driver knobs (formerly defined in app.cc).
extern const u32 FLAGS_ycsb_read_ratio;
