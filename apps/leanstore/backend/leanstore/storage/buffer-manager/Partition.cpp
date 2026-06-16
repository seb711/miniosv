#include "Partition.hpp"

#include "leanstore/utils/Misc.hpp"
#include <osv/leanstore_debug.hh>
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
#include <sys/mman.h>

#include <cstring>
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
// -------------------------------------------------------------------------------------
void* malloc_huge(size_t size)
{
   void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   madvise(p, size, MADV_HUGEPAGE);
   memset(p, 0, size);
   return p;
}// -------------------------------------------------------------------------------------
HashTable::Entry::Entry(PID key) : key(key), next(nullptr) {}
HashTable::Entry::Entry() : key(0), next(nullptr) {}
// -------------------------------------------------------------------------------------
HashTable::HashTable(u64 sizeInBits) : alloc_stack(1ull << sizeInBits)
{
   uint64_t size = (1ull << sizeInBits);
   mask = size - 1;
   entries = (Entry**)malloc_huge(size * sizeof(Entry*));
}
// -------------------------------------------------------------------------------------
u64 HashTable::hashKey(PID k)
{
   // MurmurHash64A
   const uint64_t m = 0xc6a4a7935bd1e995ull;
   const int r = 47;
   uint64_t h = 0x8445d61a4e774912ull ^ (8 * m);
   k *= m;
   k ^= k >> r;
   k *= m;
   h ^= k;
   h *= m;
   h ^= h >> r;
   h *= m;
   h ^= h >> r;
   return h;
}
// -------------------------------------------------------------------------------------
IOFrame& HashTable::insert(PID key)
{
   auto e = alloc_stack.pop();
   e->key = key;
   uint64_t pos = hashKey(key) & mask;
   e->next = entries[pos];
   entries[pos] = e;
   
   // Assert no self-loop after insertion
   assert(e->next != e && "Self-loop detected: entry points to itself");
   
   return e->value;
}
// -------------------------------------------------------------------------------------
HashTable::Handler HashTable::lookup(PID key)
{
   uint64_t pos = hashKey(key) & mask;
   Entry** e_ptr = entries + pos;
   Entry* e = *e_ptr;
   
   while (e) {
      // Assert no self-loop before processing
      assert(e->next != e && "Self-loop detected in linked list");
      
      if (e->key == key)
         return {e_ptr};
      
      e_ptr = &(e->next);
      e = e->next;
   }
   return {nullptr};
}
// -------------------------------------------------------------------------------------
void HashTable::remove(HashTable::Handler& handler)
{
   Entry* to_delete = *handler.holder;
   assert(to_delete->next != to_delete && "Self-loop detected on entry to be removed");
   
   *handler.holder = to_delete->next;
   to_delete->next = nullptr;  // Clear before returning to pool
   alloc_stack.ret(to_delete);
}
// -------------------------------------------------------------------------------------
void HashTable::remove(u64 key)
{
   auto handler = lookup(key);
   if (handler.holder == nullptr) {
      // already deleted
      return; 
   }
   assert(handler);
   remove(handler);
}
// -------------------------------------------------------------------------------------
bool HashTable::has(u64 key)
{
   uint64_t pos = hashKey(key) & mask;
   auto e = entries[pos];
   
   while (e) {
      // Assert no self-loop during traversal
      assert(e->next != e && "Self-loop detected in linked list");
      
      if (e->key == key)
         return true;
      
      e = e->next;
   }
   return false;
}

// -------------------------------------------------------------------------------------
}  // namespace storage
}  // namespace leanstore