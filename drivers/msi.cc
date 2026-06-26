/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 * Copyright (C) 2025 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <stddef.h>
#include <osv/msi.hh>
#include <osv/trace.hh>

TRACEPOINT(trace_msix_interrupt, "vector=0x%02x", unsigned);
TRACEPOINT(trace_msix_migrate, "vector=0x%02x cpu_id=0x%x",
                               unsigned, unsigned);

using namespace pci;
using namespace processor;

msix_vector::msix_vector(pci::function* dev)
    : _dev(dev)
{
    _vector = idt.register_handler([this] { interrupt(); });
}

msix_vector::~msix_vector()
{
    idt.unregister_handler(_vector);
}

pci::function* msix_vector::get_pci_function(void)
{
    return (_dev);
}

unsigned msix_vector::get_vector(void)
{
    return (_vector);
}

void msix_vector::msix_unmask_entries(void)
{
    for (auto entry_id : _entryids) {
        _dev->msix_unmask_entry(entry_id);
    }
}

void msix_vector::msix_mask_entries(void)
{
    for (auto entry_id : _entryids) {
        _dev->msix_mask_entry(entry_id);
    }
}

void msix_vector::set_handler(std::function<void ()> handler)
{
    _handler = handler;
}

void msix_vector::add_entryid(unsigned entry_id)
{
    _entryids.push_back(entry_id);
}

void msix_vector::interrupt(void)
{
    trace_msix_interrupt(_vector);
    _handler();
}

std::vector<msix_vector*> interrupt_manager::request_vectors(unsigned num_vectors)
{
    std::vector<msix_vector*> results;

    unsigned num_entries = _dev->msix_get_num_entries();

    auto num = std::min(num_vectors, num_entries);

    for (unsigned i = 0; i < num; ++i) {
        results.push_back(new msix_vector(_dev));
    }

    return (results);
}

bool interrupt_manager::assign_isr(msix_vector* vector, std::function<void ()> handler)
{
    vector->set_handler(handler);

    return (true);
}

void interrupt_manager::free_vectors(const std::vector<msix_vector*>& vectors)
{
    for (auto msix : vectors) {
        delete msix;
    }
}

bool interrupt_manager::unmask_interrupts(const std::vector<msix_vector*>& vectors)
{
    for (auto msix : vectors) {
        msix->msix_unmask_entries();
    }

    return (true);
}
