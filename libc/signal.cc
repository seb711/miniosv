/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "signal.hh"
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <osv/debug.hh>
#include <osv/printf.hh>
#include <osv/sched.hh>
#include <osv/mutex.h>
#include <osv/condvar.h>
#include <osv/power.hh>
#include <osv/clock.hh>
#include <api/setjmp.h>
#include <osv/stubbing.hh>
#include <osv/pid.h>
#include <osv/export.h>
#include <osv/kernel_config.h>

using namespace osv::clock::literals;

namespace osv {

// we can't use have __thread sigset because of the constructor
__thread __attribute__((aligned(sizeof(sigset))))
    char thread_signal_mask[sizeof(sigset)];

// Let's ignore rt signals. For standard signals, signal(7) says order is
// unspecified over multiple deliveries, so we always record the last one.  It
// also relieves us of any need for locking, since it doesn't matter if the
// pending signal changes: returning any one is fine
__thread int thread_pending_signal;

struct sigaction signal_actions[nsignals];

sigset* from_libc(sigset_t* s)
{
    return reinterpret_cast<sigset*>(s);
}

const sigset* from_libc(const sigset_t* s)
{
    return reinterpret_cast<const sigset*>(s);
}

sigset* thread_signals()
{
    return reinterpret_cast<sigset*>(thread_signal_mask);
}

sigset* thread_signals(sched::thread *t)
{
    return t->remote_thread_local_ptr<sigset>(&thread_signal_mask);
}

inline bool is_sig_dfl(const struct sigaction &sa) {
    if (sa.sa_flags & SA_SIGINFO) {
         return sa.sa_sigaction == nullptr; // a non-standard Linux extension
    } else {
         return sa.sa_handler == SIG_DFL;
    }
}

inline bool is_sig_ign(const struct sigaction &sa) {
    return (!(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == SIG_IGN);
}

//Similar to signal actions and mask, list of "waiters" for a given signal
//with number "signo" is stored at the index "signo - 1"
typedef std::list<sched::thread *> thread_list;
static std::array<thread_list, nsignals> waiters;
mutex waiters_mutex;

int wake_up_signal_waiters(int signo)
{
    SCOPE_LOCK(waiters_mutex);
    int woken = 0;

    unsigned sigidx = signo - 1;
    for (auto& t: waiters[sigidx]) {
        woken++;
        t->remote_thread_local_var<int>(thread_pending_signal) = signo;
        t->wake();
    }
    return woken;
}

void wait_for_signal(unsigned int sigidx)
{
    SCOPE_LOCK(waiters_mutex);
    waiters[sigidx].push_front(sched::thread::current());
}

void unwait_for_signal(sched::thread *t, unsigned int sigidx)
{
    SCOPE_LOCK(waiters_mutex);
    waiters[sigidx].remove(t);
}

void unwait_for_signal(unsigned int sigidx)
{
    unwait_for_signal(sched::thread::current(), sigidx);
}

void __attribute__((constructor)) signals_register_thread_notifier()
{
    sched::thread::register_exit_notifier(
        []() {
            sigset *set = thread_signals();
            if (!set->mask.any()) { return; }
            for (unsigned i = 0; i < nsignals; ++i) {
                if (set->mask.test(i)) {
                    unwait_for_signal(i);
                }
            }
        }
    );
}

void generate_signal(siginfo_t &siginfo, exception_frame* ef)
{
    unsigned sigidx = siginfo.si_signo - 1;
    if (pthread_self() && thread_signals()->mask[sigidx]) {
        // FIXME: need to find some other thread to deliver
        // FIXME: the signal to.
        //
        // There are certainly no waiters for this, because since we
        // only deliver signals through this method directly, the thread
        // needs to be running to generate them. So definitely not waiting.
        abort();
    }
    if (is_sig_dfl(signal_actions[sigidx])) {
        // Our default is to abort the process
        abort();
    } else if(!is_sig_ign(signal_actions[sigidx])) {
        arch::build_signal_frame(ef, siginfo, signal_actions[sigidx]);
    }
}

void handle_mmap_fault(ulong addr, int sig, exception_frame* ef)
{
    siginfo_t si;
    si.si_signo = sig;
    si.si_addr = reinterpret_cast<void*>(addr);
    generate_signal(si, ef);
}

}

using namespace osv;

OSV_LIBC_API
int sigemptyset(sigset_t* sigset)
{
    from_libc(sigset)->mask.reset();
    return 0;
}

OSV_LIBC_API
int sigfillset(sigset_t *sigset)
{
    from_libc(sigset)->mask.set();
    return 0;
}

OSV_LIBC_API
int sigaddset(sigset_t *sigset, int signum)
{
    if (signum < 1 || signum > (int)nsignals) {
        errno = EINVAL;
        return -1;
    }
    unsigned sigidx = signum - 1;
    from_libc(sigset)->mask.set(sigidx);
    return 0;
}

OSV_LIBC_API
int sigdelset(sigset_t *sigset, int signum)
{
    if (signum < 1 || signum > (int)nsignals) {
        errno = EINVAL;
        return -1;
    }
    unsigned sigidx = signum - 1;
    from_libc(sigset)->mask.reset(sigidx);
    return 0;
}

OSV_LIBC_API
int sigismember(const sigset_t *sigset, int signum)
{
    if (signum < 1 || signum > (int)nsignals) {
        errno = EINVAL;
        return -1;
    }
    unsigned sigidx = signum - 1;
    return from_libc(sigset)->mask.test(sigidx);
}

OSV_LIBC_API
int sigprocmask(int how, const sigset_t* _set, sigset_t* _oldset)
{
    auto set = from_libc(_set);
    auto oldset = from_libc(_oldset);
    if (oldset) {
        *oldset = *thread_signals();
    }
    if (set) {
        switch (how) {
        case SIG_BLOCK:
            for (unsigned i = 0; i < nsignals; ++i) {
                if (set->mask.test(i)) {
                    wait_for_signal(i);
                }
            }
            thread_signals()->mask |= set->mask;
            break;
        case SIG_UNBLOCK:
            for (unsigned i = 0; i < nsignals; ++i) {
                if (set->mask.test(i)) {
                    unwait_for_signal(i);
                }
            }
            thread_signals()->mask &= ~set->mask;
            break;
        case SIG_SETMASK:
            for (unsigned i = 0; i < nsignals; ++i) {
                unwait_for_signal(i);
                if (set->mask.test(i)) {
                    wait_for_signal(i);
                }
            }
            thread_signals()->mask = set->mask;
            break;
        default:
            abort();
        }
    }
    return 0;
}

UNIMPL(OSV_LIBC_API int sigsuspend(const sigset_t *mask));

OSV_LIBC_API
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact)
{
    // FIXME: We do not support any sa_flags besides SA_SIGINFO.
    if (signum < 1 || signum > (int)nsignals) {
        errno = EINVAL;
        return -1;
    }
    unsigned sigidx = signum - 1;
    if (oldact) {
        *oldact = signal_actions[sigidx];
    }
    if (act) {
        signal_actions[sigidx] = *act;
    }
    return 0;
}

// using signal() is not recommended (use sigaction instead!), but some
// programs like to call to do simple things, like ignoring a certain signal.
static sighandler_t signal(int signum, sighandler_t handler, int sa_flags)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    act.sa_flags = sa_flags;
    struct sigaction old;
    if (sigaction(signum, &act, &old) < 0) {
        return SIG_ERR;
    }
    if (old.sa_flags & SA_SIGINFO) {
        // TODO: Is there anything sane to do here?
        return nullptr;
    } else {
        return old.sa_handler;
    }
}

OSV_LIBC_API
sighandler_t signal(int signum, sighandler_t handler)
{
    return signal(signum, handler, SA_RESTART);
}

extern "C"
OSV_LIBC_API
sighandler_t __sysv_signal(int signum, sighandler_t handler)
{
    return signal(signum, handler, SA_RESETHAND | SA_NODEFER);
}

// using sigignore() and friends is not recommended as it is obsolete System V
// APIs. Nevertheless, some programs use it.
OSV_LIBC_API
int sigignore(int signum)
{
    if (signum < 1 || signum > (int)nsignals) {
        errno = EINVAL;
        return -1;
    }
    struct sigaction act;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    act.sa_handler = SIG_IGN;
    return sigaction(signum, &act, nullptr);
}

OSV_LIBC_API
int sigwait(const sigset_t *set, int *sig)
{
    sched::thread::wait_until([sig] { return *sig = thread_pending_signal; });
    thread_pending_signal = 0;
    return 0;
}

// Partially-Linux-compatible support for kill(2).
// Note that this is different from our generate_signal() - the latter is only
// suitable for delivering SIGFPE and SIGSEGV to the same thread that called
// this function.
//
// Handling kill(2)/signal(2) exactly like Linux, where one of the existing
// threads runs the signal handler, is difficult in OSv because it requires
// tracking of when we're in kernel code (to delay the signal handling until
// it returns to "user" code), and also to interrupt sleeping kernel code and
// have it return sooner.
// Instead, we provide a simple "approximation" of the signal handling -
// on each kill(), a *new* thread is created to run the signal handler code.
//
// This approximation will work in programs that do not care about the signal
// being delivered to a specific thread, and that do not intend that the
// signal should interrupt a system call (e.g., sleep() or hung read()).
// FIXME: think if our handling of nested signals is ok (right now while
// handling a signal, we can get another one of the same signal and start
// another handler thread. We should probably block this signal while
// handling it.

OSV_LIBC_API
int kill(pid_t pid, int sig)
{
  return -1;     
}

OSV_LIBC_API
int pause(void) {
  return -1;     
}

OSV_LIBC_API
unsigned int alarm(unsigned int seconds)
{
   return -1; 
}

extern "C" OSV_LIBC_API
int setitimer(int which, const struct itimerval *new_value,
    struct itimerval *old_value)
{
  return -1;     
}

extern "C" OSV_LIBC_API
int getitimer(int which, struct itimerval *curr_value)
{
    return -1; 
}

OSV_LIBC_API
int sigaltstack(const stack_t *ss, stack_t *oss)
{
    return -1;
}

extern "C" OSV_LIBC_API
int signalfd(int fd, const sigset_t *mask, int flags)
{
    return -1;
}

extern "C" OSV_LIBC_API
int sigwaitinfo(const sigset_t *__restrict mask,
                           siginfo_t *__restrict si)
{
    return -1; 
}
