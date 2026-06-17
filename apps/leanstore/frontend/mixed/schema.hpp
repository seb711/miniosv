
#pragma once
#include "types.hpp"

struct item_t {
   static constexpr int id = 0;
   struct Key {
      static constexpr int id = 0;
      uint64_t i_id;
   };
   BytesPayload<120> i_data;

   template <class T>
   static unsigned foldRecord(uint8_t* out, const T& record)
   {
      unsigned pos = 0;
      pos += fold(out + pos, record.i_id);
      return pos;
   }
   template <class T>
   static unsigned unfoldRecord(const uint8_t* in, T& record)
   {
      unsigned pos = 0;
      pos += unfold(in + pos, record.i_id);
      return pos;
   }
   static constexpr unsigned maxFoldLength() { return 0 + sizeof(Key::i_id); };
};