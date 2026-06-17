#include <osv/nvme.hh>

#include "drivers/nvme.hh"
#include "drivers/nvme-queue.hh"

extern "C" bool osv_shutdown_controller(int disk_id) {
    nvme::driver* nvme_dev = nvme::driver::get_nvme_device(disk_id); 

    if (nvme_dev == nullptr) {
        return false; 
    }

    return nvme_dev->shutdown_controller(); 
}

extern "C" int osv_nvme_nv_cmd_read(int ns, void *queue, void *payload, uint64_t addr, uint32_t len, nvme::osv_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags)
{
    // read stuff
    nvme::io_user_queue_pair *queuet = (nvme::io_user_queue_pair *)queue;

    return queuet->submit_request(ns, payload, addr, len, cb_fn, cb_arg, io_flags, nvme::NVME_COMMAND::READ) ^ 1;
}

extern "C" int osv_nvme_nv_cmd_write(int ns, void *queue, void *payload, uint64_t addr, uint32_t len, nvme::osv_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags)
{
    // read stuff
    nvme::io_user_queue_pair *queuet = (nvme::io_user_queue_pair *)queue;

    return queuet->submit_request(ns, payload, addr, len, cb_fn, cb_arg, io_flags, nvme::NVME_COMMAND::WRITE) ^ 1;
}

extern "C" int osv_nvme_qpair_process_completions(void *queue, uint32_t max_completions)
{
    nvme::io_user_queue_pair *queuet = (nvme::io_user_queue_pair *)queue;

    return queuet->process_completions(max_completions);
}

extern "C" void osv_nvme_qpair_ring_sq_doorbell(void *queue)
{
    nvme::io_user_queue_pair *queuet = (nvme::io_user_queue_pair *)queue;

    queuet->ring_sq_doorbell();
}

extern "C" uint64_t osv_nvme_get_ns_size_bytes(int disk_id, uint32_t nsid)
{
    nvme::driver* nvme_dev = nvme::driver::get_nvme_device(disk_id);

    if (nvme_dev == nullptr) {
        return 0;
    }

    auto it = nvme_dev->_ns_data.find(nsid);
    if (it == nvme_dev->_ns_data.end()) {
        return 0;
    }

    return it->second->blockcount * it->second->blocksize;
}

    // THESE ARE SUPER HACKY WAYS TO GET THE NVME THINGS
std::vector<int> osv_get_available_ssds() {
    // we could add here that we return configuration information
    nvme::driver* current_driver = nvme::driver::prev_nvme_driver; 
    std::vector<int> ids{}; 

    while (current_driver != nullptr) {
        ids.push_back(current_driver->get_id()); 
        current_driver = current_driver->_next_nvme_driver; 
    }

    return ids; 
}

extern "C" void* osv_create_io_user_queue(int disk_id, int queue_size) {
    nvme::driver* nvme_dev = nvme::driver::get_nvme_device(disk_id); 

    if (nvme_dev == nullptr) {
        return nullptr; 
    }

    return nvme_dev->create_io_user_queue(queue_size); 
}

extern "C" void *osv_create_io_int_user_queue(int disk_id, int queue_size) {
  nvme::driver *nvme_dev = nvme::driver::get_nvme_device(disk_id);

  if (nvme_dev == nullptr) {
    printf("nvme device is not existing\n");
    return nullptr;
  }

  return nvme_dev->create_io_interrupt_user_queue(queue_size);
}

extern "C" int osv_remove_io_user_queue(int disk_id, void* queue) {
    nvme::driver* nvme_dev =  nvme::driver::get_nvme_device(disk_id); 

    if (nvme_dev == nullptr) {
        return -1; 
    }

    return nvme_dev->remove_io_user_queue(queue); 
}