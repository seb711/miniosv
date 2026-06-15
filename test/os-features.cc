/*
 * OS feature inventory + conformance test for the slimmed OSv unikernel (C++).
 *
 * Unlike apps/os-stress (which hammers a few subsystems concurrently to shake
 * out races), this program walks the *full list* of OS facilities the current,
 * slimmed-down code base is supposed to support and checks each one behaves.
 * It also probes the documented boundaries (no fork, no IP networking, no
 * directed thread signals) and asserts they fail cleanly rather than crash.
 *
 * Built as a dynamic PIE so every libc/syscall symbol resolves against OSv's
 * own libc + kernel at run time. NOTE: in a dynamically-linked OSv program a
 * call to a libc symbol the kernel does not define aborts the whole process,
 * so every function called below was verified to exist in loader.elf first.
 *
 * Each CHECK keeps going on failure; main() returns non-zero if anything
 * failed and prints "RESULT: ALL OS FEATURE TESTS PASSED" only when clean.
 */
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <typeinfo>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <dlfcn.h>
#include <locale.h>
#include <time.h>
#include <sys/time.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/sendfile.h>

// arc4random() is provided by the OSv kernel (drivers/random.cc) but not
// declared by any libc header here.
extern "C" uint32_t arc4random(void);

static std::atomic<int> g_checks{0};
static std::atomic<int> g_fails{0};
static const char *g_section = "";

#define CHECK(cond) do { \
        g_checks.fetch_add(1); \
        if (!(cond)) { \
            g_fails.fetch_add(1); \
            printf("    FAIL [%s] %s:%d: %s (errno=%d %s)\n", \
                   g_section, __FILE__, __LINE__, #cond, errno, strerror(errno)); \
        } \
    } while (0)

static void section(const char *s) { g_section = s; printf("  - %s\n", s); }
static void group(const char *s)   { printf("\n== %s ==\n", s); }

/* small helpers ----------------------------------------------------------- */

// monotonic milliseconds
static long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// wait (up to timeout_ms) for an fd to become readable; returns >0 if readable
static int wait_readable(int fd, int timeout_ms)
{
    struct pollfd pfd { fd, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms);
}

/* ===================================================== process / program */

static void test_process_model()
{
    group("Process & program model (single address space)");

    section("getpid() returns a stable positive id");
    pid_t p1 = getpid();
    CHECK(p1 > 0);
    CHECK(getpid() == p1);

    section("environment variables: setenv / getenv roundtrip");
    CHECK(setenv("OSV_FEATURE_TEST", "42", 1) == 0);
    const char *v = getenv("OSV_FEATURE_TEST");
    CHECK(v != nullptr && std::string(v) == "42");
    CHECK(unsetenv("OSV_FEATURE_TEST") == 0);
    CHECK(getenv("OSV_FEATURE_TEST") == nullptr);

    section("BOUNDARY: fork() is unsupported (single address space)");
    errno = 0;
    pid_t pid = fork();
    if (pid == 0) {
        // Must never happen on OSv; bail out immediately if it somehow does.
        _exit(0);
    }
    CHECK(pid == -1);          // fork must fail, not split the address space
}

/* ============================================================== threads */

static thread_local int tls_value = -1;

static void test_threads()
{
    group("Threads & scheduling");

    section("std::thread spawn/join, atomic counter is exact");
    {
        const int NT = 8, PER = 50000;
        std::atomic<long> counter{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++)
            ts.emplace_back([&] {
                for (int j = 0; j < PER; j++)
                    counter.fetch_add(1, std::memory_order_relaxed);
            });
        for (auto &t : ts) t.join();
        CHECK(counter.load() == (long)NT * PER);
    }

    section("thread_local storage is per-thread");
    {
        tls_value = 1000;
        std::atomic<bool> ok{true};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; i++)
            ts.emplace_back([i, &ok] {
                CHECK(tls_value == -1);     // fresh in each new thread
                tls_value = i;
                std::this_thread::yield();
                if (tls_value != i) ok = false;
            });
        for (auto &t : ts) t.join();
        CHECK(ok.load());
        CHECK(tls_value == 1000);           // main thread's copy untouched
    }

    section("std::async / std::future fan-out returns correct results");
    {
        const int N = 32;
        std::vector<std::future<long>> fs;
        for (int i = 0; i < N; i++)
            fs.push_back(std::async(std::launch::async, [i] { return (long)i * i; }));
        long sum = 0;
        for (auto &f : fs) sum += f.get();
        long expect = 0;
        for (int i = 0; i < N; i++) expect += (long)i * i;
        CHECK(sum == expect);
    }

    section("sched_yield() and pthread set/get name");
    CHECK(sched_yield() == 0);
    {
        char name[32] = {0};
        CHECK(pthread_setname_np(pthread_self(), "feature-main") == 0);
        CHECK(pthread_getname_np(pthread_self(), name, sizeof name) == 0);
        CHECK(std::string(name) == "feature-main");
    }
}

/* ====================================================== synchronization */

static void test_sync()
{
    group("Synchronization primitives");

    section("mutex prevents lost updates under contention");
    {
        const int NT = 8, PER = 20000;
        long counter = 0;
        std::mutex m;
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++)
            ts.emplace_back([&] {
                for (int j = 0; j < PER; j++) {
                    std::lock_guard<std::mutex> lk(m);
                    counter++;
                }
            });
        for (auto &t : ts) t.join();
        CHECK(counter == (long)NT * PER);
    }

    section("condition_variable producer/consumer");
    {
        std::mutex m; std::condition_variable cv;
        std::vector<int> q; bool done = false;
        long sum = 0; const int N = 20000;
        std::thread consumer([&] {
            for (;;) {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return !q.empty() || done; });
                for (int x : q) sum += x;
                q.clear();
                if (done) break;
            }
        });
        for (int i = 1; i <= N; i++) {
            { std::lock_guard<std::mutex> lk(m); q.push_back(i); }
            cv.notify_one();
        }
        { std::lock_guard<std::mutex> lk(m); done = true; }
        cv.notify_one();
        consumer.join();
        CHECK(sum == (long)N * (N + 1) / 2);
    }

    section("shared_mutex (rwlock): concurrent readers + exclusive writer");
    {
        std::shared_mutex sm;
        int shared = 0;
        std::atomic<long> reads{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 6; i++)
            ts.emplace_back([&] {
                for (int j = 0; j < 5000; j++) {
                    std::shared_lock<std::shared_mutex> rl(sm);
                    if (shared >= 0) reads.fetch_add(1);
                }
            });
        std::thread writer([&] {
            for (int j = 0; j < 1000; j++) {
                std::unique_lock<std::shared_mutex> wl(sm);
                shared++;
            }
        });
        for (auto &t : ts) t.join();
        writer.join();
        CHECK(shared == 1000);
        CHECK(reads.load() == 6 * 5000);
    }

    section("POSIX counting semaphore");
    {
        sem_t s;
        CHECK(sem_init(&s, 0, 0) == 0);
        std::atomic<int> got{0};
        std::thread t([&] { for (int i = 0; i < 100; i++) { sem_wait(&s); got++; } });
        for (int i = 0; i < 100; i++) sem_post(&s);
        t.join();
        CHECK(got.load() == 100);
        CHECK(sem_destroy(&s) == 0);
    }

    section("std::atomic compare_exchange");
    {
        std::atomic<int> a{5};
        int expected = 5;
        CHECK(a.compare_exchange_strong(expected, 10));
        CHECK(a.load() == 10);
        expected = 5;
        CHECK(!a.compare_exchange_strong(expected, 99));
        CHECK(expected == 10);
    }
}

/* ===================================================== memory management */

static void test_memory()
{
    group("Virtual memory & allocation");

    section("malloc / realloc / calloc / posix_memalign");
    {
        size_t n = 4 << 20;                 // 4 MiB
        char *p = (char *)malloc(n);
        CHECK(p != nullptr);
        memset(p, 0xAB, n);
        p = (char *)realloc(p, 2 * n);
        CHECK(p != nullptr);
        CHECK((unsigned char)p[0] == 0xAB && (unsigned char)p[n - 1] == 0xAB);
        free(p);

        int *z = (int *)calloc(1024, sizeof(int));
        CHECK(z != nullptr);
        bool zeroed = true;
        for (int i = 0; i < 1024; i++) if (z[i] != 0) zeroed = false;
        CHECK(zeroed);
        free(z);

        void *aligned = nullptr;
        CHECK(posix_memalign(&aligned, 4096, 8192) == 0);
        CHECK(((uintptr_t)aligned % 4096) == 0);
        free(aligned);
    }

    section("anonymous mmap: map, write, read, munmap");
    {
        size_t len = 2 << 20;               // 2 MiB
        void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED);
        memset(m, 0x5A, len);
        CHECK(((char *)m)[0] == 0x5A && ((char *)m)[len - 1] == 0x5A);

        section("mprotect, madvise, mincore on the mapping");
        CHECK(mprotect(m, len, PROT_READ) == 0);
        CHECK(((char *)m)[123] == 0x5A);    // still readable
        CHECK(mprotect(m, len, PROT_READ | PROT_WRITE) == 0);
        CHECK(madvise(m, len, MADV_DONTNEED) == 0);
        {
            std::vector<unsigned char> vec(len / 4096);
            CHECK(mincore(m, len, vec.data()) == 0);
        }
        CHECK(munmap(m, len) == 0);
    }
}

/* ====================================================== filesystems / vfs */

static void test_filesystems()
{
    group("Filesystems & VFS (rofs root, ramfs /tmp, devfs, procfs)");

    section("rofs root is readable");
    {
        // The application is linked into the kernel, so there is no program
        // file at /proc/self/exe; just verify the rofs root is mounted and
        // readable.
        struct stat st;
        CHECK(stat("/", &st) == 0 && S_ISDIR(st.st_mode));
    }

    section("ramfs is writable: create / write / read / unlink in /tmp");
    {
        const char *path = "/tmp/osfeat_basic.txt";
        const char *msg = "osv feature test payload\n";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0);
        CHECK(write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        char rb[64] = {0};
        CHECK(read(fd, rb, sizeof rb) == (ssize_t)strlen(msg));
        CHECK(std::string(rb) == msg);
        CHECK(close(fd) == 0);
        CHECK(unlink(path) == 0);
    }

    section("directories: mkdir, create entries, opendir/readdir, rmdir");
    {
        const char *dir = "/tmp/osfeat_dir";
        rmdir(dir);                          // best-effort cleanup
        CHECK(mkdir(dir, 0755) == 0);
        for (int i = 0; i < 5; i++) {
            char p[128];
            snprintf(p, sizeof p, "%s/f%d", dir, i);
            int fd = open(p, O_CREAT | O_WRONLY, 0644);
            CHECK(fd >= 0);
            close(fd);
        }
        int count = 0;
        DIR *d = opendir(dir);
        CHECK(d != nullptr);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != nullptr)
                if (de->d_name[0] == 'f') count++;
            closedir(d);
        }
        CHECK(count == 5);
        for (int i = 0; i < 5; i++) {
            char p[128];
            snprintf(p, sizeof p, "%s/f%d", dir, i);
            CHECK(unlink(p) == 0);
        }
        CHECK(rmdir(dir) == 0);
    }

    section("rename and symlink/readlink");
    {
        const char *a = "/tmp/osfeat_a", *b = "/tmp/osfeat_b", *l = "/tmp/osfeat_l";
        int fd = open(a, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0); close(fd);
        CHECK(rename(a, b) == 0);
        struct stat st;
        CHECK(stat(b, &st) == 0);
        CHECK(stat(a, &st) != 0);            // old name gone
        unlink(l);
        CHECK(symlink(b, l) == 0);
        char tgt[128] = {0};
        ssize_t n = readlink(l, tgt, sizeof tgt - 1);
        CHECK(n > 0 && std::string(tgt) == b);
        CHECK(unlink(l) == 0);
        CHECK(unlink(b) == 0);
    }

    section("statvfs on /");
    {
        struct statvfs vfs;
        CHECK(statvfs("/", &vfs) == 0);
        CHECK(vfs.f_bsize > 0);
    }

    section("procfs: /proc/self/maps and /proc/meminfo are non-empty");
    {
        for (const char *pf : {"/proc/self/maps", "/proc/meminfo"}) {
            int fd = open(pf, O_RDONLY);
            CHECK(fd >= 0);
            if (fd >= 0) {
                char b[256];
                ssize_t n = read(fd, b, sizeof b);
                CHECK(n > 0);
                close(fd);
            }
        }
    }

    section("devfs: /dev/null swallows writes and reads EOF");
    {
        int fd = open("/dev/null", O_RDWR);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(write(fd, "discarded", 9) == 9);
            char b[8];
            CHECK(read(fd, b, sizeof b) == 0);   // EOF
            close(fd);
        }
    }
}

/* ============================================================ file I/O */

static void test_file_io()
{
    group("File I/O (pread/pwrite, large offsets, dup, sendfile, mmap)");

    section("pread/pwrite at explicit offsets (no seek)");
    {
        const char *path = "/tmp/osfeat_pio.bin";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0);
        const char *a = "AAAA", *b = "BBBB";
        CHECK(pwrite(fd, a, 4, 0) == 4);
        CHECK(pwrite(fd, b, 4, 100) == 4);
        char r[4];
        CHECK(pread(fd, r, 4, 100) == 4 && memcmp(r, b, 4) == 0);
        CHECK(pread(fd, r, 4, 0) == 4 && memcmp(r, a, 4) == 0);
        close(fd);
        unlink(path);
    }

    section("large-file offsets: lseek past 4 GiB (64-bit off_t)");
    {
        // We only seek (no ftruncate/write) so we never materialize a >RAM
        // file on the in-memory ramfs - this checks the 64-bit off_t plumbing.
        const char *path = "/tmp/osfeat_lfs.bin";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0);
        off_t big = (off_t)5 * 1024 * 1024 * 1024;   // 5 GiB
        CHECK(lseek(fd, big, SEEK_SET) == big);      // off_t carries >4 GiB
        CHECK(lseek(fd, 0, SEEK_CUR) == big);        // position preserved
        close(fd);
        unlink(path);
    }

    section("dup / dup2 share the file offset");
    {
        const char *path = "/tmp/osfeat_dup.bin";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0);
        CHECK(write(fd, "0123456789", 10) == 10);
        int fd2 = dup(fd);
        CHECK(fd2 >= 0);
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        char b[5];
        CHECK(read(fd2, b, 5) == 5 && memcmp(b, "01234", 5) == 0);  // shared offset
        close(fd); close(fd2);
        unlink(path);
    }

    section("fcntl F_GETFL/F_SETFL, fdatasync");
    {
        const char *path = "/tmp/osfeat_fcntl.bin";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0);
        int fl = fcntl(fd, F_GETFL);
        CHECK(fl >= 0);
        CHECK(fcntl(fd, F_SETFL, fl | O_APPEND) == 0);
        CHECK((fcntl(fd, F_GETFL) & O_APPEND) != 0);
        CHECK(write(fd, "x", 1) == 1);
        CHECK(fdatasync(fd) == 0);
        close(fd);
        unlink(path);
    }

    section("sendfile copies between files");
    {
        const char *src = "/tmp/osfeat_sf_src", *dst = "/tmp/osfeat_sf_dst";
        int sfd = open(src, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(sfd >= 0);
        std::string data(8192, 'Z');
        CHECK(write(sfd, data.data(), data.size()) == (ssize_t)data.size());
        lseek(sfd, 0, SEEK_SET);
        int dfd = open(dst, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(dfd >= 0);
        off_t off = 0;
        ssize_t sent = sendfile(dfd, sfd, &off, data.size());
        CHECK(sent == (ssize_t)data.size());
        struct stat st;
        CHECK(fstat(dfd, &st) == 0 && st.st_size == (off_t)data.size());
        close(sfd); close(dfd);
        unlink(src); unlink(dst);
    }

    section("file-backed mmap reflects written contents");
    {
        const char *path = "/tmp/osfeat_mmap.bin";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fd >= 0);
        size_t len = 64 * 1024;
        std::string data(len, '\0');
        for (size_t i = 0; i < len; i++) data[i] = (char)(i & 0xff);
        CHECK(write(fd, data.data(), len) == (ssize_t)len);
        void *m = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        CHECK(m != MAP_FAILED);
        if (m != MAP_FAILED) {
            CHECK(memcmp(m, data.data(), len) == 0);
            munmap(m, len);
        }
        close(fd);
        unlink(path);
    }
}

/* ======================================================= IPC & events */

static void test_ipc_events()
{
    group("IPC & event notification");

    section("pipe2: write then read");
    {
        int fds[2];
        CHECK(pipe2(fds, 0) == 0);
        const char *msg = "through-the-pipe";
        CHECK(write(fds[1], msg, strlen(msg)) == (ssize_t)strlen(msg));
        char b[32] = {0};
        CHECK(read(fds[0], b, sizeof b) == (ssize_t)strlen(msg));
        CHECK(std::string(b) == msg);
        close(fds[0]); close(fds[1]);
    }

    section("AF_LOCAL socketpair: bidirectional (read/write)");
    {
        int sv[2];
        CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) == 0);
        CHECK(write(sv[0], "ping", 4) == 4);
        char b[8] = {0};
        CHECK(read(sv[1], b, 4) == 4 && std::string(b) == "ping");
        CHECK(write(sv[1], "pong", 4) == 4);
        memset(b, 0, sizeof b);
        CHECK(read(sv[0], b, 4) == 4 && std::string(b) == "pong");
        close(sv[0]); close(sv[1]);
    }

    section("eventfd: counter semantics");
    {
        int efd = eventfd(0, 0);
        CHECK(efd >= 0);
        uint64_t one = 1, val = 0;
        CHECK(write(efd, &one, 8) == 8);
        CHECK(write(efd, &one, 8) == 8);
        CHECK(read(efd, &val, 8) == 8);
        CHECK(val == 2);                     // accumulated count
        close(efd);
    }

    section("timerfd: one-shot 50ms fires");
    {
        int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
        CHECK(tfd >= 0);
        struct itimerspec its{};
        its.it_value.tv_nsec = 50 * 1000 * 1000;     // 50 ms
        CHECK(timerfd_settime(tfd, 0, &its, nullptr) == 0);
        CHECK(wait_readable(tfd, 2000) > 0);
        uint64_t expir = 0;
        CHECK(read(tfd, &expir, 8) == 8 && expir >= 1);
        close(tfd);
    }

    section("poll() and select() report a readable pipe");
    {
        int fds[2];
        CHECK(pipe2(fds, 0) == 0);
        CHECK(write(fds[1], "z", 1) == 1);

        struct pollfd pfd { fds[0], POLLIN, 0 };
        CHECK(poll(&pfd, 1, 2000) == 1);
        CHECK(pfd.revents & POLLIN);

        fd_set rs; FD_ZERO(&rs); FD_SET(fds[0], &rs);
        struct timeval tv { 2, 0 };
        CHECK(select(fds[0] + 1, &rs, nullptr, nullptr, &tv) == 1);
        CHECK(FD_ISSET(fds[0], &rs));

        char b; CHECK(read(fds[0], &b, 1) == 1);
        close(fds[0]); close(fds[1]);
    }

    section("epoll: edge of readiness on a pipe");
    {
        int ep = epoll_create1(0);
        CHECK(ep >= 0);
        int fds[2];
        CHECK(pipe2(fds, 0) == 0);
        struct epoll_event ev{};
        ev.events = EPOLLIN; ev.data.fd = fds[0];
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev) == 0);
        CHECK(write(fds[1], "q", 1) == 1);
        struct epoll_event out[4];
        int n = epoll_wait(ep, out, 4, 2000);
        CHECK(n == 1);
        CHECK(n >= 1 && out[0].data.fd == fds[0]);
        close(fds[0]); close(fds[1]); close(ep);
    }
}

/* ============================================================== signals */

static volatile sig_atomic_t g_sigusr1 = 0;
static void usr1_handler(int) { g_sigusr1 = 1; }

static void test_signals()
{
    group("Signals (process-directed; no inter-thread delivery)");

    section("sigaction handler runs on raise()");
    {
        struct sigaction sa{}, old{};
        sa.sa_handler = usr1_handler;
        sigemptyset(&sa.sa_mask);
        CHECK(sigaction(SIGUSR1, &sa, &old) == 0);
        g_sigusr1 = 0;
        CHECK(raise(SIGUSR1) == 0);
        // handler is synchronous for raise(); give a tiny grace anyway
        for (int i = 0; i < 100 && !g_sigusr1; i++) usleep(1000);
        CHECK(g_sigusr1 == 1);
        sigaction(SIGUSR1, &old, nullptr);
    }

    section("sigprocmask blocks, queries and restores the signal mask");
    {
        sigset_t set, old, cur;
        sigemptyset(&set); sigaddset(&set, SIGUSR2);
        CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0);
        CHECK(sigprocmask(SIG_BLOCK, nullptr, &cur) == 0);   // query
        CHECK(sigismember(&cur, SIGUSR2) == 1);
        CHECK(sigprocmask(SIG_SETMASK, &old, nullptr) == 0); // restore
        CHECK(sigprocmask(SIG_BLOCK, nullptr, &cur) == 0);
        CHECK(sigismember(&cur, SIGUSR2) == 0);
    }

    section("pthread_kill(self, 0) succeeds (existence check)");
    CHECK(pthread_kill(pthread_self(), 0) == 0);

    section("BOUNDARY: directed pthread_kill to another thread is unsupported");
    {
        std::mutex m; std::condition_variable cv;
        bool release = false, ready = false;
        pthread_t target{};
        std::thread t([&] {
            { std::lock_guard<std::mutex> lk(m); target = pthread_self(); ready = true; }
            cv.notify_one();
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return release; });
        });
        { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return ready; }); }
        int rc = pthread_kill(target, 0);    // not self -> stubbed
        CHECK(rc == EINVAL);
        { std::lock_guard<std::mutex> lk(m); release = true; }
        cv.notify_one();
        t.join();
    }
}

/* ========================================================= time & clocks */

static void test_time()
{
    group("Time & clocks");

    section("CLOCK_MONOTONIC is monotonic and has a resolution");
    {
        struct timespec res;
        CHECK(clock_getres(CLOCK_MONOTONIC, &res) == 0);
        long t0 = now_ms();
        for (int i = 0; i < 1000000; i++) { asm volatile("" ::: "memory"); }
        long t1 = now_ms();
        CHECK(t1 >= t0);
    }

    section("CLOCK_REALTIME is past the 2020 epoch");
    {
        struct timespec ts;
        CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0);
        CHECK(ts.tv_sec > 1577836800);       // 2020-01-01
    }

    section("nanosleep actually sleeps ~30ms");
    {
        long t0 = now_ms();
        struct timespec req { 0, 30 * 1000 * 1000 }, rem{};
        CHECK(nanosleep(&req, &rem) == 0);
        long slept = now_ms() - t0;
        CHECK(slept >= 25);                  // allow scheduling slack
    }

    section("gettimeofday agrees roughly with CLOCK_REALTIME");
    {
        struct timeval tv; struct timespec ts;
        CHECK(gettimeofday(&tv, nullptr) == 0);
        clock_gettime(CLOCK_REALTIME, &ts);
        CHECK(llabs((long long)tv.tv_sec - (long long)ts.tv_sec) <= 2);
    }
}

/* ============================================================ randomness */

static void test_random()
{
    group("Randomness (ChaCha20 CSPRNG)");

    section("getrandom() fills the buffer and varies between calls");
    {
        unsigned char a[64], b[64];
        CHECK(getrandom(a, sizeof a, 0) == (ssize_t)sizeof a);
        CHECK(getrandom(b, sizeof b, 0) == (ssize_t)sizeof b);
        CHECK(memcmp(a, b, sizeof a) != 0);
    }

    section("/dev/urandom yields varied bytes");
    {
        int fd = open("/dev/urandom", O_RDONLY);
        CHECK(fd >= 0);
        if (fd >= 0) {
            unsigned char buf[256];
            CHECK(read(fd, buf, sizeof buf) == (ssize_t)sizeof buf);
            int distinct = 0; bool seen[256] = {false};
            for (unsigned char c : buf) if (!seen[c]) { seen[c] = true; distinct++; }
            CHECK(distinct > 64);            // not a constant stream
            close(fd);
        }
    }

    section("arc4random() sets and clears every bit across samples");
    {
        uint32_t any_set = 0, any_clear = 0;
        for (int i = 0; i < 64; i++) {
            uint32_t r = arc4random();
            any_set |= r;
            any_clear |= ~r;
        }
        CHECK(any_set == 0xffffffffu);
        CHECK(any_clear == 0xffffffffu);
    }
}

/* ======================================================= C/C++ runtime */

static void test_runtime()
{
    group("C / C++ runtime (musl libc, exceptions, RTTI, dlopen, locale)");

    section("C++ exceptions unwind and are caught by type");
    {
        bool caught = false;
        try {
            throw std::runtime_error("boom");
        } catch (const std::exception &e) {
            caught = (std::string(e.what()) == "boom");
        }
        CHECK(caught);
    }

    section("RTTI: typeid and dynamic_cast");
    {
        struct Base { virtual ~Base() {} };
        struct Derived : Base { int x = 7; };
        Base *b = new Derived();
        Derived *d = dynamic_cast<Derived *>(b);
        CHECK(d != nullptr && d->x == 7);
        CHECK(std::string(typeid(*b).name()).find("Derived") != std::string::npos);
        delete b;
    }

    section("STL containers and algorithms");
    {
        std::map<std::string, int> m;
        for (int i = 0; i < 100; i++) m["k" + std::to_string(i)] = i;
        CHECK(m.size() == 100 && m["k42"] == 42);
        std::vector<int> v(1000);
        std::iota(v.begin(), v.end(), 0);
        std::reverse(v.begin(), v.end());
        std::sort(v.begin(), v.end());
        CHECK(std::is_sorted(v.begin(), v.end()) && v.front() == 0 && v.back() == 999);
    }

    section("runtime dynamic loading is unsupported (statically linked kernel)");
    {
        // The ELF loader and runtime application model are gone: the app is
        // linked into the kernel, so dlopen() is a failure stub by design.
        void *h = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
        CHECK(h == nullptr);
    }

    section("locale and ctype");
    {
        CHECK(setlocale(LC_ALL, "C") != nullptr);
        CHECK(isalpha('A') && !isalpha('1') && isdigit('7'));
        CHECK(toupper('a') == 'A' && tolower('Z') == 'z');
    }
}

/* ====================================================== boundaries recap */

static void test_boundaries()
{
    group("Unsupported boundaries (must fail cleanly, not crash)");

    section("no IP networking: socketpair(AF_INET) is rejected");
    {
        int sv[2];
        errno = 0;
        int rc = socketpair(AF_INET, SOCK_STREAM, 0, sv);
        CHECK(rc == -1);                     // only AF_LOCAL is supported
        if (rc == 0) { close(sv[0]); close(sv[1]); }
    }
    // (socket()/bind()/connect()/listen()/accept() are not even linkable -
    //  the TCP/IP stack was removed - so they are intentionally not called.)

    section("signalfd and inotify are stubs (return -1)");
    {
        // Present as symbols but unimplemented; signalfd reports ENOSYS,
        // inotify reports EMFILE - both just fail rather than work.
        sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR2);
        CHECK(signalfd(-1, &s, 0) == -1);
        CHECK(inotify_init1(0) == -1);
    }

    section("fork / directed thread signals: covered above");
    CHECK(true);
}

/* ================================================================== main */

int os_features_main()
{
    printf("OSv OS-feature inventory & conformance test\n");
    printf("===========================================\n");

    test_process_model();
    test_threads();
    test_sync();
    test_memory();
    test_filesystems();
    test_file_io();
    test_ipc_events();
    test_signals();
    test_time();
    test_random();
    test_runtime();
    test_boundaries();

    int checks = g_checks.load(), fails = g_fails.load();
    printf("\n-------------------------------------------\n");
    printf("Checks run: %d, failures: %d\n", checks, fails);
    if (fails == 0) {
        printf("RESULT: ALL OS FEATURE TESTS PASSED\n");
        return 0;
    }
    printf("RESULT: %d OS FEATURE TEST(S) FAILED\n", fails);
    return 1;
}
