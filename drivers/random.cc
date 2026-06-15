/*
 * Copyright (C) 2026 OSv
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 *
 * A small ChaCha20 "fast key erasure" CSPRNG that backs read_random() and
 * arc4random(). It replaces the much larger FreeBSD Yarrow + AES + SHA-2
 * entropy subsystem that used to live under bsd/sys/dev/random,
 * bsd/sys/crypto and bsd/sys/libkern.
 *
 * Design (cf. D. J. Bernstein's "fast key erasure" RNG, OpenBSD arc4random and
 * Linux's ChaCha-based /dev/urandom):
 *   - A 256-bit key + 96-bit nonce drive ChaCha20 in counter mode.
 *   - After every request the key is overwritten with fresh keystream
 *     (forward secrecy: past output cannot be recovered from the new state).
 *   - Entropy is folded into the key from three sources: the CPU hardware RNG
 *     (RDRAND on x86), virtio-rng (pulled on demand), and interrupt timing
 *     harvested via random_harvest() from the interrupt path.
 *
 * Concurrency: random_harvest() runs in interrupt context and therefore never
 * takes a sleeping lock - it only XORs into an atomic input pool. Generation
 * runs in thread context under rng_lock, and hardware sources are pulled under
 * a separate rng_hw_lock so the two locks are never held at the same time.
 *
 * There is no filesystem, so the /dev/random and /dev/urandom device files are
 * gone; the kernel CSPRNG (read_random/arc4random/getrandom) is unaffected.
 */

#include "drivers/random.hh"

#include <osv/debug.hh>
#include <osv/mutex.h>
#include <osv/sched.hh>

#include <osv/random.h>

#include <string.h>
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <vector>

#ifdef __x86_64__
#include "processor.hh"
#endif

namespace {

//
// ChaCha20 block function (RFC 8439).
//
static inline uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

#define CHACHA_QR(a, b, c, d)                       \
    a += b; d ^= a; d = rotl32(d, 16);              \
    c += d; b ^= c; b = rotl32(b, 12);              \
    a += b; d ^= a; d = rotl32(d, 8);               \
    c += d; b ^= c; b = rotl32(b, 7)

static void chacha20_block(const uint32_t key[8], uint32_t counter,
                           const uint32_t nonce[3], uint8_t out[64])
{
    static const uint32_t consts[4] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
    };
    uint32_t s[16], x[16];

    s[0] = consts[0]; s[1] = consts[1]; s[2] = consts[2]; s[3] = consts[3];
    for (int i = 0; i < 8; i++) {
        s[4 + i] = key[i];
    }
    s[12] = counter; s[13] = nonce[0]; s[14] = nonce[1]; s[15] = nonce[2];

    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4 * i + 0] = v;
        out[4 * i + 1] = v >> 8;
        out[4 * i + 2] = v >> 16;
        out[4 * i + 3] = v >> 24;
    }
}

//
// A cheap high-resolution counter, used as a timing-jitter entropy source.
//
static inline uint64_t cpu_cycles()
{
#if defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

// CSPRNG state, protected by rng_lock (taken only in thread context).
mutex rng_lock;
uint32_t rng_key[8];
uint32_t rng_nonce[3];

// Interrupt-side entropy input pool. random_harvest() runs in interrupt
// context, so it must not take rng_lock; it only XORs into this pool. A benign
// data race there merely loses a little entropy, it never corrupts state.
constexpr unsigned POOL_WORDS = 16;
std::atomic<uint32_t> input_pool[POOL_WORDS];
std::atomic<unsigned> input_pos{0};

// Registered hardware entropy sources (e.g. virtio-rng), guarded by its own
// lock so it is never held together with rng_lock.
mutex rng_hw_lock;
std::vector<randomdev::hw_rng*> hw_sources;

// Fold len bytes of data into the interrupt input pool (lock-free).
void pool_mix(const void *data, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++) {
        unsigned idx = input_pos.fetch_add(1, std::memory_order_relaxed)
                       % (POOL_WORDS * 4);
        uint32_t shifted = static_cast<uint32_t>(p[i]) << ((idx & 3) * 8);
        input_pool[idx >> 2].fetch_xor(shifted, std::memory_order_relaxed);
    }
}

// XOR arbitrary entropy into the key (cycling over the 32 key bytes).
// Must hold rng_lock.
void xor_into_key(const void *data, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint8_t *k = reinterpret_cast<uint8_t *>(rng_key);
    for (size_t i = 0; i < len; i++) {
        k[i % sizeof(rng_key)] ^= p[i];
    }
}

// Overwrite the key with fresh keystream (fast key erasure). Must hold rng_lock.
void key_ratchet()
{
    uint8_t blk[64];
    chacha20_block(rng_key, 0, rng_nonce, blk);
    memcpy(rng_key, blk, sizeof(rng_key));
    if (++rng_nonce[0] == 0 && ++rng_nonce[1] == 0) {
        ++rng_nonce[2];
    }
    memset(blk, 0, sizeof(blk));
}

// Pull entropy from registered hardware sources. Acquires rng_hw_lock only
// (never rng_lock), so the per-source locks compose safely.
size_t pull_hw_entropy(char *buf, size_t max)
{
    size_t got = 0;
    WITH_LOCK(rng_hw_lock) {
        for (auto *src : hw_sources) {
            if (got >= max) {
                break;
            }
            got += src->get_random_bytes(buf + got, max - got);
        }
    }
    return got;
}

// Absorb available entropy into the key, then ratchet to diffuse it.
// Must hold rng_lock.
void reseed_locked(const char *hw, size_t hw_len)
{
    // Interrupt-timing entropy accumulated since the last reseed.
    for (unsigned i = 0; i < POOL_WORDS; i++) {
        rng_key[i % 8] ^= input_pool[i].exchange(0, std::memory_order_relaxed);
    }
    if (hw_len) {
        xor_into_key(hw, hw_len);
    }
#ifdef __x86_64__
    if (processor::features().rdrand) {
        for (int i = 0; i < 8; i++) {
            for (int retry = 0; retry < 10; retry++) {
                uint64_t v;
                if (processor::rdrand(&v)) {
                    rng_key[i] ^= static_cast<uint32_t>(v) ^
                                  static_cast<uint32_t>(v >> 32);
                    break;
                }
            }
        }
    }
#endif
    // A little timing jitter, cheap and always available.
    uint64_t jitter = cpu_cycles();
    xor_into_key(&jitter, sizeof(jitter));

    key_ratchet();
}

// Produce len bytes of keystream into buf. Must hold rng_lock.
void generate_locked(void *buf, size_t len)
{
    uint8_t *out = static_cast<uint8_t *>(buf);
    uint8_t blk[64];
    uint32_t counter = 1;  // counter 0 is reserved for key_ratchet()
    while (len > 0) {
        chacha20_block(rng_key, counter++, rng_nonce, blk);
        size_t n = std::min(len, sizeof(blk));
        memcpy(out, blk, n);
        out += n;
        len -= n;
    }
    key_ratchet();
    memset(blk, 0, sizeof(blk));
}

// Top-level generation: pull fresh hardware entropy, reseed and emit.
void csprng_generate(void *buf, size_t len)
{
    char hw[32];
    size_t hw_len = pull_hw_entropy(hw, sizeof(hw));  // outside rng_lock
    WITH_LOCK(rng_lock) {
        reseed_locked(hw, hw_len);
        generate_locked(buf, len);
    }
    memset(hw, 0, sizeof(hw));
}

void csprng_init()
{
    WITH_LOCK(rng_lock) {
        // Mix in some boot-time jitter so we are never in an all-zero state even
        // on platforms without a hardware RNG; reseed_locked() then folds in the
        // interrupt pool, RDRAND (if present) and per-call hardware entropy.
        uint64_t seed[8];
        for (int i = 0; i < 6; i++) {
            seed[i] = cpu_cycles();
        }
        seed[6] = reinterpret_cast<uintptr_t>(&seed);
        seed[7] = reinterpret_cast<uintptr_t>(sched::thread::current());
        xor_into_key(seed, sizeof(seed));
        reseed_locked(nullptr, 0);
    }
}

} // anonymous namespace

//
// C API consumed by the rest of the kernel (declarations in
// bsd/sys/sys/random.h, bsd/porting/netport.h) and by applications (arc4random
// is part of the exported libc ABI).
//
extern "C" {

int read_random(void *buf, int len)
{
    if (len <= 0) {
        return 0;
    }
    csprng_generate(buf, len);
    return len;
}

uint32_t arc4random(void)
{
    uint32_t v;
    csprng_generate(&v, sizeof(v));
    return v;
}

void arc4rand(void *ptr, u_int len, int /*reseed*/)
{
    csprng_generate(ptr, len);
}

// Called from the interrupt path (see osv/intr_random.hh) and from virtio-rng.
void random_harvest(const void *entropy, u_int size, u_int /*bits*/,
                    enum esource /*origin*/)
{
    pool_mix(entropy, size);
}

}

namespace randomdev {

void random_device::register_source(hw_rng *hwrng)
{
    WITH_LOCK(rng_hw_lock) {
        hw_sources.push_back(hwrng);
    }
}

void random_device::deregister_source(hw_rng *hwrng)
{
    WITH_LOCK(rng_hw_lock) {
        hw_sources.erase(std::remove(hw_sources.begin(), hw_sources.end(),
                                     hwrng),
                         hw_sources.end());
    }
}

random_device::random_device()
{
    csprng_init();
}

random_device::~random_device()
{
}

void randomdev_init()
{
    new random_device();
    debug("random: ChaCha20 CSPRNG initialized\n");
}

}
