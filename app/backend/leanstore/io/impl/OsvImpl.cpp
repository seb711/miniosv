#include "OsvImpl.hpp"
// -------------------------------------------------------------------------------------
#include <sys/mman.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
// -------------------------------------------------------------------------------------

namespace mean
{
// -------------------------------------------------------------------------------------
OsvEnv::~OsvEnv()
{
   controller.reset(nullptr);
   //  yes idk what we will do here
   std::cout << "OsvEnv deinit" << std::endl;
   OsvEnvironment::deinit();
}
// -------------------------------------------------------------------------------------
void OsvEnv::init(IoOptions options)
{
   OsvEnvironment::init();
   controller = std::make_unique<NVMeMultiController>();
   controller->connect("");

   // allocates for every NVMController queues
   controller->allocateQPairs();
   int qs = controller->qpairSize();
   printf("use %i channels %p\n", qs, &channels);
   for (int i = 0; i < qs; i++) {
      channels.push_back(std::make_unique<OsvChannel>(options, *controller, i));
   }
}

OsvChannel& OsvEnv::getIoChannel(int channel)
{
   printf("use channel size %lu\n", channels.size());
   printf("use %i channel %p\n", channel, &channels);

   ensure(channels.size() > 0, "don't forget to initizalize the io env first");
   ensure(channel < (int)channels.size(), "There are only " + std::to_string(channels.size()) + " channels available.");
   // std::cout << "getChannel: " << channel << std::endl << std::flush;
   printf("use channel %p\n", (void*)channels.at(channel).get());
   return *channels.at(channel);
}

int OsvEnv::channelCount()
{
   return controller->qpairSize();
}

u64 OsvEnv::storageSize()
{
   return controller->nsSize();
}

// mmap returns page-aligned memory, which already satisfies any alignment up to
// the page size (e.g. the 512-byte alignment leanstore requires for BufferFrame
// Pages). For larger alignments we over-allocate by `align` and round the base
// up. `align` must be a power of two. Returns nullptr on failure.
static void* alloc_io_mmap(size_t size, size_t align)
{
   assert(align != 0 && (align & (align - 1)) == 0);  // power of two
   const size_t page = 4096;
   const size_t map_size = (align <= page) ? size : size + align;
   void* buffer = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
   if (buffer == MAP_FAILED) {
      return nullptr;
   }
   if (align > page) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(buffer);
      buffer = reinterpret_cast<void*>((addr + align - 1) & ~(static_cast<uintptr_t>(align) - 1));
   }
   return buffer;
}

void* OsvEnv::allocIoMemory(size_t size, size_t align)
{
   void* buffer = alloc_io_mmap(size, align);
   null_check(buffer, "Memory allocation failed");
   assert((reinterpret_cast<uintptr_t>(buffer) & (align - 1)) == 0);
   // madvise(buffer, size, MADV_HUGEPAGE);
   memset(buffer, 0, size);
   return buffer;
}

void* OsvEnv::allocIoMemoryChecked(size_t size, size_t align)
{
   void* buffer = alloc_io_mmap(size, align);
   assert(buffer != nullptr);
   null_check(buffer, "Memory allocation failed");
   assert((reinterpret_cast<uintptr_t>(buffer) & (align - 1)) == 0);
   // madvise(buffer, size, MADV_HUGEPAGE);
   memset(buffer, 0, size);
   return buffer;
}

void OsvEnv::freeIoMemory(void* ptr, [[maybe_unused]] size_t size)
{
   // std::free(ptr);
}

int OsvEnv::deviceCount()
{
   return controller->controller.size();
}

DeviceInformation OsvEnv::getDeviceInfo()
{
   DeviceInformation d;

   d.devices.resize(controller->controller.size());
   for (size_t t = 0; t < controller->controller.size(); t++) {
      d.devices[t].id = 0;
   }

   return d;
}

// -------------------------------------------------------------------------------------
// Channel
// -------------------------------------------------------------------------------------
OsvChannel::OsvChannel(IoOptions ioOptions, NVMeMultiController& controller, int queue)
    : options(ioOptions), controller(controller), queue(queue), lbaSize(controller.nsLbaDataSize()), outstanding(controller.deviceCount())
{
   write_request_stack.reserve(ioOptions.iodepth);
   int c = controller.deviceCount();
   for (int i = 0; i < c; i++) {
      qpairs.emplace_back(controller.controller[i].qpairs[queue]);
   }
}

OsvChannel::~OsvChannel() {}

void OsvChannel::prepare_request(RaidRequest<OsvIoReq>* req, OsvIoReqCallback osvCb)
{
   // TODO: add the requests to osv and then just use them here
   // base
   switch (req->base.type) {
      case IoRequestType::Read:
         req->impl.type = OsvIoReqType::Read;
         break;
      case IoRequestType::Write:
         req->impl.type = OsvIoReqType::Write;
         break;
      default:
         throw std::logic_error("IoRequestType not supported" + std::to_string((int)req->base.type));
   }
   // TODO: use the things from the microbenchmark
   req->impl.buf = req->base.buffer();
   // NOTE: despite the field names, osv_nvme_nv_cmd_read/write take BYTE units
   // (addr/len) and the driver converts to LBA internally (core/nvme.cc ->
   // submit_request: slba = addr >> blockshift, nlb = len >> blockshift). So we
   // pass byte offset/length straight through here. Do NOT divide by lbaSize:
   // that double-converts (nlb -> 0) and the device rejects it as "Invalid Field
   // in Command" (sc=2). offset/len must still be block-aligned.
   assert(req->base.offset % lbaSize == 0);
   assert(req->base.len % lbaSize == 0);
   req->impl.lba = req->base.offset;
   req->impl.lba_count = req->base.len;

   req->impl.callback = osvCb;
}

void OsvChannel::_push(RaidRequest<OsvIoReq>* req)
{
   req->impl.this_ptr = this;
   prepare_request(req, [](OsvIoReq* io_req) {
      auto req = reinterpret_cast<RaidRequest<OsvIoReq>*>(io_req);

      req->base.innerCallback.callback(&req->base);
   });

   // write_request_stack.push_back(req);

   int ret = 1;
   int iterations = 0;
   while (true) {
      ret = OsvEnvironment::osv_req_type_fun_lookup[(int)req->impl.type](1, qpairs[req->base.device], req->impl.buf, req->impl.lba,
                                                                         req->impl.lba_count, NVMeController::completion, req, 0);

      if (ret == 0) {
         outstanding[req->base.device]++;
         break;
      }
   }
}

void OsvChannel::_printSpecializedCounters(std::ostream& ss)
{
   ss << "osv: ";
}

}  // namespace mean
