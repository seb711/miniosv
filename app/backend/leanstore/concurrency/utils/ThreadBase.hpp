#pragma once
// -------------------------------------------------------------------------------------
#include "Exceptions.hpp"
// -------------------------------------------------------------------------------------
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
// -------------------------------------------------------------------------------------
#include <osv/sched.hh>
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
class ThreadBase;
extern constinit thread_local ThreadBase* _this_thread;
class ThreadBase
{
  protected:
   std::string name;
   std::atomic<bool> _keep_running = false;
   std::atomic<bool> _wait_for_init = true;
   std::atomic<bool> _ready = false;
   int cpuAffinity = -2;
   const int _id = -2;
   // OSv thread pinned to its core at creation (see start()). Pinning at
   // creation -- rather than starting on CPU0 and re-pinning at runtime with
   // pthread_setaffinity_np -- keeps the thread's logical and physical CPU
   // identical. A runtime self-migration only moves the logical CPU; the thread
   // keeps physically running on CPU0, so a timed sleep arms its timer in the
   // target core's timer_list while CPU0's LAPIC is what fires -- the timer is
   // never serviced and the thread never wakes.
   sched::thread::thread_unique_ptr tWorker{nullptr, sched::thread::dispose};

   int _process()
   {
      while (_wait_for_init) {}  // wait until parent thread is done creating this thread
      _this_thread = this;
      setNameThisThread(name);
      // CPU affinity is established at creation via attr().pin(); no runtime
      // re-pin (that path leaves the thread physically on CPU0).

      _ready = true;
      // u32   tid = gettid();
      // std::cout << " page provider tid: " << tid << std::endl;
      int ret = process();
      //_this_thread = nullptr;
      return ret;
   }

  public:
   ThreadBase(std::string name, int id) : name(name), _id(id) {}
   /*
   ThreadBase(const ThreadBase& other)
           : name(other.name), _keep_running(other._keep_running.load()), _ready(other._ready.load()), cpuAffinity(other.cpuAffinity)
   {

   }
   */
   virtual ~ThreadBase() {};

   ThreadBase(const ThreadBase& other) = delete;
   ThreadBase(ThreadBase&& other) = delete;
   ThreadBase& operator=(const ThreadBase& other) = delete;
   ThreadBase& operator=(ThreadBase&& other) = delete;

   virtual int process() = 0;

   void start()
   {
      _keep_running = true;
      auto attr = sched::thread::attr().name(name);
      if (cpuAffinity >= 0) {
         ensure((size_t)cpuAffinity < sched::cpus.size());
         attr.pin(sched::cpus[cpuAffinity]);   // pin at creation, not at runtime
      }
      tWorker = sched::thread::make_unique([this] { _process(); }, attr);
      _wait_for_init = false;
      tWorker->start();
   }

   bool ready() { return _ready; }

   void stop() { _keep_running = false; }

   bool keepRunning() { return _keep_running; }

   void join()
   {
      if (tWorker) {
         tWorker->join();
         tWorker.reset();
      }
   }

   int id() const { return _id; }

   std::string getName() const { return name; }

   void setNameBeforeStart(std::string name) { this->name = name; }

   void setNameThisThread(std::string name)
   {
      this->name = name;
      // posix_check(pthread_setname_np(pthread_self(), name.c_str()) == 0);
   }

   void setCpuAffinityBeforeStart(int cpuAffinity) { this->cpuAffinity = cpuAffinity; }

   int getCpuAffinity() const { return cpuAffinity; }

   static ThreadBase& this_thread()
   {
      assert(_this_thread);
      return *_this_thread;
   }
};
class Thread : public ThreadBase
{
  protected:
   std::function<void()> fun;

  public:
   Thread(std::function<void()> fun, std::string name = "Thread", int id = -1) : ThreadBase(name, id), fun(fun) {}
   int process() override
   {
      fun();
      return 0;
   }
};
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
