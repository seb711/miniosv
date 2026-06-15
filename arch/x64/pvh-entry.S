# Copyright (C) 2013 Cloudius Systems, Ltd.
#
# This work is open source software, licensed under the terms of the
# BSD license as described in the LICENSE file in the top-level directory.

# PVH boot entry for the slim kernel.
#
# A PVH-capable loader (QEMU's -kernel loader, among others) looks for an ELF
# note in a section named ".note.xen" with type XEN_ELFNOTE_PHYS32_ENTRY (18)
# whose value is the kernel's 32-bit entry point. This is the PVH/HVMlite boot
# ABI: it reuses Xen's public ELF-note format and "Xen" vendor string, but it
# is the boot protocol only and is unrelated to Xen hypervisor support (which
# has been removed). On entry %ebx holds the physical address of the
# hvm_start_info structure.

#define XEN_ELFNOTE_PHYS32_ENTRY 18

#define elfnote(type, valtype, value) \
        .pushsection .note.xen, "a", @note; \
        .align 4; \
        .long 2f-1f; \
        .long 3f-2f; \
        .long type; \
1: \
        .asciz "Xen"; \
        .align 4; \
2: \
        valtype value; \
3: \
        .align 4; \
        .popsection

#define elfnote_val(type, value) elfnote(type, .quad, value)

elfnote_val(XEN_ELFNOTE_PHYS32_ENTRY, hvm_xen_start-OSV_KERNEL_VM_SHIFT)

.code32
hvm_xen_start:
    mov %ebx, hvm_xen_start_info-OSV_KERNEL_VM_SHIFT
    mov $0x7c00, %esp # Allocate some temporary stack
    mov $start32-OSV_KERNEL_VM_SHIFT, %eax
    jmp *%eax
