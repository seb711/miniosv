# OSv makefile
#
# Copyright (C) 2015 Cloudius Systems, Ltd.
# This work is open source software, licensed under the terms of the
# BSD license as described in the LICENSE file in the top-level directory.

# Delete the builtin make rules, as if "make -r" was used.
.SUFFIXES:

# Ask make to not delete "intermediate" results, such as the .o in the chain
# .cc -> .o -> .so. Otherwise, during the first build, make considers the .o
# to be intermediate, and deletes it, but the newly-created ".d" files lists
# the ".o" as a target - so it needs to be created again on the second make.
# See commit fac05c95 for a longer explanation.
.SECONDARY:

# Deleting partially-build targets on error should be the default, but it
# isn't, for historical reasons, so we need to turn it on explicitly...
.DELETE_ON_ERROR:
###########################################################################
# Backward-compatibility hack to support the old "make ... image=..." image
# building syntax, and pass it into scripts/build. We should eventually drop
# this support and turn the deprecated messages into errors.
compat_args=$(if $(usrskel), usrskel=$(usrskel),)
compat_args+=$(if $(fs), fs=$(fs),)
ifdef image
#$(error Please use scripts/build to build images)
$(info "make image=..." deprecated. Please use "scripts/build image=...".)
default_target:
	./scripts/build image=$(image) $(compat_args)
endif
ifdef modules
#$(error Please use scripts/build to build images)
$(info "make modules=..." deprecated. Please use "scripts/build modules=...".)
default_target:
	./scripts/build modules=$(modules) $(compat_args)
endif
.PHONY: default_target

###########################################################################

include conf/base.mk

# The build mode defaults to "release" (optimized build), the other option
# is "debug" (unoptimized build). In the latter the optimizer interferes
# less with the debugging, but the release build is fully debuggable too.
mode=release
ifeq (,$(wildcard conf/$(mode).mk))
    $(error unsupported mode $(mode))
endif
include conf/$(mode).mk


# By default, detect HOST_CXX's architecture - x64 or aarch64.
# But also allow the user to specify a cross-compiled target architecture
# by setting either "ARCH" or "arch" in the make command line, or the "ARCH"
# environment variable.
HOST_CXX := clang++

detect_arch = $(word 1, $(shell { echo "x64        __x86_64__";  \
                                  echo "aarch64    __aarch64__"; \
                       } | $1 -E -xc - | grep -v '^#' | grep ' 1$$'))

host_arch := $(call detect_arch, $(HOST_CXX))

# As an alternative to setting ARCH or arch, let's allow the user to
# directly set the CROSS_PREFIX environment variable, and learn its arch:
ifdef CROSS_PREFIX
    ARCH := $(call detect_arch, $(CROSS_PREFIX)gcc)
endif

ifndef ARCH
    ARCH := $(host_arch)
endif
arch := $(ARCH)

# ARCH_STR is like ARCH, but uses the full name x86_64 instead of x64
ARCH_STR := $(arch:x64=x86_64)

ifeq (,$(wildcard conf/$(arch).mk))
    $(error unsupported architecture $(arch))
endif
include conf/$(arch).mk

CROSS_PREFIX ?= $(if $(filter-out $(arch),$(host_arch)),$(arch)-linux-gnu-)
CXX=clang++
CC=clang
LD=$(CROSS_PREFIX)ld.bfd
export STRIP=$(shell which llvm-strip 2>/dev/null || which llvm-strip-20 2>/dev/null || echo strip)
OBJCOPY=$(shell which llvm-objcopy 2>/dev/null || which llvm-objcopy-20 2>/dev/null || echo objcopy)

# Our makefile puts all compilation results in a single directory, $(out),
# instead of mixing them with the source code. This allows us to compile
# different variants of the code - for different mode (release or debug)
# or arch (x86 or aarch64) side by side. It also makes "make clean" very
# simple, as all compilation results are in $(out) and can be removed in
# one fell swoop.
out = build/$(mode).$(arch)
outlink = build/$(mode)
outlink2 = build/last

# The kernel configuration is frozen: a single configuration is checked in at
# conf/config.mk (the conf_* options and the device-driver set, formerly the
# kconfig output and the 'all' driver profile). The arch/mode value files
# conf/base.mk, conf/$(mode).mk and conf/$(arch).mk were already included above.
# There is no kconfig, no .config, and no generation step - to change the
# configuration, edit conf/config.mk. The conf_drivers_* variables it defines
# decide which driver objects are linked in the rules below.
include conf/config.mk

ifneq ($(MAKECMDGOALS),clean)
$(info Building into $(out))
endif

###########################################################################

# This makefile wraps all commands with the $(quiet) or $(very-quiet) macros
# so that instead of half-a-screen-long command lines we short summaries
# like "CC file.cc". These macros also keep the option of viewing the
# full command lines, if you wish, with "make V=1".
quiet = $(if $V, $1, @echo " $2"; $1)
very-quiet = $(if $V, $1, @$1)

# x64 boots loader[-stripped].elf directly via QEMU -kernel (multiboot/PVH);
# there is no boot sector or compressed image, so loader.img is not built.
# aarch64 keeps loader.img (preboot stub + uncompressed loader).
ifeq ($(arch),x64)
all: $(out)/loader-stripped.elf links $(out)/vmlinuz.bin
endif
ifeq ($(arch),aarch64)
all: $(out)/loader.img links
endif
.PHONY: all

links:
	$(call very-quiet, ln -nsf $(notdir $(out)) $(outlink))
	$(call very-quiet, ln -nsf $(notdir $(out)) $(outlink2))
.PHONY: links

check:
	$(call quiet, pkill -e -9 qemu-system || true, Kill lingering QEMU process if any)
	./scripts/build check
.PHONY: check

# Remember that "make clean" needs the same parameters that set $(out) in
# the first place, so to clean the output of "make mode=debug" you need to
# do "make mode=debug clean".
clean:
	rm -rf $(out)
	rm -f $(outlink) $(outlink2)
.PHONY: clean

# Manually listing recompilation dependencies in the Makefile (such as which
# object needs to be recompiled when a header changed) is antediluvian.
# Even "makedepend" is old school! The best modern technique for automatic
# dependency generation, which we use here, works like this:
# We note that before the first compilation, we don't need to know these
# dependencies at all, as everything will be compiled anyway. But during
# this compilation, we pass to the compiler a special option (-MD) which
# causes it to also output a file with suffix ".d" listing the dependencies
# discovered during the compilation of that source file. From then on,
# on every compilation we "include" all the ".d" files generated in the
# previous compilation, and create new ".d" when a source file changed
# (and therefore got recompiled).
ifneq ($(MAKECMDGOALS),clean)
include $(shell test -d $(out) && find $(out) -name '*.d')
endif

# Before we can try to build anything in $(out), we need to make sure the
# directory exists. Unfortunately, this is not quite enough, as when we
# compile somedir/somefile.c to $(out)/somedir/somefile.o, we also need
# to make sure $(out)/somedir exists. This is why we have $(makedir) below.
# I wonder if there's a better way of doing this with dependencies, so make
# will only call mkdir for each directory once.
$(out)/%: | $(out)
$(out):
	mkdir -p $(out)

# "tags" is the default output file of ctags, "TAGS" is that of etags
tags TAGS:
	rm -f -- "$@"
	find . -name "*.cc" -o -name "*.hh" -o -name "*.h" -o -name "*.c" |\
		xargs $(if $(filter $@, tags),ctags,etags) -a
.PHONY: tags TAGS

cscope:
	find -name '*.[chS]' -o -name "*.cc" -o -name "*.hh" | cscope -bq -i-
	@echo cscope index created
.PHONY: cscope

###########################################################################

local-includes =
INCLUDES = $(local-includes) -Iarch/$(arch) -I. -Iinclude  -Iarch/common
#
# C++ standard headers come from our own libc++ (CXX_INCLUDES is set below,
# overriding anything here). The aarch64 cross-build still needs a sysroot and
# the target toolchain's standard C headers (e.g. unwind.h) for the freestanding
# bits; the native x86 build needs nothing extra.
ifeq ($(CROSS_PREFIX),aarch64-linux-gnu-)
  aarch64_gccbase = build/downloaded_packages/aarch64/gcc/install
  ifeq (,$(wildcard $(aarch64_gccbase)))
   $(error Missing $(aarch64_gccbase) directory. Please run "./scripts/download_aarch64_packages.py")
  endif
  cross-inc-base := $(dir $(shell find $(aarch64_gccbase)/ -name unwind.h))
  ifeq (,$(cross-inc-base))
    $(error Could not find standard headers like "unwind.h" under $(aarch64_gccbase) directory. Please run "./scripts/download_aarch64_packages.py")
  endif
  STANDARD_GCC_INCLUDES = -isystem $(cross-inc-base)
  gcc-sysroot = --sysroot $(aarch64_gccbase)
  standard-includes-flag = -nostdinc
else
  STANDARD_GCC_INCLUDES =
  standard-includes-flag =
endif

# --- libc++ replaces GNU libstdc++ (LLVM_LIBC_PLAN.md, Phase 8) ---
# Compile the entire C++ surface (kernel + app + vendored boost) against our
# localization-free libc++ headers instead of the system GNU C++ headers; the
# libc++ c++/v1 dir itself is added C++-only in CXXFLAGS (with -nostdinc++), so
# it does not shadow C headers (e.g. its wchar.h wrapper) for C compiles.
# libunwind's unwind.h/libunwind.h are needed by backtrace.cc (C++ only, but
# harmless for C) and libc++abi's cxxabi.h is installed alongside under c++/v1.
CXX_INCLUDES = -isystem external/llvm-project/libunwind/include
libcxx-includes = -nostdinc++ -isystem build/libcxx/$(arch)/include/c++/v1

ifeq ($(arch),aarch64)
libfdt_base = external/$(arch)/libfdt
INCLUDES += -isystem $(libfdt_base)
endif

INCLUDES += $(boost-includes)
# Starting in Gcc 6, the standard C++ header files (which we do not change)
# must precede in the include path the C header files (which we replace).
# This is explained in https://gcc.gnu.org/bugzilla/show_bug.cgi?id=70722.
# So we are forced to list here (before include/api) the system's default
# C++ include directories, though they are already in the default search path.
INCLUDES += $(CXX_INCLUDES)
INCLUDES += $(pre-include-api)
INCLUDES += -isystem include/api
INCLUDES += -isystem include/api/$(arch)
# must be after include/api, since it includes some libc-style headers:
INCLUDES += $(STANDARD_GCC_INCLUDES)
INCLUDES += -isystem $(out)/gen/include
INCLUDES += $(post-includes-bsd)


ifneq ($(werror),0)
	CFLAGS_WERROR = -Werror
endif
# $(call compiler-flag, -ffoo, option, file)
#     returns option if file builds with -ffoo, empty otherwise
compiler-flag = $(shell $(CXX) $(CFLAGS_WERROR) $1 -o /dev/null -c $3  > /dev/null 2>&1 && echo $2)

compiler-specific := $(call compiler-flag, -std=$(conf_cxx_level), -DHAVE_ATTR_COLD_LABEL, compiler/attr/cold-label.cc)
fno-instrument-functions := $(call compiler-flag, -fno-instrument-functions, -fno-instrument-functions, compiler/empty.cc)

# Flags that exist in GCC but not Clang - probed so the build works with either compiler.
wno-class-memaccess       := $(call compiler-flag, -Wno-class-memaccess,       -Wno-class-memaccess,       compiler/empty.cc)
wno-stringop-truncation   := $(call compiler-flag, -Wno-stringop-truncation,   -Wno-stringop-truncation,   compiler/empty.cc)
wno-dangling-pointer      := $(call compiler-flag, -Wno-dangling-pointer,      -Wno-dangling-pointer,      compiler/empty.cc)
wno-unused-private-field  := $(call compiler-flag, -Wno-unused-private-field,  -Wno-unused-private-field,  compiler/empty.cc)
# GCC: $(wno-maybe-uninitialized); Clang equivalent: -Wno-uninitialized
wno-maybe-uninitialized := $(call compiler-flag, -Wno-maybe-uninitialized, -Wno-maybe-uninitialized, compiler/empty.cc)
ifeq ($(wno-maybe-uninitialized),)
wno-maybe-uninitialized := $(call compiler-flag, -Wno-uninitialized, -Wno-uninitialized, compiler/empty.cc)
endif

source-dialects = -D_GNU_SOURCE

# libc has its own source dialect control
$(out)/libc/%.o: source-dialects =

# do not hide symbols in libc because it has its own hiding mechanism
$(out)/libc/%.o: cc-hide-flags =
$(out)/libc/%.o: cxx-hide-flags =

kernel-defines = -D_KERNEL $(source-dialects) $(cc-hide-flags) $(gc-flags)

# This play the same role as "_KERNEL", but _KERNEL unfortunately is too
# overloaded. A lot of files will expect it to be set no matter what, specially
# in headers. "userspace" inclusion of such headers is valid, and lacking
# _KERNEL will make them fail to compile. That is specially true for the BSD
# imported stuff like ZFS commands.
#
# To add something to the kernel build, you can write for your object:
#
#   mydir/*.o COMMON += <MY_STUFF>
#
# To add something that will *not* be part of the main kernel, you can do:
#
#   mydir/*.o EXTRA_FLAGS = <MY_STUFF>
ifeq ($(arch),x64)
EXTRA_FLAGS = -D__OSV_CORE__ -DOSV_KERNEL_BASE=$(kernel_base) -DOSV_KERNEL_VM_BASE=$(kernel_vm_base) \
	-DOSV_KERNEL_VM_SHIFT=$(kernel_vm_shift)
else
EXTRA_FLAGS = -D__OSV_CORE__ -DOSV_KERNEL_VM_BASE=$(kernel_vm_base)
endif
EXTRA_LIBS =
COMMON = $(autodepend) -g -Wall -Wno-pointer-arith $(CFLAGS_WERROR) -Wformat=0 -Wno-format-security \
	-D __BSD_VISIBLE=1 -U _FORTIFY_SOURCE -fno-stack-protector $(INCLUDES) \
	$(kernel-defines) \
	-fno-omit-frame-pointer $(compiler-specific) \
	-include compiler/include/intrinsics.hh \
	$(conf_compiler_cflags) $(conf_compiler_opt) $(tracing-flags) $(gcc-sysroot) \
	-D__OSV__ -DARCH_STRING=$(ARCH_STR) $(EXTRA_FLAGS)
COMMON += $(standard-includes-flag)
COMMON += $(wno-unused-private-field)

tracing-flags-0 =
tracing-excl := $(shell $(CXX) $(CFLAGS_WERROR) -finstrument-functions-exclude-file-list=x -o /dev/null -c compiler/empty.cc > /dev/null 2>&1 && \
    echo '-finstrument-functions-exclude-file-list=c++,trace.cc,trace.hh,align.hh,mmintrin.h')
tracing-flags-1 = -finstrument-functions $(tracing-excl)
tracing-flags = $(tracing-flags-$(conf_tracing))

cc-hide-flags-0 =
cc-hide-flags-1 = -fvisibility=hidden
cc-hide-flags = $(cc-hide-flags-$(conf_hide_symbols))

cxx-hide-flags-0 =
cxx-hide-flags-1 = -fvisibility-inlines-hidden
cxx-hide-flags = $(cxx-hide-flags-$(conf_hide_symbols))

gc-flags-0 =
gc-flags-1 = -ffunction-sections -fdata-sections
gc-flags = $(gc-flags-$(conf_hide_symbols))

gcc-opt-Og := $(call compiler-flag, -Og, -Og, compiler/empty.cc)

# The kernel (with the app statically linked in) is a single fixed-address
# executable, so use the local-exec TLS model: thread-local accesses become
# immediate offsets resolved at link time rather than GOT-based TPOFF64 dynamic
# relocations that would need processing at boot.
tls-model = -ftls-model=local-exec
CXXFLAGS = -std=$(conf_cxx_level) $(libcxx-includes) $(COMMON) $(cxx-hide-flags) $(tls-model)
CFLAGS = -std=gnu99 $(COMMON) $(tls-model)

# should be limited to files under libc/ eventually
CFLAGS += -I libc/stdio -I libc/internal -I libc/arch/$(arch) \
	-Wno-missing-braces -Wno-parentheses -Wno-unused-but-set-variable
# musl uses "str"+int idiom to skip first character conditionally
$(out)/libc/%.o: CFLAGS += -Wno-string-plus-int
# musl calls fabs() on a long double in a few spots; suppress Clang's narrowing
# warning rather than patch the vendored musl source (probed; GCC lacks the flag)
wno-absolute-value := $(call compiler-flag, -Wno-absolute-value, -Wno-absolute-value, compiler/empty.cc)
$(out)/libc/%.o: CFLAGS += $(wno-absolute-value)

ASFLAGS = -g $(autodepend) -D__ASSEMBLY__
# Assembly files use COMMON (no -std=gnu++XX) so Clang does not apply C++
# lexing to assembly comments (which breaks on apostrophes like "let's").
# Many COMMON flags (optimization, -isystem, defines) don't apply to assembly;
# Clang flags them under -Wunused-command-line-argument, so silence that.
wno-unused-cli-arg := $(call compiler-flag, -Wno-unused-command-line-argument, -Wno-unused-command-line-argument, compiler/empty.cc)
ASCOMPILE = $(CXX) $(COMMON) $(wno-unused-cli-arg)

$(out)/fs/vfs/main.o: CXXFLAGS += -Wno-sign-compare -Wno-write-strings


makedir = $(call very-quiet, mkdir -p $(dir $@))
EXTRA_LDFLAGS =
build-so = $(CC) $(CFLAGS) -o $@ $^ $(EXTRA_LDFLAGS) $(EXTRA_LIBS)
q-build-so = $(call quiet, $(build-so), LINK $@)


$(out)/%.o: %.cc include/osv/kernel_config_hide_symbols.h | generated-headers
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX $*.cc)

$(out)/%.o: %.c include/osv/kernel_config_hide_symbols.h | generated-headers
	$(makedir)
	$(call quiet, $(CC) $(CFLAGS) -c -o $@ $<, CC $*.c)

$(out)/%.o: %.S include/osv/kernel_config_hide_symbols.h
	$(makedir)
	$(call quiet, $(ASCOMPILE) $(ASFLAGS) -c -o $@ $<, AS $*.S)

$(out)/%.o: %.s include/osv/kernel_config_hide_symbols.h
	$(makedir)
	$(call quiet, $(ASCOMPILE) $(ASFLAGS) -c -o $@ $<, AS $*.s)

%.so: EXTRA_FLAGS = -fPIC
%.so: EXTRA_LDFLAGS = -shared -Wl,-z,relro -Wl,-z,lazy
%.so: %.o
	$(makedir)
	$(q-build-so)

autodepend = -MD -MT $@ -MP

tools :=

$(out)/loader-stripped.elf: $(out)/loader.elf
	$(call quiet, $(STRIP) $(out)/loader.elf -o $(out)/loader-stripped.elf, STRIP loader.elf -> loader-stripped.elf )

ifeq ($(arch),x64)

# kernel_base is where the kernel is loaded by the multiboot/PVH boot loader
# (QEMU -kernel). There is no longer a compressed/relocated copy: the loader
# ELF is placed directly by the boot loader, so OSV_KERNEL_BASE is the only
# load address.
kernel_base := 0x200000
kernel_vm_base := 0x40200000

# the default of 512 bytes can be overridden by passing the app_local_exec_tls_size
# environment variable to the make or scripts/build
app_local_exec_tls_size := 0x200

# vmlinuz.bin is the uncompressed bzImage-style wrapper around loader-stripped.elf
# for hosts that expect a Linux-kernel blob. The primary x64 boot path is
# 'qemu -kernel loader[-stripped].elf' (multiboot/PVH), which needs no wrapper.
kernel_size = $(shell stat --printf %s $(out)/loader-stripped.elf)

$(out)/arch/x64/vmlinuz-boot32.o: $(out)/loader-stripped.elf
$(out)/arch/x64/vmlinuz-boot32.o: ASFLAGS += -I$(out) -DOSV_KERNEL_SIZE=$(kernel_size)

$(out)/vmlinuz-boot.bin: $(out)/arch/x64/vmlinuz-boot32.o arch/x64/vmlinuz-boot.ld
	$(call quiet, $(LD) -static -o $@ \
		$(filter-out %.bin, $(^:%.ld=-T %.ld)), LD $@)

$(out)/vmlinuz.bin: $(out)/vmlinuz-boot.bin $(out)/loader-stripped.elf
	$(call quiet, dd if=$(out)/vmlinuz-boot.bin of=$@ > /dev/null 2>&1, DD vmlinuz.bin vmlinuz-boot.bin)
	$(call quiet, dd if=$(out)/loader-stripped.elf of=$@ conv=notrunc seek=4 > /dev/null 2>&1, \
		DD vmlinuz.bin loader-stripped.elf)

kernel_vm_shift := $(shell printf "0x%X" $(shell expr $$(( $(kernel_vm_base) - $(kernel_base) )) ))

endif # x64

ifeq ($(arch),aarch64)

kernel_vm_base := 0xfc0080000 #63GB
app_local_exec_tls_size := 0x200

include $(libfdt_base)/Makefile.libfdt
libfdt-source := $(patsubst %.c, $(libfdt_base)/%.c, $(LIBFDT_SRCS))
libfdt = $(patsubst %.c, %.o, $(libfdt-source))

$(out)/preboot.elf: arch/$(arch)/preboot.ld $(out)/arch/$(arch)/preboot.o
	$(call quiet, $(LD) -o $@ -T $^, LD $@)

$(out)/preboot.bin: $(out)/preboot.elf
	$(call quiet, $(OBJCOPY) -O binary $^ $@, OBJCOPY $@)

edata = $(shell readelf --syms $(out)/loader.elf | grep "\.edata" | awk '{print "0x" $$2}')
image_size = $$(( $(edata) - $(kernel_vm_base) ))


$(out)/loader.img: $(out)/preboot.bin $(out)/loader-stripped.elf
	$(call quiet, dd if=$(out)/preboot.bin of=$@ > /dev/null 2>&1, DD $@ preboot.bin)
	$(call quiet, dd if=$(out)/loader-stripped.elf of=$@ conv=notrunc obs=4096 seek=16 > /dev/null 2>&1, DD $@ loader-stripped.elf)
	$(call quiet, scripts/imgedit.py setsize_aarch64 "-f raw $@" $(image_size), IMGEDIT $@)
	$(call quiet, scripts/imgedit.py setargs "-f raw $@" $(cmdline), IMGEDIT $@)


endif # aarch64


# bsd headers are also included from non-bsd code; suppress the attribute/
# extern-C mismatches those pull in (probed, since GCC lacks the flag names).
wno-extern-c-compat    := $(call compiler-flag, -Wno-extern-c-compat,   -Wno-extern-c-compat,   compiler/empty.cc)
wno-ignored-attributes := $(call compiler-flag, -Wno-ignored-attributes, -Wno-ignored-attributes, compiler/empty.cc)
wno-sometimes-uninitialized := $(call compiler-flag, -Wno-sometimes-uninitialized, -Wno-sometimes-uninitialized, compiler/empty.cc)
COMMON += $(wno-extern-c-compat) $(wno-ignored-attributes) $(wno-sometimes-uninitialized)



wno-vla-cxx-extension := $(call compiler-flag, -Wno-vla-cxx-extension, -Wno-vla-cxx-extension, compiler/empty.cc)
wno-c99-designator    := $(call compiler-flag, -Wno-c99-designator,    -Wno-c99-designator,    compiler/empty.cc)
$(out)/drivers/libtsm/%.o: CXXFLAGS += $(wno-vla-cxx-extension) $(wno-c99-designator)
libtsm :=
libtsm += drivers/libtsm/tsm_render.o
libtsm += drivers/libtsm/tsm_screen.o
libtsm += drivers/libtsm/tsm_vte.o
libtsm += drivers/libtsm/tsm_vte_charsets.o

drivers :=
drivers += core/mmu.o
drivers += arch/$(arch)/early-console.o
drivers += drivers/console.o
drivers += drivers/console-multiplexer.o
drivers += drivers/console-driver.o
drivers += drivers/line-discipline.o
drivers += drivers/clock.o
drivers += drivers/clock-common.o
drivers += drivers/clockevent.o
drivers += drivers/isa-serial-base.o
drivers += drivers/random.o
drivers += drivers/device.o
ifeq ($(conf_drivers_pci),1)
drivers += drivers/pci-generic.o
drivers += drivers/pci-device.o
drivers += drivers/pci-function.o
drivers += drivers/pci-bridge.o
drivers += drivers/msi.o
endif
drivers += drivers/driver.o

ifeq ($(arch),x64)
ifeq ($(conf_drivers_vga),1)
drivers += $(libtsm)
drivers += drivers/vga.o
endif
drivers += drivers/kbd.o drivers/isa-serial.o
drivers += arch/$(arch)/pvclock-abi.o

ifeq ($(conf_drivers_virtio),1)
drivers += drivers/virtio.o
ifeq ($(conf_drivers_pci),1)
drivers += drivers/virtio-pci-device.o
endif
drivers += drivers/virtio-vring.o
ifeq ($(conf_drivers_mmio),1)
drivers += drivers/virtio-mmio.o
endif
# Block-device and filesystem virtio drivers (virtio-blk, virtio-fs, nvme)
# are gone together with the filesystem; only virtio-rng remains (virtio-net
# went with the networking stack, virtio-scsi with the SCSI layer).
drivers += drivers/virtio-rng.o
endif

drivers += drivers/kvmclock.o
ifeq ($(conf_drivers_acpi),1)
drivers += drivers/acpi.o
endif
ifeq ($(conf_drivers_hpet),1)
drivers += drivers/hpet.o
endif
drivers += drivers/rtc.o
ifeq ($(conf_drivers_ahci),1)
drivers += drivers/ahci.o
endif
ifeq ($(conf_drivers_ide),1)
drivers += drivers/ide.o
endif
endif # x64

ifeq ($(arch),aarch64)
drivers += drivers/mmio-isa-serial.o
drivers += drivers/pl011.o
drivers += drivers/pl031.o
ifeq ($(conf_drivers_cadence),1)
drivers += drivers/cadence-uart.o
endif

ifeq ($(conf_drivers_virtio),1)
drivers += drivers/virtio.o
ifeq ($(conf_drivers_pci),1)
drivers += drivers/virtio-pci-device.o
endif
ifeq ($(conf_drivers_mmio),1)
drivers += drivers/virtio-mmio.o
endif
drivers += drivers/virtio-vring.o
# Block-device and filesystem drivers are gone with the filesystem.
drivers += drivers/virtio-rng.o
endif
endif # aarch64

ifeq ($(conf_tracepoints),1)
objects += arch/$(arch)/arch-trace.o
endif
# The application is statically linked into the kernel image and entered via
# osv_app_main(). There is no separate app .so or filesystem image.
#
# Two build modes select which application is linked in. Each application lives
# in its own directory with a Makefile fragment that lists its objects in
# $(app-objects); the kernel compiles and links them with its own flags.
#   make            -> the user application   (app/)
#   make app=tests  -> the test application   (test/)
app ?= default
ifeq ($(app),tests)
include test/Makefile
else
include app/Makefile
endif
objects += $(app-objects)

# Record the selected app mode so that switching between `make` and
# `make app=tests` forces the kernel to relink (the set of linked-in app
# objects changes, which plain timestamp checking would not notice).
app_mode_dep = $(out)/app_mode.last
.PHONY: app_mode_phony
$(app_mode_dep): app_mode_phony
	$(call very-quiet, $(makedir))
	@if [ "$$(cat $(app_mode_dep) 2>/dev/null)" != "$(app)" ]; then \
		echo -n "$(app)" > $(app_mode_dep); \
	fi
# Minimal boot-time self-relocator (replaces the relocation half of the old
# ELF loader).
objects += arch/x64/relocate.o
objects += arch/$(arch)/arch-setup.o
objects += arch/$(arch)/signal.o
objects += arch/$(arch)/arch-cpu.o
objects += arch/$(arch)/backtrace.o
objects += arch/$(arch)/smp.o
objects += arch/$(arch)/tlsdesc.o
objects += arch/$(arch)/entry.o
objects += arch/$(arch)/mmu.o
objects += arch/$(arch)/exceptions.o
objects += arch/$(arch)/dump.o
objects += arch/$(arch)/cpuid.o
objects += arch/$(arch)/firmware.o
objects += arch/$(arch)/hypervisor.o
objects += arch/$(arch)/interrupt.o
ifeq ($(conf_drivers_pci),1)
objects += arch/$(arch)/pci.o
objects += arch/$(arch)/msi.o
endif
objects += arch/$(arch)/power.o
objects += arch/$(arch)/feexcept.o
ifeq ($(conf_memory_optimize),1)
wno-unknown-attributes := $(call compiler-flag, -Wno-unknown-attributes, -Wno-unknown-attributes, compiler/empty.cc)
$(out)/arch/x64/string-ssse3.o: CXXFLAGS += -mssse3 $(wno-unknown-attributes)
$(out)/arch/x64/string.o: CXXFLAGS += $(wno-unknown-attributes)
endif

ifeq ($(arch),aarch64)
objects += arch/$(arch)/psci.o
objects += arch/$(arch)/arm-clock.o
objects += arch/$(arch)/gic-common.o
objects += arch/$(arch)/gic-v2.o
objects += arch/$(arch)/gic-v3.o
objects += arch/$(arch)/arch-dtb.o
objects += arch/$(arch)/hypercall.o
ifeq ($(conf_memory_optimize),1)
objects += arch/$(arch)/memset.o
objects += arch/$(arch)/memcpy.o
objects += arch/$(arch)/memmove.o
endif
objects += arch/$(arch)/sched.o
objects += $(libfdt)
endif

ifeq ($(arch),x64)
objects += arch/x64/dmi.o
ifeq ($(conf_memory_optimize),1)
objects += arch/x64/string.o
objects += arch/x64/string-ssse3.o
endif
objects += arch/x64/ioapic.o
objects += arch/x64/apic.o
objects += arch/x64/apic-clock.o
objects += arch/x64/prctl.o
objects += arch/x64/vmlinux.o
objects += arch/x64/vmlinux-boot64.o
objects += arch/x64/pvh-boot.o
objects += arch/x64/pvh-entry.o
endif # x64

objects += core/math.o
objects += core/spinlock.o
objects += core/lfmutex.o
objects += core/rwlock.o
objects += core/semaphore.o
objects += core/condvar.o
objects += core/debug.o
objects += core/rcu.o
objects += core/mempool.o
ifeq ($(conf_memory_tracker),1)
objects += core/alloctracker.o
endif
objects += core/printf.o
ifeq ($(conf_tracepoints_sampler),1)
objects += core/sampler.o
endif

objects += core/futex.o
objects += core/sched.o
objects += core/mmio.o
objects += core/kprintf.o
ifeq ($(conf_tracepoints),1)
objects += core/trace.o
objects += core/trace-count.o
ifeq ($(conf_tracepoints_strace),1)
objects += core/strace.o
endif
objects += core/callstack.o
endif
# poll/select/epoll operate on the (removed) file-descriptor table.
ifeq ($(conf_core_newpoll),1)
objects += core/newpoll.o
endif
objects += core/power.o
objects += core/percpu.o
objects += core/per-cpu-counter.o
objects += core/percpu-worker.o
objects += core/shutdown.o
objects += core/version.o
objects += core/waitqueue.o
objects += core/chart.o
objects += core/demangle.o
objects += core/async.o
objects += core/libaio.o
objects += core/options.o
objects += core/string_utils.o

#include $(src)/libc/build.mk:
libc =
libc_to_hide =
environ_libc =

libc += internal/_chk_fail.o
libc_to_hide += internal/_chk_fail.o
libc += internal/floatscan.o
libc += internal/intscan.o
libc += internal/libc.o
libc += internal/shgetc.o



libc += env/__environ.o
libc += env/secure_getenv.o

environ_libc += env/__environ.c
environ_libc += env/clearenv.c
environ_libc += env/getenv.c
environ_libc += env/secure_getenv.c
environ_libc += env/putenv.c
environ_libc += env/setenv.c
environ_libc += env/unsetenv.c
environ_libc += string/strchrnul.c


libc += errno/strerror.o

libc += math/finitel.o

# Issue #867: Gcc 4.8.4 has a bug where it optimizes the trivial round-
# related functions incorrectly - it appears to convert calls to any
# function called round() to calls to a function called lround() -
# and similarly for roundf() and roundl().
# None of the specific "-fno-*" options disable this buggy optimization,
# unfortunately. The simplest workaround is to just disable optimization
# for the affected files.

libc += misc/error.o
libc += misc/getopt.o
libc_to_hide += misc/getopt.o
libc += misc/getopt_long.o
libc_to_hide += misc/getopt_long.o
libc += misc/realpath.o
libc += misc/backtrace.o
libc += misc/uname.o
libc += misc/lockf.o
libc += misc/__longjmp_chk.o


libc += multibyte/__mbsnrtowcs_chk.o
libc += multibyte/__mbsrtowcs_chk.o
libc += multibyte/__mbstowcs_chk.o


libc += prng/random.o
libc += random.o

libc += process/execve.o
libc += process/waitpid.o

libc += arch/$(arch)/setjmp/sigsetjmp.o
libc += signal/block.o
libc += signal/siglongjmp.o
ifeq ($(arch),x64)
libc += arch/$(arch)/ucontext/getcontext.o
libc += arch/$(arch)/ucontext/setcontext.o
libc += arch/$(arch)/ucontext/start_context.o
libc_to_hide += arch/$(arch)/ucontext/start_context.o
libc += arch/$(arch)/ucontext/ucontext.o
ifeq ($(conf_memory_optimize),1)
libc += string/memmove.o
endif
endif


# stdio is OSv-owned: the FILE struct (libc/stdio/stdio_impl.h) is ours and the
# write path goes through OSv's console (__stdout_write), so it cannot come from
# llvm-libc (whose FILE has no OSv console backend). The musl-derived sources
# were vendored into libc/stdio/ (Phase 8.11). tmpfile/tmpnam/tempnam were
# dropped: they create/stat files (dead with no filesystem, and unreferenced).
libc += stdio/__fclose_ca.o
libc += stdio/__fdopen.o
$(out)/libc/stdio/__fdopen.o: CFLAGS += --include libc/syscall_to_function.h
libc += stdio/__fmodeflags.o
libc += stdio/__fopen_rb_ca.o
libc += stdio/__fprintf_chk.o
libc += stdio/__lockfile.o
libc += stdio/__overflow.o
libc += stdio/__stdio_close.o
$(out)/libc/stdio/__stdio_close.o: CFLAGS += --include libc/syscall_to_function.h
libc += stdio/__stdio_exit.o
libc += stdio/__stdio_read.o
libc += stdio/__stdio_seek.o
$(out)/libc/stdio/__stdio_seek.o: CFLAGS += --include libc/syscall_to_function.h
libc += stdio/__stdio_write.o
$(out)/libc/stdio/__stdio_write.o: CFLAGS += --include libc/syscall_to_function.h
libc += stdio/__stdout_write.o
libc += stdio/__string_read.o
libc += stdio/__toread.o
libc += stdio/__towrite.o
libc += stdio/__uflow.o
libc += stdio/__vfprintf_chk.o
libc += stdio/ofl.o
libc += stdio/ofl_add.o
libc += stdio/asprintf.o
libc += stdio/clearerr.o
libc += stdio/dprintf.o
libc += stdio/ext.o
libc += stdio/ext2.o
libc += stdio/fclose.o
libc += stdio/feof.o
libc += stdio/ferror.o
libc += stdio/fflush.o
libc += stdio/fgetc.o
libc += stdio/fgetln.o
libc += stdio/fgetpos.o
libc += stdio/fgets.o
libc += stdio/fileno.o
libc += stdio/flockfile.o
libc += stdio/fmemopen.o
libc += stdio/fopen.o
$(out)/libc/stdio/fopen.o: CFLAGS += --include libc/syscall_to_function.h
libc += stdio/fprintf.o
libc += stdio/fputc.o
libc += stdio/fputs.o
libc += stdio/fread.o
libc += stdio/__fread_chk.o
libc += stdio/freopen.o
$(out)/libc/stdio/freopen.o: CFLAGS += --include libc/syscall_to_function.h
libc += stdio/fscanf.o
libc += stdio/fseek.o
libc += stdio/fsetpos.o
libc += stdio/ftell.o
libc += stdio/ftrylockfile.o
libc += stdio/funlockfile.o
libc += stdio/fwrite.o
libc += stdio/getc.o
libc += stdio/getc_unlocked.o
libc += stdio/getchar.o
libc += stdio/getchar_unlocked.o
libc += stdio/getdelim.o
libc += stdio/getline.o
libc += stdio/gets.o
libc += stdio/getw.o
libc += stdio/open_memstream.o
libc += stdio/perror.o
libc += stdio/printf.o
libc += stdio/putc.o
libc += stdio/putc_unlocked.o
libc += stdio/putchar.o
libc += stdio/putchar_unlocked.o
libc += stdio/puts.o
libc += stdio/putw.o
libc += stdio/remove.o
libc += stdio/rewind.o
libc += stdio/scanf.o
libc += stdio/setbuf.o
libc += stdio/setbuffer.o
libc += stdio/setlinebuf.o
libc += stdio/setvbuf.o
libc += stdio/snprintf.o
libc += stdio/sprintf.o
libc += stdio/sscanf.o
libc += stdio/stderr.o
libc += stdio/stdin.o
libc += stdio/stdout.o
libc += stdio/ungetc.o
libc += stdio/vasprintf.o
libc += stdio/vdprintf.o
libc += stdio/vfprintf.o
$(out)/libc/stdio/vfprintf.o: COMMON += $(wno-maybe-uninitialized)
libc += stdio/vfscanf.o
$(out)/libc/stdio/vfscanf.o: COMMON += $(wno-maybe-uninitialized)
libc += stdio/vprintf.o
libc += stdio/vscanf.o
libc += stdio/vsnprintf.o
libc += stdio/vsprintf.o
libc += stdio/vsscanf.o
libc += stdio/printf-hooks.o

$(out)/libc/stdlib/qsort_r.o: COMMON += $(wno-dangling-pointer)
ifeq ($(arch),x64)
libc += stdlib/unimplemented.o
endif

libc += string/__memcpy_chk.o
libc += string/explicit_bzero.o
libc += string/__explicit_bzero_chk.o
ifeq ($(conf_memory_optimize),1)
libc += string/memcpy.o
libc_to_hide += string/memcpy.o
else
endif
libc += string/__memmove_chk.o
libc += string/memset.o
libc_to_hide += string/memset.o
libc += string/__memset_chk.o
libc += string/rawmemchr.o
libc += string/__stpcpy_chk.o
libc += string/__strcat_chk.o
libc += string/__strcpy_chk.o
libc += string/strerror_r.o
libc += string/__strncat_chk.o
libc += string/__strncpy_chk.o
libc += string/stresep.o
libc_to_hide += string/stresep.o
libc += string/__wcscpy_chk.o
libc += string/__wcsncpy_chk.o
libc += string/__wmemcpy_chk.o
libc += string/__wmemmove_chk.o
libc += string/__wmemset_chk.o


libc += time/__tz.o
$(out)/libc/time/__tz.o: pre-include-api = -isystem include/api/internal_musl_headers
libc += time/__year_to_secs.o
$(out)/libc/time/ftime.o: CFLAGS += -Ilibc/include


libc += unistd/sethostname.o
libc += unistd/sync.o
libc += unistd/getpgid.o
libc += unistd/setpgid.o
libc += unistd/getpgrp.o
libc += unistd/getppid.o
libc += unistd/getsid.o
libc += unistd/ttyname_r.o


libc += pthread.o
libc_to_hide += pthread.o
libc += pthread_barrier.o
libc += libc.o
libc += dlfcn.o
libc += io.o
libc += time.o
libc_to_hide += time.o
libc += signal.o
libc_to_hide += signal.o
libc += mman.o
libc_to_hide += mman.o
libc += sem.o
libc_to_hide += sem.o
# pipe, af_local (unix sockets), mount, eventfd, timerfd, shm and inotify all
# depend on the (removed) file-descriptor table and filesystem.
libc += user.o
libc += resource.o
libc += syslog.o
libc += cxa_thread_atexit.o
libc += cpu_set.o
libc += malloc_hooks.o
libc += mallopt.o

libc += linux/makedev.o



# There is no filesystem: the kernel has no VFS, no fd table and no on-disk or
# in-memory file systems. Minimal console-backed stdio lives in libc/io.cc.
objects += $(addprefix libc/, $(libc))

libc_objects_to_hide = $(addprefix $(out)/libc/, $(libc_to_hide))
$(libc_objects_to_hide): cc-hide-flags = $(cc-hide-flags-$(conf_hide_symbols))
$(libc_objects_to_hide): cxx-hide-flags = $(cxx-hide-flags-$(conf_hide_symbols))

# Compiler builtins (soft-int helpers like __umodti3) come from LLVM compiler-rt,
# not GNU libgcc - no GCC anywhere in the toolchain. The C++ exception unwinder
# that used to come from libgcc_eh.a is now libunwind (Phase 8.7), and the C++
# runtime that used to come from libstdc++.a is now libc++/libc++abi (Phase 8.7).
compiler_rt_builtins := $(shell $(CC) --rtlib=compiler-rt --print-libgcc-file-name)
ifeq ($(filter /%,$(compiler_rt_builtins)),)
    $(error Error: LLVM compiler-rt builtins not found. Install the clang/compiler-rt runtime.)
endif

#Allow user specify non-default location of boost
# Boost is vendored under external/boost (a header-only subset, see that
# directory's README) so the build does not depend on a system-installed Boost.
# Boost.System is header-only in modern Boost, so no Boost library is linked.
boost-includes = -isystem external/boost
boost-libs :=

# nfs/ext null vfsops went with the filesystem.

$(out)/loader.o: CXXFLAGS += -DHIDE_SYMBOLS=$(conf_hide_symbols)
$(out)/core/trace.o: CXXFLAGS += -DHIDE_SYMBOLS=$(conf_hide_symbols)

# The OSv kernel is linked into an ordinary, non-PIE, executable, so there is no point in compiling
# with -fPIC or -fpie and objects that can be linked into a PIE. On the contrary, PIE-compatible objects
# have overheads and can cause problems (see issue #1112). Recently, on some systems gcc's
# default was changed to use -fpie, so we need to undo this default by explicitly specifying -fno-pie.
$(objects:%=$(out)/%) $(drivers:%=$(out)/%) $(out)/arch/$(arch)/boot.o $(out)/loader.o $(out)/runtime.o: COMMON += -fno-pie

# ld has a known bug (https://sourceware.org/bugzilla/show_bug.cgi?id=6468)
# where if the executable doesn't use shared libraries, its .dynamic section
# is dropped, even when we use the "--export-dynamic" (which is silently
# ignored). The workaround is to link loader.elf with a do-nothing library.
$(out)/dummy-shlib.so: $(out)/dummy-shlib.o
	$(call quiet, $(CXX) -nodefaultlibs -shared $(gcc-sysroot) -o $@ $^, LINK $@)

stage1_targets = $(out)/arch/$(arch)/boot.o $(out)/loader.o $(out)/runtime.o $(drivers:%=$(out)/%) $(objects:%=$(out)/%) $(out)/dummy-shlib.so
stage1: $(stage1_targets) links
.PHONY: stage1

loader_options_dep = $(out)/arch/$(arch)/loader_options.ld
$(loader_options_dep): stage1
	$(makedir)
	@if [ '$(shell cat $(loader_options_dep) 2>&1)' != 'APP_LOCAL_EXEC_TLS_SIZE = $(app_local_exec_tls_size);' ]; then \
		echo -n "APP_LOCAL_EXEC_TLS_SIZE = $(app_local_exec_tls_size);" > $(loader_options_dep) ; \
	fi

ifeq ($(conf_hide_symbols),1)
version_script_file:=$(out)/version_script
#Detect which version script to be used and copy to $(out)/version_script
#so that loader.elf/zfs_builder.elf is rebuilt accordingly if version script has changed
ifdef conf_version_script
ifeq (,$(wildcard $(conf_version_script)))
    $(error Missing version script: $(conf_version_script))
endif
ifneq ($(shell cmp $(out)/version_script $(conf_version_script)),)
$(shell cp $(conf_version_script) $(out)/version_script)
endif
else
ifneq ($(shell cmp $(out)/version_script $(out)/default_version_script),)
$(shell cp $(out)/default_version_script $(out)/version_script)
endif
endif
linker_archives_options = --no-whole-archive --start-group $(libcxx_archives) --end-group \
  $(compiler_rt_builtins) $(boost-libs) --gc-sections
else
# The C++ runtime (libc++ / libc++abi / libunwind) is linked on demand inside a
# --start-group, not whole-archived: the slim kernel links its single app at
# build time and exports no dynamic C++ ABI, so only referenced runtime objects
# are needed. The group resolves the libc++ <-> libc++abi cycle, and pulling
# each symbol once avoids the multiple-definition clash from the libunwind
# objects that the build bundles into all three archives.
linker_archives_options = --no-whole-archive --start-group $(libcxx_archives) --end-group $(boost-libs) $(compiler_rt_builtins)
endif

# LLVM libc (LLVM_LIBC_PLAN.md): llvm-libc is the generic libc, linked trailing
# so it supplies any symbol the kernel and the OSv libc objects do not define.
# The archive is a no-syscall baremetal libc built automatically by
# scripts/build-llvm-libc.sh (x86/arm only, no manual step). The functions
# llvm-libc 22.1.7 lacks (the stdio FILE glue, x87 long-double helpers, a few
# env/time/string/multibyte leaves) are now OSv-owned, musl-derived sources
# under libc/ - the musl submodule was deleted in Phase 8.13.
llvm_libc_libdir = build/llvm-libc/$(arch)/libc/lib
llvm_libc_archives = $(llvm_libc_libdir)/libc.a $(llvm_libc_libdir)/libm.a
$(out)/.llvm-libc-built: scripts/build-llvm-libc.sh tools/llvm-libc/entrypoints.txt tools/llvm-libc/config.json tools/llvm-libc/headers.txt
	$(call quiet, scripts/build-llvm-libc.sh $(arch), LLVM-LIBC $(arch))
	$(call very-quiet, touch $@)
$(llvm_libc_archives): $(out)/.llvm-libc-built
llvm_libc_dep = $(llvm_libc_archives)
# compiler-rt builtins repeat after the llvm-libc archives: their members need
# its soft-int helpers (e.g. __umodti3), and ld resolves archives in order.
linker_archives_options += $(llvm_libc_archives) $(compiler_rt_builtins)

# libc++ / libc++abi / libunwind replace the host GNU libstdc++.a and the libgcc
# exception unwinder (LLVM_LIBC_PLAN.md, Phase 8). Built by scripts/build-libcxx.sh
# (x86/arm only, no manual step) from the same pinned llvm-project, localization
# OFF. They are linked (in the --start-group in linker_archives_options above) in
# place of libstdc++.a/libgcc_eh.a; libunwind supplies the C++ exception unwinder
# (OSv uses C++ exceptions) and locates the kernel's .eh_frame via OSv's
# dl_iterate_phdr.
libcxx_libdir = build/libcxx/$(arch)/lib
libcxx_archives = $(libcxx_libdir)/libc++.a $(libcxx_libdir)/libc++abi.a $(libcxx_libdir)/libunwind.a
$(out)/.libcxx-built: scripts/build-libcxx.sh
	$(call quiet, scripts/build-libcxx.sh $(arch), LIBCXX $(arch))
	$(call very-quiet, touch $@)
$(libcxx_archives): $(out)/.libcxx-built
libcxx_dep = $(libcxx_archives)

# Former musl gaps, now resolved (Phase 8.12). Most are supplied by llvm-libc
# (the narrow ctype family, and the bulk of <time.h>: asctime/ctime/gmtime/
# localtime/mktime/strftime/difftime and the _r variants) - just deleting the
# `musl +=` lines lets the trailing archive fill them. The handful llvm-libc
# lacks were vendored into libc/ below. Dropped as dead/unreferenced: the
# __ctype_*_loc glibc table accessors, the whole temp/ family (mkstemp/mkdtemp/
# mktemp/__randname - no filesystem), and time/{strptime,timegm,getdate,ftime,
# __secs_to_tm,__tm_to_secs}.
libc += math/__fpclassifyl.o     # x87 long-double classify (vfprintf %Lf)
libc += math/__signbitl.o
libc += time/__month_to_secs.o
libc += time/time.o              # time() -> OSv clock_gettime
libc += multibyte/mbsinit.o      # vfscanf
libc += string/strchrnul.o       # provides strchrnul + hidden __strchrnul (env)
libc += string/strsignal.o
libc += env/getenv.o
libc += env/setenv.o
libc += env/putenv.o
libc += env/unsetenv.o
libc += env/clearenv.o
# env + strchrnul use the musl-internal decls (__environ, __putenv, __strchrnul)
# that live in OSv's internal_musl_headers (the kernel build does not otherwise
# put that dir on the path, unlike the libenviron.so build).
$(out)/libc/env/%.o: pre-include-api = -isystem include/api/internal_musl_headers
$(out)/libc/string/strchrnul.o: pre-include-api = -isystem include/api/internal_musl_headers

ifeq ($(arch),aarch64)
def_symbols = --defsym=OSV_KERNEL_VM_BASE=$(kernel_vm_base)
else
def_symbols = --defsym=OSV_KERNEL_BASE=$(kernel_base) \
              --defsym=OSV_KERNEL_VM_BASE=$(kernel_vm_base) \
              --defsym=OSV_KERNEL_VM_SHIFT=$(kernel_vm_shift)
endif

$(out)/loader.elf: $(stage1_targets) arch/$(arch)/loader.ld $(out)/bootfs.o $(out)/libvdso-content.o $(loader_options_dep) $(app_mode_dep) $(version_script_file) $(llvm_libc_dep) $(libcxx_dep)
	$(call quiet, $(LD) -o $@ $(def_symbols) \
		-Bdynamic --export-dynamic --eh-frame-hdr --enable-new-dtags -L$(out)/arch/$(arch) \
            $(patsubst %version_script,--version-script=%version_script,$(patsubst %.ld,-T %.ld,$(filter-out $(app_mode_dep) $(llvm_libc_dep) $(libcxx_dep),$^))) \
	    $(linker_archives_options) $(conf_linker_extra_options), \
		LINK loader.elf)
	@# Build libosv.so matching this loader.elf. This is not a separate
	@# rule because that caused bug #545.
	@readelf --dyn-syms --wide $(out)/loader.elf > $(out)/osv.syms
	@scripts/libosv.py $(out)/osv.syms $(out)/libosv.ld `scripts/osv-version.sh` | $(CC) -c -o $(out)/osv.o -x assembler -
	@echo '0000000000000000 T _text' > $(out)/osv.kallsyms
	@echo '0000000000000000 T _stext' >> $(out)/osv.kallsyms
	@grep ': 0000' $(out)/osv.syms | grep -v 'NOTYPE' | awk '{ print $$2 " T " $$8 }' | c++filt >> $(out)/osv.kallsyms
	$(call quiet, $(CC) $(out)/osv.o -nostdlib -shared -o $(out)/libosv.so -T $(out)/libosv.ld, LIBOSV.SO)



environ_sources = $(addprefix libc/, $(environ_libc))

$(out)/libenviron.so: pre-include-api = -isystem include/api/internal_musl_headers
$(out)/libenviron.so: source-dialects =

$(out)/libenviron.so: $(environ_sources)
	$(makedir)
	 $(call quiet, $(CC) $(CFLAGS) -shared -o $(out)/libenviron.so $(environ_sources), CC libenviron.so)

$(out)/libvdso.so: libc/vdso/vdso.cc
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -fno-exceptions -c -fPIC -o $(out)/libvdso.o libc/vdso/vdso.cc, CXX libvdso.o)
	$(call quiet, $(LD) -shared -z now -o $(out)/libvdso.so $(out)/libvdso.o -T libc/vdso/vdso.lds --version-script=libc/vdso/$(arch)/vdso.version, LINK libvdso.so)

bootfs_manifest ?= bootfs_empty.manifest.skel

# If parameter "bootfs_manifest" has been changed since the last make,
# bootfs.bin requires rebuilding
bootfs_manifest_dep = $(out)/bootfs_manifest.last
.PHONY: phony
$(bootfs_manifest_dep): phony
	@if [ '$(shell cat $(bootfs_manifest_dep) 2>&1)' != '$(bootfs_manifest)' ]; then \
		echo -n $(bootfs_manifest) > $(bootfs_manifest_dep) ; \
	fi

bootfs_dep := scripts/mkbootfs.py $(bootfs_manifest) $(bootfs_manifest_dep) $(out)/libenviron.so
$(out)/bootfs.bin: $(bootfs_dep)
	$(call quiet, olddir=`pwd`; cd $(out); "$$olddir"/scripts/mkbootfs.py -o bootfs.bin -d bootfs.bin.d -m "$$olddir"/$(bootfs_manifest), MKBOOTFS $@)

$(out)/bootfs.o: $(out)/bootfs.bin
$(out)/bootfs.o: ASFLAGS += -I$(out)

$(out)/libvdso-stripped.so: $(out)/libvdso.so
	$(call quiet, $(STRIP) $(out)/libvdso.so -o $(out)/libvdso-stripped.so, STRIP libvdso.so -> libvdso-stripped.so)

$(out)/libvdso-content.o: $(out)/libvdso-stripped.so
$(out)/libvdso-content.o: ASFLAGS += -I$(out)


################################################################################
# The dependencies on header files are automatically generated only after the
# first compilation, as explained above. However, header files generated by
# the Makefile are special, in that they need to be created even *before* the
# first compilation. Moreover, some (namely version.h) need to perhaps be
# re-created on every compilation. "generated-headers" is used as an order-
# only dependency on C compilation rules above, so we don't try to compile
# C code before generating these headers.
generated-headers: $(out)/gen/include/bits/alltypes.h perhaps-modify-version-h
.PHONY: generated-headers

# While other generated headers only need to be generated once, version.h
# should be recreated on every compilation. To avoid a cascade of
# recompilation, the rule below makes sure not to modify version.h's timestamp
# if the version hasn't changed.
# conf/Makefile used to pre-create $(out)/gen/include/osv when generating the
# kconfig headers; with kconfig gone the generated-headers rules create it.
perhaps-modify-version-h:
	$(call very-quiet, mkdir -p $(out)/gen/include/osv)
	$(call quiet, sh scripts/gen-version-header $(out)/gen/include/osv/version.h, GEN gen/include/osv/version.h)
.PHONY: perhaps-modify-version-h

# The CONF_drivers_* macros used by source code (e.g. arch-setup.cc) are frozen
# in the checked-in include/osv/drivers_config.h; nothing is generated here.

$(out)/gen/include/bits/alltypes.h: include/api/$(arch)/bits/alltypes.h.sh
	$(makedir)
	$(call quiet, sh $^ > $@, GEN $@)


################################################################################




