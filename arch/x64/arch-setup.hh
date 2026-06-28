/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_SETUP_HH_
#define ARCH_SETUP_HH_

#include "arch-tls.hh"
#include <string>

#include <osv/elf.hh>
#include <osv/boot-info.hh>

// A usable physical memory range, taken from the UEFI memory map carried in
// osv::boot_info. Used as the working type while the free-memory setup carves
// up RAM during early bring-up.
struct mem_range {
    u64 addr;
    u64 size;
};

void arch_init_early_console();
void arch_init_premain();
void arch_setup_tls(void *tls, const elf::tls_data& info);

void arch_setup_free_memory();
void arch_init_drivers();
bool arch_setup_console(std::string opt_console);

#endif /* ARCH_SETUP_HH_ */
