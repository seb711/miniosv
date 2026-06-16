#include "ContextPool.hpp"
#include <stdexcept>
#include <cstdlib>

namespace mean {

// MemoryPool implementation
template<size_t BlockSize>
MemoryPool<BlockSize>::MemoryPool(size_t capacity) : capacity_(capacity) {
    free_list_.reserve(capacity);
    all_blocks_.reserve(capacity);
    for (size_t i = 0; i < capacity; i++) {
        void* block = aligned_alloc(64, BlockSize);
        if (!block) throw std::bad_alloc();
        all_blocks_.push_back(block);
        free_list_.push_back(block);
    }
}

template<size_t BlockSize>
MemoryPool<BlockSize>::~MemoryPool() {
    for (void* block : all_blocks_) {
        free(block);
    }
}

template<size_t BlockSize>
void* MemoryPool<BlockSize>::allocate() {
    if (free_list_.empty()) throw std::runtime_error("MemoryPool exhausted");
    void* block = free_list_.back();
    free_list_.pop_back();
    return block;
}

template<size_t BlockSize>
void MemoryPool<BlockSize>::deallocate(void* ptr) {
    if (!ptr) return;
    free_list_.push_back(ptr);
}

// Explicit template instantiations
template class MemoryPool<sizeof(UniqueTask)>;

// TaskContextPool implementation
TaskContextPool::TaskContextPool(size_t capacity)
    : 
      task_pool_(capacity) {}

UniqueTask* TaskContextPool::allocate(TaskFunction* fun) {
    auto task = (UniqueTask*) task_pool_.allocate(); 

    new (task) UniqueTask(fun); 

    return task; 
}

void TaskContextPool::deallocate(UniqueTask* task) {
    task_pool_.deallocate(task); 
}

size_t TaskContextPool::available() const {
    return std::min({task_pool_.available()});
}

} // namespace mean