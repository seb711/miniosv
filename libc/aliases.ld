/* Symbol aliases. The glibc binary-compat aliases (the *64 file-offset names,
 * statfs64/stat64/..., getauxval, _exit, the redundant math/string/time names)
 * are gone - no external Linux-binary ABI, and llvm-libc supplies the generic
 * libc. Only two irreducible aliases remain, both pulled by code we link but do
 * not compile:
 *   __lseek          - musl's stdio FILE machinery (__stdio_seek); goes when
 *                      the stdio FILE glue becomes OSv-owned.
 *   __isoc23_strto* - libc++.a was built against the host glibc headers, whose
 *                      C23 <stdlib.h> renames strtol/strtoul/strtoll/strtoull to
 *                      __isoc23_*; only removable by rebuilding libc++ against
 *                      the llvm-libc headers. */
__lseek = lseek;
__isoc23_strtol = strtol;
__isoc23_strtoul = strtoul;
__isoc23_strtoll = strtoll;
__isoc23_strtoull = strtoull;
