#include "IoInterface.hpp"
#include <memory>
#include <stdexcept>
// -------------------------------------------------------------------------------------
// Only include the OSv backend; libaio/liburing/spdk/xnvme not available in OSv kernel.
#include "impl/OsvImpl.hpp"
// -------------------------------------------------------------------------------------
namespace mean
{
std::unique_ptr<RaidEnv<OsvEnv, OsvChannel, OsvIoReq>> IoInterface::_instance = nullptr;
// -------------------------------------------------------------------------------------
RaidEnvironment& IoInterface::initInstance(IoOptions ioOptions)
{
#ifdef LEANSTORE_INCLUDE_OSV
   if (ioOptions.engine == "osv") {
      std::cout << "create osv iochannel" << std::endl;
      _instance = std::unique_ptr<RaidEnv<OsvEnv, OsvChannel, OsvIoReq>>(new RaidEnv<OsvEnv, OsvChannel, OsvIoReq>(ioOptions));
   } else
#endif
   {
      throw std::runtime_error("IoInterface: unsupported engine in OSv build: " + ioOptions.engine);
   }
   return *_instance;
}
RaidEnvironment& IoInterface::instance()
{
   ensure(_instance.get(), "IoEnvironment not initialized.");
   return *_instance;
}
int IoInterface::channelCount()
{
   return instance().channelCount();
}
IoChannel& IoInterface::getIoChannel(int channel)
{
   return instance().getIoChannel(channel);
}
void* IoInterface::allocIoMemoryChecked(size_t size, size_t align)
{
   return instance().allocIoMemoryChecked(size, align);
}
void IoInterface::freeIoMemory(void* ptr, size_t size)
{
   instance().freeIoMemory(ptr, size);
}
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
