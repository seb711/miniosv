/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

/*
 * A printf variant that prints directly to the kernel console.
 *
 * Formats into a stack buffer and hands the result to debug_write():
 * no allocation, usable from delicate contexts. (The musl-era version
 * instead faked a FILE over debug_write; llvm-libc's FILE is an opaque
 * cookie, so that trick is gone - and a bounded buffer is what a
 * kernel-console printf should have been anyway.)
 */

#include <osv/debug.h>
#include <osv/export.h>

#include <stdio.h>
#include <string.h>

int vkprintf(const char *restrict fmt, va_list ap)
{
	char buf[1024];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0) {
		return n;
	}
	size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
	debug_write(buf, len);
	return n;
}

OSV_LIBSOLARIS_API
int kprintf(const char *restrict fmt, ...)
{
	int ret;
	va_list ap;
	va_start(ap, fmt);
	ret = vkprintf(fmt, ap);
	va_end(ap);
	return ret;
}
