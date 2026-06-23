/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mempool.hh>
#include <osv/ilog2.hh>
#include "arch-setup.hh"
#include <cassert>
#include <cstdint>
#include <new>
#include <string.h>
#include <lockfree/unordered-queue-mpsc.hh>
#include "libc/libc.hh"
#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/kernel_config.h>
#if CONF_memory_tracker
#include <osv/alloctracker.hh>
#endif
#include <atomic>
#include <osv/mmu.hh>
#include <osv/trace.hh>
#include <osv/preempt-lock.hh>
#include <osv/sched.hh>
#include <algorithm>
#include <osv/prio.hh>
#include <stdlib.h>
#include <osv/defer.hh>
#include <osv/dbg-alloc.hh>
#include <osv/migration-lock.hh>
#include <osv/export.h>

#include <osv/kernel_config.h>

#include <boost/dynamic_bitset.hpp>
#include <boost/lockfree/stack.hpp>
#include <boost/lockfree/policies.hpp>

TRACEPOINT(trace_memory_malloc, "buf=%p, len=%d, align=%d", void *, size_t,
           size_t);
TRACEPOINT(trace_memory_malloc_mempool, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_malloc_large, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_malloc_page, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_free, "buf=%p", void *);
TRACEPOINT(trace_memory_realloc, "in=%p, newlen=%d, out=%p", void *, size_t, void *);
TRACEPOINT(trace_memory_page_alloc, "page=%p", void*);
TRACEPOINT(trace_memory_page_free, "page=%p", void*);
TRACEPOINT(trace_memory_huge_failure, "page ranges=%d", unsigned long);
TRACEPOINT(trace_memory_reclaim, "shrinker %s, target=%d, delta=%d", const char *, long, long);
TRACEPOINT(trace_memory_wait, "allocation size=%d", size_t);

namespace dbg {

static size_t object_size(void* v);

}

std::atomic<unsigned int> smp_allocator_cnt{};
bool smp_allocator = false;

namespace memory {

size_t phys_mem_size;

#if CONF_memory_tracker
// Optionally track living allocations, and the call chain which led to each
// allocation. Don't set tracker_enabled before tracker is fully constructed.
alloc_tracker tracker;
bool tracker_enabled = false;
static inline void tracker_remember(void *addr, size_t size)
{
    // Check if tracker_enabled is true, but expect (be quicker in the case)
    // that it is false.
    if (__builtin_expect(tracker_enabled, false)) {
        tracker.remember(addr, size);
    }
}
static inline void tracker_forget(void *addr)
{
    if (__builtin_expect(tracker_enabled, false)) {
        tracker.forget(addr);
    }
}
#endif

//
// Before smp_allocator=true, threads are not yet available. malloc and free
// are used immediately after virtual memory is being initialized.
// sched::cpu::current() uses TLS which is set only later on.
//

static inline unsigned mempool_cpuid() {
    return (smp_allocator ? sched::cpu::current()->id: 0);
}

//
// Since the small pools are managed per-cpu, malloc() always access the correct
// pool on the same CPU that it was issued from, free() on the other hand, may
// happen from different CPUs, so for each CPU, we maintain an array of
// lockless spsc rings, which combined are functioning as huge mpsc ring.
//
// The objects freed from a different CPU are reclaimed back into the owning
// CPU's pools by that CPU itself, in pool::alloc() (see collect_garbage()).
// This used to be driven by a per-CPU worker thread woken via a "signal"; the
// worker is gone, so instead a producer that has pushed enough objects sets the
// owning CPU's pcpu_gc_pending flag, and that CPU drains on its next alloc.
//
// As much as the producer is concerned (cpu who did free()) -
// 1st index -> dest cpu
// 2nd index -> local cpu
//

// Set by a producer CPU when it has handed enough objects to `obj_cpu`; checked
// and cleared by obj_cpu in pool::alloc() to trigger collect_garbage().
static std::atomic<bool> pcpu_gc_pending[sched::max_cpus] = {};

class garbage_sink {
private:
    static const int signal_threshold = 256;
    lockfree::unordered_queue_mpsc<free_object> queue;
    int pushed_since_last_signal {};
public:
    void free(unsigned obj_cpu, free_object* obj)
    {
        queue.push(obj);
        if (++pushed_since_last_signal > signal_threshold) {
            // Ask obj_cpu to reclaim on its next allocation (no worker thread).
            pcpu_gc_pending[obj_cpu].store(true, std::memory_order_relaxed);
            pushed_since_last_signal = 0;
        }
    }

    free_object* pop()
    {
        return queue.pop();
    }
};

static garbage_sink ***pcpu_free_list;

void pool::collect_garbage()
{
    assert(!sched::preemptable());

    unsigned cpu_id = mempool_cpuid();

    for (unsigned i = 0; i < sched::cpus.size(); i++) {
        auto sink = pcpu_free_list[cpu_id][i];
        free_object* obj;
        while ((obj = sink->pop())) {
            memory::pool::from_object(obj)->free_same_cpu(obj, cpu_id);
        }
    }
}

// Memory allocation strategy
//
// Bits 44:46 of the virtual address are used to determine which memory
// allocator was used for allocation and, therefore, which one should be used
// to free the memory block.
//
// Small objects (< page size / 4) are stored in pages.  The beginning of the
// page contains a header with a pointer to a pool, consisting of all free
// objects of that size.  The pool maintains a singly linked list of free
// objects, and adds or frees pages as needed.
//
// Objects which size is in range (page size / 4, page size] are given a whole
// page from per-CPU page buffer.  Such objects don't need header they are
// known to be not larger than a single page.  Page buffer is refilled by
// allocating memory from large allocator.
//
// Large objects are rounded up to page size.  They have a header in front that
// contains the page size.  There is gap between the header and the acutal
// object to ensure proper alignment.  Unallocated page ranges are kept either
// in one of 16 doubly linked lists or in a red-black tree sorted by their
// size.  List k stores page ranges which page count is in range
// [2^k, 2^(k + 1)).  The tree stores page ranges that are too big for any of
// the lists.  Memory is allocated from the smallest, non empty list, that
// contains page ranges large enough. If there is no such list then it is a
// worst-fit allocation form the page ranges in the tree.

pool::pool(unsigned size)
    : _size(size)
    , _free()
{
    assert(size + sizeof(page_header) <= page_size);
}

pool::~pool()
{
}

const size_t pool::max_object_size = page_size / 4;
const size_t pool::min_object_size = sizeof(free_object);

pool::page_header* pool::to_header(free_object* object)
{
    return reinterpret_cast<page_header*>(
                 reinterpret_cast<std::uintptr_t>(object) & ~(page_size - 1));
}

TRACEPOINT(trace_pool_alloc, "this=%p, obj=%p", void*, void*);
TRACEPOINT(trace_pool_free, "this=%p, obj=%p", void*, void*);
TRACEPOINT(trace_pool_free_same_cpu, "this=%p, obj=%p", void*, void*);
TRACEPOINT(trace_pool_free_different_cpu, "this=%p, obj=%p, obj_cpu=%d", void*, void*, unsigned);

void* pool::alloc()
{
    void * ret = nullptr;
#if CONF_lazy_stack_invariant
    assert(sched::preemptable() && arch::irq_enabled());
#endif
#if CONF_lazy_stack
    arch::ensure_next_stack_page();
#endif
    WITH_LOCK(preempt_lock) {

        // Reclaim objects that other CPUs freed back to this CPU's pools (this
        // replaces the old per-CPU garbage-collector worker thread). Gated by a
        // per-CPU flag so it is cheap when there is nothing to collect; draining
        // first may also satisfy this allocation without adding a page.
        if (pcpu_gc_pending[mempool_cpuid()].exchange(false, std::memory_order_relaxed)) {
            collect_garbage();
        }

        // We enable preemption because add_page() may take a Mutex.
        // this loop ensures we have at least one free page that we can
        // allocate from, in from the context of the current cpu
        while (_free->empty()) {
            DROP_LOCK(preempt_lock) {
                add_page();
            }
        }

        // We have a free page, get one object and return it to the user
        auto it = _free->begin();
        page_header *header = &(*it);
        free_object* obj = header->local_free;
        ++header->nalloc;
        header->local_free = obj->next;
        if (!header->local_free) {
            _free->erase(it);
        }
        ret = obj;
    }

    trace_pool_alloc(this, ret);
    return ret;
}

unsigned pool::get_size()
{
    return _size;
}

static inline void* untracked_alloc_page();
static inline void untracked_free_page(void *v);

void pool::add_page()
{
    // FIXME: this function allocated a page and set it up but on rare cases
    // we may add this page to the free list of a different cpu, due to the
    // enablement of preemption
    void* page = untracked_alloc_page();
#if CONF_lazy_stack_invariant
    assert(sched::preemptable() && arch::irq_enabled());
#endif
#if CONF_lazy_stack
    arch::ensure_next_stack_page();
#endif
    WITH_LOCK(preempt_lock) {
        page_header* header = new (page) page_header;
        header->cpu_id = mempool_cpuid();
        header->owner = this;
        header->nalloc = 0;
        header->local_free = nullptr;
        for (auto p = static_cast<char*>(page) + page_size - _size; p >= reinterpret_cast<char*>(header + 1); p -= _size) {
            auto obj = reinterpret_cast<free_object*>(p);
            obj->next = header->local_free;
            header->local_free = obj;
        }
        _free->push_back(*header);
        if (_free->empty()) {
            /* encountered when starting to enable TLS for AArch64 in mixed
               LE / IE tls models */
            abort();
        }
    }
}

inline bool pool::have_full_pages()
{
    return !_free->empty() && _free->back().nalloc == 0;
}

void pool::free_same_cpu(free_object* obj, unsigned cpu_id)
{
    void* object = static_cast<void*>(obj);
    trace_pool_free_same_cpu(this, object);

    page_header* header = to_header(obj);
    if (!--header->nalloc && have_full_pages()) {
        if (header->local_free) {
            _free->erase(_free->iterator_to(*header));
        }
        DROP_LOCK(preempt_lock) {
            untracked_free_page(header);
        }
    } else {
        if (!header->local_free) {
            if (header->nalloc) {
                _free->push_front(*header);
            } else {
                // keep full pages on the back, so they're not fragmented
                // early, and so we find them easily in have_full_pages()
                _free->push_back(*header);
            }
        }
        obj->next = header->local_free;
        header->local_free = obj;
    }
}

void pool::free_different_cpu(free_object* obj, unsigned obj_cpu, unsigned cur_cpu)
{
    trace_pool_free_different_cpu(this, obj, obj_cpu);
    auto sink = memory::pcpu_free_list[obj_cpu][cur_cpu];
    sink->free(obj_cpu, obj);
}

void pool::free(void* object)
{
    trace_pool_free(this, object);

#if CONF_lazy_stack_invariant
    assert(sched::preemptable() && arch::irq_enabled());
#endif
#if CONF_lazy_stack
    arch::ensure_next_stack_page();
#endif
    WITH_LOCK(preempt_lock) {

        free_object* obj = static_cast<free_object*>(object);
        page_header* header = to_header(obj);
        unsigned obj_cpu = header->cpu_id;
        unsigned cur_cpu = mempool_cpuid();

        if (obj_cpu == cur_cpu) {
            // free from the same CPU this object has been allocated on.
            free_same_cpu(obj, obj_cpu);
        } else {
            // free from a different CPU. we try to hand the buffer
            // to the proper worker item that is pinned to the CPU that this buffer
            // was allocated from, so it'll free it.
            free_different_cpu(obj, obj_cpu, cur_cpu);
        }
    }
}

pool* pool::from_object(void* object)
{
    auto header = to_header(static_cast<free_object*>(object));
    return header->owner;
}

class malloc_pool : public pool {
public:
    malloc_pool();
private:
    static size_t compute_object_size(unsigned pos);
};

malloc_pool malloc_pools[ilog2_roundup_constexpr(page_size) + 1]
    __attribute__((init_priority((int)init_prio::malloc_pools)));

struct mark_smp_allocator_intialized {
    mark_smp_allocator_intialized() {
        // FIXME: Handle CPU hot-plugging.
        auto ncpus = sched::cpus.size();
        // Our malloc() is very coarse so allocate all the queues in one large buffer.
        // We allocate at least one page because current implementation of aligned_alloc()
        // is not capable of ensuring aligned allocation for small allocations.
        auto buf = aligned_alloc(alignof(garbage_sink),
                    std::max(page_size, sizeof(garbage_sink) * ncpus * ncpus));
        pcpu_free_list = new garbage_sink**[ncpus];
        for (auto i = 0U; i < ncpus; i++) {
            pcpu_free_list[i] = new garbage_sink*[ncpus];
            for (auto j = 0U; j < ncpus; j++) {
                static_assert(!(sizeof(garbage_sink) %
                        alignof(garbage_sink)), "garbage_sink align");
                auto p = pcpu_free_list[i][j] = reinterpret_cast<garbage_sink *>(
                        static_cast<char*>(buf) + sizeof(garbage_sink) * (i * ncpus + j));
                new (p) garbage_sink;
            }
        }
    }
} s_mark_smp_alllocator_initialized __attribute__((init_priority((int)init_prio::malloc_pools)));

malloc_pool::malloc_pool()
    : pool(compute_object_size(this - malloc_pools))
{
}

size_t malloc_pool::compute_object_size(unsigned pos)
{
    size_t size = 1 << pos;
    if (size > max_object_size) {
        size = max_object_size;
    }
    return size;
}

page_range::page_range(size_t _size)
    : size(_size)
{
}

struct addr_cmp {
    bool operator()(const page_range& fpr1, const page_range& fpr2) const {
        return &fpr1 < &fpr2;
    }
};

namespace bi = boost::intrusive;

mutex free_page_ranges_lock;

// Our notion of free memory is "whatever is in the page ranges". Therefore it
// starts at 0, and increases as we add page ranges.
//
// Updates to total should be fairly rare. We only expect updates upon boot,
// and eventually hotplug in an hypothetical future
static std::atomic<size_t> total_memory(0);
static std::atomic<size_t> free_memory(0);
static size_t watermark_lo(0);

static void on_free(size_t mem)
{
    free_memory.fetch_add(mem);
}

static void on_alloc(size_t mem)
{
    free_memory.fetch_sub(mem);
}

static void on_new_memory(size_t mem)
{
    total_memory.fetch_add(mem);
    watermark_lo = stats::total() * 10 / 100;
}

namespace stats {
    size_t free() { return free_memory.load(std::memory_order_relaxed); }
    size_t total() { return total_memory.load(std::memory_order_relaxed); }

    size_t max_no_reclaim()
    {
        auto total = total_memory.load(std::memory_order_relaxed);
        return total - watermark_lo;
    }
}

void oom()
{
    abort("Out of memory: could not reclaim any further. Current memory: %d Kb", stats::free() >> 10);
}

class page_range_allocator {
public:
    static constexpr unsigned max_order = page_ranges_max_order;

    page_range_allocator() : _deferred_free(nullptr) { }

    template<bool UseBitmap = true>
    page_range* alloc(size_t size, bool contiguous = true);
    page_range* alloc_aligned(size_t size, size_t offset, size_t alignment,
                              bool fill = false);
    void free(page_range* pr);

    void initial_add(page_range* pr);

    template<typename Func>
    void for_each(unsigned min_order, Func f);
    template<typename Func>
    void for_each(Func f) {
        for_each<Func>(0, f);
    }

    bool empty() const {
        return _not_empty.none();
    }
    size_t size() const {
        size_t size = _free_huge.size();
        for (auto&& list : _free) {
            size += list.size();
        }
        return size;
    }

    void stats(stats::page_ranges_stats& stats) const {
        stats.order[max_order].ranges_num = _free_huge.size();
        stats.order[max_order].bytes = 0;
        for (auto& pr : _free_huge) {
            stats.order[max_order].bytes += pr.size;
        }

        for (auto order = max_order; order--;) {
            stats.order[order].ranges_num = _free[order].size();
            stats.order[order].bytes = 0;
            for (auto& pr : _free[order]) {
                stats.order[order].bytes += pr.size;
            }
        }
    }

private:
    template<bool UseBitmap = true>
    void insert(page_range& pr) {
        auto addr = static_cast<void*>(&pr);
        auto pr_end = reinterpret_cast<page_range**>(static_cast<char*>(addr) + pr.size - sizeof(page_range**));
        *pr_end = &pr;
        auto order = ilog2(pr.size / page_size);
        if (order >= max_order) {
            _free_huge.insert(pr);
            _not_empty[max_order] = true;
        } else {
            _free[order].push_front(pr);
            _not_empty[order] = true;
        }
        if (UseBitmap) {
            set_bits(pr, true);
        }
    }
    void remove_huge(page_range& pr) {
        _free_huge.erase(_free_huge.iterator_to(pr));
        if (_free_huge.empty()) {
            _not_empty[max_order] = false;
        }
    }
    void remove_list(unsigned order, page_range& pr) {
        _free[order].erase(_free[order].iterator_to(pr));
        if (_free[order].empty()) {
            _not_empty[order] = false;
        }
    }
    void remove(page_range& pr) {
        auto order = ilog2(pr.size / page_size);
        if (order >= max_order) {
            remove_huge(pr);
        } else {
            remove_list(order, pr);
        }
    }

    unsigned get_bitmap_idx(page_range& pr) const {
        auto idx = reinterpret_cast<uintptr_t>(&pr);
        idx -= reinterpret_cast<uintptr_t>(mmu::phys_mem);
        return idx / page_size;
    }
    void set_bits(page_range& pr, bool value, bool fill = false) {
        auto end = pr.size / page_size - 1;
        if (fill) {
            for (unsigned idx = 0; idx <= end; idx++) {
                _bitmap[get_bitmap_idx(pr) + idx] = value;
            }
        } else {
            _bitmap[get_bitmap_idx(pr)] = value;
            _bitmap[get_bitmap_idx(pr) + end] = value;
        }
    }

    bi::multiset<page_range,
                 bi::member_hook<page_range,
                                 bi::set_member_hook<>,
                                 &page_range::set_hook>,
                 bi::constant_time_size<false>> _free_huge;
    bi::list<page_range,
             bi::member_hook<page_range,
                             bi::list_member_hook<>,
                             &page_range::list_hook>,
             bi::constant_time_size<false>> _free[max_order];

    std::bitset<max_order + 1> _not_empty;

    template<typename T>
    class bitmap_allocator {
    public:
        typedef T value_type;
        T* allocate(size_t n);
        void deallocate(T* p, size_t n);
        size_t get_size(size_t n) {
            return align_up(sizeof(T) * n, page_size);
        }
    };
    boost::dynamic_bitset<unsigned long,
                          bitmap_allocator<unsigned long>> _bitmap;
    page_range* _deferred_free;
};

page_range_allocator free_page_ranges
    __attribute__((init_priority((int)init_prio::fpranges)));

template<typename T>
T* page_range_allocator::bitmap_allocator<T>::allocate(size_t n)
{
    auto size = get_size(n);
    on_alloc(size);
    auto pr = free_page_ranges.alloc<false>(size);
    return reinterpret_cast<T*>(pr);
}

template<typename T>
void page_range_allocator::bitmap_allocator<T>::deallocate(T* p, size_t n)
{
    auto size = get_size(n);
    on_free(size);
    auto pr = new (p) page_range(size);
    assert(!free_page_ranges._deferred_free);
    free_page_ranges._deferred_free = pr;
}

template<bool UseBitmap>
page_range* page_range_allocator::alloc(size_t size, bool contiguous)
{
    auto exact_order = ilog2_roundup(size / page_size);
    if (exact_order > max_order) {
        exact_order = max_order;
    }
    auto bitset = _not_empty.to_ulong();
    if (exact_order) {
        bitset &= ~((1 << exact_order) - 1);
    }
    auto order = count_trailing_zeros(bitset);

    page_range* range = nullptr;
    if (!bitset) {
        if (!contiguous || !exact_order || _free[exact_order - 1].empty()) {
            return nullptr;
        }
        // This linear search makes worst case complexity of the allocator
        // O(n). Unfortunately we do not have choice for contiguous allocation
        // so let us hope there is large enough range.
        for (auto&& pr : _free[exact_order - 1]) {
            if (pr.size >= size) {
                range = &pr;
                remove_list(exact_order - 1, *range);
                break;
            }
        }
        if (!range) {
            return nullptr;
        }
    } else if (order == max_order) {
        range = &*_free_huge.rbegin();
        if (range->size < size) {
            return nullptr;
        }
        remove_huge(*range);
    } else {
        range = &_free[order].front();
        remove_list(order, *range);
    }

    auto& pr = *range;
    if (pr.size > size) {
        auto& np = *new (reinterpret_cast<char*>(&pr) + size)
                        page_range(pr.size - size);
        insert<UseBitmap>(np);
        pr.size = size;
    }
    if (UseBitmap) {
        set_bits(pr, false);
    }
    return &pr;
}

page_range* page_range_allocator::alloc_aligned(size_t size, size_t offset,
                                                size_t alignment, bool fill)
{
    page_range* ret_header = nullptr;
    for_each(std::max(ilog2(size / page_size), 1u) - 1, [&] (page_range& header) {
        char* v = reinterpret_cast<char*>(&header);
        auto expected_ret = v + header.size - size + offset;
        auto alignment_shift = expected_ret - align_down(expected_ret, alignment);
        if (header.size >= size + alignment_shift) {
            remove(header);
            if (alignment_shift) {
                insert(*new (v + header.size - alignment_shift)
                            page_range(alignment_shift));
                header.size -= alignment_shift;
            }
            if (header.size == size) {
                ret_header = &header;
            } else {
                header.size -= size;
                insert(header);
                ret_header = new (v + header.size) page_range(size);
            }
            set_bits(*ret_header, false, fill);
            return false;
        }
        return true;
    });
    return ret_header;
}

void page_range_allocator::free(page_range* pr)
{
    auto idx = get_bitmap_idx(*pr);
    if (idx && _bitmap[idx - 1]) {
        auto pr2 = *(reinterpret_cast<page_range**>(pr) - 1);
        remove(*pr2);
        pr2->size += pr->size;
        pr = pr2;
    }
    auto next_idx = get_bitmap_idx(*pr) + pr->size / page_size;
    if (next_idx < _bitmap.size() && _bitmap[next_idx]) {
        auto pr2 = reinterpret_cast<page_range*>(reinterpret_cast<char*>(pr) + pr->size);
        remove(*pr2);
        pr->size += pr2->size;
    }
    insert(*pr);
}

void page_range_allocator::initial_add(page_range* pr)
{
    auto idx = get_bitmap_idx(*pr) + pr->size / page_size;
    if (idx > _bitmap.size()) {
        auto prev_idx = get_bitmap_idx(*pr) - 1;
        if (_bitmap.size() > prev_idx && _bitmap[prev_idx]) {
            auto pr2 = *(reinterpret_cast<page_range**>(pr) - 1);
            remove(*pr2);
            pr2->size += pr->size;
            pr = pr2;
        }
        insert<false>(*pr);
        _bitmap.reset();
        _bitmap.resize(idx);

        for_each([this] (page_range& pr) { set_bits(pr, true); return true; });
        if (_deferred_free) {
            free(_deferred_free);
            _deferred_free = nullptr;
        }
    } else {
        free(pr);
    }
}

template<typename Func>
void page_range_allocator::for_each(unsigned min_order, Func f)
{
    for (auto& pr : _free_huge) {
        if (!f(pr)) {
            return;
        }
    }
    for (auto order = max_order; order-- > min_order;) {
        for (auto& pr : _free[order]) {
            if (!f(pr)) {
                return;
            }
        }
    }
}

namespace stats {
    void get_page_ranges_stats(page_ranges_stats &stats)
    {
        WITH_LOCK(free_page_ranges_lock) {
            free_page_ranges.stats(stats);
        }
    }
}

static void* mapped_malloc_large(size_t size, size_t offset)
{
    //TODO: For now pre-populate the memory, in future consider doing lazy population
    void* obj = mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_read | mmu::perm_write);
    size_t* ret_header = static_cast<size_t*>(obj);
    *ret_header = size;
    return static_cast<char*>(obj) + offset;
}

static void mapped_free_large(void *object)
{
    object = align_down(static_cast<char*>(object) - 1, mmu::page_size);
    size_t* ret_header = static_cast<size_t*>(object);
    mmu::munmap(object, *ret_header);
}

static void* malloc_large(size_t size, size_t alignment, bool contiguous = true)
{
    auto requested_size = size;
    size_t offset;
    if (alignment < page_size) {
        offset = align_up(sizeof(page_range), alignment);
    } else {
        offset = page_size;
    }
    size += offset;
    size = align_up(size, page_size);

    // Use mmap if requested memory greater than "huge page" size
    // and does not need to be contiguous
    if (size >= mmu::huge_page_size && !contiguous) {
        void* obj = mapped_malloc_large(size, offset);
        trace_memory_malloc_large(obj, requested_size, size, alignment);
        return obj;
    }

    while (true) {
        WITH_LOCK(free_page_ranges_lock) {
            page_range* ret_header;
            if (alignment > page_size) {
                ret_header = free_page_ranges.alloc_aligned(size, page_size, alignment);
            } else {
                ret_header = free_page_ranges.alloc(size, contiguous);
            }
            if (ret_header) {
                on_alloc(size);
                void* obj = reinterpret_cast<char*>(ret_header) + offset;
                trace_memory_malloc_large(obj, requested_size, size, alignment);
                return obj;
            } else if (!contiguous) {
                // If we failed to get contiguous memory allocation and
                // the caller does not require one let us use map-based allocation
                // which we do after the loop below
                break;
            }

            return nullptr;
        }
    }

    // We are deliberately executing this code here because doing it
    // in WITH_LOCK section above, would likely lead to a deadlock,
    // as map_anon() eventually would be pulling memory from free_page_ranges
    // to satisfy the request and even worse this method might get
    // called recursively.
    void* obj = mapped_malloc_large(size, offset);
    trace_memory_malloc_large(obj, requested_size, size, alignment);
    return obj;
}

// Return a page range back to free_page_ranges. Note how the size of the
// page range is range->size, but its start is at range itself.
static void free_page_range_locked(page_range *range)
{
    on_free(range->size);
    free_page_ranges.free(range);
}

// Return a page range back to free_page_ranges. Note how the size of the
// page range is range->size, but its start is at range itself.
static void free_page_range(page_range *range)
{
    WITH_LOCK(free_page_ranges_lock) {
        free_page_range_locked(range);
    }
}

static void free_page_range(void *addr, size_t size)
{
    new (addr) page_range(size);
    free_page_range(static_cast<page_range*>(addr));
}

static void free_large(void* obj)
{
    obj = align_down(static_cast<char*>(obj) - 1, page_size);
    free_page_range(static_cast<page_range*>(obj));
}

static size_t large_object_offset(void *&obj)
{
    void *original_obj = obj;
    obj = align_down(static_cast<char*>(obj) - 1, page_size);
    return reinterpret_cast<uint64_t>(original_obj) - reinterpret_cast<uint64_t>(obj);
}

static size_t large_object_size(void *obj)
{
    size_t offset = large_object_offset(obj);
    auto header = static_cast<page_range*>(obj);
    return header->size - offset;
}


static void* early_alloc_page()
{
    WITH_LOCK(free_page_ranges_lock) {
        on_alloc(page_size);
        return static_cast<void*>(free_page_ranges.alloc(page_size));
    }
}

static void early_free_page(void* v)
{
    auto pr = new (v) page_range(page_size);
    free_page_range(pr);
}
//
// Following variables and functions are used to implement simple
// early (pre-SMP) memory allocation scheme.
mutex early_alloc_lock;
// early_object_pages holds a pointer to the beginning of the current page
// intended to be used for next early object allocation
static char* early_object_page = nullptr;
// early_alloc_next_offset points to the 0-relative address of free
// memory within a page pointed by early_object_page. Normally it is an
// offset of the first byte right after last byte of the previously
// allocated object in current page. Typically it is NOT an offset
// of next object to be allocated as we need to account for proper
// alignment and space for 2-bytes size field preceding every
// allocated object.
static size_t early_alloc_next_offset = 0;
static size_t early_alloc_previous_offset = 0;

static early_page_header* to_early_page_header(void* object)
{
    return reinterpret_cast<early_page_header*>(
            reinterpret_cast<std::uintptr_t>(object) & ~(page_size - 1));
}

static void setup_early_alloc_page() {
    early_object_page = static_cast<char*>(early_alloc_page());
    early_page_header *page_header = to_early_page_header(early_object_page);
    // Set the owner field to null so that functions that free objects
    // or compute object size can differentiate between post-SMP malloc pool
    // and early (pre-SMP) allocation
    page_header->owner = nullptr;
    page_header->allocations_count = 0;
    early_alloc_next_offset = sizeof(early_page_header);
}

static bool will_fit_in_early_alloc_page(size_t size, size_t alignment)
{
    auto lowest_offset = align_up(sizeof(early_page_header) + sizeof(unsigned short), alignment);
    return lowest_offset + size <= page_size;
}

//
// This function implements simple but effective scheme
// of allocating objects of size < 4096 before SMP is setup. It does so by
// remembering where within current page free memory starts. Then it
// calculates next closest offset matching specified alignment
// and verifies there is enough space until end of the current
// page to allocate from. If not it allocates next full page
// to find enough requested space.
static void* early_alloc_object(size_t size, size_t alignment)
{
    WITH_LOCK(early_alloc_lock) {
        if (!early_object_page) {
            setup_early_alloc_page();
        }

        // Each object is preceded by 2 bytes (unsigned short) of size
        // so make sure there is enough room between new object and previous one
        size_t offset = align_up(early_alloc_next_offset + sizeof(unsigned short), alignment);

        if (offset + size > page_size) {
            setup_early_alloc_page();
            offset = align_up(early_alloc_next_offset + sizeof(unsigned short), alignment);
        }

        // Verify we have enough space to satisfy this allocation
        assert(offset + size <= page_size);

        auto ret = early_object_page + offset;
        early_alloc_previous_offset = early_alloc_next_offset;
        early_alloc_next_offset = offset + size;

        // Save size of the allocated object 2 bytes before it address
        *reinterpret_cast<unsigned short *>(ret - sizeof(unsigned short)) =
                static_cast<unsigned short>(size);
        to_early_page_header(early_object_page)->allocations_count++;
        return ret;
    }
}

//
// This function fairly rarely actually frees previously
// allocated memory. It does so only if all objects
// have been freed in a page based on allocations_count or
// if the object being freed is the last one that was allocated.
static void early_free_object(void *object)
{
    WITH_LOCK(early_alloc_lock) {
        early_page_header *page_header = to_early_page_header(object);
        assert(!page_header->owner);
        unsigned short *size_addr = reinterpret_cast<unsigned short*>(static_cast<char*>(object) - sizeof(unsigned short));
        unsigned short size = *size_addr;
        if (!size) {
            return;
        }

        *size_addr = 0; // Reset size to 0 so that we know this object was freed and prevent from freeing again
        page_header->allocations_count--;
        if (page_header->allocations_count <= 0) { // Any early page
            early_free_page(page_header);
            if (early_object_page == reinterpret_cast<char*>(page_header)) {
                early_object_page = nullptr;
            }
        }
        else if(early_object_page == reinterpret_cast<char*>(page_header)) { // Current early page
            // Assuming we are freeing the object that was the last one allocated,
            // simply subtract its size from the early_alloc_next_offset to arrive at the previous
            // value of early_alloc_next_offset it was when allocating last object
            void *last_obj = reinterpret_cast<char*>(page_header) + (early_alloc_next_offset - size);
            // Check if we are freeing last allocated object (free followed by malloc)
            // and deallocate if so by moving the early_alloc_next_offset to the previous
            // position
            if (last_obj == object) {
                early_alloc_next_offset = early_alloc_previous_offset;
            }
        }
    }
}

static size_t early_object_size(void* v)
{
    return *reinterpret_cast<unsigned short*>(static_cast<char*>(v) - sizeof(unsigned short));
}

static void* untracked_alloc_page()
{
    void* ret;

    if (!smp_allocator) {
        ret = early_alloc_page();
    } else {
        oom();  
    }
    trace_memory_page_alloc(ret);
    return ret;
}

void* alloc_page()
{
    void *p = untracked_alloc_page();
#if CONF_memory_tracker
    tracker_remember(p, page_size);
#endif
    return p;
}

static inline void untracked_free_page(void *v)
{
    trace_memory_page_free(v);
    if (!smp_allocator) {
        return early_free_page(v);
    }
}

void free_page(void* v)
{
    untracked_free_page(v);
#if CONF_memory_tracker
    tracker_forget(v);
#endif
}

/* Allocate a huge page of a given size N (which must be a power of two)
 * N bytes of contiguous physical memory whose address is a multiple of N.
 * Memory allocated with alloc_huge_page() must be freed with free_huge_page(),
 * not free(), as the memory is not preceded by a header.
 */
void* alloc_huge_page(size_t N)
{
    WITH_LOCK(free_page_ranges_lock) {
        auto pr = free_page_ranges.alloc_aligned(N, 0, N, true);
        if (pr) {
            on_alloc(N);
            return static_cast<void*>(pr);
            // TODO: consider using tracker.remember() for each one of the small
            // pages allocated. However, this would be inefficient, and since we
            // only use alloc_huge_page in one place, maybe not worth it.
        }
        return nullptr;
    }
}

void free_huge_page(void* v, size_t N)
{
    free_page_range(v, N);
}

void free_initial_memory_range(void* addr, size_t size)
{
    if (!size) {
        return;
    }
    auto a = reinterpret_cast<uintptr_t>(addr);
    auto delta = align_up(a, page_size) - a;
    if (delta > size) {
        return;
    }
    addr = static_cast<char*>(addr) + delta;
    size -= delta;
    size = align_down(size, page_size);
    if (!size) {
        return;
    }

    on_new_memory(size);

    on_free(size);

    auto pr = new (addr) page_range(size);
    free_page_ranges.initial_add(pr);
}

void  __attribute__((constructor(init_prio::mempool))) setup()
{
    arch_setup_free_memory();
}

}

extern "C" {
    void* malloc(size_t size);
    void free(void* object);
    size_t malloc_usable_size(void *object);
}

static inline void* std_malloc(size_t size, size_t alignment)
{
    if ((ssize_t)size < 0)
        return libc_error_ptr<void *>(ENOMEM);
    void *ret;
    size_t minimum_size = std::max(size, memory::pool::min_object_size);
    if (smp_allocator && size <= memory::pool::max_object_size && alignment <= minimum_size) {
        unsigned n = ilog2_roundup(minimum_size);
        ret = memory::malloc_pools[n].alloc();
        ret = translate_mem_area(mmu::mem_area::main, mmu::mem_area::mempool,
                                 ret);
        trace_memory_malloc_mempool(ret, size, 1 << n, alignment);
    } else if (smp_allocator && alignment <= memory::pool::max_object_size && minimum_size <= alignment) {
        unsigned n = ilog2_roundup(alignment);
        ret = memory::malloc_pools[n].alloc();
        ret = translate_mem_area(mmu::mem_area::main, mmu::mem_area::mempool,
                                 ret);
        trace_memory_malloc_mempool(ret, size, 1 << n, alignment);
    } else if (!smp_allocator && memory::will_fit_in_early_alloc_page(size,alignment)) {
        ret = memory::early_alloc_object(size, alignment);
        ret = translate_mem_area(mmu::mem_area::main, mmu::mem_area::mempool,
                                 ret);
    } else if (minimum_size <= mmu::page_size && alignment <= mmu::page_size) {
        ret = mmu::translate_mem_area(mmu::mem_area::main, mmu::mem_area::page,
                                       memory::alloc_page());
        trace_memory_malloc_page(ret, size, mmu::page_size, alignment);
    } else {
        ret = memory::malloc_large(size, alignment, false);
    }
#if CONF_memory_tracker
    memory::tracker_remember(ret, size);
#endif
    return ret;
}

void* calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return malloc(0);
    if (nmemb > std::numeric_limits<size_t>::max() / size)
        return nullptr;
    auto n = nmemb * size;
    auto p = malloc(n);
    if (!p)
        return nullptr;
    memset(p, 0, n);
    return p;
}

static size_t object_size(void *object)
{
    if (!mmu::is_linear_mapped(object, 0)) {
        size_t offset = memory::large_object_offset(object);
        size_t* ret_header = static_cast<size_t*>(object);
        return *ret_header - offset;
    }

    switch (mmu::get_mem_area(object)) {
    case mmu::mem_area::main:
        return memory::large_object_size(object);
    case mmu::mem_area::mempool:
        object = mmu::translate_mem_area(mmu::mem_area::mempool,
                                         mmu::mem_area::main, object);
        {
            auto pool = memory::pool::from_object(object);
            if (pool)
                return pool->get_size();
            else
                return memory::early_object_size(object);
        }
    case mmu::mem_area::page:
        return mmu::page_size;
    case mmu::mem_area::debug:
        return dbg::object_size(object);
    default:
        abort();
    }
}

static inline void* std_realloc(void* object, size_t size)
{
    if (!object)
        return malloc(size);
    if (!size) {
        free(object);
        return nullptr;
    }

    size_t old_size = object_size(object);
    size_t copy_size = size > old_size ? old_size : size;
    void* ptr = malloc(size);
    if (ptr) {
        memcpy(ptr, object, copy_size);
        free(object);
    }

    return ptr;
}

void free(void* object)
{
    trace_memory_free(object);
    if (!object) {
        return;
    }
#if CONF_memory_tracker
    memory::tracker_forget(object);
#endif

    if (!mmu::is_linear_mapped(object, 0)) {
        return memory::mapped_free_large(object);
    }

    switch (mmu::get_mem_area(object)) {
    case mmu::mem_area::page:
        object = mmu::translate_mem_area(mmu::mem_area::page,
                                         mmu::mem_area::main, object);
        return memory::free_page(object);
    case mmu::mem_area::main:
         return memory::free_large(object);
    case mmu::mem_area::mempool:
        object = mmu::translate_mem_area(mmu::mem_area::mempool,
                                         mmu::mem_area::main, object);
        {
            auto pool = memory::pool::from_object(object);
            if (pool)
                return pool->free(object);
            else
                return memory::early_free_object(object);
        }
    case mmu::mem_area::debug:
        return dbg::free(object);
    default:
        abort();
    }
}

namespace dbg {

// debug allocator - give each allocation a new virtual range, so that
// any use-after-free will fault.

bool enabled;

using mmu::debug_base;
// FIXME: we assume the debug memory space is infinite (which it nearly is)
// and don't reuse space
std::atomic<char*> free_area{debug_base};
struct header {
    explicit header(size_t sz) : size(sz), size2(sz) {
        memset(fence, '$', sizeof fence);
    }
    ~header() {
        assert(size == size2);
        assert(std::all_of(fence, std::end(fence), [=](char c) { return c == '$'; }));
    }
    size_t size;
    char fence[16];
    size_t size2;
};
static const size_t pad_before = 2 * mmu::page_size;
static const size_t pad_after = mmu::page_size;

static __thread bool recursed;

void* malloc(size_t size, size_t alignment)
{
    if (!enabled || recursed) {
        return std_malloc(size, alignment);
    }

    recursed = true;
    auto unrecurse = defer([&] { recursed = false; });

    // there will be multiple allocations needed to satisfy this allocation; request
    // access to the emergency pool to avoid us holding some lock and then waiting
    // in an internal allocation
    auto asize = align_up(size, mmu::page_size);
    auto padded_size = pad_before + asize + pad_after;
    if (alignment > mmu::page_size) {
        // Our allocations are page-aligned - might need more
        padded_size += alignment - mmu::page_size;
    }
    char* v = free_area.fetch_add(padded_size, std::memory_order_relaxed);
    // change v so that (v + pad_before) is aligned.
    v += align_up(v + pad_before, alignment) - (v + pad_before);
    mmu::vpopulate(v, mmu::page_size);
    new (v) header(size);
    v += pad_before;
    mmu::vpopulate(v, asize);
    memset(v + size, '$', asize - size);
    // fill the memory with garbage, to catch use-before-init
    uint8_t garbage = 3;
    std::generate_n(v, size, [&] { return garbage++; });
    return v;
}

void free(void* v)
{
    assert(!recursed);
    recursed = true;
    auto unrecurse = defer([&] { recursed = false; });
    auto h = reinterpret_cast<header*>(static_cast<char*>(v) - pad_before);
    auto size = h->size;
    auto asize = align_up(size, mmu::page_size);
    char* vv = reinterpret_cast<char*>(v);
    assert(std::all_of(vv + size, vv + asize, [=](char c) { return c == '$'; }));
    h->~header();
    mmu::vdepopulate(h, mmu::page_size);
    mmu::vdepopulate(v, asize);
    mmu::vcleanup(h, pad_before + asize);
}

static inline size_t object_size(void* v)
{
    return reinterpret_cast<header*>(static_cast<char*>(v) - pad_before)->size;
}

}

void* malloc(size_t size)
{
    static_assert(alignof(max_align_t) >= 2 * sizeof(size_t),
                  "alignof(max_align_t) smaller than glibc alignment guarantee");
    auto alignment = alignof(max_align_t);
    if (alignment > size) {
        alignment = 1ul << ilog2_roundup(size);
    }
#if CONF_memory_debug == 0
    void* buf = std_malloc(size, alignment);
#else
    void* buf = dbg::malloc(size, alignment);
#endif

    trace_memory_malloc(buf, size, alignment);
    return buf;
}

OSV_LIBC_API
void* realloc(void* obj, size_t size)
{
    void* buf = std_realloc(obj, size);
    trace_memory_realloc(obj, size, buf);
    return buf;
}

extern "C" OSV_LIBC_API
void *reallocarray(void *ptr, size_t nmemb, size_t elem_size)
{
    size_t bytes;
    if (__builtin_mul_overflow(nmemb, elem_size, &bytes)) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(ptr, nmemb * elem_size);
}

OSV_LIBC_API
size_t malloc_usable_size(void* obj)
{
    if ( obj == nullptr ) {
        return 0;
    }
    return object_size(obj);
}

// posix_memalign() and C11's aligned_alloc() return an aligned memory block
// that can be freed with an ordinary free().

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    // posix_memalign() but not aligned_alloc() adds an additional requirement
    // that alignment is a multiple of sizeof(void*). We don't verify this
    // requirement, and rather always return memory which is aligned at least
    // to sizeof(void*), even if lesser alignment is requested.
    if (!is_power_of_two(alignment)) {
        return EINVAL;
    }
#if CONF_memory_debug == 0
    void* ret = std_malloc(size, alignment);
#else
    void* ret = dbg::malloc(size, alignment);
#endif
    trace_memory_malloc(ret, size, alignment);
    if (!ret) {
        return ENOMEM;
    }
    // Until we have a full implementation, just croak if we didn't get
    // the desired alignment.
    assert (!(reinterpret_cast<uintptr_t>(ret) & (alignment - 1)));
    *memptr = ret;
    return 0;

}

void *aligned_alloc(size_t alignment, size_t size)
{
    void *ret;
    int error = posix_memalign(&ret, alignment, size);
    if (error) {
        errno = error;
        return NULL;
    }
    return ret;
}

// memalign() is an older variant of aligned_alloc(), which does not require
// that size be a multiple of alignment.
// memalign() is considered to be an obsolete SunOS-ism, but Linux's glibc
// supports it, and some applications still use it.
OSV_LIBC_API
void *memalign(size_t alignment, size_t size)
{
    return aligned_alloc(alignment, size);
}

namespace memory {

void enable_debug_allocator()
{
    dbg::enabled = true;
}

void* alloc_phys_contiguous_aligned(size_t size, size_t align)
{
    assert(is_power_of_two(align));
    // make use of the standard large allocator returning properly aligned
    // physically contiguous memory:
    auto ret = malloc_large(size, align, true);
    assert (!(reinterpret_cast<uintptr_t>(ret) & (align - 1)));
    return ret;
}

void free_phys_contiguous_aligned(void* p)
{
    free_large(p);
}

}
