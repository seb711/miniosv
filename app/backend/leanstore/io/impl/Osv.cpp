#include "Osv.hpp"

#include <cstddef>
#include <regex>

bool OsvEnvironment::initialized = false;
cmd_fun OsvEnvironment::osv_req_type_fun_lookup[(int)OsvIoReqType::COUNT + 1];

int OsvEnvironment::qpair_process_completions(void* qpair, uint32_t max)
{
   // fixme that can be done better
   assert(qpair);
   return ((nvme::io_user_queue_pair*)qpair)->process_completions(max);
}
void OsvEnvironment::ensureInitialized()
{
   if (!isInitialized()) {
      throw std::logic_error("OsvEnvironment not initialized");
   }
}
bool OsvEnvironment::isInitialized(bool init)
{
   if (init) {
      initialized = init;
   }
   return initialized;
};
void OsvEnvironment::init()
{
   if (isInitialized()) {
      throw std::logic_error("OsvEnvironment already initialized");
   }

   OsvEnvironment::osv_req_type_fun_lookup[(int)OsvIoReqType::Read] =
       [](int ns, void* queue, void* payload, uint64_t addr, uint32_t len, osv_nvme_cmd_cb cb_fn, void* cb_arg, uint32_t io_flags) {
          return ((nvme::io_user_queue_pair*)queue)->submit_request(ns, payload, addr, len, cb_fn, cb_arg, io_flags, nvme::NVME_COMMAND::READ) ^ 1;
       };
   OsvEnvironment::osv_req_type_fun_lookup[(int)OsvIoReqType::Write] =
       [](int ns, void* queue, void* payload, uint64_t addr, uint32_t len, osv_nvme_cmd_cb cb_fn, void* cb_arg, uint32_t io_flags) {
          return ((nvme::io_user_queue_pair*)queue)->submit_request(ns, payload, addr, len, cb_fn, cb_arg, io_flags, nvme::NVME_COMMAND::WRITE) ^ 1;
       };
   OsvEnvironment::osv_req_type_fun_lookup[(int)OsvIoReqType::COUNT] = nullptr;

   isInitialized(true);
}

void OsvEnvironment::deinit()
{
   if (isInitialized()) {
      // TODO: CHECK IF WE NEED TO REMOVE SOMETHING IN THE OSV KERNEL
   }
}
// -------------------------------------------------------------------------------------
// NVMeController
// -------------------------------------------------------------------------------------
NVMeController::NVMeController()
{
   OsvEnvironment::ensureInitialized();
}
// -------------------------------------------------------------------------------------
NVMeController::~NVMeController()
{
   // TODO: somehow we should see if we release the io_queues but for now we wont do that -> just exit
   // for (auto& qpair : qpairs) {
   //    assert(device_id != -1);
   //    nvme::driver::get_nvme_device(device_id)->remove_io_user_queue(qpair);
   // }
   // qpairs.clear();
   std::cout << "shutdown controller " << device_id << std::endl;
   if (nvme::driver* dev = nvme::driver::get_nvme_device(device_id)) {
      dev->shutdown_controller();
   }
}
// -------------------------------------------------------------------------------------
uint32_t NVMeController::nsLbaDataSize()
{
   // TODO: RETRIEVE THIS BY OSV DRIVER (-> IMPLEMENT THE METHODS IN OSV)
   return 512;
}
// -------------------------------------------------------------------------------------
uint64_t NVMeController::nsSize()
{
   // TODO: RETRIEVE THIS BY OSV DRIVER (-> IMPLEMENT THE METHODS IN OSV)
   return 1ull << 32ull;
}
// -------------------------------------------------------------------------------------
uint32_t NVMeController::queueDepth()
{
   // TODO: RETRIEVE THIS BY OSV DRIVER (-> IMPLEMENT THE METHODS IN OSV)
   // FIXME: THE QUEUE DEPTH SHOULD BE SET TO A MAXIMUM BUT NOT TOO HIGH
   return 512;
}
// -------------------------------------------------------------------------------------
void NVMeController::allocateQPairs()
{
   allocateQPairs(-1);
}
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
uint32_t NVMeController::numberNamespaces()
{
   // TODO: RETRIEVE THIS BY OSV DRIVER (-> IMPLEMENT THE METHODS IN OSV)
   return 1;
}
// -------------------------------------------------------------------------------------
uint64_t NVMeController::nsNumLbas()
{
   // TODO: RETRIEVE THIS BY OSV DRIVER (-> IMPLEMENT THE METHODS IN OSV)
   return 1 << (32 - 9);
}
uint32_t NVMeController::requestMaxQPairs()
{
   // TODO: RETRIEVE THIS BY OSV DRIVER (-> IMPLEMENT THE METHODS IN OSV)
   return 10;
}
// -------------------------------------------------------------------------------------
void NVMeController::allocateQPairs(int number)
{
   if (number < 0) {
      number = requestMaxQPairs();
   }

   for (int i = 0; i < number; i++) {
      assert(device_id != -1);

      nvme::driver* dev = nvme::driver::get_nvme_device(device_id);
#ifndef IS_INTERRUPT_NVME
      auto* qpair = (nvme::io_user_queue_pair*)(dev ? dev->create_io_user_queue(queueDepth()) : nullptr);
#else
      auto* qpair = (nvme::io_user_queue_pair*)(dev ? dev->create_io_interrupt_user_queue(queueDepth()) : nullptr);
#endif

      if (!qpair) {
         throw std::logic_error("ERROR: leanstore_create_io_user_queue() failed\n");
      }

      qpairs.push_back(qpair);
   }
}
// -------------------------------------------------------------------------------------
void NVMeController::completion(void* cb_arg, const nvme_sq_entry_t* sqe)
{
   auto request = static_cast<OsvIoReq*>(cb_arg);

   request->callback(request);
};

// -------------------------------------------------------------------------------------
int NVMeMultiController::deviceCount()
{
   return controller.size();
}
// -------------------------------------------------------------------------------------
int32_t NVMeController::qpairSize()
{
   return qpairs.size();
}
// -------------------------------------------------------------------------------------
void NVMeMultiController::connect(std::string connectionString)
{
   // Walk the linked list of probed NVMe controllers to collect their ids.
   std::vector<int> all_ids;
   for (nvme::driver* d = nvme::driver::prev_nvme_driver; d != nullptr; d = d->_next_nvme_driver) {
      all_ids.push_back(d->get_id());
   }

   // When booting via UEFI (or on EC2) the small boot/ESP volume shows up
   // as just another NVMe controller; striping data over it would overflow
   // it. Only use controllers large enough to plausibly be data disks.
   // 2GiB: EBS volumes are at least 1GiB, so 256MB would not catch the
   // EC2 boot volume; data volumes must be > target_gib anyway, so > 2GiB.
   constexpr uint64_t min_data_disk_bytes = 2ull << 30;
   std::vector<int> ids;
   for (int id : all_ids) {
      uint64_t bytes = 0;
      if (nvme::driver* dev = nvme::driver::get_nvme_device(id)) {
         auto it = dev->_ns_data.find(1);
         if (it != dev->_ns_data.end()) {
            bytes = it->second->blockcount * it->second->blocksize;
         }
      }
      if (bytes >= min_data_disk_bytes) {
         ids.push_back(id);
      } else {
         std::cout << "skipping nvme id " << id << " (" << (bytes >> 20)
                   << " MiB, likely the boot disk)" << std::endl;
      }
   }

   controller.resize(ids.size());
   std::cout << "NVMeMultiController has size=" << ids.size() << std::endl;

   for (unsigned int i = 0; i < ids.size(); i++) {
      std::cout << "controller idx=" << i << " with id " << ids[i] << std::endl;
      controller[i].setDeviceId(ids[i]);
   }
}
// -------------------------------------------------------------------------------------
void NVMeMultiController::allocateQPairs()
{
   for (auto& c : controller) {
      c.allocateQPairs();
   }
}
// -------------------------------------------------------------------------------------
int32_t NVMeMultiController::qpairSize()
{
   int32_t min = std::numeric_limits<int32_t>::max();
   for (auto& c : controller) {
      min = std::min(min, c.qpairSize());
   }
   if (min <= 0 || min == std::numeric_limits<int32_t>::max()) {
      throw std::logic_error("could not get qpair size");
   }
   return min;
}
// -------------------------------------------------------------------------------------
// TODO: OPTIMIZE
uint32_t NVMeMultiController::nsLbaDataSize()
{
   uint32_t dataSize = controller[0].nsLbaDataSize();
   for (auto& c : controller) {
      if (dataSize != c.nsLbaDataSize()) {
         throw std::logic_error("lba size must be equal for all devices");
      }
   }
   return dataSize;
}
// -------------------------------------------------------------------------------------
uint64_t NVMeMultiController::nsSize()
{
   uint64_t size = std::numeric_limits<uint64_t>::max();
   for (auto& c : controller) {
      size = std::min(size, c.nsSize());
   }
   return size;
}
// -------------------------------------------------------------------------------------


