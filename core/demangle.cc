/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <string.h>
#include <stdio.h>
#include <algorithm>
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

void lookup_name_demangled(void *addr, char *buf, size_t len)
{
    // The ELF symbol table is no longer kept in the kernel (the application is
    // statically linked in), so backtraces report raw addresses rather than
    // resolved, demangled symbol names.
    snprintf(buf, len, "%p", addr);
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
