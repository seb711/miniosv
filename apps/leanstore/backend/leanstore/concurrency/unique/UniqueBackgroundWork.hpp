#pragma once

#include <array>
#include <functional>
#include <memory>
#include "Units.hpp"
#include "UniqueTask.hpp"

namespace mean
{

// UniqueBackgroundWork (updated)
struct UniqueBackgroundWorkMeta {
    uint64_t timestamp = 0;
    uint16_t cfrequency = 100;
    uint32_t last_work = 0;
    uint32_t max_work = 8;
};

struct UniqueBackgroundWork {
    // Reordered to match initialization order in constructors
    UniqueTask* bg_task;
    TaskState linked_state;
    bool has_linked_state;
    std::unique_ptr<UniqueBackgroundWorkMeta> meta;
    
    UniqueBackgroundWork(UniqueTask* bgctx,
                        TaskState state, 
                        std::unique_ptr<UniqueBackgroundWorkMeta>&& meta)
        : bg_task(bgctx), linked_state(state), has_linked_state(true), meta(std::move(meta))
    {
    }
    
    UniqueBackgroundWork(UniqueTask* bgctx,
                        std::unique_ptr<UniqueBackgroundWorkMeta>&& meta)
        : bg_task(bgctx), linked_state(TaskState::New), has_linked_state(false), meta(std::move(meta))
    {
    }
};

}  // namespace mean