#pragma once
#include <Units.hpp>
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace utils
{
namespace threadlocal
{
template <class CountersClass, class CounterType, typename T = u64>
T sum(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, CounterType CountersClass::* c)
{
   T local_c = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (counters[t]) {
         local_c += ((*counters[t]).*c).exchange(0);
      }
   }
   return local_c;
}
// -------------------------------------------------------------------------------------
template <class CountersClass, class CounterType, typename T = u64>
T sum(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, CounterType CountersClass::* c, u64 index)
{
   T local_c = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (counters[t]) {
         local_c += ((*counters[t]).*c)[index].exchange(0);
      }
   }
   return local_c;
}
// -------------------------------------------------------------------------------------
template <class CountersClass, class CounterType, typename T = u64>
T sum(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, CounterType CountersClass::* c, u64 row, u64 col)
{
   T local_c = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (counters[t]) {
         local_c += ((*counters[t]).*c)[row][col].exchange(0);
      }
   }
   return local_c;
}
// -------------------------------------------------------------------------------------
template <class CountersClass, class CounterType, typename T = u64>
T max(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, CounterType CountersClass::* c, u64 row)
{
   T local_c = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (counters[t]) {
         local_c = std::max<T>(((*counters[t]).*c)[row].exchange(0), local_c);
      }
   }
   return local_c;
}
template <class CountersClass, class CounterType, typename T = u64>
T thr_aggr_max(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, CounterType CountersClass::* c)
{
   T local_c = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (counters[t]) {
         local_c = std::max(local_c, ((*counters[t]).*c).exchange(0));
      }
   }

   return local_c;
}
template <class CountersClass, class CounterType, typename T = u64>
T thr_aggr_max(std::array<std::atomic<CountersClass*>, MAX_CORES>& counters, CounterType CountersClass::* c, u8 index)
{
   T local_c = 0;
   for (size_t t = 0; t < MAX_CORES; t++) {
      if (counters[t]) {
         local_c = std::max(local_c, ((*counters[t]).*c)[index].exchange(0));
      }
   }
   return local_c;
}
// -------------------------------------------------------------------------------------
}  // namespace threadlocal
}  // namespace utils
}  // namespace leanstore
