/*
 * Copyright (C) 2026 miniosv contributors
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_BOOT_INFO_HH
#define OSV_BOOT_INFO_HH

#include <stdint.h>

//
// Architecture-independent hand-off contract between the UEFI stub and the
// kernel. The stub (see boot/uefi/) fills this structure while still in the
// UEFI boot environment, calls ExitBootServices(), and passes a pointer to it
// (by physical address) to the kernel entry point. It is the single source of
// boot information for both x64 and aarch64 - it replaces the x86 multiboot
// struct and the aarch64 device tree as the carrier of the memory map, the
// ACPI root pointer and the command line.
//
// All addresses are physical. The structure and the arrays it points at live
// in memory the stub allocated as EfiLoaderData, so they survive
// ExitBootServices and remain valid until the kernel reclaims that memory.
//

namespace osv {

// Type of a normalized physical memory range. The stub collapses the much
// richer UEFI memory map into just these categories: anything the kernel may
// use as RAM (EfiConventionalMemory plus the boot-services regions that become
// free after ExitBootServices) is reported as usable; everything else is
// reserved.
enum class boot_mem_type : uint32_t {
    usable   = 1,
    reserved = 2,
};

struct boot_memmap_entry {
    uint64_t addr;
    uint64_t size;
    boot_mem_type type;
} __attribute__((packed));

// Magic identifying a valid boot_info and catching ABI drift between a stub
// and a kernel built from different trees.
static constexpr uint64_t boot_info_magic = 0x4f53565542494631ull; // "OSVUBIF1"

// NB: arch/aarch64/boot.S reads kernel_phys_base by its byte offset (40); keep
// the layout in sync if you reorder these fields.
struct boot_info {
    uint64_t magic;                 // boot_info_magic               (offset 0)
    uint64_t acpi_rsdp;             // phys addr of ACPI RSDP, 0 if none (8)
    uint64_t memmap_addr;           // phys addr of boot_memmap_entry[]  (16)
    uint32_t memmap_count;          // number of entries in the array    (24)
    uint32_t cmdline_len;           // length of cmdline, excluding NUL  (28)
    uint64_t cmdline_addr;          // phys addr of NUL-terminated cmdline (32)
    uint64_t kernel_phys_base;      // phys address the kernel loaded at  (40)
    uint64_t boot_unixtime_ns;      // wall-clock at boot (UEFI GetTime), 0 if none (48)
    uint64_t uart_base;             // phys addr of the ACPI SPCR console UART, 0 if none (56)
    uint8_t  uart_type;             // SPCR Interface Type: 0/1 = 16550, 3/0x0e = PL011/SBSA (64)
    uint8_t  uart_access_width;     // SPCR GAS access width in bytes: 1, 2 or 4 (65)
} __attribute__((packed));

}

#endif /* OSV_BOOT_INFO_HH */
