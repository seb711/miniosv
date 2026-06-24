#pragma once
#include <atomic>
#include <cstdint>
#include <exception>

#ifndef LEANSTORE_INCLUDE_OSV
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mean
{
struct SharedConfig {
   volatile uint64_t version;
   uint64_t freq;  // Base rate (requests per second)
   uint64_t var;   // Variance/deviation percentage (0-100)
#ifdef LEANSTORE_INCLUDE_OSV
   // In a single-VM unikernel there is no inter-VM ivshmem peer, so back the
   // "shared" config with a process-local buffer. The function-local static in
   // this implicitly-inline member has a single instance across all TUs.
   static volatile SharedConfig* get_config()
   {
      static char shared_mem_buf[4096] __attribute__((aligned(4096)));
      return reinterpret_cast<volatile SharedConfig*>(shared_mem_buf);
   }
#else
   static volatile SharedConfig* get_config_from_file(const char* filepath)
   {

      static void* mapped_addr = nullptr;

      if (!mapped_addr) {
         int fd = open(filepath, O_RDWR);
         if (fd == -1) {
            perror("open");
            std::cout << "failed to open file" << std::endl; 
            return nullptr;
         }

         mapped_addr = mmap(nullptr, sizeof(SharedConfig), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

         close(fd);  // Can close fd after mmap

         if (mapped_addr == MAP_FAILED) {
            perror("mmap");
            std::cout << "failed to mmap file" << std::endl; 
            return nullptr;
         }
      }

      return (volatile SharedConfig*)mapped_addr;
   }
#endif
} __attribute__((packed));

}  // namespace mean