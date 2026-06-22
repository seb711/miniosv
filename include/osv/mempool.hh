/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEMPOOL_HH
#define MEMPOOL_HH

#include <functional>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/list.hpp>
#include <osv/mutex.h>
#include <arch.hh>
#include <osv/pagealloc.hh>
#include <osv/percpu.hh>
#include <osv/condvar.h>
#include <osv/semaphore.hh>
#include <osv/mmu.hh>
#include <osv/contiguous_alloc.hh>
#include <osv/kernel_config.h>

extern "C" void thread_mark_emergency();

namespace memory {

const size_t page_size = 4096;

extern size_t phys_mem_size;

void setup_free_memory(void* start, size_t bytes);

namespace bi = boost::intrusive;

// Please note that early_page_header and pool:page_header
// structs have common 'owner' field. The owner field
// in early_page_header is always set to 'nullptr' and allows
// us to differentiate between pages used by early
// malloc and regular malloc pools
struct early_page_header {
    void* owner;
    unsigned short allocations_count;
};

struct free_object {
    free_object* next;
};

class pool {
public:
    explicit pool(unsigned size);
    ~pool();
    void* alloc();
    void free(void* object);
    unsigned get_size();
    static pool* from_object(void* object);
    static void collect_garbage();
private:
    struct page_header;
private:
    bool have_full_pages();
    void add_page();
    static page_header* to_header(free_object* object);

    // should get called with the preemption lock taken
    void free_same_cpu(free_object* obj, unsigned cpu_id);
    void free_different_cpu(free_object* obj, unsigned obj_cpu, unsigned cur_cpu);
private:
    unsigned _size;

    struct page_header {
        pool* owner;
        unsigned cpu_id;
        unsigned nalloc;
        bi::list_member_hook<> free_link;
        free_object* local_free;  // free objects in this page
    };

    typedef bi::list<page_header,
                     bi::member_hook<page_header,
                                     bi::list_member_hook<>,
                                     &page_header::free_link>,
                     bi::constant_time_size<false>
                    > free_list_base_type;
    class free_list_type : public free_list_base_type {
    public:
        ~free_list_type() { assert(empty()); }
    };
    // maintain a list of free pages percpu
    dynamic_percpu<free_list_type> _free;
public:
    static const size_t max_object_size;
    static const size_t min_object_size;
};

struct page_range {
    explicit page_range(size_t size);
    bool operator<(const page_range& pr) const {
        return size < pr.size;
    }
    size_t size;
    boost::intrusive::set_member_hook<> set_hook;
    boost::intrusive::list_member_hook<> list_hook;
};

void free_initial_memory_range(void* addr, size_t size);
void enable_debug_allocator();

const unsigned page_ranges_max_order = 16;

namespace stats {
    size_t free();
    size_t total();
    size_t max_no_reclaim();

    struct page_ranges_stats {
        struct {
            size_t bytes;
            size_t ranges_num;
        } order[page_ranges_max_order + 1];
    };

    void get_page_ranges_stats(page_ranges_stats &stats);

    struct pool_stats {
        size_t _max;
        size_t _nr;
        size_t _watermark_lo;
        size_t _watermark_hi;
    };

    void get_global_l2_stats(pool_stats &stats);
    void get_l1_stats(unsigned int cpu_id, stats::pool_stats &stats);
}

class phys_contiguous_memory final {
public:
    phys_contiguous_memory(size_t size, size_t align) {
        _va = alloc_phys_contiguous_aligned(size, align);
        if(!_va)
            throw std::bad_alloc();
        _pa = mmu::virt_to_phys(_va);
        _size = size;
    }

    ~phys_contiguous_memory() {
        free_phys_contiguous_aligned(_va);
    }

    void* get_va(void) const { return _va; }
    mmu::phys get_pa(void) const { return _pa; }
    size_t get_size(void) const { return _size; }

private:

    void *_va;
    mmu::phys _pa;
    size_t _size;
};

struct phys_deleter {
    void operator()(void* p) { free_phys_contiguous_aligned(p); }
};

template <typename T>
using phys_ptr = std::unique_ptr<T, memory::phys_deleter>;

template <typename T, size_t align, typename... Args>
inline
phys_ptr<T> make_phys_ptr(Args&&... args)
{
    static_assert(!std::is_array<T>::value, "use make_phys_array() to allocate arrays");
    void* ptr = memory::alloc_phys_contiguous_aligned(sizeof(T), align);
    // we can't put ptr into a phys_ptr<T> until it's fully constructed, otherwise
    // if the constructor throws, we'll run the destructor
    try {
        new (ptr) T(std::forward<Args>(args)...);
    } catch (...) {
        memory::free_phys_contiguous_aligned(ptr);
    }
    return phys_ptr<T>(static_cast<T*>(ptr));
}

template <typename T>
inline
phys_ptr<T[]> make_phys_array(size_t n, size_t align)
{
    // we have nowhere to store n, so we can't run any destructors
    static_assert(std::is_trivially_destructible<T>::value,
            "make_phys_ptr<T[]> must have a trivially destructible type");
    void* ptr = memory::alloc_phys_contiguous_aligned(sizeof(T) * n, align);
    // we can't put ptr into a phys_ptr<T> until it's fully constructed, otherwise
    // if the constructor throws, we'll run the destructor
    try {
        new (ptr) T[n];
    } catch (...) {
        memory::free_phys_contiguous_aligned(ptr);
    }
    return phys_ptr<T[]>(static_cast<T*>(ptr));
}

template <typename T>
inline
mmu::phys
virt_to_phys(const phys_ptr<T>& p)
{
    return mmu::virt_to_phys_dynamic_phys(p.get());
}
};

#endif
