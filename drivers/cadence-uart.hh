/*
 * Copyright (C) 2021 DornerWorks, Ltd
 * Author: Stewart Hildebrand
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef CADENCE_UART_HH
#define CADENCE_UART_HH

#include "console-driver.hh"
#include "exceptions.hh"
#include <osv/interrupt.hh>
#include <osv/types.h>

namespace console {

class Cadence_Console : public console_driver {
public:
    virtual void write(const char *str, size_t len) override;
    virtual void flush() override;

    void set_base_addr(u64 addr);
    u64 get_base_addr();
    void set_irqid(int irqid);

    static bool active;
private:
    virtual void dev_start() override;
    // Default UART irq = SPI 21 = 32 + 21
    unsigned int irqid = 53;
};

}

#endif /* CADENCE_UART_HH */
