/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H

#include "drivers/pci-device.hh"
#include "drivers/nvme-user-queue.hh"

namespace nvme {
// Interrupt-driven queue pair. In this user-queue-only build it is only used as
// the base for the admin queue (no block-io io_queue_pair exists). The driver
// configures the controller and creates/destroys user I/O queues through the
// admin queue; all data-path I/O goes through io_user_queue_pair.
class queue_interrupt_pair : public queue_pair
{
public:
    queue_interrupt_pair(
        int driver_id,
        u32 id,
        int qsize,
        pci::device& dev,
        u32* sq_doorbell,
        u32* cq_doorbell,
        std::map<u32, nvme_ns_t*>& ns
    );

    ~queue_interrupt_pair();

    void wait_for_completion_queue_entries();

protected:
    int _driver_id;

    mutex _lock;
};

// Pair of SQ and CQ queues used for setting up/configuring controller
// like creating I/O queues
class admin_queue_pair : public queue_interrupt_pair {
public:
    admin_queue_pair(
        int driver_id,
        int id,
        int qsize,
        pci::device& dev,
        u32* sq_doorbell,
        u32* cq_doorbell,
        std::map<u32, nvme_ns_t*>& ns
    );

    void req_done();
    nvme_cq_entry_t submit_and_return_on_completion(nvme_sq_entry_t* cmd, void* data = nullptr, unsigned int datasize = 0);
private:
    sched::thread_handle _req_waiter;
    nvme_cq_entry_t _req_res;
    volatile bool new_cq;
};

}

#endif
