/* OSv-owned long-double survivors (LLVM_LIBC_PLAN.md, Phases 2.3/7).
 *
 * llvm-libc at 22.1.7 does not implement logl, __fpclassifyl or
 * __signbitl for either long-double format OSv targets (x86 80-bit
 * extended, aarch64 IEEE binary128). These are the only long-double
 * symbols the link still needs (consumers: leanstore's logl, OSv's
 * printf %Lf classification via libstdc++), so they get small
 * OSv-owned implementations.
 */

#include <math.h>
#include <stdint.h>

#if defined(__x86_64__)

/* x86 80-bit extended precision: 64-bit mantissa with explicit
 * integer bit, then 15-bit biased exponent + sign. */
union ldbits {
	long double f;
	struct {
		uint64_t m;
		uint16_t se;
	} i;
};

int __fpclassifyl(long double x)
{
	union ldbits u = {x};
	int e = u.i.se & 0x7fff;
	int intbit = u.i.m >> 63;
	if (e == 0) {
		if (intbit)
			return FP_NORMAL; /* pseudo-denormal; hw treats as valid */
		return u.i.m ? FP_SUBNORMAL : FP_ZERO;
	}
	if (e == 0x7fff) {
		if (!intbit)
			return FP_NAN; /* pseudo-inf/NaN: invalid on anything post-287 */
		return u.i.m << 1 ? FP_NAN : FP_INFINITE;
	}
	/* finite, nonzero exponent: a clear integer bit ("unnormal") is an
	 * invalid encoding and behaves as a NaN */
	return intbit ? FP_NORMAL : FP_NAN;
}

int __signbitl(long double x)
{
	union ldbits u = {x};
	return u.i.se >> 15;
}

/* ln(x) = ln(2) * log2(x), computed by the FPU with the full 64-bit
 * internal mantissa. fyl2x's own special-case handling matches C99:
 * negative -> invalid/NaN, +-0 -> divide-by-zero/-inf, +inf -> +inf,
 * NaN propagates. */
long double logl(long double x)
{
	long double r;
	__asm__("fldln2\n\t"
	        "fxch %%st(1)\n\t"
	        "fyl2x"
	        : "=t"(r)
	        : "0"(x)
	        : "st(1)");
	return r;
}

#elif defined(__aarch64__)

/* aarch64 long double is IEEE binary128: 112-bit mantissa, 15-bit
 * biased exponent, sign. */
union ldbits {
	long double f;
	struct {
		uint64_t lo;
		uint64_t hi; /* sign(63) | exp(62..48) | mantissa head */
	} i;
};

int __fpclassifyl(long double x)
{
	union ldbits u = {x};
	int e = (u.i.hi >> 48) & 0x7fff;
	uint64_t m = (u.i.hi & 0xffffffffffffULL) | u.i.lo;
	if (e == 0)
		return m ? FP_SUBNORMAL : FP_ZERO;
	if (e == 0x7fff)
		return m ? FP_NAN : FP_INFINITE;
	return FP_NORMAL;
}

int __signbitl(long double x)
{
	union ldbits u = {x};
	return u.i.hi >> 63;
}

/* Same shortcut musl takes for the binary128 format (its ld128 logl
 * is "return log(x)"): compute in double. The consumers (leanstore
 * latency math, %Lg formatting of sane values) don't need the last
 * 60 bits. */
long double logl(long double x)
{
	return log(x);
}

#else
#error "unsupported long double format"
#endif
