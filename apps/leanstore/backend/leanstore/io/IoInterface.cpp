#include "IoInterface.hpp"
#include <memory>
#include <stdexcept>
// -------------------------------------------------------------------------------------
// Only include the OSv backend; libaio/liburing/spdk/xnvme not available in OSv kernel.
#include "impl/OsvImpl.hpp"
// -------------------------------------------------------------------------------------
namespace mean
{
std::unique_ptr<RaidEnvironment> IoInterface::_instance = nullptr;
// -------------------------------------------------------------------------------------
RaidEnvironment& IoInterface::initInstance(IoOptions ioOptions)
{
#ifdef LEANSTORE_INCLUDE_OSV
   if (ioOptions.engine == "osv") {
      std::cout << "create osv iochannel" << std::endl; 
      _instance = std::unique_ptr<RaidEnvironment>(new RaidEnv<OsvEnv, OsvChannel, OsvIoReq>(ioOptions));
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
int IoInterface::channelCount() {
   return instance().channelCount();
}
IoChannel& IoInterface::getIoChannel(int channel) {
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
// -------------------------------------------------------------------------------------
/*
RaidChannelManger::RaidChannelManger(IoOptions options) : ioOptions(options) {
   if (ioOptions.engine == "auto") {
      if (ioOptions.path.find("traddr") != std::string::npos) {
         ioOptions.engine = "spdk";
      } else {
         ioOptions.engine = "libaio";
      }
   }
   // -------------------------------------------------------------------------------------
   if (ioOptions.engine == "spdk") {
      io_env = std::make_unique<SpdkEnv>();
   } else if (ioOptions.engine == "libaio") {
      io_env = std::unique_ptr<LibaioEnv>(new LibaioEnv());
   } else if (ioOptions.engine == "liburing") {
      io_env = std::unique_ptr<LiburingEnv>(new LiburingEnv());
#ifdef LEANSTORE_INCLUDE_XNVME
   } else if (ioOptions.engine == "xnvme") {
      io_env = std::unique_ptr<XnvmeEnv>(new XnvmeEnv());
#endif // LEANSTORE_INCLUDE_XNVME
   } else {
      throw std::logic_error("ioEngine does not exist: " + ioOptions.engine);
   }
   io_env->init(ioOptions);
}
IoChannel& RaidChannelManger::getIoChannel(int channel) {
   auto f = channels.find(channel);
   if (f == channels.end()) {
      // TODO If...
      auto ch = std::unique_ptr<IoChannel>(new Raid5Channel<LibaioEnv, LibaioChannel, LibaioIoRequest>(io_env->getIoChannel(channel)));
      channels[channel] = std::move(ch);
      return *channels[channel];
   }
   return *f->second;
}
int RaidChannelManger::channelCount() {
   return io_env->channelCount();
}
u64 RaidChannelManger::storageSize() {
   return io_env->storageSize(); // FIXME RAID dependent..
}
*/
}  // namespace mean
// -------------------------------------------------------------------------------------
