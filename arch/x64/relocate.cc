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
 * fixed-address ELF executable. It carries only a handful of dynamic
 * relocations, all originating from the prebuilt libstdc++/libgcc: a few
 * GLOB_DAT/JUMP_SLOT/TPOFF64 entries plus some IRELATIVE ifunc resolvers.
 * Because the image runs at its link address, every referenced symbol is
 * already resolved in the dynamic symbol table, so we can apply each
 * relocation straight from symtab[sym].st_value without any name/hash lookup.
 *
 * This replaces the relocation half of the former full ELF loader. It also
 * extracts the TLS template and the init-array, which premain() needs.
 */

#include <osv/elf.hh>
#include <osv/debug.h>
#include <osv/align.hh>

namespace elf {

init_table get_init(Elf64_Ehdr* header)
{
    char* pbase = reinterpret_cast<char*>(header);
    char* base = pbase;
    auto phdr = reinterpret_cast<Elf64_Phdr*>(pbase + header->e_phoff);
    auto n = header->e_phnum;
    bool base_adjusted = false;

    init_table ret = {};
    Elf64_Sym* symtab = nullptr;
    const Elf64_Rela* rela = nullptr;
    const Elf64_Rela* jmp = nullptr;
    unsigned nrela = 0;
    unsigned njmp = 0;

    // First pass: determine the load slide, the TLS template and the locations
    // of the relocation/symbol tables. Done as a separate pass so we do not
    // depend on the relative ordering of the PT_TLS and PT_DYNAMIC segments.
    for (int i = 0; i < n; ++i, ++phdr) {
        if (!base_adjusted && phdr->p_type == PT_LOAD) {
            base_adjusted = true;
            base -= phdr->p_vaddr;
        }
        if (phdr->p_type == PT_TLS) {
            ret.tls.start = reinterpret_cast<void*>(phdr->p_vaddr);
            ret.tls.filesize = phdr->p_filesz;
            ret.tls.size = align_up(phdr->p_memsz, (size_t)64);
        } else if (phdr->p_type == PT_DYNAMIC) {
            auto dyn = reinterpret_cast<Elf64_Dyn*>(phdr->p_vaddr);
            unsigned ndyn = phdr->p_memsz / sizeof(*dyn);
            for (auto d = dyn; d < dyn + ndyn; ++d) {
                switch (d->d_tag) {
                case DT_INIT_ARRAY:
                    ret.start = reinterpret_cast<void (**)()>(d->d_un.d_ptr);
                    break;
                case DT_INIT_ARRAYSZ:
                    ret.count = d->d_un.d_val / sizeof(ret.start);
                    break;
                case DT_SYMTAB:
                    symtab = reinterpret_cast<Elf64_Sym*>(d->d_un.d_ptr);
                    break;
                case DT_RELA:
                    rela = reinterpret_cast<const Elf64_Rela*>(d->d_un.d_ptr);
                    break;
                case DT_RELASZ:
                    nrela = d->d_un.d_val / sizeof(*rela);
                    break;
                case DT_JMPREL:
                    jmp = reinterpret_cast<const Elf64_Rela*>(d->d_un.d_ptr);
                    break;
                case DT_PLTRELSZ:
                    njmp = d->d_un.d_val / sizeof(*jmp);
                    break;
                }
            }
        }
    }

    auto apply = [&](const Elf64_Rela* tab, unsigned cnt) {
        if (!tab) {
            return;
        }
        for (unsigned k = 0; k < cnt; ++k) {
            const Elf64_Rela* r = &tab[k];
            u32 sym = r->r_info >> 32;
            u32 type = r->r_info & 0xffffffff;
            void* addr = base + (size_t)r->r_offset;
            switch (type) {
            case R_X86_64_NONE:
                break;
            case R_X86_64_RELATIVE:
                *static_cast<void**>(addr) = base + r->r_addend;
                break;
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT:
                *static_cast<u64*>(addr) = symtab[sym].st_value;
                break;
            case R_X86_64_64:
                *static_cast<u64*>(addr) = symtab[sym].st_value + r->r_addend;
                break;
            case R_X86_64_DTPOFF64:
                *static_cast<u64*>(addr) = symtab[sym].st_value;
                break;
            case R_X86_64_TPOFF64:
                *static_cast<u64*>(addr) = symtab[sym].st_value - ret.tls.size;
                break;
            case R_X86_64_IRELATIVE:
                *static_cast<void**>(addr) =
                    reinterpret_cast<void *(*)()>(base + r->r_addend)();
                break;
            default:
                debug_early_u64("Unsupported relocation type=", type);
                abort();
            }
        }
    };

    apply(rela, nrela);
    apply(jmp, njmp);

    return ret;
}

}
