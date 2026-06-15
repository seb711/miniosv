#ifndef _OSV_BITS_TYPES_MBSTATE_T_H
#define _OSV_BITS_TYPES_MBSTATE_T_H

/* OSv-owned <bits/types/mbstate_t.h>.
 *
 * libc++ (built against glibc, i.e. not in musl mode) resolves mbstate_t in its
 * <__mbstate_t.h> by preferring `#include <bits/types/mbstate_t.h>`. On the
 * glibc build host that picks up glibc's definition, but in the OSv kernel build
 * include/api is searched first, so this file wins and routes the lookup to the
 * single OSv definition (from <bits/alltypes.h>, the musl way). That keeps
 * libc++'s ::mbstate_t identical to the one <wchar.h> and the rest of OSv's libc
 * use, avoiding a glibc-vs-OSv redefinition conflict. */

#define __NEED_mbstate_t
#include <bits/alltypes.h>

#endif
