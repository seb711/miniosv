#pragma once
// -------------------------------------------------------------------------------------
#include <atomic>
#include <cassert>
#include <memory>
#include <stdexcept>
#include "Exceptions.hpp"
#include "leanstore/io/IoRequest.hpp"
// -------------------------------------------------------------------------------------
// Minimal lock-free primitives (std-only replacements for boost::lockfree)
// -------------------------------------------------------------------------------------

// Treiber stack  (LIFO, unbounded node count capped by external slot pool)
// T must be a pointer type.
template <typename T>
class TreiberStack
{
    struct Node {
        T       value;
        Node*   next;
    };
    std::atomic<Node*> head_{nullptr};

    // Node storage is pre-allocated from a flat array supplied at construction
    // so that push() never calls operator new at runtime.
    std::unique_ptr<Node[]> nodes_;
    // A tiny lock-free freelist for Node objects themselves (bootstrapped from
    // the pre-allocated array during construction with plain pointer chasing --
    // no lock needed because construction is single-threaded).
    std::atomic<Node*> node_free_{nullptr};

    Node* alloc_node()
    {
        Node* n = node_free_.load(std::memory_order_acquire);
        while (n && !node_free_.compare_exchange_weak(
                         n, n->next,
                         std::memory_order_acquire,
                         std::memory_order_relaxed))
        {}
        return n;  // nullptr iff pool exhausted (push will throw)
    }
    void free_node(Node* n)
    {
        n->next = node_free_.load(std::memory_order_relaxed);
        while (!node_free_.compare_exchange_weak(
                   n->next, n,
                   std::memory_order_release,
                   std::memory_order_relaxed))
        {}
    }

public:
    explicit TreiberStack(int capacity)
        : nodes_(std::make_unique<Node[]>(static_cast<std::size_t>(capacity)))
    {
        // Chain all nodes into the node freelist (single-threaded init).
        for (int i = capacity - 1; i >= 0; --i) {
            nodes_[i].next = node_free_.load(std::memory_order_relaxed);
            node_free_.store(&nodes_[i], std::memory_order_relaxed);
        }
    }

    // Returns false only if the internal node pool is exhausted (i.e. more
    // items were pushed than capacity allows, which is a logic error).
    bool push(T value)
    {
        Node* n = alloc_node();
        if (!n) return false;
        n->value = value;
        n->next  = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(
                   n->next, n,
                   std::memory_order_release,
                   std::memory_order_relaxed))
        {}
        return true;
    }

    bool pop(T& out)
    {
        Node* n = head_.load(std::memory_order_acquire);
        while (n && !head_.compare_exchange_weak(
                         n, n->next,
                         std::memory_order_acquire,
                         std::memory_order_relaxed))
        {}
        if (!n) return false;
        out = n->value;
        free_node(n);
        return true;
    }
};

// -------------------------------------------------------------------------------------
// MPMC bounded ring-buffer queue (power-of-two capacity).
// Based on the classic Dmitry Vyukov MPMC queue design.
// T must be a pointer type (or any trivially copyable scalar).
template <typename T>
class MpmcQueue
{
    struct Cell {
        std::atomic<std::size_t> sequence;
        T                        data;
    };

    static constexpr std::size_t kCacheLine = 64;

    std::unique_ptr<Cell[]> buffer_;
    const std::size_t       mask_;   // capacity - 1  (capacity is a power of 2)

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};

    static std::size_t next_pow2(std::size_t n)
    {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

public:
    explicit MpmcQueue(int capacity)
        : buffer_(std::make_unique<Cell[]>(next_pow2(static_cast<std::size_t>(capacity))))
        , mask_(next_pow2(static_cast<std::size_t>(capacity)) - 1)
    {
        std::size_t cap = mask_ + 1;
        for (std::size_t i = 0; i < cap; ++i)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool push(T value)
    {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq)
                                - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed))
                {
                    cell.data = value;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // queue full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(T& out)
    {
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq)
                                - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed))
                {
                    out = cell.data;
                    cell.sequence.store(pos + mask_ + 1,
                                        std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // queue empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
};

// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
// Thread-safe slot pool built on top of a lock-free Treiber stack and a
// lock-free MPMC ring queue (both std-only, no Boost required).
//
// IMPORTANT correctness rules used throughout this class:
//   * The lock-free container op (push/pop) is the ONLY authoritative gate.
//     Never decide whether to proceed by reading a side counter first --
//     between the counter read and the container op another thread can change
//     things, so a counter pre-check is a race.
//   * Side counters (`free`, `pushed`, `outstanding_count`) are accounting
//     hints only.  To keep them from ever being observed as negative we always:
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
    std::unique_ptr<R[]>  requests;
    TreiberStack<R*>      free_stack;
    MpmcQueue<R*>         submit_stack;
#ifndef NDEBUG
    std::atomic<int>      outstanding_count{0};
#endif
    const int             max_entries;
    std::atomic<int>      free;
    std::atomic<int>      pushed{0};

    explicit RequestStackLockfree(int max_entries_)
        : free_stack(max_entries_)
        , submit_stack(max_entries_)
        , max_entries(max_entries_)
        , free(max_entries_)
    {
        requests = std::make_unique<R[]>(static_cast<std::size_t>(max_entries));
        for (int i = 0; i < max_entries; i++) {
            bool ok = free_stack.push(&requests[i]);
            (void)ok;
            assert(ok);
        }
    }

    ~RequestStackLockfree() = default;

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
        if (free_stack.pop(out)) {
            free.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /* user -> to submit */
    void pushToSubmitStack(R* req)
    {
        pushed.fetch_add(1, std::memory_order_relaxed);
        if (!submit_stack.push(req)) {
            pushed.fetch_sub(1, std::memory_order_relaxed);
            throw std::logic_error("RequestStackLockfree: submit queue overflow");
        }
    }

    /* free -> submit / direct path (not like popFromFree and pushToSubmit) */
    bool moveFreeToSubmitStack(R*& out)
    {
        if (!free_stack.pop(out)) {
            return false;
        }
        free.fetch_sub(1, std::memory_order_relaxed);
        pushed.fetch_add(1, std::memory_order_relaxed);
        if (!submit_stack.push(out)) {
            pushed.fetch_sub(1, std::memory_order_relaxed);
            free.fetch_add(1, std::memory_order_relaxed);
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
            pushed.fetch_sub(1, std::memory_order_relaxed);
#ifndef NDEBUG
            outstanding_count.fetch_add(1, std::memory_order_relaxed);
#endif
            // WARNING: `item` is discarded here. If these requests still need
            // to be submitted or tracked, this silently loses them -- drain
            // with popFromSubmitStack(out) instead.
        }
    }

    /* submit -> outstanding */
    bool popFromSubmitStack(R*& out)
    {
        if (submit_stack.pop(out)) {
            pushed.fetch_sub(1, std::memory_order_relaxed);
#ifndef NDEBUG
            outstanding_count.fetch_add(1, std::memory_order_relaxed);
#endif
            return true;
        }
        return false;
    }

    /* outstanding -> free */
    void returnToFreeList(R* ptr)
    {
#ifndef NDEBUG
        outstanding_count.fetch_sub(1, std::memory_order_relaxed);
#endif
        free.fetch_add(1, std::memory_order_relaxed);
        if (!free_stack.push(ptr)) {
            free.fetch_sub(1, std::memory_order_relaxed);
#ifndef NDEBUG
            outstanding_count.fetch_add(1, std::memory_order_relaxed);
#endif
            throw std::logic_error("RequestStackLockfree: free stack overflow");
        }
    }
};
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
