/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef EXECINFO_HH_
#define EXECINFO_HH_

// Backtrace helper used by the panic/abort path. Walks .eh_frame via
// libgcc's _Unwind_Backtrace, so no frame pointers are required.
int backtrace_safe(void** pc, int nr);

// Same as backtrace_safe(), but also fills `cfa` with the canonical frame
// address of each frame. Useful for spotting recursion / stack overflow.
int backtrace_safe(void** pc, unsigned long* cfa, int nr);


#endif /* EXECINFO_HH_ */
