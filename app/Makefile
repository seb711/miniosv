# LeanStore YCSB benchmark – statically linked into the OSv kernel.
#
# This is the single Makefile for the app. It is `include`d by the top-level
# OSv Makefile, which compiles every entry in $(app-objects) with the kernel's
# own flags and links them into the kernel image. The kernel enters the app via
# osv_app_main() (defined in app.cc), which runs the YCSB benchmark with the
# hardcoded config (dram_gib=1, target_gib=1, threads=1, io=osv).
#
# Build:
#   make                 # build the kernel with this app linked in
#   make app=tests       # build the test app instead (test/Makefile)
#
# Layout (after flattening the upstream leanstore tree into app/):
#   app/app.cc                     – YCSB driver / osv_app_main()
#   app/net_stubs.cc               – empty POSIX network stubs
#   app/backend/leanstore/...      – leanstore engine sources + headers
#   app/shared-headers/...         – Time.hpp, Units.hpp, etc.
#
# Only the MEAN_USE_TASKING build is wired up here.

app-dir          = app
leanstore-backend = $(app-dir)/backend/leanstore

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
    $(leanstore-backend)/utils/FNVHash.cpp \
    $(leanstore-backend)/utils/MMapVector.cpp \
    $(leanstore-backend)/utils/Misc.cpp \
    $(leanstore-backend)/utils/Parallelize.cpp \
    $(leanstore-backend)/utils/RandomGenerator.cpp \
    $(leanstore-backend)/utils/ScrambledZipfGenerator.cpp \
    $(leanstore-backend)/utils/ZipfGenerator.cpp

# Object paths mirror the source paths; the top Makefile prepends $(out)/ and
# resolves each via the %.cc / %.cpp pattern rules.
leanstore-objs = $(leanstore-srcs:%.cpp=%.o)

# Boost.Context provides leanstore's user-space task switching. Boost ships no
# library we can link into the kernel, so we build the upstream assembly
# context-switch routines (vendored under external/boost/asm) plus a small C++
# shim (fcontext_bridge.cpp) that maps Boost's namespaced API onto the asm's
# extern-"C" symbols.
ifeq ($(arch),aarch64)
boost-context-asm = \
    external/boost/asm/jump_arm64_aapcs_elf_gas.S \
    external/boost/asm/make_arm64_aapcs_elf_gas.S \
    external/boost/asm/ontop_arm64_aapcs_elf_gas.S
else
boost-context-asm = \
    external/boost/asm/jump_x86_64_sysv_elf_gas.S \
    external/boost/asm/make_x86_64_sysv_elf_gas.S \
    external/boost/asm/ontop_x86_64_sysv_elf_gas.S
endif
boost-context-objs = $(boost-context-asm:%.S=%.o) external/boost/fcontext_bridge.o

app-objects = $(app-dir)/app.o $(leanstore-objs) $(boost-context-objs)

# Pattern rule for leanstore's .cpp sources (the kernel tree itself is all .cc,
# so the top Makefile only ships a %.cc rule). Mirrors that rule.
$(out)/%.o: %.cpp | generated-headers $(out)/.libcxx-built
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX $*.cpp)

# ---------------------------------------------------------------------------
# Compilation flags for the leanstore sources + app.cc
# ---------------------------------------------------------------------------

# tabulate is a header-only dependency used by LeanStore.cpp. Vendor it under
# external/tabulate/include (v1.5.0, MIT), like boost, before building.
TABULATE_INC = external/tabulate/include

# UB handling for leanstore (built with clang; upstream leanstore targets gcc).
#   Default          -> mask the UB clang commonly exploits (workaround).
#   LEANSTORE_UBSAN=1 -> expose UB instead. Trap-on-error mode needs no runtime
#                        lib (none exists in-kernel): a UB site executes ud2 and
#                        faults; symbolize the RIP from the crash dump with
#                        llvm-symbolizer/addr2line on build/$(mode)/loader.elf.
# The two are mutually exclusive: -fwrapv would hide signed-overflow UB from UBSan.
ifeq ($(LEANSTORE_UBSAN),1)
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

# Single-threaded clang-miscompile diagnostics (opt-in, combine freely):
#   LEANSTORE_AUTOINIT=1 -> -ftrivial-auto-var-init=pattern (read of uninit var?)
#   LEANSTORE_O0=1       -> -O0 (optimization-exploited UB?)
ifeq ($(LEANSTORE_AUTOINIT),1)
leanstore-ub-flags += -ftrivial-auto-var-init=pattern
endif
ifeq ($(LEANSTORE_O0),1)
leanstore-ub-flags += -O0
endif

ifeq ($(arch),aarch64)
# headers extracted by scripts/download_aarch64_libs.sh; the cross sysroot
# supplies the linux/asm uapi headers PerfEvent.hpp includes (x64 falls back to
# the host's /usr/include for those - no -nostdinc)
aarch64-pkgs = build/downloaded_packages/aarch64
arch-sysroot-includes = \
    -isystem $(aarch64-pkgs)/include \
    -isystem $(aarch64-pkgs)/gcc/install/usr/aarch64-linux-gnu/include
else
arch-sysroot-includes =
endif

leanstore-includes = \
    -I$(app-dir)/backend \
    -I$(app-dir)/shared-headers \
    -I$(TABULATE_INC) \
    $(arch-sysroot-includes) \
    -DMEAN_USE_TASKING \
    -DIS_OSV=1 \
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

# Apply the leanstore flags to every leanstore object and to app.cc itself
# (app.cc includes the leanstore headers). net_stubs.cc keeps the plain kernel
# flags. The top Makefile resolves $(out)/... for each target.
$(leanstore-objs:%=$(out)/%): CXXFLAGS += $(leanstore-includes)
$(out)/$(app-dir)/app.o: CXXFLAGS += $(leanstore-includes)
