#pragma once
// -------------------------------------------------------------------------------------
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include "Exceptions.hpp"
#include "leanstore/io/IoRequest.hpp"
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
/**
 * Thread-safe implementation of RequestStack
 */
template <typename R>
class RequestStackLock
{
public:
    std::unique_ptr<R[]> requests;
    std::unique_ptr<R*[]> free_stack;
    std::unique_ptr<R*[]> submit_stack;
    
    const int max_entries;
    std::atomic<int> free;
    std::atomic<int> pushed = {0};
    std::atomic<int> outstanding_cnt = {0};

    // Mutexes for thread-safety
    std::mutex free_stack_mutex;
    std::mutex submit_stack_mutex;
    std::mutex outstanding_mutex;
    
    RequestStackLock(int max_entries) : max_entries(max_entries), free(max_entries)
    {
        requests = std::make_unique<R[]>(max_entries);
        free_stack = std::make_unique<R*[]>(max_entries);
        submit_stack = std::make_unique<R*[]>(max_entries);
        for (int i = 0; i < max_entries; i++) {
            free_stack[i] = &requests[i];
        }
    };
    
    ~RequestStackLock() {}
    
    int outstanding()
    {
        assert(max_entries - free.load() - pushed.load() == outstanding_cnt.load());
        return max_entries - free.load() - pushed.load();
    }
    
    int submitStackSize() {
        return pushed.load();
    }
    
    bool full() {
        return free.load() == 0;
    }
    
    /* free -> to user (untracked)*/
    bool popFromFreeStack(R*& out)
    {
        std::lock_guard<std::mutex> lock(free_stack_mutex);
        
        assert(free >= 0);
        if (free == 0) {
            return false;
        }
        free--;
        out = free_stack[free];
        return true;
    }
    
    /* user -> to submit */
    void pushToSubmitStack(R* req)
    {
        std::lock_guard<std::mutex> lock(submit_stack_mutex);
        
        submit_stack[pushed] = req;
        pushed++;
    }
    
    /* free -> submit / direct path (not like popFromFree and pushToSubmit ) */
    bool moveFreeToSubmitStack(R*& out)
    {
        // Need to lock both stacks to ensure atomic operation
        std::lock(free_stack_mutex, submit_stack_mutex);
        std::lock_guard<std::mutex> free_lock(free_stack_mutex, std::adopt_lock);
        std::lock_guard<std::mutex> submit_lock(submit_stack_mutex, std::adopt_lock);
        
        assert(free >= 0);
        if (free == 0) {
            return false;
        }
        free--;
        assert(free < max_entries);
        out = free_stack[free];
        assert(pushed < max_entries);
        submit_stack[pushed] = out;
        pushed++;
        return true;
    }
    
    /* submit -> outstanding */
    bool popFromSubmitStack(R*& out)
    {
        std::lock(submit_stack_mutex, outstanding_mutex);
        std::lock_guard<std::mutex> submit_lock(submit_stack_mutex, std::adopt_lock);
        std::lock_guard<std::mutex> outstanding_lock(outstanding_mutex, std::adopt_lock);
        
        if (pushed <= 0) {
            return false;
        }
        pushed--;
        out = submit_stack[pushed];
        
        outstanding_cnt++; 

        return true;
    }
    
    /* outstanding -> free */
    void returnToFreeList(R* ptr)
    {
        std::lock(free_stack_mutex, outstanding_mutex);
        std::lock_guard<std::mutex> free_lock(free_stack_mutex, std::adopt_lock);
        std::lock_guard<std::mutex> outstanding_lock(outstanding_mutex, std::adopt_lock);
        
        outstanding_cnt--;         
        free_stack[free] = ptr;
        free++;
    }
};
// -------------------------------------------------------------------------------------
} // namespace mean
// -------------------------------------------------------------------------------------