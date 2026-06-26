/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_SWITCH_HH_
#define ARCH_SWITCH_HH_

#include "msr.hh"
#include <osv/barrier.hh>
#include <osv/kernel_config.h>
#include <string.h>

extern "C" {
void thread_main(void);
void thread_main_c(sched::thread *t);

bool fsgsbase_avail = false;
}

namespace sched {

void set_fsbase_msr(u64 v) { processor::wrmsr(msr::IA32_FS_BASE, v); }

void set_fsbase_fsgsbase(u64 v) { processor::wrfsbase(v); }

extern "C" void (*resolve_set_fsbase(void))(u64 v) {
  // can't use processor::features, because it is not initialized
  // early enough.
  if (processor::features().fsgsbase) {
    fsgsbase_avail = true;
    return set_fsbase_fsgsbase;
  } else {
    return set_fsbase_msr;
  }
}

void set_fsbase(u64 v) __attribute__((ifunc("resolve_set_fsbase")));

void thread::switch_to() {
  thread *old = current();
  // writing to fs_base invalidates memory accesses, so surround with
  // barriers
  barrier();
  set_fsbase(reinterpret_cast<u64>(_tcb));
  barrier();
  auto c = _detached_state->_cpu;
  old->_state.exception_stack = c->arch.get_exception_stack();
  c->arch.set_interrupt_stack(&_arch);
  c->arch.set_exception_stack(_state.exception_stack);
  // set this cpu current thread kernel TCB address to TCB address of the new
  // thread we are switching to
  c->arch._current_thread_kernel_tcb = reinterpret_cast<u64>(_tcb);
  auto fpucw = processor::fnstcw();
  auto mxcsr = processor::stmxcsr();
  asm volatile("mov %%rbp, %c[rbp](%0) \n\t"
               "movq $1f, %c[rip](%0) \n\t"
               "mov %%rsp, %c[rsp](%0) \n\t"
               "mov %c[rsp](%1), %%rsp \n\t"
               "mov %c[rbp](%1), %%rbp \n\t"
               "jmpq *%c[rip](%1) \n\t"
               "1: \n\t"
               :
               : "a"(&old->_state),
                 "c"(&this->_state), [rsp] "i"(offsetof(thread_state, rsp)),
                 [rbp] "i"(offsetof(thread_state, rbp)),
                 [rip] "i"(offsetof(thread_state, rip))
               : "rbx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
                 "r13", "r14", "r15", "memory");
  // As the catch-all solution, reset FPU state and more specifically
  // its status word. For details why we need it please see issue #1020.
  asm volatile("emms");
  processor::fldcw(fpucw);
  processor::ldmxcsr(mxcsr);
}

void thread::switch_to_first() {
  barrier();
  processor::wrmsr(msr::IA32_FS_BASE, reinterpret_cast<u64>(_tcb));
  barrier();
  s_current = this;
  current_cpu = _detached_state->_cpu;
  remote_thread_local_var(percpu_base) = _detached_state->_cpu->percpu_base;
  _detached_state->_cpu->arch.set_interrupt_stack(&_arch);
  _detached_state->_cpu->arch.set_exception_stack(&_arch);
  _detached_state->_cpu->arch._current_thread_kernel_tcb =
      reinterpret_cast<u64>(_tcb);
  asm volatile("mov %c[rsp](%0), %%rsp \n\t"
               "mov %c[rbp](%0), %%rbp \n\t"
               "jmp *%c[rip](%0)"
               :
               : "c"(&this->_state), [rsp] "i"(offsetof(thread_state, rsp)),
                 [rbp] "i"(offsetof(thread_state, rbp)),
                 [rip] "i"(offsetof(thread_state, rip))
               : "rbx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
                 "r13", "r14", "r15", "memory");
}

void thread::init_stack() {
  auto &stack = _attr._stack;
  if (!stack.size) {
    stack.size = CONF_threads_default_kernel_stack_size;
  }
  if (!stack.begin) {
    stack.begin = malloc(stack.size);
    stack.deleter = stack.default_deleter;
  } else {
    // The thread will run thread_main_c() with preemption disabled
    // for a short while (see 695375f65303e13df1b9de798577ee9a4f8f9892)
    // so page faults are forbidden - so we need the top of the stack
    // to be pre-faulted. When we call malloc() above ourselves above
    // we know this is the case, but if the user allocates the stack
    // with mmap without MAP_STACK or MAP_POPULATE, this might not be
    // the case, so we need to fault it in now, with preemption on.
    (void)*((volatile char *)stack.begin + stack.size - 1);
  }
  void **stacktop =
      reinterpret_cast<void **>(static_cast<char *>(stack.begin) + stack.size);
  _state.rbp = this;
  _state.rip = reinterpret_cast<void *>(thread_main);
  _state.rsp = stacktop;
  _state.exception_stack =
      _arch.exception_stack + sizeof(_arch.exception_stack);
}

void thread::setup_tcb() { //
  // This method allocates the per-thread TLS memory region and sets up the
  // TCB (Thread Control Block) that points to it. The region holds the
  // thread-local variables (with __thread modifier) defined in the kernel
  // and the statically-linked application.
  //
  // The TLS layout conforms to variant II (2), which means all variable
  // offsets are negative (the block is laid out to the left of the TCB).

  assert(sched::tls.size);

  // The application is statically linked into the kernel, so a thread's TLS
  // is just the kernel TLS template (module 0); there is no separately
  // loaded executable/shared-object TLS to splice in.
  // In arch/x64/loader.ld the TLS template segment is aligned to 64 bytes,
  // and the objects placed in it assume that, so allocate with the same
  // alignment.
  auto kernel_tls_size = sched::tls.size;
  assert(align_check(kernel_tls_size, (size_t)64));

  char *p =
      static_cast<char *>(aligned_alloc(64, kernel_tls_size + sizeof(*_tcb)));
  memcpy(p, sched::tls.start, sched::tls.filesize);
  memset(p + sched::tls.filesize, 0, kernel_tls_size - sched::tls.filesize);

  _tcb = reinterpret_cast<thread_control_block *>(p + kernel_tls_size);
  _tcb->self = _tcb;
  _tcb->tls_base = p;
}

void thread::free_tcb() {
  // tls_base points at the start of the allocation made in setup_tcb().
  free(_tcb->tls_base);
}

void thread::update_dtv() {}

void thread_main_c(thread *t) {
  arch::irq_enable();
#if CONF_preempt
  preempt_enable();
#endif
  // make sure thread starts with clean fpu state instead of
  // inheriting one from a previous running thread
  processor::init_fpu();
  t->main();
  t->complete();
}

} // namespace sched

#endif /* ARCH_SWITCH_HH_ */
