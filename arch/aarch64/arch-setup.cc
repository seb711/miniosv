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
#include "arch-dtb.hh"
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

// Derive the physical memory layout and ELF extents from the UEFI memory map,
// replacing what dtb_setup() used to compute from the device tree. Runs at the
// same init priority as the (now inert) dtb_setup constructor; elf_header,
// kernel_vm_shift and osv_boot_info were already set by uefi_entry.
void __attribute__((constructor(init_prio::dtb))) uefi_memory_setup()
{
    auto bi = osv_boot_info;
    auto mm = reinterpret_cast<osv::boot_memmap_entry*>(bi->memmap_addr);
    u64 kbase = bi->kernel_phys_base;

    // Find the usable RAM region that contains the kernel.
    u64 region_end = 0;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (mm[i].type != osv::boot_mem_type::usable)
            continue;
        if (kbase >= mm[i].addr && kbase < mm[i].addr + mm[i].size) {
            region_end = mm[i].addr + mm[i].size;
            break;
        }
    }
    if (!region_end) {
        abort("uefi_memory_setup: kernel not within any usable memory region.\n");
    }

    // mem_addr is the 2MB-aligned base the boot page tables map the kernel
    // window onto; phys_mem_size spans from there to the end of the region.
    mmu::mem_addr = kbase & ~((u64)0x200000 - 1);
    memory::phys_mem_size = region_end - mmu::mem_addr;

    // Command line provided by the UEFI stub (LoadOptions).
    cmdline = reinterpret_cast<char*>(bi->cmdline_addr);

    // Compute the ELF extents exactly as dtb_setup() did.
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
void arch_setup_pci()
{
    pci::set_pci_ecam(dtb_get_pci_is_ecam());

    /* linear_map [TTBR0 - PCI config space] */
    u64 pci_cfg;
    size_t pci_cfg_len;
    if (!dtb_get_pci_cfg(&pci_cfg, &pci_cfg_len)) {
        return;
    }

    pci::set_pci_cfg(pci_cfg, pci_cfg_len);
    pci_cfg = pci::get_pci_cfg(&pci_cfg_len);
    mmu::linear_map((void *)pci_cfg, (mmu::phys)pci_cfg, pci_cfg_len,
		    "pci_cfg", mmu::page_size, mmu::mattr::dev);

    /* linear_map [TTBR0 - PCI I/O and memory ranges] */
    u64 ranges[2]; size_t ranges_len[2];
    if (!dtb_get_pci_ranges(ranges, ranges_len, 2)) {
        abort("arch-setup: failed to get PCI ranges.\n");
    }
    pci::set_pci_io(ranges[0], ranges_len[0]);
    pci::set_pci_mem(ranges[1], ranges_len[1]);
    ranges[0] = pci::get_pci_io(&ranges_len[0]);
    ranges[1] = pci::get_pci_mem(&ranges_len[1]);
    mmu::linear_map((void *)ranges[0], (mmu::phys)ranges[0], ranges_len[0],
                    "pci_io", mmu::page_size, mmu::mattr::dev);
    mmu::linear_map((void *)ranges[1], (mmu::phys)ranges[1], ranges_len[1],
                    "pci_mem", mmu::page_size, mmu::mattr::dev);
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
    // tables are parsed (init_prio::acpi) and before the timer needs it.

#if CONF_drivers_pci
    if (!opt_pci_disabled) {
        arch_setup_pci();
    }
#endif

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

    u64 gicd = 0, gicr = 0, gits = 0, gicc_cpuif = 0;
    size_t gicr_len = 0;
    u8 version = 0;

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
            gicr = r->discovery_range_base_address;
            gicr_len = r->discovery_range_length;
            break;
        }
        case acpi::MADT_GIC_ITS: {
            auto i = reinterpret_cast<const acpi::madt_gic_its*>(s);
            gits = i->physical_base_address;
            break;
        }
        case acpi::MADT_GICC: {
            auto c = reinterpret_cast<const acpi::madt_gicc*>(s);
            // GICv2 takes the CPU interface base from here; GICv3 may publish
            // the per-cpu redistributor here instead of in a GICR subtable.
            if (!gicc_cpuif) {
                gicc_cpuif = c->physical_base_address;
            }
            if (!gicr && c->gicr_base_address) {
                gicr = c->gicr_base_address;
                gicr_len = 0x20000;
            }
            break;
        }
        default:
            break;
        }
        subtable += s->length;
    }

    // The MADT carries base addresses but not region sizes; use the
    // architectural defaults (the GICR length does come from the MADT).
    constexpr size_t GICD_LEN = 0x10000;
    constexpr size_t GITS_LEN = 0x20000;

    if (version == 3 || gicr) {
        gic::gic = new gic::gic_v3_driver(gicd, GICD_LEN, gicr, gicr_len,
                                          gits, gits ? GITS_LEN : 0);
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
        int irqmap_count = dtb_get_pci_irqmap_count();
        if (irqmap_count > 0) {
            u32 mask = dtb_get_pci_irqmask();
            u32 *bdfs = (u32 *)alloca(sizeof(u32) * irqmap_count);
            int *irqs  = (int *)alloca(sizeof(int) * irqmap_count);
            if (!dtb_get_pci_irqmap(bdfs, irqs, irqmap_count)) {
                abort("arch-setup: failed to get PCI irqmap.\n");
            }
            pci::set_pci_irqmap(bdfs, irqs, irqmap_count, mask);
        }

#if CONF_logger_debug
        pci::dump_pci_irqmap();
#endif

        // Enumerate PCI devices
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
    console::mmio_isa_serial_console::_phys_mmio_address = 0;

    int irqid;
    u64 mmio_serial_address = dtb_get_mmio_serial_console(&irqid);
    if (mmio_serial_address) {
        console::mmio_isa_serial_console::early_init(mmio_serial_address);

        new (&console::aarch64_console.isa_serial) console::mmio_isa_serial_console();
        console::aarch64_console.isa_serial.set_irqid(irqid);
        console::arch_early_console = console::aarch64_console.isa_serial;
        return;
    }

    new (&console::aarch64_console.pl011) console::PL011_Console();
    console::arch_early_console = console::aarch64_console.pl011;
    console::PL011_Console::active = true;
    u64 addr = dtb_get_uart(&irqid);
    if (!addr) {
        /* keep using default addresses */
        return;
    }

    console::aarch64_console.pl011.set_base_addr(addr);
    console::aarch64_console.pl011.set_irqid(irqid);
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
