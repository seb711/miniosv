/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "exceptions.hh"
#include "dump.hh"
#include <osv/mmu.hh>
#include "processor.hh"
#include <osv/interrupt.hh>
#include <osv/sched.hh>
#include <osv/debug.hh>
#include <apic.hh>
#include <osv/prio.hh>
#include <osv/mutex.h>
#include <osv/intr_random.hh>
#include <osv/kernel_config.h>

#include "fault-fixup.hh"

__thread exception_frame* current_interrupt_frame;
interrupt_descriptor_table idt __attribute__((init_priority((int)init_prio::idt)));

extern "C" {
    void ex_de();
    void ex_db();
    void ex_nmi();
    void ex_bp();
    void ex_of();
    void ex_br();
    void ex_ud();
    void ex_nm();
    void ex_df();
    void ex_ts();
    void ex_np();
    void ex_ss();
    void ex_gp();
    void ex_pf();
    void ex_mf();
    void ex_ac();
    void ex_mc();
    void ex_xm();
}

interrupt_descriptor_table::interrupt_descriptor_table()
{

    add_entry(0, 1, ex_de);
    add_entry(1, 1, ex_db);
    add_entry(2, 1, ex_nmi);
    add_entry(3, 1, ex_bp);
    add_entry(4, 1, ex_of);
    add_entry(5, 1, ex_br);
    add_entry(6, 1, ex_ud);
    add_entry(7, 1, ex_nm);
    add_entry(8, 1, ex_df);
    add_entry(10, 1, ex_ts);
    add_entry(11, 1, ex_np);
    add_entry(12, 1, ex_ss);
    add_entry(13, 1, ex_gp);
    add_entry(14, 1, ex_pf);
    add_entry(16, 1, ex_mf);
    add_entry(17, 1, ex_ac);
    add_entry(18, 1, ex_mc);
    add_entry(19, 1, ex_xm);

    extern char interrupt_entry[];
    for (unsigned i = 32; i < 256; ++i) {
        add_entry(i, 2, reinterpret_cast<void (*)()>(interrupt_entry + (i - 32) * 16));
    }
}

void interrupt_descriptor_table::add_entry(unsigned vec, unsigned ist, void (*handler)())
{
    ulong addr = reinterpret_cast<ulong>(handler);
    idt_entry e = { };
    e.offset0 = addr;
    e.selector = processor::read_cs();
    // We can't take interrupts on the main stack due to the x86-64 redzone
    e.ist = ist;
    e.type = type_intr_gate;
    e.s = s_special;
    e.dpl = 0;
    e.p = 1;
    e.offset1 = addr >> 16;
    e.offset2 = addr >> 32;
    _idt[vec] = e;
}

void
interrupt_descriptor_table::load_on_cpu()
{
    processor::desc_ptr d(sizeof(_idt) - 1,
                               reinterpret_cast<ulong>(&_idt));
    processor::lidt(d);
}

unsigned interrupt_descriptor_table::register_interrupt_handler(
        std::function<bool ()> pre_eoi,
        std::function<void ()> eoi,
        std::function<void ()> post_eoi)
{
    // The common IPI / APIC-timer handlers are registered single-threaded during
    // boot (IPIs at static init, timer at clock init) into the boot CPU's table,
    // which is the template copied to every CPU at bringup. An atomic release
    // store publishes the handler, so no lock is needed here - and because each
    // CPU's table is mutated only at boot and read only by its own
    // invoke_interrupt(), that path needs no lock either.
    for (unsigned i = 32; i < 256; ++i) {
        if (_handlers[0][i].load(std::memory_order_relaxed) == nullptr) {
            auto n = new handler(nullptr, pre_eoi, eoi, post_eoi);
            _handlers[0][i].store(n, std::memory_order_release);
            return i;
        }
    }
    abort();
}

unsigned interrupt_descriptor_table::register_handler(std::function<void ()> post_eoi)
{
    return register_interrupt_handler([] { return true; }, [] { processor::apic->eoi(); }, post_eoi);
}

void interrupt_descriptor_table::unregister_handler(unsigned vector)
{
    // Only the IPI globals use this, and they are never destroyed at runtime.
    auto o = _handlers[0][vector].load(std::memory_order_relaxed);
    _handlers[0][vector].store(nullptr, std::memory_order_relaxed);
    delete o;
}

void interrupt_descriptor_table::copy_handlers_to_cpu(sched::cpu *cpu)
{
    static_assert(max_cpus >= sched::max_cpus,
                  "per-CPU handler table must cover every CPU");
    // Point this CPU's table at the boot CPU's template handlers. We copy the
    // POINTERS, not the handler objects: the common IPI/timer handlers are
    // immutable after registration and never freed, so they can be shared by
    // every CPU. Crucially this allocates nothing - smp_main runs before this
    // CPU's per-CPU memory allocator is set up, so calling new here would hang.
    // (Per-CPU device handlers, when added, are allocated later and owned by
    // their own CPU, so sharing the template entries causes no double-free.)
    // `cpu` is passed explicitly because sched::cpu::current() is not usable
    // this early in smp_main.
    unsigned id = cpu->id;
    if (id == 0) {
        return;  // boot CPU uses _handlers[0] (the template) directly
    }
    for (unsigned i = 0; i < 256; ++i) {
        _handlers[id][i].store(_handlers[0][i].load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    }
}

void interrupt_descriptor_table::register_interrupt(inter_processor_interrupt *interrupt)
{
    unsigned v = register_handler(interrupt->get_handler());
    interrupt->set_vector(v);
}

void interrupt_descriptor_table::unregister_interrupt(inter_processor_interrupt *interrupt)
{
    unregister_handler(interrupt->get_vector());
}

void interrupt_descriptor_table::invoke_interrupt(unsigned vector)
{
#if CONF_lazy_stack_invariant
    assert(!arch::irq_enabled());
#endif
    // Interrupt context (irq disabled), so the current CPU id is stable and we
    // read this CPU's own handler table - no lock/RCU needed.
    auto ptr = _handlers[sched::cpu::current()->id][vector].load(std::memory_order_consume);
    if (!ptr) {
        // No handler for this vector on this CPU. Common vectors (IPIs, timer)
        // are in every CPU's table via the template copy, so this can only be a
        // device MSI delivered to a CPU that doesn't own it - a routing/pinning
        // bug, which the "every MSI is pinned" invariant is meant to prevent.
        // (We deliberately do NOT EOI: the APIC spurious vector also lands with
        // no handler and must not be EOI'd.)
        assert(false && "interrupt delivered to a CPU with no handler for this vector");
        return;
    }

    unsigned i, nr_shared = ptr->size();
    bool handled = false;
    for (i = 0 ; i < nr_shared; i++) {
        handled = ptr->pre_eois[i]();
        if (handled) {
            break;
        }
    }

    ptr->eoi();

    if (handled) {
        ptr->post_eois[i]();
    }
}

extern "C" { void interrupt(exception_frame* frame); }

void interrupt(exception_frame* frame)
{
    sched::fpu_lock fpu;
    SCOPE_LOCK(fpu);
    // Rather that force the exception frame down the call stack,
    // remember it in a global here.  This works because our interrupts
    // don't nest.
    current_interrupt_frame = frame;
    unsigned vector = frame->error_code;
    harvest_interrupt_randomness(vector, frame);
    idt.invoke_interrupt(vector);
    // must call scheduler after EOI, or it may switch contexts and miss the EOI
    current_interrupt_frame = nullptr;
    // FIXME: layering violation
    sched::preempt();
}

bool fixup_fault(exception_frame* ef)
{
    fault_fixup v{ef->rip, 0};
    auto ff = std::lower_bound(fault_fixup_start, fault_fixup_end, v);
    if (ff != fault_fixup_end && ff->pc == ef->rip) {
        ef->rip = ff->divert;
        return true;
    }
    return false;
}

// Implement the various x86 exception handlers mentioned in arch/x64/entry.S.
// Note page_fault() is implemented in core/mmu.cc.

extern "C" void divide_error(exception_frame *ef);
void divide_error(exception_frame *ef)
{
    abort("divide error.");
}

extern "C" void simd_exception(exception_frame *ef)
{
    abort("simd exception");
}

extern "C" void nmi(exception_frame* ef)
{
    while (true) {
        processor::cli_hlt();
    }
}

extern "C"
void general_protection(exception_frame* ef)
{
    sched::exception_guard g;
    sched::fpu_lock fpu;
    SCOPE_LOCK(fpu);
    if (fixup_fault(ef)) {
        return;
    }

    dump_registers(ef);

    abort("general protection fault\n");
}

#define DUMMY_HANDLER(x) \
     extern "C" void x(exception_frame* ef); void x(exception_frame *ef) { dump_registers(ef); abort("DUMMY_HANDLER for " #x " aborting.\n"); }

DUMMY_HANDLER(debug_exception)
DUMMY_HANDLER(breakpoint)
DUMMY_HANDLER(overflow)
DUMMY_HANDLER(bound_range_exceeded)
DUMMY_HANDLER(invalid_opcode)
DUMMY_HANDLER(device_not_available)
DUMMY_HANDLER(double_fault)
DUMMY_HANDLER(invalid_tss)
DUMMY_HANDLER(segment_not_present)
DUMMY_HANDLER(stack_fault)
DUMMY_HANDLER(math_fault)
DUMMY_HANDLER(alignment_check)
DUMMY_HANDLER(machine_check)
