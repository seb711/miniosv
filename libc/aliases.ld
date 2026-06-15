/* Symbol aliases. None remain: the glibc binary-compat aliases were dropped
 * (no external Linux-binary ABI; llvm-libc supplies the generic libc), and the
 * last alias __lseek went when the stdio FILE glue became OSv-owned (Phase 8.11,
 * __stdio_seek now calls lseek directly). This file is kept empty until the link
 * stops referencing it (removed in Phase 8.13). */
