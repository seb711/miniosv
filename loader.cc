/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include <osv/kernel_config.h>
#include <bsd/init.hh>
#include <bsd/net.hh>
#include <cctype>
#include <osv/elf.hh>
#include "arch-tls.hh"
#include <osv/debug.hh>
#include <osv/clock.hh>
#include <osv/version.hh>

#include "smp.hh"

#include <osv/sched.hh>
#include <osv/barrier.hh>
#include "arch.hh"
#include "arch-setup.hh"
#include "osv/trace.hh"
#include <osv/strace.hh>
#include <osv/power.hh>
#include <osv/rcu.hh>
#include <osv/mempool.hh>
#if CONF_networking_stack
#include <bsd/porting/networking.hh>
#endif
#include <bsd/porting/shrinker.h>
#if CONF_networking_stack
#include <bsd/porting/route.h>
#endif
#if CONF_networking_dhcp
#include <osv/dhcp.hh>
#endif
#include <osv/version.h>
#include <osv/shutdown.hh>
#include <osv/boot.hh>
#include <osv/sampler.hh>
#include <osv/firmware.hh>
#if CONF_drivers_xen
#include <osv/xen.hh>
#endif

#include "drivers/random.hh"
#include "drivers/console.hh"

#include "libc/network/__dns.hh"
#include <processor.hh>
#include <dlfcn.h>
#include <osv/string_utils.hh>

using namespace osv;
using namespace osv::clock::literals;

asm(".pushsection \".debug_gdb_scripts\", \"MS\",@progbits,1 \n"
    ".byte 1 \n"
    ".asciz \"scripts/loader.py\" \n"
    ".popsection \n");

elf::Elf64_Ehdr* elf_header __attribute__ ((aligned(8)));

size_t elf_size;
void* elf_start;
elf::tls_data tls_data;

boot_time_chart boot_time;

static void setup_tls(elf::init_table& inittab)
{
    tls_data = inittab.tls;
    sched::init_tls(tls_data);

    extern char tcb0[]; // defined by linker script
    arch_setup_tls(tcb0, tls_data);
}

extern "C" {
    void premain();
    // The statically linked-in application entry point (see app.cc).
    void osv_app_main();
}

void premain()
{
    arch_init_early_console();

    /* besides reporting the OSV version, this string has the function
       to check if the early console really works early enough,
       without depending on prior initialization. */
    debug_early("OSv " OSV_VERSION "\n");

    arch_init_premain();

#ifdef __x86_64__
    auto elf_header_virt_address = (char*)elf_header + OSV_KERNEL_VM_SHIFT;
#endif

#ifdef __aarch64__
    extern u64 kernel_vm_shift;
    auto elf_header_virt_address = (char*)elf_header + kernel_vm_shift;
#endif

    // Publish the kernel ELF header address for the dynamic-linker
    // introspection used by the C++ exception unwinder (see libc/dlfcn.cc).
    extern void* __kernel_ehdr;
    __kernel_ehdr = elf_header_virt_address;

    auto inittab = elf::get_init(reinterpret_cast<elf::Elf64_Ehdr*>(
        elf_header_virt_address));

    if (inittab.tls.start == nullptr) {
        debug_early("premain: failed to get TLS data from ELF\n");
        arch::halt_no_interrupts();
    }

    setup_tls(inittab);
    boot_time.event(3,"TLS initialization");
    for (auto init = inittab.start; init < inittab.start + inittab.count; ++init) {
        (*init)();
    }
    boot_time.event(".init functions");
}

int main()
{
    smp_initial_find_current_cpu()->init_on_cpu();
    void main_cont();
    sched::init([] { main_cont(); });
}

#if CONF_memory_tracker
static bool opt_leak = false;
#endif
static bool opt_noshutdown = false;
bool opt_power_off_on_abort = false;
#if CONF_tracepoints
static bool opt_log_backtrace = false;
static bool opt_list_tracepoints = false;
#if CONF_tracepoints_strace
static bool opt_strace = false;
#endif
#endif
static bool opt_random = true;
static std::string opt_console = "all";
static bool opt_bootchart = false;
static std::chrono::nanoseconds boot_delay;
bool opt_maxnic = false;
int maxnic;
bool opt_pci_disabled = false;

#if CONF_tracepoints_sampler
static int sampler_frequency;
static bool opt_enable_sampler = false;
#endif



void* do_main_thread(void *_main_args)
{
    if (!arch_setup_console(opt_console)) {
        abort("Unknown console:%s\n", opt_console.c_str());
    }
    arch_init_drivers();
    console::console_init();
    if (opt_random) {
        randomdev::randomdev_init();
    }
    boot_time.event("drivers loaded");

    // There is no filesystem: nothing to mount.

#if CONF_networking_stack
    bool has_if = false;
    osv::for_each_if([&has_if] (std::string if_name) {
        if (if_name == "lo0")
            return;

        has_if = true;
        // Start DHCP by default and wait for an IP
        if (osv::start_if(if_name, "0.0.0.0", "255.255.255.0") != 0 ||
            osv::ifup(if_name) != 0)
            debug("Could not initialize network interface.\n");
    });
    if (has_if) {
#if CONF_networking_dhcp
        if (opt_ip.size() == 0) {
            dhcp_start(true);
        } else {
#endif
            for (auto t : opt_ip) {
                std::vector<std::string> tmp;
                osv::split(tmp, t, " ,", true);
                if (tmp.size() != 3)
                    abort("incorrect parameter on --ip");

                printf("%s: %s\n",tmp[0].c_str(),tmp[1].c_str());

                if (osv::start_if(tmp[0], tmp[1], tmp[2]) != 0)
                    debug("Could not initialize network interface.\n");
            }
            if (opt_defaultgw.size() != 0) {
                osv_route_add_network("0.0.0.0",
                                      "0.0.0.0",
                                      opt_defaultgw.c_str());
            }
            if (opt_nameserver.size() != 0) {
                auto addr = boost::asio::ip::make_address_v4(opt_nameserver);
                osv::set_dns_config({boost::asio::ip::address(addr)}, std::vector<std::string>());
            }
#if CONF_networking_dhcp
        }
#endif
    }

    std::string if_ip;
    auto nr_ips = 0;

    osv::for_each_if([&](std::string if_name) {
        if (if_name == "lo0")
            return;
        if_ip = osv::if_ip(if_name);
        nr_ips++;
    });
    if (nr_ips == 1) {
       setenv("OSV_IP", if_ip.c_str(), 1);
    }
#endif

#if CONF_memory_tracker
    if (opt_leak) {
        debug("Enabling leak detector.\n");
        memory::tracker_enabled = true;
    }
#endif

    boot_time.event("Total time");
#ifdef __x86_64__
    // Some hypervisors like firecracker when booting OSv
    // look for this write to this port as a signal of end of
    // boot time.
    processor::outb(123, 0x3f0);
#endif /* __x86_64__ */

    if (opt_bootchart) {
        boot_time.print_chart();
    } else {
        boot_time.print_total_time();
    }

    // Enter the statically linked-in application.
    osv_app_main();

    return nullptr;
}

void main_cont()
{
    osv::firmware_probe();

    debugf("Firmware vendor: %s\n", osv::firmware_vendor().c_str());

    setenv("OSV_VERSION", osv::version().c_str(), 1);

#if CONF_drivers_xen
    xen::irq_init();
#endif
    smp_launch();
    setenv("OSV_CPUS", std::to_string(sched::cpus.size()).c_str(), 1);
    boot_time.event("SMP launched");

    auto end = osv::clock::uptime::now() + boot_delay;
    while (end > osv::clock::uptime::now()) {
        // spin
    }

    memory::enable_debug_allocator();

    if (sched::cpus.size() > sched::max_cpus) {
        printf("Too many cpus, can't boot with greater than %u cpus.\n", sched::max_cpus);
        poweroff();
    }

#if CONF_tracepoints
    if (opt_list_tracepoints) {
        list_all_tracepoints();
    }

    enable_trace();
    if (opt_log_backtrace) {
        // can only do this after smp_launch, otherwise the IDT is not initialized,
        // and backtrace_safe() fails as soon as we get an exception
        enable_backtraces();
    }
#if CONF_tracepoints_strace
    if (opt_strace) {
        start_strace();
    }
#endif
#endif
    sched::init_detached_threads_reaper();

    bsd_init();

#if CONF_networking_stack
    net_init();
    boot_time.event("Network initialized");
#endif

    arch::irq_enable();

#ifndef AARCH64_PORT_STUB
#if CONF_tracepoints_sampler
    if (opt_enable_sampler) {
        prof::config config{std::chrono::nanoseconds(1000000000 / sampler_frequency)};
        prof::start_sampler(config);
    }
#endif
#endif /* !AARCH64_PORT_STUB */

    // multiple programs can be run -> separate their arguments

    pthread_t pthread;
    // run the payload in a pthread, so pthread_self() etc. work
    // start do_main_thread unpinned (== pinned to all cpus)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (size_t ii=0; ii<sched::cpus.size(); ii++) {
        CPU_SET(ii, &cpuset);
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
    pthread_create(&pthread, &attr, do_main_thread, nullptr);
    void* retval;
    pthread_join(pthread, &retval);

    if (opt_noshutdown) {
        // If the --noshutdown option is given, continue running the system,
        // and whatever threads might be running, even after main returns
        debug("main() returned.\n");
        sched::thread::wait_until([] { return false; });
    }

#if CONF_memory_tracker
    if (memory::tracker_enabled) {
        debug("Leak testing done. Please use 'osv leak show' in gdb to analyze results.\n");
        osv::halt();
    } else {
#endif
        osv::shutdown();
#if CONF_memory_tracker
    }
#endif
}
