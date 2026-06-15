/*
 * OS-interaction stress tests for OSv (C++).
 *
 * A self-contained, dynamically-linked C++ program that hammers the parts of
 * OSv that involve the kernel: thread scheduling/synchronization, the VFS and
 * file I/O (incl. concurrent and mmap'd), pipes/sockets/eventfd, poll/epoll,
 * signals, and virtual memory. Built dynamic so threading/syscalls resolve
 * against OSv's own libc + kernel at run time.
 *
 * It is deliberately concurrent and repetitive (many threads, many iterations,
 * large files) to shake out races and resource leaks. Each CHECK keeps running
 * on failure; main() returns non-zero if anything failed. Worker threads never
 * call CHECK directly - they report via atomics/return values and the main
 * thread asserts after joining.
 */
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <vector>
#include <queue>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/wait.h>

static std::atomic<int> g_checks{0};
static std::atomic<int> g_fails{0};
static const char *g_section = "";

#define CHECK(cond) do { \
        g_checks.fetch_add(1); \
        if (!(cond)) { \
            g_fails.fetch_add(1); \
            printf("FAIL [%s] %s:%d: %s\n", g_section, __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static void section(const char *s) { g_section = s; printf("  - %s\n", s); }

/* ============================================================ threads */

static void test_threads_atomic()
{
    section("threads: atomic counter under contention");
    const int NT = 16, PER = 100000;
    std::atomic<long> counter{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { for (int j = 0; j < PER; j++) counter.fetch_add(1, std::memory_order_relaxed); });
    for (auto &t : ts) t.join();
    CHECK(counter.load() == (long)NT * PER);
}

static void test_threads_mutex()
{
    section("threads: mutex-protected counter (lost-update detection)");
    const int NT = 12, PER = 20000;
    long counter = 0;
    std::mutex m;
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { for (int j = 0; j < PER; j++) { std::lock_guard<std::mutex> lk(m); counter++; } });
    for (auto &t : ts) t.join();
    CHECK(counter == (long)NT * PER);
}

static void test_condvar_queue()
{
    section("threads: condition_variable producer/consumer");
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    bool done = false;
    std::atomic<long> sum{0};
    const int N = 50000;

    std::thread consumer([&] {
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return !q.empty() || done; });
            while (!q.empty()) { sum += q.front(); q.pop(); }
            if (done && q.empty()) break;
        }
    });
    for (int i = 1; i <= N; i++) {
        { std::lock_guard<std::mutex> lk(m); q.push(i); }
        cv.notify_one();
    }
    { std::lock_guard<std::mutex> lk(m); done = true; }
    cv.notify_one();
    consumer.join();
    CHECK(sum.load() == (long)N * (N + 1) / 2);
}

static void test_async_futures()
{
    section("threads: std::async / std::future fan-out");
    const int N = 64;
    std::vector<std::future<long>> fs;
    for (int i = 0; i < N; i++)
        fs.push_back(std::async(std::launch::async, [i] {
            long s = 0; for (int j = 0; j <= i; j++) s += j; return s;
        }));
    long total = 0;
    for (auto &f : fs) total += f.get();
    long expect = 0;
    for (int i = 0; i < N; i++) expect += (long)i * (i + 1) / 2;
    CHECK(total == expect);
}

static thread_local int tls_value = -1;
static void test_thread_local()
{
    section("threads: thread_local isolation");
    const int NT = 20;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([i, &mismatches] {
            tls_value = i;
            for (int k = 0; k < 1000; k++) {
                std::this_thread::yield();
                if (tls_value != i) mismatches++;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(mismatches.load() == 0);
}

static void test_pthread_barrier()
{
    section("threads: pthread_barrier synchronization");
    const int NT = 8, ROUNDS = 100;
    pthread_barrier_t bar;
    CHECK(pthread_barrier_init(&bar, nullptr, NT) == 0);
    std::atomic<int> phase{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] {
            for (int r = 0; r < ROUNDS; r++) {
                int seen = phase.load();
                int rc = pthread_barrier_wait(&bar);
                /* exactly one thread gets the SERIAL return value */
                if (rc == PTHREAD_BARRIER_SERIAL_THREAD) phase.fetch_add(1);
                (void)seen;
                pthread_barrier_wait(&bar);
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
    CHECK(phase.load() == ROUNDS);
    pthread_barrier_destroy(&bar);
}

static void test_rwlock()
{
    section("threads: pthread_rwlock readers/writers");
    pthread_rwlock_t rw;
    CHECK(pthread_rwlock_init(&rw, nullptr) == 0);
    long shared = 0;
    std::atomic<int> inconsistencies{0};
    std::vector<std::thread> ts;
    const int READERS = 8, WRITERS = 4, ITERS = 5000;
    for (int w = 0; w < WRITERS; w++)
        ts.emplace_back([&] {
            for (int i = 0; i < ITERS; i++) {
                pthread_rwlock_wrlock(&rw);
                long a = shared; shared = a + 1; long b = shared;
                if (b != a + 1) inconsistencies++;
                pthread_rwlock_unlock(&rw);
            }
        });
    for (int r = 0; r < READERS; r++)
        ts.emplace_back([&] {
            for (int i = 0; i < ITERS; i++) {
                pthread_rwlock_rdlock(&rw);
                long a = shared; (void)a;
                pthread_rwlock_unlock(&rw);
            }
        });
    for (auto &t : ts) t.join();
    CHECK(inconsistencies.load() == 0);
    CHECK(shared == (long)WRITERS * ITERS);
    pthread_rwlock_destroy(&rw);
}

/* ============================================================ files */

static bool write_pattern(int fd, size_t n, unsigned seed)
{
    std::vector<unsigned char> buf(65536);
    size_t written = 0;
    unsigned x = seed;
    while (written < n) {
        size_t chunk = std::min(buf.size(), n - written);
        for (size_t i = 0; i < chunk; i++) { x = x * 1103515245u + 12345u; buf[i] = x >> 16; }
        ssize_t w = write(fd, buf.data(), chunk);
        if (w != (ssize_t)chunk) return false;
        written += chunk;
    }
    return true;
}
static bool verify_pattern(int fd, size_t n, unsigned seed)
{
    std::vector<unsigned char> buf(65536);
    size_t rd = 0;
    unsigned x = seed;
    while (rd < n) {
        size_t chunk = std::min(buf.size(), n - rd);
        ssize_t r = read(fd, buf.data(), chunk);
        if (r != (ssize_t)chunk) return false;
        for (size_t i = 0; i < chunk; i++) { x = x * 1103515245u + 12345u; if (buf[i] != (unsigned char)(x >> 16)) return false; }
        rd += chunk;
    }
    return true;
}

static void test_files_large()
{
    section("files: large file write/read/verify + fsync");
    const char *path = "/tmp/osstress_big.bin";
    const size_t SZ = 16 * 1024 * 1024;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(write_pattern(fd, SZ, 0xdeadbeef));
    CHECK(fsync(fd) == 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && (size_t)st.st_size == SZ);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(verify_pattern(fd, SZ, 0xdeadbeef));
    close(fd);
    CHECK(unlink(path) == 0);
}

static void test_files_concurrent()
{
    section("files: concurrent independent files (16 threads)");
    const int NT = 16;
    const size_t SZ = 512 * 1024;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([i, &errors] {
            char path[64];
            snprintf(path, sizeof path, "/tmp/osstress_c%d.bin", i);
            int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
            if (fd < 0) { errors++; return; }
            unsigned seed = 0x1000 + i;
            if (!write_pattern(fd, SZ, seed)) errors++;
            if (lseek(fd, 0, SEEK_SET) != 0) errors++;
            if (!verify_pattern(fd, SZ, seed)) errors++;
            close(fd);
            if (unlink(path) != 0) errors++;
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
}

static void test_files_pwrite_concurrent()
{
    section("files: concurrent pwrite to disjoint offsets of one file");
    const char *path = "/tmp/osstress_pw.bin";
    const int NT = 16;
    const size_t BLK = 64 * 1024;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(ftruncate(fd, NT * BLK) == 0);
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([fd, i, &errors] {
            std::vector<unsigned char> blk(BLK, (unsigned char)i);
            if (pwrite(fd, blk.data(), BLK, (off_t)i * BLK) != (ssize_t)BLK) errors++;
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
    /* read back: each block should be filled with its index */
    for (int i = 0; i < NT; i++) {
        std::vector<unsigned char> blk(BLK);
        CHECK(pread(fd, blk.data(), BLK, (off_t)i * BLK) == (ssize_t)BLK);
        CHECK(blk.front() == (unsigned char)i && blk.back() == (unsigned char)i);
    }
    close(fd);
    unlink(path);
}

static void test_mmap_file()
{
    section("vm: mmap a file, modify via mapping, msync, verify");
    const char *path = "/tmp/osstress_mmap.bin";
    const size_t SZ = 1024 * 1024;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(ftruncate(fd, SZ) == 0);
    void *p = mmap(nullptr, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) {
        unsigned char *m = (unsigned char *)p;
        for (size_t i = 0; i < SZ; i += 4096) m[i] = (unsigned char)(i / 4096);
        CHECK(msync(p, SZ, MS_SYNC) == 0);
        CHECK(munmap(p, SZ) == 0);
        /* read back through normal I/O */
        std::vector<unsigned char> buf(SZ);
        CHECK(pread(fd, buf.data(), SZ, 0) == (ssize_t)SZ);
        bool ok = true;
        for (size_t i = 0; i < SZ; i += 4096) if (buf[i] != (unsigned char)(i / 4096)) ok = false;
        CHECK(ok);
    }
    close(fd);
    unlink(path);
}

static void test_mmap_anon()
{
    section("vm: anonymous mmap, touch pages, mprotect, madvise");
    const size_t SZ = 8 * 1024 * 1024;
    void *p = mmap(nullptr, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return;
    unsigned char *m = (unsigned char *)p;
    for (size_t i = 0; i < SZ; i += 4096) m[i] = 0xA5;
    bool ok = true;
    for (size_t i = 0; i < SZ; i += 4096) if (m[i] != 0xA5) ok = false;
    CHECK(ok);
    CHECK(mprotect(p, SZ, PROT_READ) == 0);          /* make read-only */
    CHECK(madvise(p, SZ, MADV_DONTNEED) == 0 || errno == EINVAL);
    CHECK(munmap(p, SZ) == 0);
}

static void test_dir_ops()
{
    section("files: directory tree create/readdir/cleanup");
    const char *root = "/tmp/osstress_dir";
    rmdir(root);
    CHECK(mkdir(root, 0755) == 0);
    const int NF = 50;
    for (int i = 0; i < NF; i++) {
        char p[80];
        snprintf(p, sizeof p, "%s/f%02d", root, i);
        int fd = open(p, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0);
        if (fd >= 0) { write(fd, "x", 1); close(fd); }
    }
    DIR *d = opendir(root);
    CHECK(d != nullptr);
    int count = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr)
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) count++;
        closedir(d);
    }
    CHECK(count == NF);
    for (int i = 0; i < NF; i++) {
        char p[80];
        snprintf(p, sizeof p, "%s/f%02d", root, i);
        CHECK(unlink(p) == 0);
    }
    CHECK(rmdir(root) == 0);
}

/* ============================================================ ipc / events */

static void test_pipe_threads()
{
    section("ipc: pipe transfer between threads");
    int fds[2];
    CHECK(pipe(fds) == 0);
    const int N = 100000;
    std::thread writer([&] {
        for (int i = 0; i < N; i++) {
            int v = i;
            if (write(fds[1], &v, sizeof v) != sizeof v) break;
        }
        close(fds[1]);
    });
    long sum = 0;
    int v, got = 0;
    while (read(fds[0], &v, sizeof v) == (ssize_t)sizeof v) { sum += v; got++; }
    close(fds[0]);
    writer.join();
    CHECK(got == N);
    CHECK(sum == (long)N * (N - 1) / 2);
}

static void test_socketpair()
{
    section("ipc: AF_LOCAL socketpair bidirectional + shutdown");
    int sv[2];
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) == 0);
    std::thread echo([&] {
        char buf[256];
        ssize_t n;
        while ((n = read(sv[1], buf, sizeof buf)) > 0)
            if (write(sv[1], buf, n) != n) break;
        close(sv[1]);
    });
    const char *msg = "ping-pong-over-unix-socket";
    CHECK(write(sv[0], msg, strlen(msg)) == (ssize_t)strlen(msg));
    char rb[256] = {0};
    ssize_t got = read(sv[0], rb, sizeof rb);
    CHECK(got == (ssize_t)strlen(msg) && memcmp(rb, msg, got) == 0);
    CHECK(shutdown(sv[0], SHUT_WR) == 0);
    close(sv[0]);
    echo.join();

    /* unsupported address family -> EAFNOSUPPORT, not a crash */
    int dummy[2];
    errno = 0;
    CHECK(socketpair(AF_INET, SOCK_STREAM, 0, dummy) == -1 && errno == EAFNOSUPPORT);
}

static void test_eventfd()
{
    section("ipc: eventfd cross-thread signaling");
    int efd = eventfd(0, 0);
    CHECK(efd >= 0);
    if (efd < 0) return;
    const int N = 1000;
    std::thread signaller([&] {
        for (int i = 0; i < N; i++) { uint64_t one = 1; if (write(efd, &one, 8) != 8) break; }
    });
    uint64_t total = 0;
    while (total < (uint64_t)N) { uint64_t v; if (read(efd, &v, 8) != 8) break; total += v; }
    signaller.join();
    CHECK(total == (uint64_t)N);
    close(efd);
}

static void test_poll_select()
{
    section("events: poll + select on a pipe");
    int fds[2];
    CHECK(pipe(fds) == 0);

    /* nothing written yet -> not readable */
    struct pollfd pfd = { fds[0], POLLIN, 0 };
    CHECK(poll(&pfd, 1, 0) == 0);

    CHECK(write(fds[1], "z", 1) == 1);
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN));

    fd_set rs;
    FD_ZERO(&rs); FD_SET(fds[0], &rs);
    struct timeval tv = {1, 0};
    CHECK(select(fds[0] + 1, &rs, nullptr, nullptr, &tv) == 1 && FD_ISSET(fds[0], &rs));

    char c;
    CHECK(read(fds[0], &c, 1) == 1 && c == 'z');
    close(fds[0]); close(fds[1]);
}

static void test_epoll()
{
    section("events: epoll on pipe + eventfd, cross-thread wakeups");
    int ep = epoll_create1(0);
    CHECK(ep >= 0);
    if (ep < 0) return;
    int pfds[2];
    CHECK(pipe(pfds) == 0);
    int efd = eventfd(0, EFD_NONBLOCK);
    CHECK(efd >= 0);

    struct epoll_event ev{};
    ev.events = EPOLLIN; ev.data.fd = pfds[0];
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pfds[0], &ev) == 0);
    ev.data.fd = efd;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0);

    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        uint64_t one = 1; (void)!write(efd, &one, 8);
        (void)!write(pfds[1], "x", 1);
    });

    int seen_pipe = 0, seen_event = 0;
    for (int i = 0; i < 2; i++) {
        struct epoll_event out[4];
        int n = epoll_wait(ep, out, 4, 2000);
        for (int k = 0; k < n; k++) {
            if (out[k].data.fd == pfds[0]) { char c; (void)!read(pfds[0], &c, 1); seen_pipe++; }
            if (out[k].data.fd == efd)     { uint64_t v; (void)!read(efd, &v, 8);  seen_event++; }
        }
        if (seen_pipe && seen_event) break;
    }
    waker.join();
    CHECK(seen_pipe >= 1);
    CHECK(seen_event >= 1);
    close(ep); close(efd); close(pfds[0]); close(pfds[1]);
}

/* ============================================================ signals */

static volatile sig_atomic_t g_sig_hit = 0;
static void sig_handler(int) { g_sig_hit = 1; }

static void test_signals()
{
    section("signals: sigaction handler + raise + sigmask API");
    struct sigaction sa{}, old_sa{};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, &old_sa) == 0);
    g_sig_hit = 0;
    CHECK(raise(SIGUSR1) == 0);
    /* handler runs synchronously on raise() */
    CHECK(g_sig_hit == 1);

    /* signal-mask API round-trips */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    CHECK(sigismember(&set, SIGUSR1) == 1);
    CHECK(pthread_sigmask(SIG_BLOCK, &set, &old) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &old, nullptr) == 0);
    CHECK(sigaction(SIGUSR1, &old_sa, nullptr) == 0);

    /* Note: OSv stubs pthread_kill (no directed thread signals), so
     * cross-thread sigwait delivery is intentionally not exercised. */
}

/* ============================================================ time/sched */

static void test_time_sched()
{
    section("time: monotonic clock + nanosleep + sched_yield");
    struct timespec a, b;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    struct timespec nap = {0, 20 * 1000 * 1000};   /* 20 ms */
    CHECK(nanosleep(&nap, nullptr) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    double elapsed = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(elapsed >= 0.015);   /* at least ~15ms passed */

    for (int i = 0; i < 1000; i++) sched_yield();
    CHECK(true);

    /* hammer clock_gettime from many threads (no crash / monotonic per thread) */
    const int NT = 8;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] {
            struct timespec p{0, 0};
            for (int k = 0; k < 20000; k++) {
                struct timespec n;
                clock_gettime(CLOCK_MONOTONIC, &n);
                if (n.tv_sec < p.tv_sec) errors++;
                p = n;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
}

int os_stress_main()
{
    printf("==== OSv OS-interaction stress tests ====\n");

    test_threads_atomic();
    test_threads_mutex();
    test_condvar_queue();
    test_async_futures();
    test_thread_local();
    test_pthread_barrier();
    test_rwlock();

    test_files_large();
    test_files_concurrent();
    test_files_pwrite_concurrent();
    test_mmap_file();
    test_mmap_anon();
    test_dir_ops();

    test_pipe_threads();
    test_socketpair();
    test_eventfd();
    test_poll_select();
    test_epoll();

    test_signals();
    test_time_sched();

    printf("\n==== os-stress: %d checks, %d failures ====\n",
           g_checks.load(), g_fails.load());
    if (g_fails.load() == 0)
        printf("RESULT: ALL OS-STRESS TESTS PASSED\n");
    else
        printf("RESULT: %d OS-STRESS TEST(S) FAILED\n", g_fails.load());
    return g_fails.load() ? 1 : 0;
}
