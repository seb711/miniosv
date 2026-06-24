#pragma once
// -------------------------------------------------------------------------------------
#include <atomic>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <boost/lockfree/stack.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/atomic.hpp>
#include "Exceptions.hpp"
#include "leanstore/io/IoRequest.hpp"
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
// Thread-safe slot pool built on top of two Boost lock-free containers.
//
// IMPORTANT correctness rules used throughout this class:
//   * The lock-free container op (push/pop) is the ONLY authoritative gate.
//     Never decide whether to proceed by reading a side counter first --
//     between the counter read and the container op another thread can change
//     things, so a counter pre-check is a race (it can drop a just-pushed item
//     or spuriously report "empty"/"full").
//   * Side counters (`free`, `pushed`, `outstanding_count`) are accounting hints
//     only. To keep them from ever being observed as negative we always:
//        - increment the counter BEFORE publishing an item with push(), and
//        - decrement the counter AFTER removing an item with a successful pop().
//     This guarantees the counter is always >= the true number of items, so a
//     concurrent reader can over-count transiently but never sees a negative.
//   * Because the counters and the containers are still two separate objects,
//     no reader gets an atomic snapshot across them: size()/full()/outstanding()
//     are estimates, never correctness gates.
template <typename R>
class RequestStackLockfree
{
public:
    std::unique_ptr<R[]> requests;
    boost::lockfree::stack<R*> free_stack;
    boost::lockfree::queue<R*> submit_stack;

#ifndef NDEBUG
    std::atomic<int> outstanding_count{0};
#endif

    const int max_entries;
    boost::atomic<int> free;
    boost::atomic<int> pushed{0};

    RequestStackLockfree(int max_entries) :
        free_stack(max_entries),
        submit_stack(max_entries),
        max_entries(max_entries),
        free(max_entries)
    {
        requests = std::make_unique<R[]>(max_entries);
        for (int i = 0; i < max_entries; i++) {
            bool ok = free_stack.push(&requests[i]);
            (void)ok;
            assert(ok);
        }
    }

    ~RequestStackLockfree() {}

    // Estimate only (see class note). Clamped at 0 because transient
    // over-counting of `free`/`pushed` can briefly push the difference below 0.
    int outstanding()
    {
        int est = max_entries - free.load() - pushed.load();
        return est < 0 ? 0 : est;
    }

    // Estimate only.
    int submitStackSize()
    {
        int p = pushed.load();
        return p < 0 ? 0 : p;
    }

    // Estimate only; do not use as a hard gate -- let the actual pop decide.
    bool full()
    {
        return free.load() <= 0;
    }

    /* free -> to user (untracked) */
    bool popFromFreeStack(R*& out)
    {
        // The pop itself is the gate; do NOT pre-check `free`.
        if (free_stack.pop(out)) {
            free.fetch_sub(1);  // decrement AFTER the slot has left the stack
            return true;
        }
        return false;
    }

    /* user -> to submit */
    void pushToSubmitStack(R* req)
    {
        pushed.fetch_add(1);  // count BEFORE the item is visible to consumers
        if (!submit_stack.push(req)) {
            pushed.fetch_sub(1);
            throw std::logic_error("RequestStackLockfree: submit queue overflow");
        }
    }

    /* free -> submit / direct path (not like popFromFree and pushToSubmit) */
    bool moveFreeToSubmitStack(R*& out)
    {
        if (!free_stack.pop(out)) {
            return false;
        }
        free.fetch_sub(1);

        pushed.fetch_add(1);  // count BEFORE publishing
        if (!submit_stack.push(out)) {
            // roll back: return the slot to the free stack so it is not leaked
            pushed.fetch_sub(1);
            free.fetch_add(1);
            free_stack.push(out);
            throw std::logic_error("RequestStackLockfree: submit queue overflow");
        }
        return true;
        // NOTE: `out` is already in the submit queue when this returns. A
        // consumer may pop it before the caller touches *out, so the caller
        // MUST NOT fill the request after calling this. If the request needs
        // to be populated first, use popFromFreeStack + pushToSubmitStack.
    }

    /* submit -> outstanding */
    void emptySubmitStack()
    {
        R* item;
        while (submit_stack.pop(item)) {
            pushed.fetch_sub(1);  // decrement AFTER a successful pop
#ifndef NDEBUG
            outstanding_count.fetch_add(1);
#endif
            // WARNING: `item` is discarded here. If these requests still need to
            // be submitted or tracked, this silently loses them -- drain with
            // popFromSubmitStack(out) instead so the caller receives each R*.
        }
    }

    /* submit -> outstanding */
    bool popFromSubmitStack(R*& out)
    {
        // The pop is the gate; do NOT pre-check `pushed` (that races with a
        // producer that has pushed but not yet incremented the counter).
        if (submit_stack.pop(out)) {
            pushed.fetch_sub(1);
#ifndef NDEBUG
            outstanding_count.fetch_add(1);
#endif
            return true;
        }
        return false;
    }

    /* outstanding -> free */
    void returnToFreeList(R* ptr)
    {
#ifndef NDEBUG
        outstanding_count.fetch_sub(1);
#endif
        free.fetch_add(1);  // count BEFORE publishing to the free stack
        if (!free_stack.push(ptr)) {
            free.fetch_sub(1);
#ifndef NDEBUG
            outstanding_count.fetch_add(1);
#endif
            throw std::logic_error("RequestStackLockfree: free stack overflow");
        }
    }
};
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------