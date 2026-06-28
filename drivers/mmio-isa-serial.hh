/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef DRIVERS_MMIO_ISA_SERIAL_HH
#define DRIVERS_MMIO_ISA_SERIAL_HH

#include "isa-serial-base.hh"

namespace console {

class mmio_isa_serial_console : public isa_serial_console_base {
public:
    void set_irqid(int irqid) { this->irqid = irqid; }
    static void early_init(u64 mmio_phys_address);
    // Polled init for a UART the firmware already configured (e.g. the ACPI SPCR
    // console on AWS Graviton: a 16550 whose registers are 32-bit and reg-shift
    // 2). access_width is the register access width in bytes (1, 2 or 4); the
    // register stride is derived from it. Does not reprogram the baud rate -
    // that would assume a fixed input clock and garble output on real hardware.
    static void early_init_polled(u64 mmio_phys_address, int access_width);
    static void memory_map();
    static void clean_cmdline(char *cmdline);
    static mmioaddr_t _addr_mmio;
    static u64 _phys_mmio_address;
    // Register access geometry. Default (width 1, shift 0) matches a classic
    // byte-wide, contiguous 16550; AWS Graviton sets width 4, shift 2 (mmio32).
    static int _access_width;       // bytes per register access: 1, 2 or 4
    static int _reg_shift;          // byte offset of register r is r << _reg_shift
private:
    unsigned int irqid;
    virtual void dev_start() override {}
};

}

#endif
