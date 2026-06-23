/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef EXCEPTIONS_HH
#define EXCEPTIONS_HH

#include <stdint.h>
#include <atomic>
#include <functional>
#include <osv/types.h>
#include <osv/mutex.h>
#include <vector>

namespace sched { struct cpu; }

class inter_processor_interrupt;

struct exception_frame {
    ulong r15;
    ulong r14;
    ulong r13;
    ulong r12;
    ulong r11;
    ulong r10;
    ulong r9;
    ulong r8;
    ulong rbp;
    ulong rdi;
    ulong rsi;
    ulong rdx;
    ulong rcx;
    ulong rbx;
    ulong rax;
    u16 error_code;
    ulong rip;
    ulong cs;
    ulong rflags;
    ulong rsp;
    ulong ss;

    void *get_pc(void) { return (void*)rip; }
    unsigned int get_error(void) { return error_code; }
};

extern __thread exception_frame* current_interrupt_frame;

class interrupt_descriptor_table {
public:
    interrupt_descriptor_table();
    void load_on_cpu();
    void register_interrupt(inter_processor_interrupt *interrupt);
    void unregister_interrupt(inter_processor_interrupt *interrupt);
    void invoke_interrupt(unsigned vector);

    // The hardware IDT is identical on every CPU (the stubs are shared code),
    // but each CPU owns its own software handler table. The common handlers
    // (IPIs, APIC timer) are registered before SMP into the boot CPU's table,
    // which is copied to each AP at bringup by this call.
    void copy_handlers_to_cpu(sched::cpu *cpu);

    /* TODO: after merge of MSI and Xen callbacks as interrupt class,
     * exposing these as 'public' should not be necessary anymore.
     */
    unsigned register_interrupt_handler(std::function<bool ()> pre_eoi,
                                        std::function<void ()> eoi,
                                        std::function<void ()> post_eoi);

    /* register_handler is a simplified way to call register_interrupt_handler
     * with no pre_eoi, and apic eoi. These target the boot CPU's table (= the
     * template copied to every CPU) and are used for the common IPI/timer
     * handlers registered before SMP.
     */
    unsigned register_handler(std::function<void ()> post_eoi);
    void unregister_handler(unsigned vector);

private:
    enum {
        type_intr_gate = 14,
    };
    enum {
        s_special = 0,
    };
    struct idt_entry {
        u16 offset0;
        u16 selector;
        u8 ist : 3;
        u8 res0 : 5;
        u8 type : 4;
        u8 s : 1;
        u8 dpl : 2;
        u8 p : 1;
        u16 offset1;
        u32 offset2;
        u32 res1;
    } __attribute__((aligned(16)));
    void add_entry(unsigned vec, unsigned ist, void (*handler)());
    idt_entry _idt[256];
    struct handler {
        handler(handler *h, unsigned d)
        {
            if (h) {
                *this = *h;
            }
            for (unsigned i = 0; i < size(); i++) {
                if (ids[i] == d) {
                    ids.erase(ids.begin() + i);
                    pre_eois.erase(pre_eois.begin() + i);
                    post_eois.erase(post_eois.begin() + i);
                    break;
                }
            }
            gsi = -1u;
        }

        handler(handler *h,
                std::function<bool ()> _pre_eoi,
                std::function<void ()> _eoi,
                std::function<void ()> _post_eoi)
        {
            if (h) {
                *this = *h;
            }
            eoi = _eoi;
            ids.push_back(id++);
            pre_eois.push_back(_pre_eoi);
            post_eois.push_back(_post_eoi);
            gsi = -1u;
        }

        unsigned size()
        {
            return ids.size();
        }

        std::vector<std::function<bool ()>> pre_eois;
        std::function<void ()> eoi;
        std::vector<std::function<void ()>> post_eois;
        std::vector<unsigned> ids;
        unsigned id;
        unsigned gsi;
    };
    // One software handler table per CPU, indexed by cpu id. Plain atomic
    // pointers (no RCU, no lock): each table is read only by its owning CPU's
    // invoke_interrupt() and written only single-threaded at boot. _handlers[0]
    // is the boot CPU's table and doubles as the template copied into each AP's
    // table at bringup.
    static constexpr unsigned max_cpus = 64;
    std::atomic<handler*> _handlers[max_cpus][256] = {};
};

extern interrupt_descriptor_table idt;

extern "C" {
    void page_fault(exception_frame* ef);
}

bool fixup_fault(exception_frame*);

#endif
