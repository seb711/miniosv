/*
 * Copyright (C) 2024 OSv
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

/*
 * Minimal dynamic-linker introspection.
 *
 * The application is statically linked into the kernel, so there is no runtime
 * loading of shared objects: dlopen()/dlsym() always fail. The C++ exception
 * unwinder (in libgcc/libstdc++) still calls dl_iterate_phdr() and
 * _dl_find_object() to locate exception-handling data, so those are implemented
 * to report the one and only object - the kernel image itself.
 */

#include <dlfcn.h>
#include <__dlfcn.h>
#include <link.h>
#include <elf.h>
#include <string.h>
#include <osv/export.h>

// Published by premain() (loader.cc): the virtual address of the kernel's own
// ELF header. The kernel is an EXEC linked at - and running at - its link
// address, so program-header virtual addresses need no load-bias adjustment.
void* __kernel_ehdr;

static const Elf64_Phdr* kernel_phdrs(size_t& count)
{
    auto e = static_cast<Elf64_Ehdr*>(__kernel_ehdr);
    count = e->e_phnum;
    return reinterpret_cast<const Elf64_Phdr*>(
        reinterpret_cast<char*>(e) + e->e_phoff);
}

extern "C" OSV_LIBC_API
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info* info, size_t size, void* data),
                    void* data)
{
    size_t n;
    auto phdr = kernel_phdrs(n);

    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr = 0; // EXEC at its link address: no load bias
    info.dlpi_name = "kernel";
    info.dlpi_phdr = phdr;
    info.dlpi_phnum = n;
    info.dlpi_adds = 1;
    info.dlpi_subs = 0;
    return callback(&info, sizeof(info), data);
}

extern "C" OSV_LIBC_API
int _dl_find_object(void* address, struct dl_find_object* result)
{
    size_t n;
    auto phdr = kernel_phdrs(n);

    uintptr_t lo = ~uintptr_t(0), hi = 0;
    void* eh_frame = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_vaddr < lo) {
                lo = phdr[i].p_vaddr;
            }
            if (phdr[i].p_vaddr + phdr[i].p_memsz > hi) {
                hi = phdr[i].p_vaddr + phdr[i].p_memsz;
            }
        } else if (phdr[i].p_type == PT_GNU_EH_FRAME) {
            eh_frame = reinterpret_cast<void*>(phdr[i].p_vaddr);
        }
    }

    auto addr = reinterpret_cast<uintptr_t>(address);
    if (addr < lo || addr >= hi) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->dlfo_map_start = reinterpret_cast<void*>(lo);
    result->dlfo_map_end = reinterpret_cast<void*>(hi);
    result->dlfo_eh_frame = eh_frame;
    return 0;
}

extern "C" int dladdr(const void*, Dl_info* info)
{
    memset(info, 0, sizeof(*info));
    return 0;
}

extern "C" void* dlopen(const char*, int)
{
    return nullptr;
}

extern "C" void* dlsym(void*, const char*)
{
    return nullptr;
}

extern "C" int dlclose(void*)
{
    return 0;
}

extern "C" char* dlerror(void)
{
    return const_cast<char*>("dynamic loading is not supported");
}
