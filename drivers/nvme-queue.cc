/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

// User-queue-only build: this file only implements the admin queue (and its
// interrupt-driven base). The data path lives in nvme-user-queue.cc; there is
// no block-device (bio) io_queue_pair here.

#include <sys/cdefs.h>

#include <vector>
#include <memory>
#include <iostream>

#include <osv/contiguous_alloc.hh>
#include <osv/trace.hh>
#include <osv/mempool.hh>
#include <osv/align.hh>

#include "nvme-queue.hh"

#include <queue>


TRACEPOINT(trace_nvme_aq_cq_wait, "nvme%d qid=%d, cq_head=%d", int, int, int);
TRACEPOINT(trace_nvme_aq_cq_woken, "nvme%d qid=%d, have_elements=%d", int, int, bool);
TRACEPOINT(trace_nvme_aq_req_done_error, "nvme%d qid=%d, cid=%d, status type=%#x, status code=%#x", int, int, u16, u8, u8);
TRACEPOINT(trace_nvme_aq_req_done_success, "nvme%d qid=%d, cid=%d", int, int, u16);
TRACEPOINT(trace_nvme_aq_cmd_submit, "nvme%d qid=%d, cid=%d, opc=%d", int, int, int, u8);


using namespace memory;

namespace nvme
{

    queue_interrupt_pair::queue_interrupt_pair(
        int did,
        u32 id,
        int qsize,
        pci::device &dev,
        u32 *sq_doorbell,
        u32 *cq_doorbell,
        std::map<u32, nvme_ns_t *> &ns)
        : queue_pair(did,
                        id,
                        qsize,
                        dev,
                        sq_doorbell,
                        cq_doorbell,
                        ns)
    {}

    queue_interrupt_pair::~queue_interrupt_pair()
    {}

    void queue_interrupt_pair::wait_for_completion_queue_entries()
    {
        trace_nvme_aq_cq_wait(_driver_id, _id, _cq._head);
        sched::thread::wait_until([this]
                                  {
        bool have_elements = this->completion_queue_not_empty();
        if (!have_elements) {
            this->enable_interrupts();
            //check if we got a new cqe between completion_queue_not_empty()
            //and enable_interrupts()
            have_elements = this->completion_queue_not_empty();
            if (have_elements) {
                this->disable_interrupts();
            }
        }

        trace_nvme_aq_cq_woken(_driver_id, _id, have_elements);
        return have_elements; });
    }

    admin_queue_pair::admin_queue_pair(
        int driver_id,
        int id,
        int qsize,
        pci::device &dev,
        u32 *sq_doorbell,
        u32 *cq_doorbell,
        std::map<u32, nvme_ns_t *> &ns) : queue_interrupt_pair(driver_id,
                                                     id,
                                                     qsize,
                                                     dev,
                                                     sq_doorbell,
                                                     cq_doorbell,
                                                     ns) {}

    void admin_queue_pair::req_done()
    {
        nvme_cq_entry_t *cqe = nullptr;
        while (true)
        {
            wait_for_completion_queue_entries();
            while ((cqe = get_completion_queue_entry()))
            {
                u16 cid = cqe->cid;
                if (cqe->sct != 0 || cqe->sc != 0)
                {
                    trace_nvme_aq_req_done_error(_driver_id, _id, cid, cqe->sct, cqe->sc);
                    NVME_ERROR("Admin queue cid=%d, sct=%#x, sc=%#x\n", cid, cqe->sct, cqe->sc);
                }
                else
                {
                    trace_nvme_aq_req_done_success(_driver_id, _id, cid);
                }

                _sq._head = cqe->sqhd; // Update sq_head
                _req_res = *cqe;       // Save the cqe so that the requesting thread can return it

                advance_cq_head();
            }
            mmio_setl(_cq._doorbell, _cq._head);

            // Wake up the thread that requested the admin command
            new_cq = true;
            _req_waiter.wake_from_kernel_or_with_irq_disabled();
        }
    }

    nvme_cq_entry_t
    admin_queue_pair::submit_and_return_on_completion(nvme_sq_entry_t *cmd, void *data, unsigned int datasize)
    {
        SCOPE_LOCK(_lock);

        _req_waiter.reset(*sched::thread::current());

        // for now admin cid = sq_tail
        u16 cid = _sq._tail;
        cmd->rw.common.cid = cid;

        if (data != nullptr && datasize > 0)
        {
            cmd->rw.common.prp1 = (u64)data;
            cmd->rw.common.prp2 = 0;
        }

        trace_nvme_aq_cmd_submit(_driver_id, _id, cid, cmd->set_features.common.opc);
        submit_cmd(cmd);

        sched::thread::wait_until([this]
                                  { return this->new_cq; });
        _req_waiter.clear();

        new_cq = false;

        return _req_res;
    }
}
