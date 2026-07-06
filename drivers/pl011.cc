/*
 * Copyright (C) 2014-2015 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 *
 */

#include "pl011.hh"

/* spec: see PrimeCell UART (PL011) Technical Reference Manual.
 * implemented according to Revision: r1p5.
 */

namespace console {

/* default base addr */
static volatile char *uart = (char *)0x9000000;

enum {
    UARTDR    = 0x000,
    UARTFR    = 0x018,
    UARTIMSC  = 0x038,
    UARTMIS   = 0x040,
    UARTICR   = 0x044
};

enum {
    UARTFR_BUSY = (1 << 3), /* UART busy transmitting */
    UARTFR_TXFF = (1 << 5)  /* transmit FIFO full */
};


bool PL011_Console::active = false;

void PL011_Console::set_base_addr(u64 addr)
{
    uart = (char *)addr;
}

void PL011_Console::set_irqid(int irqid)
{
    this->irqid = irqid;
}

u64 PL011_Console::get_base_addr()
{
    return (u64)uart;
}

void PL011_Console::flush() {
    return;
}

void PL011_Console::write(const char *str, size_t len) {
    while (len > 0) {
        /* PL011 registers must be accessed with 32-bit MMIO reads/writes.
         * The SBSA UART exposed by AWS Graviton only decodes word accesses;
         * byte accesses (which QEMU's model tolerates, so it works locally)
         * transmit nothing on real hardware. This matches Linux's amba-pl011
         * (vendor_sbsa.access_32b -> UPIO_MEM32 -> writel/readl) and Nanos.
         *
         * Wait for room in the transmit FIFO (TXFF), then write one byte. */
        while (*(volatile u32 *)(uart + UARTFR) & UARTFR_TXFF)
            ;
        *(volatile u32 *)(uart + UARTDR) = (u8)*str++;
        len--;
    }
}

}
