#ifndef SYSTEM_STATE_HPP
#define SYSTEM_STATE_HPP

#include <algorithm>
#include <cstdint>

#include "leanstore/concurrency/batch/Task.hpp"

namespace mean
{

class SystemState
{
  public:
   void increment(TaskState state) { counters[static_cast<uint8_t>(state)]++; }

   void decrement(TaskState state) { counters[static_cast<uint8_t>(state)]--; }

   uint32_t get(TaskState state) { return counters[static_cast<uint8_t>(state)]; }

  private:
   std::array<uint32_t, static_cast<uint8_t>(TaskState::LAST_ELEMENT)> counters = {0};
};
}  // namespace mean

#endif