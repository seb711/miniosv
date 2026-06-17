# Frozen OSv kernel configuration.
#
# This file replaces the former kconfig-generated build/$mode/gen/config/kernel_conf.mk
# plus the driver-profile includes. OSv is a single-app kernel with exactly one
# configuration, so the options are baked in here instead of being selected at
# build time. The arch/mode-specific values (compiler flags, optimisation level,
# C++ level, preempt/tracing/debug toggles, lazy stack) still live in their plain
# value files conf/base.mk, conf/$(mode).mk and conf/$(arch).mk, which the main
# Makefile includes directly.
#
# To change a value, edit it here. To remove a subsystem (a driver, networking,
# the filesystem), delete its line below and the corresponding source/Makefile
# wiring - there is no config tool to run.

# --- tracepoints -----------------------------------------------------------
conf_tracepoints=1
conf_tracepoints_sampler=1
conf_tracepoints_strace=1

# --- core ------------------------------------------------------------------
conf_core_c_wrappers=1
conf_core_commands_runscript=1
conf_core_rcu_defer_queue_size=2000
conf_core_debug_buffer_size=0xc800
conf_core_dynamic_percpu_size=65536

# --- memory ----------------------------------------------------------------
conf_memory_optimize=1
conf_memory_l1_pool_size=512
conf_memory_page_batch_size=32

# --- filesystem ------------------------------------------------------------
conf_fs_max_file_descriptors=0x4000

# --- threads / stacks ------------------------------------------------------
conf_threads_default_kernel_stack_size=65536
conf_threads_default_pthread_stack_size=0x100000
conf_interrupt_stack_size=0x1000

# --- device drivers (former drivers_profile=all) ---------------------------
# Used by the main Makefile to decide which driver objects to link; the matching
# CONF_drivers_* macros for source code are frozen in include/osv/drivers_config.h.
conf_drivers_acpi=1
conf_drivers_pci=1
