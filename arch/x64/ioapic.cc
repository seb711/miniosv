/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/interrupt.hh>
#include "exceptions.hh"
#include <osv/mmu.hh>
#include <osv/mutex.h>

namespace ioapic {

constexpr u64 base_phys = 0xfec00000;
volatile void* const base = mmu::phys_cast<void>(0xfec00000);
constexpr unsigned index_reg_offset = 0;
constexpr unsigned data_reg_offset = 0x10;

mutex mtx;

volatile u32* index_reg()
{
    return reinterpret_cast<volatile u32*>(static_cast<volatile char*>(base) + index_reg_offset);
}

volatile u32* data_reg()
{
    return reinterpret_cast<volatile u32*>(static_cast<volatile char*>(base) + data_reg_offset);
}

u32 read(unsigned reg)
{
    *index_reg() = reg;
    return *data_reg();
}

void write(unsigned reg, u32 data)
{
    *index_reg() = reg;
    *data_reg() = data;
}

void init()
{
    mmu::linear_map(const_cast<void*>(base), base_phys, 4096, "ioapic");
}

}
