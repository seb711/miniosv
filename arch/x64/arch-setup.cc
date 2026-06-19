/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include "arch.hh"
#include "arch-cpu.hh"
#include "arch-setup.hh"
#include <osv/mempool.hh>
#include <osv/mmu.hh>
#include "processor.hh"
#include "processor-flags.h"
#include "msr.hh"
#include <osv/elf.hh>
#include <osv/types.h>
#include <alloca.h>
#include <string.h>
#include <osv/boot.hh>
#include "dmi.hh"
#if CONF_drivers_acpi
#include "drivers/acpi.hh"
#endif

// Physical pointer to the hand-off structure filled by the UEFI stub; stored
// by start64 in boot.S.
osv::boot_info* osv_boot_info;

void setup_temporary_phys_map()
{
    // duplicate 1:1 mapping into phys_mem
    u64 cr3 = processor::read_cr3();
    auto pt = reinterpret_cast<u64*>(cr3);
    for (auto&& area : mmu::identity_mapped_areas) {
        auto base = reinterpret_cast<void*>(get_mem_area_base(area));
        pt[mmu::pt_index(base, 3)] = pt[0];
    }
}

// Iterate the usable physical RAM ranges reported by the UEFI stub in
// osv::boot_info.
void for_each_usable_range(void (*f)(mem_range e))
{
    auto mm = reinterpret_cast<osv::boot_memmap_entry*>(osv_boot_info->memmap_addr);
    for (u32 i = 0; i < osv_boot_info->memmap_count; i++) {
        if (mm[i].type == osv::boot_mem_type::usable) {
            f(mem_range{mm[i].addr, mm[i].size});
        }
    }
}

bool intersects(const mem_range& ent, u64 a)
{
    return a > ent.addr && a < ent.addr + ent.size;
}

mem_range truncate_below(mem_range ent, u64 a)
{
    u64 delta = a - ent.addr;
    ent.addr += delta;
    ent.size -= delta;
    return ent;
}

mem_range truncate_above(mem_range ent, u64 a)
{
    u64 delta = ent.addr + ent.size - a;
    ent.size -= delta;
    return ent;
}

extern elf::Elf64_Ehdr* elf_header;
extern size_t elf_size;
extern void* elf_start;
extern boot_time_chart boot_time;

void arch_setup_free_memory()
{
    static ulong edata, edata_phys;
    asm ("movl $.edata, %0" : "=rm"(edata));
    edata_phys = edata - OSV_KERNEL_VM_SHIFT;

    for_each_usable_range([] (mem_range ent) {
        memory::phys_mem_size += ent.size;
    });
    constexpr u64 initial_map = 1 << 30; // 1GB mapped by startup code

    auto c = processor::cpuid(0x80000000);
    if (c.a >= 0x80000008) {
        c = processor::cpuid(0x80000008);
        mmu::phys_bits = c.a & 0xff;
        mmu::virt_bits = (c.a >> 8) & 0xff;
        if(mmu::phys_bits > mmu::max_phys_bits){
            mmu::phys_bits = mmu::max_phys_bits;
        }
    }

    setup_temporary_phys_map();

    // setup all memory up to 1GB.  We can't free any more, because no
    // page tables have been set up, so we can't reference the memory being
    // freed.
    for_each_usable_range([] (mem_range ent) {
        // can't free anything below edata_phys, it's core code.
        // can't free anything below kernel at this moment
        if (ent.addr + ent.size <= edata_phys) {
            return;
        }
        if (intersects(ent, edata_phys)) {
            ent = truncate_below(ent, edata_phys);
        }
        // ignore anything above 1GB, we haven't mapped it yet
        if (intersects(ent, initial_map)) {
            ent = truncate_above(ent, initial_map);
        } else if (ent.addr >= initial_map) {
            return;
        }
        mmu::free_initial_memory_range(ent.addr, ent.size);
    });
    for (auto&& area : mmu::identity_mapped_areas) {
        auto base = reinterpret_cast<void*>(get_mem_area_base(area));
        mmu::linear_map(base, 0, initial_map,
            area == mmu::mem_area::main ? "main" :
            area == mmu::mem_area::page ? "page" : "mempool",
            initial_map);
    }
    // Map the core, loaded by the boot loader
    // In order to properly setup mapping between virtual
    // and physical we need to take into account where kernel
    // is loaded in physical memory - elf_phys_start - and
    // where it is linked to start in virtual memory - elf_start
    static mmu::phys elf_phys_start = reinterpret_cast<mmu::phys>(elf_header);
    // There is simple invariant between elf_phys_start and elf_start
    // as expressed by the assignment below
    elf_start = reinterpret_cast<void*>(elf_phys_start + OSV_KERNEL_VM_SHIFT);
    elf_size = edata_phys - elf_phys_start;
    mmu::linear_map(elf_start, elf_phys_start, elf_size, "kernel", OSV_KERNEL_BASE);
    // now that we have some free memory, we can start mapping the rest
    mmu::switch_to_runtime_page_tables();
    for_each_usable_range([] (mem_range ent) {
        //
        // Free the memory below elf_phys_start which we could not before
        if (ent.addr < (u64)elf_phys_start) {
            auto ent_below_kernel = ent;
            if (ent.addr + ent.size >= (u64)elf_phys_start) {
                ent_below_kernel = truncate_above(ent, (u64) elf_phys_start);
            }
            mmu::free_initial_memory_range(ent_below_kernel.addr, ent_below_kernel.size);
            // If there is nothing left below elf_phys_start return
            if (ent.addr + ent.size <= (u64)elf_phys_start) {
               return;
            }
        }
        //
        // Ignore memory already freed above
        if (ent.addr + ent.size <= initial_map) {
            return;
        }
        if (intersects(ent, initial_map)) {
            ent = truncate_below(ent, initial_map);
        }
        for (auto&& area : mmu::identity_mapped_areas) {
            auto base = reinterpret_cast<char*>(get_mem_area_base(area));
            mmu::linear_map(base + ent.addr, ent.addr, ent.size,
               area == mmu::mem_area::main ? "main" :
               area == mmu::mem_area::page ? "page" : "mempool", ~0);
        }
        mmu::free_initial_memory_range(ent.addr, ent.size);
    });
}

void arch_setup_tls(void *tls, const elf::tls_data& info)
{
    struct thread_control_block *tcb;
    memcpy(tls, info.start, info.filesize);
    memset(static_cast<char*>(tls) + info.filesize, 0, info.size - info.filesize);
    tcb = reinterpret_cast<struct thread_control_block *>(static_cast<char*>(tls) + info.size);
    tcb->self = tcb;
    processor::wrmsr(msr::IA32_FS_BASE, reinterpret_cast<uint64_t>(tcb));
}

static inline void disable_pic()
{
    processor::outb(0xff, 0x21);
    processor::outb(0xff, 0xa1);
}

void arch_init_premain()
{
#if CONF_drivers_acpi
    // The UEFI stub passes the ACPI RSDP it read from the firmware config table.
    acpi::pvh_rsdp_paddr = osv_boot_info->acpi_rsdp;
#endif

    disable_pic();
}

#include "drivers/driver.hh"

extern bool opt_pci_disabled;
void arch_init_drivers()
{
#if CONF_drivers_pci
    if (!opt_pci_disabled) {
        // Enumerate PCI devices
        pci::pci_device_enumeration();
        boot_time.event("pci enumerated");
    }
#endif

    // Initialize all drivers
    hw::driver_manager* drvman = hw::driver_manager::instance();
    boot_time.event("drivers probe");
    drvman->load_all();
    drvman->list_drivers();
}

#include "drivers/console.hh"
#include "drivers/isa-serial.hh"
#include "early-console.hh"

void arch_init_early_console()
{
    console::isa_serial_console::early_init();
}

bool arch_setup_console(std::string opt_console)
{
    // Serial-only console (no VGA). "vga" is no longer a valid choice.
    if (opt_console.compare("serial") == 0 || opt_console.compare("all") == 0) {
        console::console_driver_add(&console::arch_early_console);
    } else {
        return false;
    }
    return true;
}
