/* Symbol aliases. The glibc binary-compat aliases (the *64 file-offset names,
 * statfs64/stat64/..., getauxval, _exit, the redundant math/string/time names)
 * are gone - no external Linux-binary ABI, and llvm-libc supplies the generic
 * libc. Only two irreducible aliases remain, both pulled by code we link but do
 * not compile:
 *   __lseek          - musl's stdio FILE machinery (__stdio_seek); goes when
 *                      the stdio FILE glue becomes OSv-owned.
 *   __isoc23_strtoul - the host's glibc-built libstdc++.a references the C23
 *                      strtoul; only removable by rebuilding libstdc++ against
 *                      a non-glibc libc. */
__lseek = lseek;
__isoc23_strtoul = strtoul;
