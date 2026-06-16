# Leanstore YCSB benchmark – statically linked into the OSv kernel.
#
# The leanstore source lives at apps/leanstore/ (add as submodule:
#   git submodule add https://github.com/seb711/osv-apps apps/leanstore-apps
# or symlink/copy the leanstore tree from osv-slim/apps/leanstore).
#
# Build the kernel with:
#   make
#
# The kernel calls osv_app_main(), which runs the YCSB benchmark with the
# hardcoded config below (dram_gib=1, target_gib=1, threads=1, io=osv).

LEANSTORE = apps/leanstore

leanstore-backend = $(LEANSTORE)/backend/leanstore

leanstore-srcs = \
    $(leanstore-backend)/Config.cpp \
    $(leanstore-backend)/LeanStore.cpp \
    $(leanstore-backend)/concurrency-recovery/CRMG.cpp \
    $(leanstore-backend)/concurrency-recovery/GroupCommiter.cpp \
    $(leanstore-backend)/concurrency-recovery/Worker.cpp \
    $(leanstore-backend)/concurrency/Mean.cpp \
    $(leanstore-backend)/concurrency/batch/Task.cpp \
    $(leanstore-backend)/concurrency/batch/TaskExecutor.cpp \
    $(leanstore-backend)/concurrency/batch/TaskManager.cpp \
    $(leanstore-backend)/concurrency/default/DefaultThreadingManager.cpp \
    $(leanstore-backend)/concurrency/unique/UniqueTask.cpp \
    $(leanstore-backend)/concurrency/unique/UniqueTaskExecutor.cpp \
    $(leanstore-backend)/concurrency/unique/UniqueTaskManager.cpp \
    $(leanstore-backend)/concurrency/unique/helper/ContextPool.cpp \
    $(leanstore-backend)/concurrency/utils/MessageHandler.cpp \
    $(leanstore-backend)/concurrency/utils/ThreadBase.cpp \
    $(leanstore-backend)/concurrency/utils/YieldLock.cpp \
    $(leanstore-backend)/io/IoAbstraction.cpp \
    $(leanstore-backend)/io/IoChannel.cpp \
    $(leanstore-backend)/io/IoInterface.cpp \
    $(leanstore-backend)/io/IoRequest.cpp \
    $(leanstore-backend)/io/impl/Osv.cpp \
    $(leanstore-backend)/io/impl/OsvImpl.cpp \
    $(leanstore-backend)/profiling/counters/CPUCounters.cpp \
    $(leanstore-backend)/profiling/counters/CRCounters.cpp \
    $(leanstore-backend)/profiling/counters/PPCounters.cpp \
    $(leanstore-backend)/profiling/counters/SSDCounters.cpp \
    $(leanstore-backend)/profiling/counters/ThreadCounters.cpp \
    $(leanstore-backend)/profiling/counters/WorkerCounters.cpp \
    $(leanstore-backend)/profiling/tables/BMTable.cpp \
    $(leanstore-backend)/profiling/tables/CPUTable.cpp \
    $(leanstore-backend)/profiling/tables/CRTable.cpp \
    $(leanstore-backend)/profiling/tables/ConfigsTable.cpp \
    $(leanstore-backend)/profiling/tables/DTTable.cpp \
    $(leanstore-backend)/profiling/tables/SSDTable.cpp \
    $(leanstore-backend)/profiling/tables/ThreadTable.cpp \
    $(leanstore-backend)/storage/btree/BTreeLL.cpp \
    $(leanstore-backend)/storage/btree/BTreeVI.cpp \
    $(leanstore-backend)/storage/btree/BTreeVW.cpp \
    $(leanstore-backend)/storage/btree/core/BTreeGeneric.cpp \
    $(leanstore-backend)/storage/btree/core/BTreeGenericIterator.cpp \
    $(leanstore-backend)/storage/btree/core/BTreeNode.cpp \
    $(leanstore-backend)/storage/buffer-manager/BufferFrame.cpp \
    $(leanstore-backend)/storage/buffer-manager/BufferManager.cpp \
    $(leanstore-backend)/storage/buffer-manager/DTRegistry.cpp \
    $(leanstore-backend)/storage/buffer-manager/FreeList.cpp \
    $(leanstore-backend)/storage/buffer-manager/PageProviderThread.cpp \
    $(leanstore-backend)/storage/buffer-manager/Partition.cpp \
    $(leanstore-backend)/storage/buffer-manager/Swip.cpp \
    $(leanstore-backend)/sync-primitives/JumpMU.cpp \
    $(leanstore-backend)/utils/CpuTopology.cpp \
    $(leanstore-backend)/utils/FNVHash.cpp \
    $(leanstore-backend)/utils/Files.cpp \
    $(leanstore-backend)/utils/MMapVector.cpp \
    $(leanstore-backend)/utils/Misc.cpp \
    $(leanstore-backend)/utils/Parallelize.cpp \
    $(leanstore-backend)/utils/RandomGenerator.cpp \
    $(leanstore-backend)/utils/ScrambledZipfGenerator.cpp \
    $(leanstore-backend)/utils/ZipfGenerator.cpp

# app-objects holds RELATIVE paths; the main Makefile prepends $(out)/ itself
leanstore-objs = $(leanstore-srcs:%.cpp=%.o)

# gflags (v2.2.2) is vendored under external/gflags and compiled here, with the
# kernel's own C++ flags, so it uses the libc++/llvm-libc ABI. The host's
# prebuilt libgflags_nothreads.a is built against GNU libstdc++/glibc and is
# ABI-incompatible with this kernel (undefined std::__cxx11::*/__isoc23_* refs).
gflags-objs = \
    external/gflags/src/gflags.o \
    external/gflags/src/gflags_completions.o \
    external/gflags/src/gflags_reporting.o

# core/leanstore_debug.o is OSv-side glue (sched/trace/NVMe hooks the leanstore
# Mean runtime calls). Compiled with the kernel's own flags, not the leanstore
# ones, so it is listed as a plain app object.
app-objects = app/app.o app/exit_shim.o core/leanstore_debug.o \
              $(leanstore-objs) $(gflags-objs)

# gflags TUs need its public (generated) headers and its internal headers
# (config.h/defines.h/mutex.h/util.h). Built with -UNDEBUG kept off; silence the
# warnings upstream gflags trips under clang.
$(gflags-objs:%=$(out)/%): CXXFLAGS += -Iexternal/gflags/include -Iexternal/gflags/src \
    -Wno-error -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -Wno-format-nonliteral

# Compilation flags for all leanstore files
# tabulate is vendored under external/ (header-only, v1.5.0, MIT) like boost.
TABULATE_INC = external/tabulate/include

# UB handling for leanstore (built with clang; upstream leanstore targets gcc).
#   Default        -> mask the UB clang commonly exploits (workaround).
#   LEANSTORE_UBSAN=1 -> expose UB instead. Trap-on-error mode needs no runtime
#                        lib (none exists in-kernel): a UB site executes ud2 and
#                        faults; symbolize the RIP from the crash dump with
#                        llvm-symbolizer/addr2line on build/$(mode)/loader.elf.
# The two are mutually exclusive: -fwrapv would hide signed-overflow UB from UBSan.
ifeq ($(LEANSTORE_UBSAN),1)
# `alignment` is excluded: BufferFrame Pages are 512-aligned by construction
# (page-aligned mmap base + 512-multiple stride), so that check can't legitimately
# fire. Dropping it means a trap can only be the null/overflow/shift/bounds checks
# (e.g. the null BufferFrame deref in BTreeLL::insert). Re-add it to audit alignment.
leanstore-ub-flags = \
    -fsanitize=undefined \
    -fno-sanitize=vptr,function,alignment \
    -fsanitize-trap=undefined
else
leanstore-ub-flags = \
    -fno-strict-aliasing \
    -fwrapv \
    -fno-delete-null-pointer-checks
endif

# Single-threaded clang-miscompile diagnostics (opt-in, combine freely). The bug
# is gcc-works/clang-fails with no concurrency, so it is UB clang exploits.
#   LEANSTORE_AUTOINIT=1 -> -ftrivial-auto-var-init=pattern. If this makes clang
#                           work, the bug is a read of an UNINITIALIZED variable.
#   LEANSTORE_O0=1       -> -O0. If this makes clang work, it is optimization-
#                           exploited UB (narrow further with __attribute__((optnone))).
ifeq ($(LEANSTORE_AUTOINIT),1)
leanstore-ub-flags += -ftrivial-auto-var-init=pattern
endif
ifeq ($(LEANSTORE_O0),1)
leanstore-ub-flags += -O0
endif

# gflags headers are vendored (external/gflags/include), so <gflags/gflags.h>
# resolves identically on both arches and matches the gflags TUs compiled above.
gflags-includes = -Iexternal/gflags/include

ifeq ($(arch),aarch64)
# headers extracted by scripts/download_aarch64_libs.sh; the cross sysroot
# supplies the linux/asm uapi headers PerfEvent.hpp includes (x64 falls back to
# the host's /usr/include for those - no -nostdinc)
gflags-includes += -isystem build/downloaded_packages/aarch64/include \
                   -isystem build/downloaded_packages/aarch64/gcc/install/usr/aarch64-linux-gnu/include
endif

leanstore-includes = \
    -I$(LEANSTORE)/backend \
    -I$(LEANSTORE)/shared-headers \
    -I$(TABULATE_INC) \
    $(gflags-includes) \
    -DMEAN_USE_TASKING \
    -DIS_OSV=1 \
    -DUSE_C_INTERFACE \
    -DLEANSTORE_INCLUDE_OSV \
    -DMACRO_FLAG_COUNTERS=1 \
    -U_KERNEL \
    -UNDEBUG \
    -DDEBUG \
    $(leanstore-ub-flags) \
    -ftls-model=local-exec \
    -UPAGE_SIZE \
    -Wno-error \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-sign-compare \
    -Wno-missing-field-initializers \
    -Wno-maybe-uninitialized \
    -Wno-delete-non-abstract-non-virtual-dtor 

# Apply leanstore flags to all leanstore object targets (main Makefile resolves $(out)/...)
$(leanstore-objs:%=$(out)/%): CXXFLAGS += $(leanstore-includes)
$(out)/app/app.o: CXXFLAGS += $(leanstore-includes)

# The exit_shim object provides _exit() (referenced by boost.context, absent
# from OSv and llvm-libc).
