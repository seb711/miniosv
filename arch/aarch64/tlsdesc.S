// size_t __tlsdesc_static(size_t *a)
// {
// 	return a[1];
// }
.global __tlsdesc_static
.hidden __tlsdesc_static
.type __tlsdesc_static,@function
__tlsdesc_static:
	ldr x0,[x0,#8]
	ret

// __tlsdesc_dynamic (the general-dynamic TLS-descriptor resolver) was removed:
// the whole kernel + statically-linked app + prebuilt libc++/llvm-libc are
// compiled with -ftls-model=local-exec, so no R_AARCH64_TLSDESC relocations are
// emitted and it was never referenced. It also called __tls_dynamic_setup, which
// lived in the ELF loader deleted in Phase 4.

.section .note.GNU-stack,"",@progbits
