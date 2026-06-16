/*
 * Copyright (C) 2024 OSv
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

/*
 * Minimal boot-time self-relocator for the OSv kernel image.
 *
 * The kernel - with the application statically linked in - is a single,
 * static, fixed-address ELF executable with NO .dynamic section. It runs at its
 * link address, so the only relocations left are the IRELATIVE ifunc resolvers
 * from the prebuilt archives (e.g. the CPU-dispatched string ops). A static
 * link brackets them with __rela_iplt_start/__rela_iplt_end, so we apply them
 * straight from there - no dynamic table, no symbol lookup.
 *
 * The init-array (global constructors) and the TLS template, which premain()
 * also needs, come from the linker-script symbols _init_array_start/_end and
 * the PT_TLS program header respectively.
 */

#include <osv/elf.hh>
#include <osv/debug.h>
#include <osv/align.hh>

// Bracketing symbols: _init_array_start/_end from loader.ld, and the IRELATIVE
// reloc range the static link emits around .rela.iplt.
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

    // Apply IRELATIVE ifunc relocations: run the resolver, store its result.
    for (const Elf64_Rela* r = __rela_iplt_start; r < __rela_iplt_end; ++r) {
        u32 type = r->r_info & 0xffffffff;
        void* addr = base + (size_t)r->r_offset;
        if (type == R_X86_64_IRELATIVE) {
            *static_cast<void**>(addr) =
                reinterpret_cast<void *(*)()>(base + r->r_addend)();
        } else {
            debug_early_u64("Unsupported relocation type=", type);
            abort();
        }
    }

    return ret;
}

}
