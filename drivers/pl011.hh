/*
 * Copyright (C) 2014-2015 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef PL011_HH
#define PL011_HH

#include "console-driver.hh"
#include "exceptions.hh"
#include <osv/interrupt.hh>

namespace console {

class PL011_Console : public console_driver {
public:
    virtual void write(const char *str, size_t len) override;
    virtual void flush() override;

    void set_base_addr(u64 addr);
    u64 get_base_addr();
    void set_irqid(int irqid);

    static bool active;
private:
    virtual void dev_start() override {}
    /* default UART irq = SPI 1 = 32 + 1 */
    unsigned int irqid = 33;
};

}

#endif /* PL011_HH */
