/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <cxxabi.h>

#include <osv/demangle.hh>

namespace osv {

bool demangle(const char *name, char *buf, size_t len)
{
    // libc++abi provides __cxa_demangle (the Itanium ABI demangler) but not
    // gcc's __gcclibcxx_demangle_callback, so demangle into the malloc'd buffer
    // __cxa_demangle returns and copy it into the caller's fixed buffer.
    int status;
    char *demangled = abi::__cxa_demangle(name, nullptr, 0, &status);
    if (status != 0 || !demangled) {
        free(demangled);
        return false;
    }
    strncpy(buf, demangled, len - 1);
    buf[len - 1] = '\0';
    free(demangled);
    return true;
}

// Weak stubs let the stage-1 link resolve; the strong definitions from
// gen/ksymtab.o override them in the final link.
extern "C" {
    __attribute__((weak)) uint32_t __ksym_count = 0;
    __attribute__((weak)) uint64_t __ksym_addresses[1] = {0};
    __attribute__((weak)) uint32_t __ksym_name_offsets[1] = {0};
    __attribute__((weak)) char __ksym_names_blob[1] = {0};
}

void lookup_name_demangled(void *addr, char *buf, size_t len)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    uint32_t n = __ksym_count, lo = 0, hi = n;
    if (!n || a < __ksym_addresses[0]) { snprintf(buf, len, "%p", addr); return; }
    while (hi - lo > 1) {
        uint32_t m = lo + (hi - lo) / 2;
        (__ksym_addresses[m] <= a ? lo : hi) = m;
    }
    const char *raw = &__ksym_names_blob[__ksym_name_offsets[lo]];
    char d[512];
    const char *disp = demangle(raw, d, sizeof(d)) ? d : raw;
    snprintf(buf, len, "%s+0x%lx", disp, (unsigned long)(a - __ksym_addresses[lo]));
}

std::unique_ptr<char> demangle(const char *name)
{
    int status;
    char *demangled = abi::__cxa_demangle(name, nullptr, 0, &status);
    // Note: __cxa_demangle used malloc() to allocate demangled, and we'll
    // need to use free() to free it. Here we're assuming that unique_ptr's
    // default deallocator, "delete", is the same thing as free().
    return std::unique_ptr<char>(demangled);
}

demangler::~demangler()
{
    free(_buf);
}
const char *demangler::operator()(const char * name)
{
    int status;
    auto *demangled = abi::__cxa_demangle(name, _buf, &_len, &status);
    if (demangled) {
        _buf = demangled;
        return _buf;
    } else {
        return nullptr;
    }
}

}
