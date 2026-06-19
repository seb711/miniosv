/*
 * Copyright (C) 2026 miniosv contributors
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

/*
 * Embeds the built kernel ELF into the UEFI stub image as a binary blob, so
 * the stub is a single self-contained EFI application that needs no file-system
 * access to load the kernel. The path to the kernel ELF is supplied by the
 * build via -DKERNEL_ELF_FILE="...".
 */

#ifndef KERNEL_ELF_FILE
#error "KERNEL_ELF_FILE must be defined to the path of the kernel ELF"
#endif

	.section .kernel, "a"
	.balign 8
	.global kernel_elf_start
	.hidden kernel_elf_start
kernel_elf_start:
	.incbin KERNEL_ELF_FILE
	.global kernel_elf_end
	.hidden kernel_elf_end
kernel_elf_end:
