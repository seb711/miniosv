#pragma once
#include "BTreeNode.hpp"
// -------------------------------------------------------------------------------------
#include <cstring>
#include <cwchar>
#include <iosfwd>
#include <string>
#include <string_view>
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using namespace leanstore::storage;
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
namespace btree
{
enum class WAL_LOG_TYPE : u8 {
   WALInsert = 1,
   WALUpdate = 2,
   WALRemove = 3,
   WALAfterBeforeImage = 4,
   WALAfterImage = 5,
   WALLogicalSplit = 10,
   WALInitPage = 11
};
struct WALEntry {
   WAL_LOG_TYPE type;
};
// -------------------------------------------------------------------------------------
enum class OP_RESULT : u8 { OK = 0, NOT_FOUND = 1, DUPLICATE = 2, ABORT_TX = 3, NOT_ENOUGH_SPACE = 4, OTHER = 5, UNREACHABLE = 6 };
struct WALUpdateGenerator {
   void (*before)(u8* tuple, u8* entry);
   void (*after)(u8* tuple, u8* entry);
   u16 entry_size;
};
// -------------------------------------------------------------------------------------
// Interface
class BTreeInterface
{
  public:
   virtual OP_RESULT lookup(u8* key, u16 key_length, function<void(const u8*, u16)> payload_callback) = 0;
   virtual OP_RESULT insert(u8* key, u16 key_length, u8* value, u16 value_length) = 0;
   virtual OP_RESULT updateSameSize(u8* key, u16 key_length, function<void(u8* value, u16 value_size)>, WALUpdateGenerator = {{}, {}, 0}) = 0;
   virtual OP_RESULT remove(u8* key, u16 key_length) = 0;
   virtual OP_RESULT scanAsc(u8* start_key,
                             u16 key_length,
                             function<bool(const u8* key, u16 key_length, const u8* value, u16 value_length)>,
                             function<void()>) = 0;
   virtual OP_RESULT scanDesc(u8* start_key,
                              u16 key_length,
                              function<bool(const u8* key, u16 key_length, const u8* value, u16 value_length)>,
                              function<void()>) = 0;
   // -------------------------------------------------------------------------------------
   virtual u64 countPages() = 0;
   virtual u64 countEntries() = 0;
   virtual u64 getHeight() = 0;
};
// -------------------------------------------------------------------------------------
}  // namespace btree
}  // namespace storage
// libc++ (OSv's STL) only specializes std::char_traits for the standard
// character types, so std::basic_string_view<u8> / std::basic_string<u8> do not
// compile (libstdc++ provides a generic char_traits, which is why this built
// before). Provide a minimal char_traits-compatible type for u8 and use it.
struct u8_char_traits {
   using char_type = u8;
   using int_type = int;
   using off_type = std::streamoff;
   using pos_type = std::streampos;
   using state_type = std::mbstate_t;

   static constexpr void assign(char_type& a, const char_type& b) noexcept { a = b; }
   static constexpr bool eq(char_type a, char_type b) noexcept { return a == b; }
   static constexpr bool lt(char_type a, char_type b) noexcept { return a < b; }

   static constexpr int compare(const char_type* a, const char_type* b, size_t n) noexcept
   {
      for (size_t i = 0; i < n; ++i) {
         if (lt(a[i], b[i]))
            return -1;
         if (lt(b[i], a[i]))
            return 1;
      }
      return 0;
   }
   static constexpr size_t length(const char_type* s) noexcept
   {
      size_t n = 0;
      while (s[n] != char_type(0))
         ++n;
      return n;
   }
   static constexpr const char_type* find(const char_type* s, size_t n, const char_type& a) noexcept
   {
      for (size_t i = 0; i < n; ++i)
         if (eq(s[i], a))
            return s + i;
      return nullptr;
   }
   static char_type* move(char_type* dst, const char_type* src, size_t n) noexcept
   {
      return n == 0 ? dst : static_cast<char_type*>(std::memmove(dst, src, n));
   }
   static char_type* copy(char_type* dst, const char_type* src, size_t n) noexcept
   {
      return n == 0 ? dst : static_cast<char_type*>(std::memcpy(dst, src, n));
   }
   static char_type* assign(char_type* s, size_t n, char_type a) noexcept
   {
      for (size_t i = 0; i < n; ++i)
         s[i] = a;
      return s;
   }

   static constexpr int_type eof() noexcept { return -1; }
   static constexpr int_type not_eof(int_type c) noexcept { return c == eof() ? 0 : c; }
   static constexpr char_type to_char_type(int_type c) noexcept { return static_cast<char_type>(c); }
   static constexpr int_type to_int_type(char_type c) noexcept { return static_cast<int_type>(c); }
   static constexpr bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
};

using Slice = std::basic_string_view<u8, u8_char_traits>;
using StringU = std::basic_string<u8, u8_char_traits>;
struct MutableSlice {
   u8* ptr;
   u64 len;
   MutableSlice(u8* ptr, u64 len) : ptr(ptr), len(len) {}
   u64 length() { return len; }
   u8* data() { return ptr; }
};
}  // namespace leanstore
