// The OSv application.
//
// Unlike upstream OSv, the application is not a separate ELF shared object
// loaded at runtime from a filesystem image. It is compiled and statically
// linked directly into the kernel image (app.o in loader.elf). The kernel
// calls osv_app_main() once, after early initialization, on a dedicated thread.
//
// For the slimming work the in-kernel application is the os-features breadth
// conformance test (Phase 0 guardrail, see REFACTORING.md): it exercises the
// facilities the refactoring promises to preserve - threads, memory mapping,
// time, and the libc surface - and nothing it does should ever stop working as
// subsystems are deleted. It prints a PASS/FAIL line per check and a final
// tally, then powers the machine off so a run exits cleanly.

#include <atomic>
#include <cerrno>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <osv/power.hh>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

// ---------------------------------------------------------------------------
// Threads: creation/join, mutex, condition variable, TLS.
// ---------------------------------------------------------------------------

static std::atomic<int> g_counter{0};

static void *worker(void *arg)
{
    long n = (long)arg;
    for (long i = 0; i < n; i++) {
        g_counter.fetch_add(1, std::memory_order_relaxed);
    }
    return (void *)(n * 2);
}

static void test_threads()
{
    const int N = 8;
    const long per = 10000;
    pthread_t t[N];
    g_counter.store(0);
    bool created = true;
    for (int i = 0; i < N; i++) {
        if (pthread_create(&t[i], nullptr, worker, (void *)per) != 0) {
            created = false;
        }
    }
    long sum = 0;
    for (int i = 0; i < N; i++) {
        void *r = nullptr;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    check("thread create/join", created);
    check("thread shared atomic count", g_counter.load() == N * per);
    check("thread return values", sum == (long)N * per * 2);

    // mutex + condition variable handshake
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    bool ready = false;
    struct ctx { pthread_mutex_t *m; pthread_cond_t *c; bool *ready; } cx{&m, &c, &ready};
    pthread_t signaller;
    pthread_create(&signaller, nullptr, [](void *a) -> void * {
        auto *x = (ctx *)a;
        pthread_mutex_lock(x->m);
        *x->ready = true;
        pthread_cond_signal(x->c);
        pthread_mutex_unlock(x->m);
        return nullptr;
    }, &cx);
    pthread_mutex_lock(&m);
    while (!ready) {
        pthread_cond_wait(&c, &m);
    }
    pthread_mutex_unlock(&m);
    pthread_join(signaller, nullptr);
    check("mutex + condition variable", ready);

    // thread-local storage isolation
    static thread_local int tls = 0;
    tls = 42;
    bool tls_ok = true;
    pthread_t tt;
    pthread_create(&tt, nullptr, [](void *r) -> void * {
        static thread_local int local = 0;
        *(bool *)r = (local == 0); // fresh in the new thread
        local = 7;
        return nullptr;
    }, &tls_ok);
    pthread_join(tt, nullptr);
    check("thread-local storage isolation", tls_ok && tls == 42);
}

// ---------------------------------------------------------------------------
// Memory: anonymous mmap, mprotect, munmap.
// ---------------------------------------------------------------------------

static void test_mmap()
{
    const size_t len = 256 * 1024;
    void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap anonymous", p != MAP_FAILED);
    if (p == MAP_FAILED) {
        return;
    }
    auto *bytes = (volatile unsigned char *)p;
    for (size_t i = 0; i < len; i += 4096) {
        bytes[i] = (unsigned char)i;
    }
    bool readback = true;
    for (size_t i = 0; i < len; i += 4096) {
        if (bytes[i] != (unsigned char)i) {
            readback = false;
        }
    }
    check("mmap read/write across pages", readback);
    check("mprotect read-only", mprotect(p, len, PROT_READ) == 0);
    check("munmap", munmap(p, len) == 0);
}

// ---------------------------------------------------------------------------
// Time: clocks are monotonic / advance, nanosleep waits.
// ---------------------------------------------------------------------------

static void test_time()
{
    struct timespec a{}, b{};
    bool got = clock_gettime(CLOCK_MONOTONIC, &a) == 0;
    struct timespec req{0, 5 * 1000 * 1000}; // 5ms
    nanosleep(&req, nullptr);
    got = got && clock_gettime(CLOCK_MONOTONIC, &b) == 0;
    check("clock_gettime(CLOCK_MONOTONIC)", got);
    double da = a.tv_sec + a.tv_nsec / 1e9;
    double db = b.tv_sec + b.tv_nsec / 1e9;
    check("monotonic clock advances over nanosleep", db > da);

    struct timespec rt{};
    check("clock_gettime(CLOCK_REALTIME)", clock_gettime(CLOCK_REALTIME, &rt) == 0);

    // gmtime/mktime round-trip on a fixed epoch.
    time_t epoch = 1700000000; // 2023-11-14T22:13:20Z
    struct tm tmv{};
    gmtime_r(&epoch, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    check("gmtime_r + strftime", strcmp(buf, "2023-11-14T22:13:20") == 0);
}

// ---------------------------------------------------------------------------
// libc surface: string/stdlib, formatted I/O, math, sort/search,
// setjmp/longjmp, errno discipline, malloc.
// ---------------------------------------------------------------------------

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

static jmp_buf g_jb;

static void test_libc()
{
    // string / memory
    char s[32];
    strcpy(s, "hello");
    strcat(s, ", world");
    check("strcpy/strcat/strlen", strlen(s) == 12 && strcmp(s, "hello, world") == 0);
    check("memcmp/memset", (memset(s, 'x', 4), memcmp(s, "xxxx", 4) == 0));

    // stdlib conversions
    check("strtol", strtol("  -123", nullptr, 10) == -123);
    check("strtod hex float", strtod("0x1.8p3", nullptr) == 12.0);

    // formatted output (golden vector)
    char out[64];
    int n = snprintf(out, sizeof(out), "%d %5.2f %s %#x", 42, 3.14159, "ok", 255);
    check("snprintf golden vector",
          n == (int)strlen(out) && strcmp(out, "42  3.14 ok 0xff") == 0);

    // formatted input
    int iv = 0; double dv = 0;
    int matched = sscanf("7 2.5", "%d %lf", &iv, &dv);
    check("sscanf", matched == 2 && iv == 7 && dv == 2.5);

    // math
    check("sqrt/pow", std::fabs(std::sqrt(2.0) * std::sqrt(2.0) - 2.0) < 1e-12 &&
                      std::fabs(std::pow(2.0, 10.0) - 1024.0) < 1e-9);
    check("log/exp", std::fabs(std::log(std::exp(1.0)) - 1.0) < 1e-12);

    // qsort / bsearch
    int arr[] = {5, 2, 9, 1, 7, 3};
    const int len = sizeof(arr) / sizeof(arr[0]);
    qsort(arr, len, sizeof(int), cmp_int);
    int key = 7;
    int *found = (int *)bsearch(&key, arr, len, sizeof(int), cmp_int);
    check("qsort sorts", arr[0] == 1 && arr[len - 1] == 9);
    check("bsearch finds", found != nullptr && *found == 7);

    // setjmp / longjmp
    volatile int hops = 0;
    if (setjmp(g_jb) == 0) {
        hops = 1;
        longjmp(g_jb, 99);
    } else {
        hops = 2;
    }
    check("setjmp/longjmp", hops == 2);

    // errno discipline
    errno = 0;
    FILE *f = fopen("/this/does/not/exist", "r");
    check("errno set on failure", f == nullptr && errno != 0);
    if (f) {
        fclose(f);
    }

    // malloc / realloc / free
    void *m = malloc(1024);
    bool malloc_ok = m != nullptr;
    m = realloc(m, 4096);
    malloc_ok = malloc_ok && m != nullptr;
    if (m) {
        memset(m, 0, 4096);
    }
    free(m);
    check("malloc/realloc/free", malloc_ok);
}

extern "C" void osv_app_main()
{
    printf("== os-features conformance ==\n");
    test_threads();
    test_mmap();
    test_time();
    test_libc();
    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    osv::poweroff();
}
