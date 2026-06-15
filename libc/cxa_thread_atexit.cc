/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

// When a C++11 "thread_local" variable is first used in a thread, the C++
// compiler allocates an instance for this thread, and then calls the
// __cxa_thread_atexit_impl() function, which we implement here, to register
// the destructor to be called when this thread exits.
//
// The C++ compiler relies on the C library to implement this function,
// because its implementation is intertwined with the implementation of
// threads in the C library - i.e., only the C library can known when a
// thread end.
//
// The use of __cxa_thread_atexit_impl() is fairly new to gcc, and
// may not be relevant to other C++ compilers. Older versions of gcc did
// not use this function at all, and instead made use of Posix Threads'
// "Thread Specific Data" (pthread_key_create() et al.) - a portable
// technique of running certain callbacks each time a thread ends.

#include <assert.h>

#include <osv/sched.hh>

typedef void (*destructor) (void *);

struct linked_destructor {
    destructor dtor;
    void *obj;
    linked_destructor *next;

    // The application is statically linked into the kernel, which is never
    // unloaded, so (unlike upstream OSv) there is no shared object to keep
    // alive until the thread-local destructor runs.
    linked_destructor(destructor dtor, void* obj, void* dso_symbol,
            linked_destructor *next) : dtor(dtor), obj(obj), next(next)
    {
    }
};
static __thread linked_destructor *thread_local_destructors;

extern "C"
void __cxa_thread_atexit_impl(destructor dtor, void* obj, void* dso_symbol)
{
    auto *item = new linked_destructor(
            dtor, obj, dso_symbol, thread_local_destructors);
    thread_local_destructors = item;
}

static void __attribute__((constructor)) register_call_destructor()
{
    sched::thread::register_exit_notifier([] {
        while (thread_local_destructors) {
            auto item = thread_local_destructors;
            thread_local_destructors = thread_local_destructors->next;
            item->dtor(item->obj);
            delete item;
        }
    });
}
