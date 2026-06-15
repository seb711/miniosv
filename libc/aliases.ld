/* Linker symbol aliases. The glibc binary-compat aliases (the *64 file-offset
 * names, the __isoc23_* C23 names, statfs64/stat64/..., getauxval, _exit, ...)
 * were dropped: the slim in-kernel-app model has no external Linux-binary ABI,
 * and llvm-libc supplies the generic libc. Only the few __-prefixed internal
 * aliases still referenced by the kernel/libstdc++ remain. */
__lseek = lseek;
__mmap = mmap;
__munmap = munmap;
__isoc23_strtoul = strtoul;
