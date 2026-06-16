#ifndef OSV_LEANSTORE_H_
#define OSV_LEANSTORE_H_

// TODO: Provide a more high-level C++ API, see shrinker.
#include <atomic>
#include <functional> 

namespace leanstore_osv_debug
{
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
void trace_try_lock(void* id, void* pid); 
void trace_try_lock2(void* id, void* pid); 
void trace_wait_lock(int id, int pid); 
void trace_lock(void* id, void* pid); 
void trace_unlock(void* id, void* pid); 
void trace_wait(void* id, void* pid);
void trace_wait_unlock(int id, int pid); 
void trace_finish_transaction( int pid);

void trace_prealloc_pop(void* stack, void* entry, int idx, int stack_pos, int pop_count, int ret_count);
void trace_prealloc_ret(void* stack, void* entry, int idx, int stack_pos, int pop_count, int ret_count);
void trace_prealloc_error_invalid_ptr(void* stack, void* entry, int stack_pos, int pop_count, int ret_count);
void trace_prealloc_error_double_pop(void* stack, void* entry, int idx, int stack_pos, int pop_count, int ret_count);
void trace_prealloc_error_double_ret(void* stack, void* entry, int idx, int stack_pos, int pop_count, int ret_count);
void trace_prealloc_error_overflow(void* stack, void* entry, int stack_pos, int size, int pop_count, int ret_count);

void trace_leanstore_states(int iopoll, int iosubmit, int pageprovider, int nic);
void trace_leanstore_sched_comp(int id, double work_ratio, int freq);

void trace_interrupted(void* p, int counter);

void trace_bg_start(int job);
void trace_bg_end(int job);

void set_priority(double p);
void yield();
unsigned int get_cpu_id(); 

bool is_app(); 
void* get_interrupt_stack(); 
void set_interrupt_stack(void* stack, size_t size);
void disable_scheduler();
void create_watchdog(uint64_t time, int cpuid, int vec, std::atomic<uint64_t> &timestamp); 
void send_watchdog_ipi(int vec, int cpuid); 

#ifdef __cplusplus
} // temporarily close extern "C"
std::function<bool()> get_completion_queue_not_empty_ptr(void* qp);
extern "C" { // reopen extern "C"
#endif

volatile void* get_shared_memory(); 

class Waiter {
    public: 
    std::atomic<void*> thread; 

    Waiter(); 

    void wake(); 
    void wait() const; 
}; 

#ifdef __cplusplus
}
#endif

}
#endif
