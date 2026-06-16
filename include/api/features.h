#ifndef _FEATURES_H
#define _FEATURES_H

#if defined(_ALL_SOURCE) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#if defined(_DEFAULT_SOURCE) && !defined(_BSD_SOURCE)
#define _BSD_SOURCE 1
#endif

#if !defined(_POSIX_SOURCE) && !defined(_POSIX_C_SOURCE) \
 && !defined(_XOPEN_SOURCE) && !defined(_GNU_SOURCE) \
 && !defined(_BSD_SOURCE) && !defined(__STRICT_ANSI__)
#define _BSD_SOURCE 1
#define _XOPEN_SOURCE 700
#endif

#if __STDC_VERSION__ >= 199901L
#define __restrict restrict
#elif !defined(__GNUC__)
#define __restrict
#endif

#if __STDC_VERSION__ >= 199901L || defined(__cplusplus)
#define __inline inline
#elif !defined(__GNUC__)
#define __inline
#endif

#if __STDC_VERSION__ >= 201112L
#elif defined(__GNUC__)
#define _Noreturn __attribute__((__noreturn__))
#else
#define _Noreturn
#endif

/* Standard C library header macros that the kernel headers expect. Formerly
 * supplied by include/glibc-compat/features.h (removed in Phase 9.6); merged
 * here so include/api is self-sufficient. __BEGIN_DECLS / __THROW come from
 * <sys/cdefs.h>, which many <osv/*> headers rely on <features.h> already having
 * pulled in.
 *
 * NOTE: we deliberately do NOT define __GLIBC__/__GNU_LIBRARY__ here. OSv is not
 * glibc, and claiming to be breaks the libc++ build (it selects a glibc-specific
 * <random> path that clashes with our headers). boost's predef falls back to
 * architecture-based endian detection without it. */
#include <sys/cdefs.h>

#define hidden __attribute__((__visibility__("hidden")))

#endif
