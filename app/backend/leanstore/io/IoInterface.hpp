#pragma once

#include <unordered_map>
#include "IoAbstraction.hpp"
#include "leanstore/concurrency/utils/MessageHandler.hpp"
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
class IoInterface
{
   static std::unique_ptr<RaidEnvironment> _instance;
   IoInterface() = delete;
   IoInterface(const IoInterface&) = delete;

  public:
   static RaidEnvironment& initInstance(IoOptions ioOptions);  // TODO pretty ugly to have mm here
   static RaidEnvironment& instance();
   // -------------------------------------------------------------------------------------
   static int channelCount();
   static IoChannel& getIoChannel(int channel);
   static void* allocIoMemoryChecked(size_t size, size_t align);
   static void freeIoMemory(void* ptr, size_t size = 0);
};
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
