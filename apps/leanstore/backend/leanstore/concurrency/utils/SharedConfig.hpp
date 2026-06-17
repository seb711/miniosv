#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
#include <osv/leanstore_debug.hh>

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
   static volatile SharedConfig* get_config() { return (volatile SharedConfig*)leanstore_osv_debug::get_shared_memory(); }
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