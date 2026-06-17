#include <osv/leanstore_debug.hh>
#include "leanstore/LeanStore.hpp"
#include "types.hpp"

#define CYCLE 1000000
#define PROBABLITY 5000
#define LONGRUNNING 1
#define MAX_ENTRIES 134217728

class Workload
{
  public:
   std::atomic<unsigned> highest_inserted;
   std::atomic<unsigned> current_idx;

  private:
   utils::ScrambledZipfGenerator zipf_random;
   utils::MersenneTwister twister;
   LeanStoreAdapter<item_t>& kv_store;
   std::atomic<uint64_t> last_scan;

   void lookupTblRnd(uint64_t w_id)
   {
      uint64_t key = zipf_random.rand();
      BytesPayload<120> result;  /// FIXME remove this check
      kv_store.lookup1({w_id}, [&](const item_t& item) { result = item.i_data; });
   }
   // we need one easy look up
   void newIncrEntry(unsigned id)
   {
      BytesPayload<120> payload;
      utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(BytesPayload<120>));
      kv_store.insert({id}, {payload});
   }

   void updateRnd()
   {
      uint64_t key = zipf_random.rand() % (current_idx.load(std::memory_order_relaxed) & 0xffff0000);
      BytesPayload<120> payload;
      utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(BytesPayload<120>));

      kv_store.update1({key}, [&](item_t& item) { item.i_data = payload; }, WALUpdate1(item_t, i_data));
   }

   // we need one scan
   void scanSeqTbl()
   {
      unsigned curr = highest_inserted.load();

      kv_store.scanDesc(
          {highest_inserted},
          [&](const item_t::Key& key, const item_t& payload) {
             if (curr - key.i_id > CYCLE)
                return false;
             return true;
          },
          []() {});
   }

  public:
   Workload(LeanStoreAdapter<item_t>& kv_store)
       : highest_inserted(0),
         current_idx(0),
         zipf_random(0, MAX_ENTRIES, FLAGS_zipf_factor),
         twister(),
         kv_store(kv_store),
         last_scan(mean::readTSC()) {};

   int tx()
   {
      auto time = mean::readTSC();
      if (mean::tscDifferenceMs(mean::readTSC(), last_scan) > 200) {
         last_scan.store(time);
         scanSeqTbl();
         return 1;
      } else {
         int rnd = leanstore::utils::RandomGenerator::getRand(0, 100);

         if (rnd < 60) {
            newIncrEntry(highest_inserted.fetch_add(1));
         } else {
            updateRnd();
         }
         return 0;
      }
   }

   int insert(unsigned id)
   {
      newIncrEntry(id);
      return 0;
   }
};