#pragma once
// -------------------------------------------------------------------------------------
#include <stddef.h>
#include <stdint.h>

#include <cassert>
#include <cwchar>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
// -------------------------------------------------------------------------------------
// LeanStore's Slice type is std::basic_string_view<u8> (== unsigned char). GNU
// libstdc++ ships a non-standard char_traits<unsigned char> specialization that
// makes this work; LLVM libc++ does not, so basic_string_view<unsigned char>
// hits "implicit instantiation of undefined template 'char_traits<unsigned
// char>'". Provide the specialization here, in the base header included before
// any Slice is instantiated, so every TU agrees (no ODR mismatch).
namespace std
{
template <>
struct char_traits<unsigned char> {
   using char_type = unsigned char;
   using int_type = int;
   using off_type = streamoff;
   using pos_type = streampos;
   using state_type = mbstate_t;

   static constexpr void assign(char_type& a, const char_type& b) noexcept { a = b; }
   static constexpr bool eq(char_type a, char_type b) noexcept { return a == b; }
   static constexpr bool lt(char_type a, char_type b) noexcept { return a < b; }

   static constexpr int compare(const char_type* s1, const char_type* s2, size_t n) noexcept
   {
      for (; n; --n, ++s1, ++s2) {
         if (lt(*s1, *s2)) return -1;
         if (lt(*s2, *s1)) return 1;
      }
      return 0;
   }
   static constexpr size_t length(const char_type* s) noexcept
   {
      size_t len = 0;
      while (!eq(*s++, char_type(0))) ++len;
      return len;
   }
   static constexpr const char_type* find(const char_type* s, size_t n, const char_type& a) noexcept
   {
      for (; n; --n, ++s)
         if (eq(*s, a)) return s;
      return nullptr;
   }
   static char_type* move(char_type* s1, const char_type* s2, size_t n) noexcept
   {
      return n == 0 ? s1 : static_cast<char_type*>(__builtin_memmove(s1, s2, n));
   }
   static char_type* copy(char_type* s1, const char_type* s2, size_t n) noexcept
   {
      return n == 0 ? s1 : static_cast<char_type*>(__builtin_memcpy(s1, s2, n));
   }
   static char_type* assign(char_type* s, size_t n, char_type a) noexcept
   {
      char_type* r = s;
      for (; n; --n, ++s) assign(*s, a);
      return r;
   }
   static constexpr int_type not_eof(int_type c) noexcept { return eq_int_type(c, eof()) ? ~eof() : c; }
   static constexpr char_type to_char_type(int_type c) noexcept { return char_type(c); }
   static constexpr int_type to_int_type(char_type c) noexcept { return int_type(c); }
   static constexpr bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
   static constexpr int_type eof() noexcept { return int_type(-1); }
};
}  // namespace std
// -------------------------------------------------------------------------------------
// Build-mode selectors. These are always referenced via `#if <FLAG>` (value-based),
// so each must be defined with a numeric value (0/1). Provide a default of 0 here so
// every build system agrees: a build may override via -D<FLAG>=1, but must never pass
// a bare -D<FLAG> (which would leave `#if` without an expression).
#ifndef IS_PERIODIC
#define IS_PERIODIC 0
#endif
// -------------------------------------------------------------------------------------
using std::atomic;
using std::cerr;
using std::cout;
using std::endl;
using std::make_unique;
using std::string;
using std::to_string;
using std::tuple;
using std::unique_ptr;
// -------------------------------------------------------------------------------------
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using u128 = unsigned __int128;
// -------------------------------------------------------------------------------------
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
// -------------------------------------------------------------------------------------
using SIZE = size_t;
using PID = u64;
using LID = u64;   // Log ID
using TTS = u64;   // Transaction Time Stamp
using DTID = s64;  // Datastructure ID
// -------------------------------------------------------------------------------------
using TINYINT = s8;
using SMALLINT = s16;
using INTEGER = s32;
using UINTEGER = u32;
using DOUBLE = double;
using STRING = string;
using BITMAP = u8;
// -------------------------------------------------------------------------------------
using str = std::string_view;
// -------------------------------------------------------------------------------------
using BytesArray = std::unique_ptr<u8[]>;
// -------------------------------------------------------------------------------------
template <int s>
struct getTheSizeOf;
// -------------------------------------------------------------------------------------
constexpr u64 MSB = u64(1) << 63;
constexpr u64 MSB_MASK = ~(MSB);
constexpr u64 MSB2 = u64(1) << 62;
constexpr u64 MSB2_MASK = ~(MSB2);
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
constexpr double GIGA = 1000 * 1000 * 1000ll;
constexpr double MEGA = 1000 * 1000ll;
constexpr double KILO = 1000ll;
// -------------------------------------------------------------------------------------
constexpr u64 GIBI = 1024 * 1024 * 1024ll;
constexpr u64 MEBI = 1024 * 1024ll;
constexpr u64 KIBI = 1024ll;
// -------------------------------------------------------------------------------------
constexpr double MILLI = 1e-3;
constexpr double MICRO = 1e-6;
constexpr double NANO = 1e-9;
// -------------------------------------------------------------------------------------
constexpr u64 MAX_CORES = 12ll;
