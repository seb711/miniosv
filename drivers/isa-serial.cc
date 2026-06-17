/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "isa-serial.hh"

namespace console {

u8 isa_serial_console_base::read_byte(int reg) {
    return pci::inb(isa_serial_console::ioport + reg);
};

void isa_serial_console_base::write_byte(u8 val, int reg) {
    pci::outb(val, isa_serial_console::ioport + reg);
};

void isa_serial_console::early_init()
{
    common_early_init();
}

}
