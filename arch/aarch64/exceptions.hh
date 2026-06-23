/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * Copyright (C) 2014-2015 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef EXCEPTIONS_HH
#define EXCEPTIONS_HH

#include <stdint.h>
#include <functional>
#include <osv/types.h>
#include <osv/mutex.h>
#include <osv/interrupt.hh>
#include <vector>
#include <atomic>

#include "gic-common.hh"

namespace sched { struct cpu; }

struct exception_frame {
    u64 regs[31];
    u64 sp;
    u64 elr;
    u64 spsr;
    u32 esr;
    u32 align1;
    u64 far;

    void *get_pc(void) { return (void *)elr; }
    unsigned int get_error(void) { return esr; }
};

extern __thread exception_frame* current_interrupt_frame;

class interrupt_desc {
public:
    interrupt_desc(interrupt_desc *old, interrupt *interrupt);
    interrupt_desc(interrupt_desc *old);

    std::vector<std::function<void ()>> handlers;
    std::vector<std::function<bool ()>> acks;
};

class interrupt_table {
public:
    interrupt_table();
    void register_interrupt(interrupt *interrupt);
    void unregister_interrupt(interrupt *interrupt);

    unsigned register_handler(std::function<void ()> handler);
    void unregister_handler(unsigned vector);

    /* invoke_interrupt returns false if unhandled */
    bool invoke_interrupt(unsigned int id);

    void init_msi_vector_base(u32 initial);
    void set_max_msi_vector(u32 max) { _max_msi_vector = max; }

    // Each CPU owns its own handler tables. The common handlers (timer PPI, IPI
    // SGIs) are registered before SMP into the boot CPU's tables, which are
    // copied to each AP at bringup by this call (see arch/aarch64/smp.cc). This
    // mirrors the x64 per-core IDT and removes the single shared table.
    void copy_handlers_to_cpu(sched::cpu *cpu);

private:
    void enable_irq(int id);
    void disable_irq(int id);

    void enable_msi_vector(unsigned vector);

    static constexpr unsigned max_cpus = 64;

    std::atomic<u32> _next_msi_vector;
    u32 _max_msi_vector;
    u32 _msi_vector_base;
    // Per-CPU handler tables, indexed by cpu id. _*[0] is the boot CPU's table
    // and doubles as the template shared into each AP at bringup. irq_desc holds
    // pointers (shared by pointer - the desc objects are immutable once
    // published); _msi_handlers holds std::function values (empty until a device
    // registers one, so the bringup copy allocates nothing).
    std::function<void ()> _msi_handlers[max_cpus][gic::max_msi_handlers] = {};

    unsigned int nr_irqs; /* number of supported InterruptIDs, read from gic */
    std::atomic<interrupt_desc*> irq_desc[max_cpus][gic::max_nr_irqs];
    mutex _lock;
};

extern class interrupt_table idt;

extern "C" {
    void page_fault(exception_frame* ef);
}

bool fixup_fault(exception_frame*);

#endif /* EXCEPTIONS_HH */
