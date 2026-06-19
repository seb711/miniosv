/*
 * Copyright (C) 2026 miniosv contributors
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

//
// miniosv UEFI boot stub.
//
// This is a small, freestanding UEFI application - the image the firmware
// loads from the EFI System Partition (\EFI\BOOT\BOOTX64.EFI /
// \EFI\BOOT\BOOTAA64.EFI). The kernel ELF is embedded into this image as a
// binary blob (see kernel-blob.S). The stub:
//
//   1. locates the ACPI RSDP from the UEFI configuration table,
//   2. reads the command line from the loaded-image LoadOptions,
//   3. loads the embedded kernel ELF's PT_LOAD segments to their physical
//      link addresses,
//   4. normalizes the UEFI memory map into osv::boot_memmap_entry[],
//   5. calls ExitBootServices(),
//   6. hands control to the kernel entry point with a pointer to osv::boot_info.
//
// It is the single boot path for both architectures; everything arch-specific
// is confined to the tiny handoff trampoline at the bottom and to the kernel
// side (arch/<arch>/boot.S uefi_entry).
//

#include "efi.hh"
#include <osv/boot-info.hh>

// The kernel is linked at a fixed virtual base and loaded at a fixed physical
// base; the constant difference (OSV_KERNEL_VM_SHIFT, supplied by the build)
// lets the stub place each segment without relying on ELF p_paddr, which the
// aarch64 (position-independent) link does not set usefully.
#ifndef OSV_KERNEL_VM_SHIFT
#error "OSV_KERNEL_VM_SHIFT must be defined for the UEFI stub"
#endif

// ---- embedded kernel ELF (kernel-blob.S) ---------------------------------
extern "C" const unsigned char kernel_elf_start[];
extern "C" const unsigned char kernel_elf_end[];

// ---- minimal ELF64 ---------------------------------------------------------
namespace {

struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint64_t EFI_PAGE_SIZE = 4096;

EFI_SYSTEM_TABLE  *ST;
EFI_BOOT_SERVICES *BS;

} // namespace

// Freestanding mem helpers (the stub links no libc). The volatile destination
// stops clang's loop-idiom pass from rewriting these loops back into calls to
// memcpy/memset (which would recurse). The compiler may also emit calls to
// these for aggregate copies/zeroing, so they carry C linkage.
extern "C" void *memcpy(void *dst, const void *src, unsigned long n)
{
    auto d = static_cast<volatile unsigned char *>(dst);
    auto s = static_cast<const unsigned char *>(src);
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

extern "C" void *memset(void *dst, int c, unsigned long n)
{
    auto d = static_cast<volatile unsigned char *>(dst);
    for (unsigned long i = 0; i < n; i++)
        d[i] = static_cast<unsigned char>(c);
    return dst;
}

namespace {

void print(const char *s)
{
    if (!ST || !ST->ConOut)
        return;
    CHAR16 buf[2];
    buf[1] = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[0] = '\r';
            ST->ConOut->OutputString(ST->ConOut, buf);
        }
        buf[0] = static_cast<CHAR16>(*s);
        ST->ConOut->OutputString(ST->ConOut, buf);
    }
}

[[noreturn]] void die(const char *msg)
{
    print("UEFI stub: ");
    print(msg);
    print("\n");
    for (;;)
        ;
}

// Walk the UEFI configuration table for the ACPI RSDP, preferring ACPI 2.0+.
uint64_t find_rsdp()
{
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
    uint64_t found = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        auto &e = ST->ConfigurationTable[i];
        if (efi_guid_eq(e.VendorGuid, acpi20))
            return reinterpret_cast<uint64_t>(e.VendorTable);
        if (efi_guid_eq(e.VendorGuid, acpi10))
            found = reinterpret_cast<uint64_t>(e.VendorTable);
    }
    return found;
}

// Wall-clock at boot from UEFI GetTime(), in nanoseconds since the Unix epoch
// (0 if the firmware has no time source). This gives the kernel a real-time
// base without an RTC driver - important on aarch64, whose generic timer is a
// monotonic counter only.
uint64_t read_boot_time()
{
    if (!ST->RuntimeServices) {
        return 0;
    }
    EFI_TIME t;
    if (EFI_ERROR(ST->RuntimeServices->GetTime(&t, nullptr))) {
        return 0;
    }
    // Days since 1970-01-01 (Howard Hinnant's days_from_civil).
    int64_t y = t.Year;
    unsigned m = t.Month, d = t.Day;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    int64_t secs = days * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second;
    // EFI_TIME is local time; convert to UTC when the zone is specified.
    if (t.TimeZone != 0x07ff) {
        secs -= static_cast<int64_t>(t.TimeZone) * 60;
    }
    return static_cast<uint64_t>(secs) * 1000000000ull + t.Nanosecond;
}

// Copy the loaded-image LoadOptions (UTF-16) into an ASCII command line.
// Returns length (excluding NUL); cmdline buffer is caller-provided.
uint32_t read_cmdline(EFI_HANDLE image, char *out, uint32_t cap)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = nullptr;
    if (EFI_ERROR(BS->HandleProtocol(image, &li_guid,
                                     reinterpret_cast<void **>(&li))) || !li)
        return 0;
    auto opts = static_cast<const CHAR16 *>(li->LoadOptions);
    uint32_t n16 = li->LoadOptionsSize / sizeof(CHAR16);
    uint32_t len = 0;
    for (uint32_t i = 0; i < n16 && len + 1 < cap; i++) {
        CHAR16 c = opts[i];
        if (c == 0)
            break;
        out[len++] = (c < 0x80) ? static_cast<char>(c) : '?';
    }
    out[len] = 0;
    return len;
}

// Load the embedded kernel ELF. Returns the physical entry point.
uint64_t load_kernel(uint64_t *kernel_phys_base)
{
    auto blob = kernel_elf_start;
    auto ehdr = reinterpret_cast<const Elf64_Ehdr *>(blob);
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
        die("embedded kernel is not an ELF");

    uint64_t lowest_phys = ~0ull;
    uint64_t entry_phys = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        auto ph = reinterpret_cast<const Elf64_Phdr *>(
            blob + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;

        // Physical load address = virtual link address minus the fixed shift.
        uint64_t seg_pa = ph->p_vaddr - OSV_KERNEL_VM_SHIFT;

        // Reserve the segment's physical range at its fixed load address. The
        // kernel's boot page tables hard-map these physical addresses, so the
        // kernel must land exactly where it was linked.
        EFI_PHYSICAL_ADDRESS pa = seg_pa & ~(EFI_PAGE_SIZE - 1);
        UINTN pages = (seg_pa + ph->p_memsz - pa + EFI_PAGE_SIZE - 1)
                          / EFI_PAGE_SIZE;
        EFI_PHYSICAL_ADDRESS got = pa;
        // Allocate as EfiLoaderCode so the firmware maps the kernel image
        // executable. The kernel entry runs a few instructions under the
        // firmware's identity map before installing its own page tables, and
        // strict firmware (e.g. AArch64 AAVMF) faults on instruction fetch
        // from non-executable EfiLoaderData pages.
        if (EFI_ERROR(BS->AllocatePages(AllocateAddress, EfiLoaderCode,
                                        pages, &got)))
            die("cannot reserve kernel physical memory (load address busy)");

        memcpy(reinterpret_cast<void *>(seg_pa),
                blob + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset(reinterpret_cast<void *>(seg_pa + ph->p_filesz), 0,
                    ph->p_memsz - ph->p_filesz);

        if (seg_pa < lowest_phys)
            lowest_phys = seg_pa;
        // Translate the virtual entry point to physical via its segment.
        if (ehdr->e_entry >= ph->p_vaddr &&
            ehdr->e_entry < ph->p_vaddr + ph->p_memsz)
            entry_phys = seg_pa + (ehdr->e_entry - ph->p_vaddr);
    }
    if (!entry_phys)
        die("could not resolve kernel entry point");
    *kernel_phys_base = lowest_phys;
    return entry_phys;
}

// Per-arch handoff: set the kernel's first argument register to boot_info and
// jump to its physical entry point. Runs after ExitBootServices, in the
// firmware's identity mapping. Never returns.
[[noreturn]] void handoff(uint64_t entry_phys, osv::boot_info *bi)
{
#if defined(__x86_64__)
    asm volatile("mov %0, %%rdi\n\t"
                 "jmp *%1\n\t"
                 : : "r"(bi), "r"(entry_phys) : "rdi", "memory");
#elif defined(__aarch64__)
    asm volatile("mov x0, %0\n\t"
                 "br %1\n\t"
                 : : "r"(bi), "r"(entry_phys) : "x0", "memory");
#else
#error "unsupported architecture"
#endif
    __builtin_unreachable();
}

} // namespace

extern "C" EFIAPI EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;

    print("miniosv UEFI stub starting\n");

    uint64_t rsdp = find_rsdp();

    // Allocate the hand-off structures as EfiLoaderData so they survive
    // ExitBootServices. Size the memmap generously - the UEFI map is bounded
    // by available RAM regions and never approaches this.
    constexpr uint32_t MAX_MEMMAP = 512;
    constexpr uint32_t CMDLINE_CAP = 4096;

    osv::boot_info *bi = nullptr;
    osv::boot_memmap_entry *mm = nullptr;
    char *cmdline = nullptr;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, sizeof(*bi),
                                   reinterpret_cast<void **>(&bi))) ||
        EFI_ERROR(BS->AllocatePool(EfiLoaderData,
                                   sizeof(*mm) * MAX_MEMMAP,
                                   reinterpret_cast<void **>(&mm))) ||
        EFI_ERROR(BS->AllocatePool(EfiLoaderData, CMDLINE_CAP,
                                   reinterpret_cast<void **>(&cmdline))))
        die("out of memory allocating boot info");

    uint32_t cmdline_len = read_cmdline(image, cmdline, CMDLINE_CAP);

    uint64_t kernel_phys_base = 0;
    uint64_t entry_phys = load_kernel(&kernel_phys_base);

    bi->magic = osv::boot_info_magic;
    bi->acpi_rsdp = rsdp;
    bi->memmap_addr = reinterpret_cast<uint64_t>(mm);
    bi->cmdline_addr = reinterpret_cast<uint64_t>(cmdline);
    bi->cmdline_len = cmdline_len;
    bi->kernel_phys_base = kernel_phys_base;
    bi->boot_unixtime_ns = read_boot_time();

    // Fetch the UEFI memory map, normalize it, and ExitBootServices. The map
    // key can go stale between GetMemoryMap and ExitBootServices, so retry.
    // No allocation happens inside the loop: the EFI map buffer is sized once.
    UINTN efi_map_cap = 0, efi_desc_size = 0;
    UINTN map_key = 0;
    uint32_t desc_ver = 0;
    {
        // Probe required size, then over-allocate to absorb churn.
        UINTN need = 0;
        BS->GetMemoryMap(&need, nullptr, &map_key, &efi_desc_size, &desc_ver);
        efi_map_cap = need + 8 * efi_desc_size;
    }
    EFI_MEMORY_DESCRIPTOR *efi_map = nullptr;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, efi_map_cap,
                                   reinterpret_cast<void **>(&efi_map))))
        die("out of memory for EFI memory map");

    for (int attempt = 0; attempt < 8; attempt++) {
        UINTN map_size = efi_map_cap;
        if (EFI_ERROR(BS->GetMemoryMap(&map_size, efi_map, &map_key,
                                       &efi_desc_size, &desc_ver)))
            die("GetMemoryMap failed");

        uint32_t count = 0;
        UINTN entries = map_size / efi_desc_size;
        for (UINTN i = 0; i < entries && count < MAX_MEMMAP; i++) {
            auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(
                reinterpret_cast<unsigned char *>(efi_map) + i * efi_desc_size);
            bool usable = d->Type == EfiConventionalMemory ||
                          d->Type == EfiBootServicesCode ||
                          d->Type == EfiBootServicesData ||
                          d->Type == EfiLoaderCode;
            mm[count].addr = d->PhysicalStart;
            mm[count].size = d->NumberOfPages * EFI_PAGE_SIZE;
            mm[count].type = usable ? osv::boot_mem_type::usable
                                    : osv::boot_mem_type::reserved;
            count++;
        }
        bi->memmap_count = count;

        if (!EFI_ERROR(BS->ExitBootServices(image, map_key)))
            handoff(entry_phys, bi); // never returns
        // else: map changed, loop and try again
    }
    die("ExitBootServices failed repeatedly");
}
