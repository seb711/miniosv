#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>
#include "Units.hpp"

#if false // NDEBUG
#define RB_DEBUG_COUNTER(x)
#else
#define RB_DEBUG_COUNTER(x) x
#endif

namespace leanstore {
namespace utils {

// Bounded, lock-free multi-producer / multi-consumer queue (Vyukov algorithm).
//
// Every slot carries a monotonically increasing sequence number. A producer
// claims the next enqueue slot with a single CAS on `enqueue_pos`; a consumer
// claims the next dequeue slot with a single CAS on `dequeue_pos`. No thread
// can ever block another, so the structure is lock-free under any mix of
// concurrent producers and consumers.
//
// Differences vs. the previous SPSC implementation:
//   * Safe with multiple producers AND multiple consumers.
//   * Capacity is rounded up to the next power of two (see `max_size`).
//   * `front()` is gone: returning a reference into the buffer cannot be made
//     safe when another consumer may pop the element concurrently. Use
//     `try_pop` instead.
//   * `size()/empty()/full()` are approximate under concurrency (they sample
//     two atomics that may move between the two loads) -- treat them as hints.
template <typename TValue>
class RingBuffer {
    // 64-byte alignment keeps neighbouring cells off the same cache line,
    // avoiding false sharing between producers/consumers touching adjacent slots.
    struct alignas(64) Cell {
        std::atomic<u64> sequence;
        TValue data;
    };

    static constexpr u64 CACHELINE = 64;

    static u64 round_up_pow2(u64 v) {
        if (v < 2) return 2;
        v--;
        v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
        v |= v >> 8;  v |= v >> 16; v |= v >> 32;
        return v + 1;
    }

    std::vector<Cell> buffer;
    const u64 buffer_mask;

    // Separate cache lines for the two hot counters.
public: 
    alignas(CACHELINE) std::atomic<u64> enqueue_pos;
    alignas(CACHELINE) std::atomic<u64> dequeue_pos;

public:
    RB_DEBUG_COUNTER(
        std::atomic<u64> inserted{0};
        std::atomic<u64> erased{0};
    )

    static constexpr int POP_MAX = 32;
    const u64 max_size;  // actual capacity == buffer size (power of two, >= requested)

    explicit RingBuffer(u64 requested_size)
        : buffer(round_up_pow2(requested_size)),
          buffer_mask(buffer.size() - 1),
          enqueue_pos(0),
          dequeue_pos(0),
          max_size(buffer.size()) {
        for (u64 i = 0; i < buffer.size(); ++i)
            buffer[i].sequence.store(i, std::memory_order_relaxed);
    }

    RingBuffer(RingBuffer const&) = delete;
    RingBuffer& operator=(RingBuffer const&) = delete;

    // ---- producer side -----------------------------------------------------

    // Non-throwing: returns false when the queue is full.
    bool try_push(TValue&& value)      { return emplace(std::move(value)); }
    bool try_push(const TValue& value) { return emplace(value); }

    // Throwing variants, kept for API compatibility with the old interface.
    void push_back(const TValue& value) {
        if (!emplace(value))
            throw std::logic_error("cannot push more into ringbuffer: full");
    }
    void push_back(TValue&& value) {
        if (!emplace(std::move(value)))
            throw std::logic_error("cannot push more into ringbuffer: full");
    }

    // ---- consumer side -----------------------------------------------------

    bool try_pop(TValue& ret) {
        Cell* cell;
        u64 pos = dequeue_pos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer[pos & buffer_mask];
            u64 seq = cell->sequence.load(std::memory_order_acquire);
            std::int64_t dif = static_cast<std::int64_t>(seq) -
                               static_cast<std::int64_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos.load(std::memory_order_relaxed);
            }
        }
        ret = std::move(cell->data);
        RB_DEBUG_COUNTER(cell->data = TValue{};)  // clear for debugging
        // Release the slot to producers one full lap ahead.
        cell->sequence.store(pos + buffer_mask + 1, std::memory_order_release);
        RB_DEBUG_COUNTER(erased.fetch_add(1, std::memory_order_relaxed);)
        return true;
    }

    int pop_multiple(std::array<TValue, POP_MAX>& pop_into, int pop_max) {
        int limit = (pop_max < POP_MAX) ? pop_max : POP_MAX;
        int popped = 0;
        while (popped < limit && try_pop(pop_into[popped]))
            ++popped;
        return popped;
    }

    // ---- observers (approximate under concurrency) -------------------------

    u64 size() {
        u64 e = enqueue_pos.load(std::memory_order_acquire);
        u64 d = dequeue_pos.load(std::memory_order_acquire);
        return (e > d) ? (e - d) : 0;
    }
    bool empty() { return size() == 0; }
    bool full()  { return size() >= max_size; }

private:
    template <typename U>
    bool emplace(U&& value) {
        Cell* cell;
        u64 pos = enqueue_pos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer[pos & buffer_mask];
            u64 seq = cell->sequence.load(std::memory_order_acquire);
            std::int64_t dif = static_cast<std::int64_t>(seq) -
                               static_cast<std::int64_t>(pos);
            if (dif == 0) {
                if (enqueue_pos.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::forward<U>(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        RB_DEBUG_COUNTER(inserted.fetch_add(1, std::memory_order_relaxed);)
        return true;
    }
};

}  // namespace utils
}  // namespace leanstore