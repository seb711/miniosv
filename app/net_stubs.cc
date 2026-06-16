/*
 * Weak stubs for POSIX socket, network, and file-allocation functions that
 * DuckDB's bundled HTTP client and LocalFileSystem::Trim reference but that
 * an in-memory DuckDB instance never actually calls.
 *
 * All stubs return -1 / ENOSYS so callers get a clear failure rather than
 * undefined behaviour if they are ever reached.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#define STUB_ENOSYS(ret) do { errno = ENOSYS; return (ret); } while (0)

extern "C" {

// boost.context's stack "finish" routine (and a few gflags error paths) call
// _exit(), which neither OSv nor llvm-libc provides. Route it to OSv's exit()
// (runtime.cc -> osv::shutdown()); using llvm-libc's _Exit() instead would pull
// in its __llvm_libc_exit platform hook, which OSv does not define.
__attribute__((weak, noreturn))
void _exit(int code) { exit(code); __builtin_unreachable(); }

__attribute__((weak))
int fallocate(int, int, long long, long long) { STUB_ENOSYS(-1); }

__attribute__((weak))
int socket(int, int, int) { STUB_ENOSYS(-1); }

__attribute__((weak))
int bind(int, const struct sockaddr *, socklen_t) { STUB_ENOSYS(-1); }

__attribute__((weak))
int connect(int, const struct sockaddr *, socklen_t) { STUB_ENOSYS(-1); }

__attribute__((weak))
int shutdown(int, int) { STUB_ENOSYS(-1); }

__attribute__((weak))
ssize_t recv(int, void *, size_t, int) { STUB_ENOSYS(-1); }

__attribute__((weak))
ssize_t send(int, const void *, size_t, int) { STUB_ENOSYS(-1); }

__attribute__((weak))
int getsockname(int, struct sockaddr *, socklen_t *) { STUB_ENOSYS(-1); }

__attribute__((weak))
int getpeername(int, struct sockaddr *, socklen_t *) { STUB_ENOSYS(-1); }

__attribute__((weak))
int getsockopt(int, int, int, void *, socklen_t *) { STUB_ENOSYS(-1); }

__attribute__((weak))
int setsockopt(int, int, int, const void *, socklen_t) { STUB_ENOSYS(-1); }

__attribute__((weak))
int getaddrinfo(const char *, const char *,
                const struct addrinfo *, struct addrinfo **) { STUB_ENOSYS(EAI_SYSTEM); }

__attribute__((weak))
void freeaddrinfo(struct addrinfo *) {}

__attribute__((weak))
int getnameinfo(const struct sockaddr *, socklen_t,
                char *, socklen_t, char *, socklen_t, int) { STUB_ENOSYS(EAI_SYSTEM); }

__attribute__((weak))
int getifaddrs(struct ifaddrs **) { STUB_ENOSYS(-1); }

__attribute__((weak))
void freeifaddrs(struct ifaddrs *) {}

__attribute__((weak))
const char *inet_ntop(int, const void *, char *, socklen_t)
{
    errno = ENOSYS;
    return NULL;
}

} // extern "C"
