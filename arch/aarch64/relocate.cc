/*
 * Copyright (C) 2024 OSv
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

/*
 * Minimal boot-time self-relocator for the OSv kernel image (aarch64).
 *
 * The aarch64 counterpart of arch/x64/relocate.cc - see that file for the full
 * rationale. The kernel is a static, fixed-address ELF with no .dynamic
 * section: it runs at its link address, so (being -ftls-model=local-exec, no
 * dynamic TLS) the only relocations left are RELATIVE and the IRELATIVE ifunc
 * resolvers, bracketed by __rela_iplt_start/__rela_iplt_end. No symbol lookup.
 * init-array and TLS come from _init_array_start/_end and the PT_TLS header.
 */

#include <osv/elf.hh>
#include <osv/debug.h>
#include <osv/align.hh>

extern "C" {
    extern void (*_init_array_start[])();
    extern void (*_init_array_end[])();
    extern const elf::Elf64_Rela __rela_iplt_start[] __attribute__((weak));
    extern const elf::Elf64_Rela __rela_iplt_end[] __attribute__((weak));
}

namespace elf {

init_table get_init(Elf64_Ehdr* header)
{
    char* pbase = reinterpret_cast<char*>(header);
    char* base = pbase;
    auto phdr = reinterpret_cast<Elf64_Phdr*>(pbase + header->e_phoff);
    auto n = header->e_phnum;
    bool base_adjusted = false;

    init_table ret = {};

    // Determine the load slide and the TLS template from the program headers.
    for (int i = 0; i < n; ++i, ++phdr) {
        if (!base_adjusted && phdr->p_type == PT_LOAD) {
            base_adjusted = true;
            base -= phdr->p_vaddr;
        }
        if (phdr->p_type == PT_TLS) {
            ret.tls.start = reinterpret_cast<void*>(phdr->p_vaddr);
            ret.tls.filesize = phdr->p_filesz;
            ret.tls.size = align_up(phdr->p_memsz, (size_t)64);
        }
    }

    // Global constructors: the init-array bracketed by the linker script.
    ret.start = _init_array_start;
    ret.count = _init_array_end - _init_array_start;

    // Apply the static-link relocations (RELATIVE + IRELATIVE ifuncs).
    for (const Elf64_Rela* r = __rela_iplt_start; r < __rela_iplt_end; ++r) {
        u32 type = r->r_info & 0xffffffff;
        void* addr = base + (size_t)r->r_offset;
        switch (type) {
        case R_AARCH64_NONE:
        case R_AARCH64_NONE2:
            break;
        case R_AARCH64_RELATIVE:
            *static_cast<void**>(addr) = base + r->r_addend;
            break;
        case R_AARCH64_IRELATIVE:
            *static_cast<void**>(addr) =
                reinterpret_cast<void *(*)()>(base + r->r_addend)();
            break;
        default:
            debug_early_u64("Unsupported relocation type=", type);
            abort();
        }
    }

    return ret;
}

}
