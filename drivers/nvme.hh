/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef NVME_DRIVER_H
#define NVME_DRIVER_H

#include "drivers/driver.hh"
#include "drivers/nvme-queue.hh"
#include "drivers/nvme-user-queue.hh"
#include "drivers/pci-device.hh"
#include <map>
#include <memory>
#include <osv/aligned_new.hh>
#include <osv/interrupt.hh>
#include <osv/mempool.hh>
#include <osv/msi.hh>
#include <osv/nvme-structs.h>
#include <unordered_map>
#include <vector>

// #define USE_INTERRUPT
// #define USE_COALESCING
// #define USE_POLLING_THREAD

// this means that the OS is not creating any IO queues that can work with BIO
#define USE_USER_IO_QUEUES

#define NVME_QUEUE_PER_CPU_ENABLED 0

// Volatile Write Cache
#define NVME_VWC_ENABLED 1

#define NVME_ADMIN_QUEUE_SIZE 8
#define NVME_IO_QUEUE_SIZE 32

// Will be lower if the device doesnt support the specified queue size

namespace nvme {

enum NVME_IO_QUEUE_PRIORITY {
  NVME_IO_QUEUE_PRIORITY_URGENT = 0,
  NVME_IO_QUEUE_PRIORITY_HIGH = 1,
  NVME_IO_QUEUE_PRIORITY_MEDIUM = 2,
  NVME_IO_QUEUE_PRIORITY_LOW = 3,
};

class driver : public hw_driver {
public:
  explicit driver(pci::device &dev);
  virtual ~driver() {};

  virtual std::string get_name() const { return "nvme"; }

  virtual void dump_config();

  static hw_driver *probe(hw_device *dev);

  std::map<u32, nvme_ns_t *> _ns_data;

  // should be private and add a get-method for it to make it readonly
  static driver *prev_nvme_driver;

  // can be removed later
  const int get_id() { return this->_id; };
  driver *_next_nvme_driver;

  // for dynamic queue generation/destruction
  static driver *get_nvme_device(int id);
  void *create_io_user_queue(int individual_qsize); // returns qid
  void *create_io_interrupt_user_queue(int individual_qsize);
  int remove_io_user_queue(void *queue);
  int remove_all_io_user_queues();
  bool reset_and_destroy_controller();
  bool shutdown_controller();

private:
  int identify_controller();
  int identify_namespace(u32 ns);

  void create_admin_queue();
  void register_admin_interrupt();

  // Used by create_io_interrupt_user_queue() to wire an MSI-X vector to a
  // user I/O queue's completion processing. (The block-io io_queue_pair path
  // and its create_io_queues()/setup_io_wo_interrupt() helpers are gone.)
  bool register_io_interrupt(unsigned int iv, unsigned int qid,
                             sched::cpu *cpu);

  // user io queues
  void create_io_user_queue_endpoints();

  void init_controller_config();

  int get_worst_cast_time();
  int enable_disable_controller(bool enable);
  int wait_for_controller_ready_change(int ready);
  int wait_for_controller_shutdown_done();

  int set_number_of_queues(u16 num, u16 *ret);
  int set_interrupt_coalescing(u8 threshold, u8 time);

  bool parse_pci_config();
  void enable_msix();

  void enable_write_cache();

  bool msix_register(unsigned iv,
                     // high priority ISR
                     std::function<void()> isr,
                     // bottom half
                     sched::thread *t,
                     // set affinity of the vector to the cpu running t
                     bool assign_affinity = false);

  bool msix_register_io_queue(unsigned iv, unsigned qid);

  // Maintains the nvme instance number for multiple adapters
  static int _instance;
  int _id;

  // Disk index number
  static int _disk_idx;

  std::vector<std::unique_ptr<msix_vector>> _msix_vectors;

  std::unique_ptr<admin_queue_pair, aligned_new_deleter<admin_queue_pair>>
      _admin_queue;

  std::vector<std::unique_ptr<io_user_queue_pair,
                              aligned_new_deleter<io_user_queue_pair>>>
      _io_queues;
  size_t _max_id = 0; // TODO: ids of user and normal io queues DO NOT intersect

  u32 _doorbell_stride;
  u32 _qsize;

  std::unique_ptr<nvme_identify_ctlr_t> _identify_controller;
  nvme_controller_reg_t *_control_reg = nullptr;

  pci::device &_dev;
  interrupt_manager _msi;

  pci::bar *_bar0 = nullptr;
};

} // namespace nvme
#endif
