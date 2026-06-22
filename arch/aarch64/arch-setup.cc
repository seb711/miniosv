/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include <osv/kernel_config.h>
#include "arch-setup.hh"
#include <osv/sched.hh>
#include <osv/mempool.hh>
#include <osv/elf.hh>
#include <osv/types.h>
#include <string.h>
#include <osv/boot.hh>
#include <osv/debug.hh>
#include <osv/commands.hh>

#include "arch-mmu.hh"
#include "gic-v2.hh"
#include "gic-v3.hh"
#include "drivers/acpi.hh"

#include "drivers/console.hh"
#include "drivers/pl011.hh"
#include "early-console.hh"
#if CONF_drivers_pci
#include <osv/pci.hh>
#endif
#include "drivers/mmio-isa-serial.hh"

#include <osv/boot-info.hh>
#include <osv/prio.hh>
#include <alloca.h>

// Physical pointer to the hand-off structure filled by the UEFI stub; stored
// by uefi_entry in boot.S.
osv::boot_info* osv_boot_info;

// Kernel command line (set from the UEFI LoadOptions in uefi_memory_setup).
char *cmdline;

// Wall-clock at boot (UEFI GetTime), nanoseconds since the Unix epoch. Captured
// early because boot_info lives in low RAM that is unmapped once the runtime
// page tables are installed, before the clock constructor runs.
u64 osv_boot_unixtime_ns;

// Derive the physical memory layout and ELF extents from the UEFI memory map.
// Runs as an early constructor (init_prio::dtb); elf_header, kernel_vm_shift
// and osv_boot_info were already set by uefi_entry.
void __attribute__((constructor(init_prio::dtb))) uefi_memory_setup()
{
    auto bi = osv_boot_info;
    auto mm = reinterpret_cast<osv::boot_memmap_entry*>(bi->memmap_addr);
    u64 kbase = bi->kernel_phys_base;

    // Re-mark the boot identity map: any usable RAM below 4 GiB must be Normal
    // cacheable, not Device. The static map in boot.S marks the low 1 GiB Device
    // because on QEMU/AWS/GCP that range is pure MMIO and RAM starts at 1 GiB -
    // but Azure places RAM in the low 1 GiB, and running RAM (atomics, unaligned
    // access) through a Device mapping faults. ident_pt_l2_0_ttbr0 is the base
    // of four contiguous 512-entry L2 tables (entry i maps physical i*2 MiB over
    // 0-4 GiB); rewrite the blocks covered by usable RAM to Normal (attr index
    // 4 = 0x411), leaving MMIO blocks Device. This runs before any heap/atomic
    // use (init_prio::dtb, the first post-banner constructor) and while we are
    // still on the boot page tables, so the tables are writable.
    extern u64 ident_pt_l2_0_ttbr0[];
    constexpr u64 BLOCK = 0x200000;            // 2 MiB
    constexpr u64 NR_BLOCKS_4G = 0x100000000ull / BLOCK;  // 2048
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (mm[i].type != osv::boot_mem_type::usable)
            continue;
        u64 first = mm[i].addr / BLOCK;
        u64 last = (mm[i].addr + mm[i].size + BLOCK - 1) / BLOCK;  // exclusive
        if (last > NR_BLOCKS_4G)
            last = NR_BLOCKS_4G;
        for (u64 b = first; b < last; b++)
            ident_pt_l2_0_ttbr0[b] = (b * BLOCK) | 0x411;  // Normal cacheable block
    }
    mmu::flush_tlb_all();

    // mem_addr is the 2MB-aligned base the boot page tables map the kernel
    // window onto. The UEFI stub loaded the kernel here with AllocatePages,
    // which splits the surrounding RAM into several map entries, so we take the
    // top of usable RAM at or above mem_addr as the end of memory (QEMU virt
    // and cloud ARM present RAM as one contiguous block from there up).
    mmu::mem_addr = kbase & ~((u64)0x200000 - 1);

    // Grow a single contiguous run of usable memory upward from the kernel
    // base. OSv models RAM as one [mem_addr, region_end) block, but firmware
    // memory maps are not always contiguous: AWS Nitro presents a low RAM
    // island separated by a reserved hole from a larger high island. Taking
    // the highest usable end (the old behaviour) would span that hole and
    // later free non-existent physical pages into the allocator, faulting on
    // first touch. So extend only across adjacent/overlapping usable entries
    // and stop at the first gap; RAM above the gap is discarded for now (this
    // matches the old aarch64 stub, which deliberately exposed one range).
    u64 region_end = mmu::mem_addr;
    for (bool grew = true; grew;) {
        grew = false;
        for (u32 i = 0; i < bi->memmap_count; i++) {
            if (mm[i].type != osv::boot_mem_type::usable)
                continue;
            u64 start = mm[i].addr, end = start + mm[i].size;
            if (start <= region_end && end > region_end) {
                region_end = end;
                grew = true;
            }
        }
    }
    if (region_end <= mmu::mem_addr) {
        abort("uefi_memory_setup: no usable memory above the kernel.\n");
    }
    memory::phys_mem_size = region_end - mmu::mem_addr;

    // Command line and wall-clock base provided by the UEFI stub.
    cmdline = reinterpret_cast<char*>(bi->cmdline_addr);
    osv_boot_unixtime_ns = bi->boot_unixtime_ns;

    // Compute the ELF extents (physical/virtual start and size).
    u64 edata;
    asm volatile ("adrp %0, .edata" : "=r"(edata));
    extern elf::Elf64_Ehdr *elf_header;
    extern size_t elf_size;
    extern void *elf_start;
    extern u64 kernel_vm_shift;
    mmu::elf_phys_start = reinterpret_cast<void *>(elf_header);
    elf_start = static_cast<char *>(mmu::elf_phys_start) + kernel_vm_shift;
    elf_size = (u64)edata - (u64)elf_start;

    // Account for the memory the kernel image itself occupies.
    mmu::phys addr = (mmu::phys)mmu::elf_phys_start + elf_size;
    memory::phys_mem_size -= addr - mmu::mem_addr;
}

void setup_temporary_phys_map()
{
    // duplicate 1:1 mapping into the lower part of phys_mem
    u64 *pt_ttbr0 = reinterpret_cast<u64*>(processor::read_ttbr0());
    for (auto&& area : mmu::identity_mapped_areas) {
        auto base = reinterpret_cast<void*>(get_mem_area_base(area));
        pt_ttbr0[mmu::pt_index(base, 3)] = pt_ttbr0[0];
    }
    mmu::flush_tlb_all();
}

#if CONF_drivers_pci
// Locate the PCIe ECAM config space from the ACPI MCFG table and map it. Like
// Nanos, this is the only PCI information we take from firmware: the BARs are
// left exactly as UEFI programmed them (arch_add_bar reads them as-is, so the
// io/mem allocation windows are never needed), and device interrupts are
// delivered as MSI-X through the GIC ITS, so there is no INTx routing table to
// parse. Called from arch_init_drivers, after the ACPI tables are parsed.
void arch_setup_pci()
{
    auto mcfg = reinterpret_cast<const acpi::mcfg*>(acpi::find_table(ACPI_SIG_MCFG));
    if (!mcfg) {
        return;   // no PCIe host bridge described - leave PCI disabled
    }

    // Use the first configuration-space allocation (segment 0).
    auto alloc = reinterpret_cast<const acpi::mcfg_alloc*>(
        reinterpret_cast<const char*>(mcfg) + sizeof(acpi::mcfg));
    u64 ecam_base = alloc->base_address;
    size_t ecam_len = (static_cast<size_t>(alloc->end_bus - alloc->start_bus + 1)) << 20;

    pci::set_pci_ecam(true);
    pci::set_pci_cfg(ecam_base, ecam_len);
    mmu::linear_map((void *)ecam_base, (mmu::phys)ecam_base, ecam_len,
                    "pci_cfg", mmu::page_size, mmu::mattr::dev);
}
#endif

extern bool opt_pci_disabled;
void arch_setup_free_memory()
{
    setup_temporary_phys_map();

    /* import from loader.cc */
    extern size_t elf_size;
    extern elf::Elf64_Ehdr* elf_header;

    mmu::phys addr = (mmu::phys)elf_header + elf_size;
    mmu::free_initial_memory_range(addr, memory::phys_mem_size);

    /* linear_map [TTBR1] */
    for (auto&& area : mmu::identity_mapped_areas) {
        auto base = reinterpret_cast<char*>(get_mem_area_base(area));
        mmu::linear_map(base + addr, addr, memory::phys_mem_size,
            area == mmu::mem_area::main ? "main" :
            area == mmu::mem_area::page ? "page" : "mempool");
    }

    /* linear_map [TTBR0 - boot, DTB and ELF] */
    /* physical memory layout - relative to the 2MB-aligned address PA stored in mmu::mem_addr
       PA +     0x0 - PA + 0x80000: boot
       PA + 0x80000 - PA + 0x90000: DTB copy
       PA + 0x90000 -       [addr]: kernel ELF */
    mmu::linear_map((void *)(OSV_KERNEL_VM_BASE - 0x80000), (mmu::phys)mmu::mem_addr,
                    addr - mmu::mem_addr, "kernel");

    if (console::PL011_Console::active) {
        /* linear_map [TTBR0 - UART] */
        addr = (mmu::phys)console::aarch64_console.pl011.get_base_addr();
        mmu::linear_map((void *)addr, addr, 0x1000, "pl011", mmu::page_size,
                        mmu::mattr::dev);
    }

    // The GIC is discovered from the ACPI MADT and constructed in a constructor
    // at init_prio::gic (see init_gic_acpi below), which runs after the ACPI
    // tables are parsed (init_prio::acpi) and before the timer needs it. PCI is
    // set up later in arch_init_drivers, once ACPI (MCFG) is available.

    // Strip console= options from the command line before memory is unmapped.
    // The slim kernel does not turn the cmdline into commands (the app is
    // statically linked in), so there is no parse_cmdline step.
    console::mmio_isa_serial_console::clean_cmdline(cmdline);

    mmu::switch_to_runtime_page_tables();

    console::mmio_isa_serial_console::memory_map();
}

// Not inlined into arch_setup_tls: clang CSEs "mrs tpidr_el0" within a
// function, so a TLS read inlined after the msr that installs the
// thread pointer may use a stale base (see also arch-switch.hh).
static __attribute__((noinline)) void check_boot_tls()
{
    /* check that the tls variable preempt_counter is correct */
    assert(sched::get_preempt_counter() == 1);
}

void arch_setup_tls(void *tls, const elf::tls_data& info)
{
    struct thread_control_block *tcb;
    memset(tls, 0, sizeof(*tcb) + info.size);

    tcb = (thread_control_block *)tls;
    tcb[0].tls_base = &tcb[1];

    memcpy(&tcb[1], info.start, info.filesize);
    asm volatile ("msr tpidr_el0, %0; isb; " :: "r"(tcb) : "memory");

    check_boot_tls();
}

void arch_init_premain()
{
#if CONF_drivers_acpi
    // Hand the RSDP the UEFI stub found to the ACPI layer before its early_init
    // constructor runs - exactly as x86 does in its arch_init_premain.
    acpi::pvh_rsdp_paddr = osv_boot_info->acpi_rsdp;
#endif
}

#if CONF_drivers_acpi
// Discover the GIC from the ACPI MADT and construct the matching driver. This
// walks the MADT subtables the same way arch/x64/smp.cc does, so both arches
// share one ACPI parsing model. It runs after acpi::early_init (init_prio::acpi)
// and before the generic timer, which needs the GIC (init_prio::gic).
static void __attribute__((constructor(init_prio::gic))) init_gic_acpi()
{
    auto madt = reinterpret_cast<const acpi::madt*>(acpi::find_table(ACPI_SIG_MADT));
    if (!madt) {
        abort("arch-setup: no MADT table - cannot locate the GIC.\n");
    }

    u64 gicd = 0, gits = 0, gicc_cpuif = 0;
    u8 version = 0;

    // Collect redistributor regions in both forms the MADT may use: contiguous
    // GICR discovery ranges (one subtable, many frames - QEMU/AWS) and per-CPU
    // gicr_base_address in each GICC entry (one frame each - Azure). Keep them
    // separate and prefer the GICR subtables; fall back to the GICC form only
    // when no GICR subtable is present (the two are mutually exclusive in
    // practice, and a system using GICR subtables sets gicr_base_address to 0).
    mmu::phys gicr_base[MAX_GICR_REGIONS];
    size_t    gicr_len[MAX_GICR_REGIONS];
    int       nr_gicr = 0;
    mmu::phys gicc_redist_base[MAX_GICR_REGIONS];
    int       nr_gicc_redist = 0;

    auto subtable = reinterpret_cast<const char*>(madt + 1);
    auto madt_end = reinterpret_cast<const char*>(madt) + madt->header.length;
    while (subtable < madt_end) {
        auto s = reinterpret_cast<const acpi::madt_subtable*>(subtable);
        switch (s->type) {
        case acpi::MADT_GICD: {
            auto d = reinterpret_cast<const acpi::madt_gicd*>(s);
            gicd = d->physical_base_address;
            version = d->gic_version;
            break;
        }
        case acpi::MADT_GICR: {
            auto r = reinterpret_cast<const acpi::madt_gicr*>(s);
            if (nr_gicr < MAX_GICR_REGIONS) {
                gicr_base[nr_gicr] = r->discovery_range_base_address;
                gicr_len[nr_gicr] = r->discovery_range_length;
                nr_gicr++;
            }
            break;
        }
        case acpi::MADT_GIC_ITS: {
            auto i = reinterpret_cast<const acpi::madt_gic_its*>(s);
            gits = i->physical_base_address;
            break;
        }
        case acpi::MADT_GICC: {
            auto c = reinterpret_cast<const acpi::madt_gicc*>(s);
            // GICv2 takes the CPU interface base from here; GICv3 publishes a
            // per-cpu redistributor here when there is no GICR subtable.
            if (!gicc_cpuif) {
                gicc_cpuif = c->physical_base_address;
            }
            if (c->gicr_base_address && nr_gicc_redist < MAX_GICR_REGIONS) {
                gicc_redist_base[nr_gicc_redist++] = c->gicr_base_address;
            }
            break;
        }
        default:
            break;
        }
        subtable += s->length;
    }

    // The MADT carries base addresses but not region sizes; use the
    // architectural defaults (the GICR discovery-range length comes from the
    // MADT; per-CPU GICC redistributors are one 0x20000 frame each).
    constexpr size_t GICD_LEN = 0x10000;
    constexpr size_t GITS_LEN = 0x20000;
    constexpr size_t GICR_FRAME_LEN = 0x20000;

    if (nr_gicr == 0) {
        for (int i = 0; i < nr_gicc_redist; i++) {
            gicr_base[i] = gicc_redist_base[i];
            gicr_len[i] = GICR_FRAME_LEN;
        }
        nr_gicr = nr_gicc_redist;
    }

    if (version == 3 || nr_gicr) {
        gic::gic = new gic::gic_v3_driver(gicd, GICD_LEN, gicr_base, gicr_len,
                                          nr_gicr, gits, gits ? GITS_LEN : 0);
    } else if (gicd && gicc_cpuif) {
        gic::gic = new gic::gic_v2_driver(gicd, GICD_LEN, gicc_cpuif, 0x2000, 0, 0);
    } else {
        abort("arch-setup: MADT has no usable GIC description.\n");
    }
}
#endif

#include "drivers/driver.hh"

void arch_init_drivers()
{
    extern boot_time_chart boot_time;

#if CONF_drivers_pci
    if (!opt_pci_disabled) {
        // Discover the ECAM config space from the ACPI MCFG (available now that
        // the ACPI tables are parsed), then enumerate. Device interrupts are
        // MSI-X via the GIC ITS, so there is no INTx irqmap to set up.
        arch_setup_pci();

        size_t pci_cfg_len;
        if (pci::get_pci_cfg(&pci_cfg_len)) {
            pci::pci_device_enumeration();
            boot_time.event("pci enumerated");
        }
    }
#endif

    // Initialize all drivers
    hw::driver_manager* drvman = hw::driver_manager::instance();
    boot_time.event("drivers probe");
    drvman->load_all();
    drvman->list_drivers();
}

void arch_init_early_console()
{
    // Like the x86 early console (a fixed COM port), the aarch64 early console
    // uses a compiled-in default UART. UEFI hands off no device tree, and ACPI
    // is not parsed this early, so we rely on the PL011 driver's default base
    // address and IRQ (the ARM PL011 at the standard platform address, as used
    // by QEMU's virt machine and typical ARM firmware).
    console::mmio_isa_serial_console::_phys_mmio_address = 0;

    new (&console::aarch64_console.pl011) console::PL011_Console();
    // Prefer the console UART the firmware advertised via the ACPI SPCR table
    // (the UEFI stub passes its base in boot_info). AWS Graviton's UART is not
    // at the QEMU virt address, so the compiled-in default produces no output
    // there. Fall back to that default when SPCR gave us nothing.
    if (osv_boot_info && osv_boot_info->uart_base) {
        console::aarch64_console.pl011.set_base_addr(osv_boot_info->uart_base);
    }
    console::arch_early_console = console::aarch64_console.pl011;
    console::PL011_Console::active = true;
}

bool arch_setup_console(std::string opt_console)
{
    if (opt_console.compare("pl011") == 0) {
        console::console_driver_add(&console::arch_early_console);
    } else if (opt_console.compare("all") == 0) {
        console::console_driver_add(&console::arch_early_console);
    } else {
        return false;
    }
    return true;
}
