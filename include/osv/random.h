/*-
 * Copyright (c) 2000-2013 Mark R. V. Murray
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The kernel RNG entropy-source contract. The esource enum and the
 * random_harvest() prototype were the only survivors of the FreeBSD
 * sys/random.h; they were relocated here when the bsd/ tree was removed.
 * The ChaCha20 CSPRNG (drivers/random.cc) folds harvested entropy into its
 * pool and ignores the origin tag, so the enum is kept only for the call
 * sites (the interrupt path uses RANDOM_INTERRUPT).
 */

#ifndef _OSV_RANDOM_H_
#define _OSV_RANDOM_H_

#include <sys/cdefs.h>
#include <sys/types.h>

enum esource {
	RANDOM_START = 0,
	RANDOM_CACHED = 0,
	RANDOM_ATTACH,
	RANDOM_KEYBOARD,
	RANDOM_MOUSE,
	RANDOM_NET_TUN,
	RANDOM_NET_ETHER,
	RANDOM_NET_NG,
	RANDOM_INTERRUPT,
	RANDOM_SWI,
	RANDOM_PURE_OCTEON,
	RANDOM_PURE_SAFE,
	RANDOM_PURE_GLXSB,
	RANDOM_PURE_UBSEC,
	RANDOM_PURE_HIFN,
	RANDOM_PURE_RDRAND,
	RANDOM_PURE_NEHEMIAH,
	RANDOM_PURE_RNDTEST,
	RANDOM_PURE_VIRTIO,
	ENTROPYSOURCE
};

__BEGIN_DECLS
void random_harvest(const void *, u_int, u_int, enum esource);
__END_DECLS

#endif /* _OSV_RANDOM_H_ */
