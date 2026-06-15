/* Symbol aliases. The glibc binary-compat aliases (the *64 file-offset names,
 * statfs64/stat64/..., getauxval, _exit, the redundant math/string/time names)
 * are gone - no external Linux-binary ABI, and llvm-libc supplies the generic
 * libc. One alias remains, pulled by code we link but do not compile:
 *   __lseek - musl's stdio FILE machinery (__stdio_seek); goes when the stdio
 *             FILE glue becomes OSv-owned. */
__lseek = lseek;
