#ifndef NVME_H_
#define NVME_H_

#include <functional>
#include <vector>
#include "nvme-structs.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef void (*osv_nvme_cmd_cb)(void *ctx, const nvme_sq_entry_t* cpl);

typedef struct osv_nvme_callback {
    osv_nvme_cmd_cb cb;
    void* cb_args;
} osv_nvme_callback;

bool osv_shutdown_controller(int nvme_id);

int osv_remove_io_user_queue(int nvme_id, void* queue);

void*osv_create_io_user_queue(int nvme_id, int queue_depth);
void *osv_create_io_int_user_queue(int disk_id, int queue_size);

uint64_t osv_nvme_get_ns_size_bytes(int nvme_id, uint32_t nsid);

int osv_nvme_nv_cmd_read(int, void*, void*, uint64_t, uint32_t, void (*osv_nvme_cmd_cb)(void *ctx, const nvme_sq_entry_t* cpl), void *, uint32_t);
int osv_nvme_nv_cmd_write(int, void*, void*, uint64_t, uint32_t, void (*osv_nvme_cmd_cb)(void *ctx, const nvme_sq_entry_t* cpl), void *, uint32_t);
int osv_nvme_qpair_process_completions(void*, uint32_t);
// Flush all submissions placed since the last ring to the controller with a
// single SQ-doorbell MMIO write. osv_nvme_nv_cmd_read/write now only PLACE the
// command; the caller batches N submits and calls this once to ring them.
void osv_nvme_qpair_ring_sq_doorbell(void*);

#ifdef __cplusplus
}

/* C++ only declarations (can use std:: types) */
#include <vector>
#include <functional>
std::vector<int> osv_get_available_ssds();
typedef std::function<int(int, void*, void*, uint64_t, uint32_t, void (*osv_nvme_cmd_cb)(void *ctx, const nvme_sq_entry_t* cpl), void *, uint32_t)> leanstore_osv_rw_fn;
#endif


#endif
