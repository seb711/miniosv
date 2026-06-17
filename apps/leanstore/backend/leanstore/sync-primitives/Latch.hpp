#pragma once
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/concurrency/utils/YieldLock.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
// -------------------------------------------------------------------------------------
#include "JumpMU.hpp"
#include "leanstore/utils/UserJumpReasons.hpp"
// -------------------------------------------------------------------------------------
#ifdef __x86_64__
#include <emmintrin.h>
#define MYPAUSE() _mm_pause()
#endif
#ifdef __aarch64__
/* no arm_acle.h: gcc's copy (in the cross sysroot include path) uses
 * gcc-only builtins clang rejects, and the yield needs no header */
#define MYPAUSE() asm("YIELD");
#endif
#include <unistd.h>

#include <atomic>
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
#define MAX_BACKOFF FLAGS_backoff  // FLAGS_x
#define BACKOFF_STRATEGIES()                                                                                                              \
   if (FLAGS_nopp && (jumpmu::user_jump_reason() == UserJumpReason::NoFreePages || jumpmu::user_jump_reason() == UserJumpReason::Lock)) { \
                     leanstore::WorkerCounters::myCounters().time_counter_2++;                                                                     \
      if (jumpmu::user_jump_reason() == UserJumpReason::NoFreePages) {   \
         mean::task::yield(mean::TaskState::ReadyNoFreePages);                                                                            \
      } else {                                                                                                                            \
         mean::task::yield(mean::TaskState::ReadyJumpLock);                                                                               \
      }                                                                                                                                   \
   } else {                                                                                                                               \
      leanstore::WorkerCounters::myCounters().time_counter_3++;                                                                           \
      if (FLAGS_backoff) {                                                                                                                \
         for (u64 i = utils::RandomGenerator::getRandU64(0, mask); i; --i) {                                                              \
            MYPAUSE();                                                                                                                    \
         }                                                                                                                                \
         mask = mask < MAX_BACKOFF ? mask << 1 : MAX_BACKOFF;                                                                             \
      }                                                                                                                                   \
   }
// -------------------------------------------------------------------------------------
struct RestartException {
  public:
   RestartException() {}
};
// -------------------------------------------------------------------------------------
constexpr static u64 LATCH_EXCLUSIVE_BIT = 1ull;
constexpr static u64 LATCH_VERSION_MASK = ~(0ull);
// -------------------------------------------------------------------------------------
using VersionType = atomic<u64>;
struct alignas(64) HybridLatch {
   VersionType version;
   mean::SharedYieldLock mutex;
   // -------------------------------------------------------------------------------------
   template <typename... Args>
   HybridLatch(Args&&... args) : version(std::forward<Args>(args)...)
   {
   }
   VersionType* operator->() { return &version; }
   // -------------------------------------------------------------------------------------
   VersionType* ptr() { return &version; }
   VersionType& ref() { return version; }
   // -------------------------------------------------------------------------------------
   void assertExclusivelyLatched() { assert(isExclusivelyLatched()); }
   void assertNotExclusivelyLatched() { assert(!isExclusivelyLatched()); }
   // -------------------------------------------------------------------------------------
   bool isExclusivelyLatched() const { return (version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT; }
};
static_assert(sizeof(HybridLatch) == 64, "");
// -------------------------------------------------------------------------------------
enum class GUARD_STATE { UNINITIALIZED, OPTIMISTIC, SHARED, EXCLUSIVE, MOVED };
enum class LATCH_FALLBACK_MODE : u8 { SHARED = 0, EXCLUSIVE = 1, JUMP = 2, SPIN = 3, SHOULD_NOT_HAPPEN = 4 };
// -------------------------------------------------------------------------------------
struct Guard {
   HybridLatch* latch = nullptr;
   GUARD_STATE state = GUARD_STATE::UNINITIALIZED;
   u64 version;
   bool faced_contention = false;
   // Prevent the compiler from reordering/caching the plain (racy) reads of the
   // protected node data across the optimistic version load and recheck. x86 has
   // no load-load reordering, so a compiler barrier is sufficient.
   static inline void compilerBarrier() { __asm__ __volatile__("" ::: "memory"); }
   // -------------------------------------------------------------------------------------
   Guard() : latch(nullptr) {}
   Guard(HybridLatch& latch) : latch(&latch) {}
   Guard(HybridLatch* latch) : latch(latch) {}
   // -------------------------------------------------------------------------------------
   Guard(HybridLatch& latch, GUARD_STATE state) : latch(&latch), state(state), version(latch.ref().load()) {}
   // -------------------------------------------------------------------------------------
   // Move
   Guard(Guard&& other) : latch(other.latch), state(other.state), version(other.version), faced_contention(other.faced_contention)
   {
      other.state = GUARD_STATE::MOVED;
   }
   Guard& operator=(Guard&& other)
   {
      unlock();
      // -------------------------------------------------------------------------------------
      latch = other.latch;
      state = other.state;
      version = other.version;
      faced_contention = other.faced_contention;
      // -------------------------------------------------------------------------------------
      other.state = GUARD_STATE::MOVED;
      // -------------------------------------------------------------------------------------
      return *this;
   }
   // -------------------------------------------------------------------------------------
   bool tryRecheck()
   {
      // Compiler barrier before re-reading the version: the optimistic section
      // read node data with plain (non-atomic) loads that race with the writer
      // holding the exclusive lock. Without this, clang may sink/cache those
      // reads across the version reload, defeating the validation (a data race,
      // i.e. UB -- gcc happens not to exploit it). On x86 a compiler barrier
      // suffices (no load-load reordering in hardware).
      compilerBarrier();
      // maybe only if state == optimistic
      assert(state == GUARD_STATE::OPTIMISTIC || version == latch->ref().load());
      if (state == GUARD_STATE::OPTIMISTIC && version != latch->ref().load()) {
         return false;
      }
      return true;
   }
   void recheck()
   {
      compilerBarrier();  // see tryRecheck()
      // maybe only if state == optimistic
      assert(state == GUARD_STATE::OPTIMISTIC || version == latch->ref().load());
      if (state == GUARD_STATE::OPTIMISTIC && version != latch->ref().load()) {
         jumpmu::jump();
      }
   }
   // -------------------------------------------------------------------------------------
   inline void unlock()
   {
      if (state == GUARD_STATE::EXCLUSIVE) {
         version += LATCH_EXCLUSIVE_BIT;
         latch->ref().store(version, std::memory_order_release);
         latch->mutex.unlock();
         state = GUARD_STATE::OPTIMISTIC;
      } else if (state == GUARD_STATE::SHARED) {
         latch->mutex.unlock_shared();
         state = GUARD_STATE::OPTIMISTIC;
      }
   }
   inline bool tryToOptimistic()
   {
      assert(state == GUARD_STATE::UNINITIALIZED && latch != nullptr && state != GUARD_STATE::MOVED);
      version = latch->ref().load();
      if ((version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT) {
         return false;
      } else {
         state = GUARD_STATE::OPTIMISTIC;
         compilerBarrier();  // pin the optimistic snapshot before plain data reads
         return true;
      }
   }
   // -------------------------------------------------------------------------------------
   inline void toOptimisticSpin()
   {
      assert(state == GUARD_STATE::UNINITIALIZED && latch != nullptr && state != GUARD_STATE::MOVED);
      version = latch->ref().load();
      if ((version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT) {
         faced_contention = true;
         do {
            version = latch->ref().load();
         } while ((version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT);
      }
      state = GUARD_STATE::OPTIMISTIC;
      compilerBarrier();  // pin the optimistic snapshot before plain data reads
   }
   inline void toOptimisticOrJump()
   {
      assert(state == GUARD_STATE::UNINITIALIZED && latch != nullptr && state != GUARD_STATE::MOVED);
      version = latch->ref().load();
      if ((version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT) {
         jumpmu::jump();
      } else {
         state = GUARD_STATE::OPTIMISTIC;
         compilerBarrier();  // pin the optimistic snapshot before plain data reads
      }
   }
   inline void toOptimisticOrShared()
   {
      assert(state == GUARD_STATE::UNINITIALIZED && latch != nullptr && state != GUARD_STATE::MOVED);
      version = latch->ref().load();
      if ((version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT) {
         latch->mutex.lock_shared();
         version = latch->ref().load();
         state = GUARD_STATE::SHARED;
         faced_contention = true;
      } else {
         state = GUARD_STATE::OPTIMISTIC;
      }
   }
   inline void toOptimisticOrExclusive()
   {
      assert(state == GUARD_STATE::UNINITIALIZED && latch != nullptr && state != GUARD_STATE::MOVED);
      version = latch->ref().load();
      if ((version & LATCH_EXCLUSIVE_BIT) == LATCH_EXCLUSIVE_BIT) {
         latch->mutex.lock();
         version = latch->ref().load() + LATCH_EXCLUSIVE_BIT;
         latch->ref().store(version, std::memory_order_release);
         state = GUARD_STATE::EXCLUSIVE;
         faced_contention = true;
      } else {
         state = GUARD_STATE::OPTIMISTIC;
      }
   }
   inline bool tryToExclusive()
   {
      assert(state != GUARD_STATE::SHARED && state != GUARD_STATE::EXCLUSIVE);
      /*
      if (state == GUARD_STATE::EXCLUSIVE)
         return false;
      */
      if (state == GUARD_STATE::OPTIMISTIC) {
         const u64 new_version = version + LATCH_EXCLUSIVE_BIT;
         u64 expected = version;
         if (!latch->mutex.try_lock()) {
            return false;
         }
         if (!latch->ref().compare_exchange_strong(expected, new_version)) {
            latch->mutex.unlock();
            return false;
         }
         version = new_version;
         state = GUARD_STATE::EXCLUSIVE;
      } else {
         if (!latch->mutex.try_lock()) {
            return false;
         }
         version = latch->ref().load() + LATCH_EXCLUSIVE_BIT;
         latch->ref().store(version, std::memory_order_release);
         state = GUARD_STATE::EXCLUSIVE;
      }
      return true;
   }
   inline void toExclusive()
   {
      assert(state != GUARD_STATE::SHARED);
      if (state == GUARD_STATE::EXCLUSIVE)
         return;
      if (state == GUARD_STATE::OPTIMISTIC) {
         const u64 new_version = version + LATCH_EXCLUSIVE_BIT;
         u64 expected = version;
         latch->mutex.lock();  // changed from try_lock because of possible retries b/c lots of readers
         if (!latch->ref().compare_exchange_strong(expected, new_version)) {
            latch->mutex.unlock();
            jumpmu::jump();
         }
         version = new_version;
         state = GUARD_STATE::EXCLUSIVE;
      } else {
         latch->mutex.lock();
         version = latch->ref().load() + LATCH_EXCLUSIVE_BIT;
         latch->ref().store(version, std::memory_order_release);
         state = GUARD_STATE::EXCLUSIVE;
      }
   }
   inline void toShared()
   {
      assert(state == GUARD_STATE::OPTIMISTIC || state == GUARD_STATE::SHARED);
      if (state == GUARD_STATE::SHARED)
         return;
      if (state == GUARD_STATE::OPTIMISTIC) {
         if (!latch->mutex.try_lock_shared()) {
            jumpmu::jump();
         }
         if (latch->ref().load() != version) {
            latch->mutex.unlock_shared();
            jumpmu::jump();
         }
         state = GUARD_STATE::SHARED;
      } else {
         latch->mutex.lock_shared();
         version = latch->ref().load();
         state = GUARD_STATE::SHARED;
      }
   }
};
// -------------------------------------------------------------------------------------
}  // namespace storage
}  // namespace leanstore