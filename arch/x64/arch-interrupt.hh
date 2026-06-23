/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_INTERRUPT_HH
#define ARCH_INTERRUPT_HH

#include <osv/sched.hh>

class inter_processor_interrupt : public interrupt {
public:
    explicit inter_processor_interrupt(enum ipi_id, std::function<void ()>);
    ~inter_processor_interrupt();

    void set_vector(unsigned v);
    unsigned get_vector();

    void send(sched::cpu* cpu);
    void send_allbutself();
private:
    unsigned _vector;
};

#endif /* ARCH_INTERRUPT_HH */
