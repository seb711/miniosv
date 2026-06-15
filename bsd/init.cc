/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/debug.hh>
#include <osv/mempool.hh>

#include <bsd/sys/sys/eventhandler.h>

static void physmem_init()
{
    physmem = memory::phys_mem_size / memory::page_size;
}

void bsd_init(void)
{
    debug("bsd: initializing");

    physmem_init();

    eventhandler_init(NULL);

    debug(" - done\n");
}
