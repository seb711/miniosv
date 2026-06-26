/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_PCI_HH
#define ARCH_PCI_HH

#include <osv/mmio.hh>
#include "exceptions.hh"
#include <osv/interrupt.hh>
#include "drivers/pci-device.hh"

namespace pci {

void set_pci_ecam(bool is_ecam);
bool get_pci_ecam();

void set_pci_cfg(u64 addr, size_t len);
u64 get_pci_cfg(size_t *len);
void set_pci_io(u64 addr, size_t len);
u64 get_pci_io(size_t *len);
void set_pci_mem(u64 addr, size_t len);
u64 get_pci_mem(size_t *len);

void outb(u8 val, u16 port);
void outw(u16 val, u16 port);
void outl(u32 val, u16 port);
u8 inb(u16 port);
u16 inw(u16 port);
u32 inl(u16 port);

} /* namespace pci */

// PCI device interrupts are delivered as MSI-X through the GIC ITS; there is no
// legacy INTx line routing (which on a device tree came from the PCI _PRT-style
// interrupt-map, and on ACPI would require AML). Drivers use msi/msix_interrupt.

#endif /* ARCH_PCI_HH */
