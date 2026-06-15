/*
 * Frozen OSv configuration (formerly kconfig-generated). Edit conf/config.mk
 * and the matching include/osv/kernel_config_*.h header to change a value.
 */
#define CONF_tracepoints 1
#define CONF_compiler_cflags " -msse2"
#define CONF_networking_dhcp 1
#define CONF_core_c_wrappers 1
#define CONF_memory_optimize 1
#define CONF_fs_max_file_descriptors 0x4000
#define CONF_core_rcu_defer_queue_size 2000
#define CONF_syscalls_list_file ""
#define CONF_version_script ""
#define CONF_fs_sysfs 1
#define CONF_core_namespaces 1
#define CONF_compiler_opt " -O2 -DNDEBUG"
#define CONF_fs_buffer_cache_size 256
#define CONF_core_debug_buffer_size 0xc800
#define CONF_tracepoints_sampler 1
#define CONF_memory_l1_pool_size 512
#define CONF_threads_default_kernel_stack_size 65536
#define CONF_drivers_profile_all 1
#define CONF_tracepoints_strace 1
#define CONF_core_epoll 1
#define CONF_threads_default_pthread_stack_size 0x100000
#define CONF_preempt 1
#define CONF_fs_procfs 1
#define CONF_networking_stack 1
#define CONF_core_dynamic_percpu_size 65536
#define CONF_compiler_cxx_level "gnu++14"
#define CONF_core_commands_runscript 1
#define CONF_memory_page_batch_size 32
#define CONF_interrupt_stack_size 0x1000
