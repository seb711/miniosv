/*
 * Copyright (C) 2019 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef GETOPT_HH_
#define GETOPT_HH_

// Historically OSv ran the application as a separate (often position
// independent) ELF object, which had its own copies of getopt()'s global
// variables (optind, opterr, ...) distinct from the kernel's. This RAII helper
// used to copy those values back and forth across the kernel/app boundary.
//
// The application is now statically linked into the kernel, so there is a
// single set of these globals and nothing to synchronize. The helper is kept
// as an empty no-op so the getopt sources don't need to change.
class getopt_caller_vars_copier {
public:
    // User-declared (non-trivial) ctor/dtor so instances are not flagged as
    // unused variables at the call sites.
    getopt_caller_vars_copier() {}
    ~getopt_caller_vars_copier() {}
};

extern "C" int __getopt(int argc, char * const argv[], const char *optstring);

#endif
