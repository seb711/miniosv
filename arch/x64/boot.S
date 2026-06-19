# Copyright (C) 2013 Cloudius Systems, Ltd.
#
# This work is open source software, licensed under the terms of the
# BSD license as described in the LICENSE file in the top-level directory.

#include "processor-flags.h"

#define BOOT_CR0 ( X86_CR0_PE \
                 | X86_CR0_WP \
                 | X86_CR0_PG \
                 | X86_CR0_MP )

#define BOOT_CR4 ( X86_CR4_DE         \
                 | X86_CR4_PSE        \
                 | X86_CR4_PAE        \
                 | X86_CR4_PGE        \
                 | X86_CR4_PCE        \
                 | X86_CR4_OSFXSR     \
                 | X86_CR4_OSXMMEXCPT )

.text
.code32

.data
.align 4096
.global ident_pt_l4
.hidden ident_pt_l4
ident_pt_l4:
    # The addresses of the paging tables have to be the physical ones, so we have to
    # manually subtract OSV_KERNEL_VM_SHIFT in all relevant places
    .quad ident_pt_l3 + 0x67 - OSV_KERNEL_VM_SHIFT
    .rept 511
    .quad 0
    .endr
#if OSV_KERNEL_VM_SHIFT != 0x40000000 && OSV_KERNEL_VM_SHIFT != 0
#error This code only works correctly for OSV_KERNEL_VM_SHIFT = 0x40000000 or 0
#endif
ident_pt_l3:
    # Each of the 512 entries in this table maps the very 1st 512 GiB of
    # virtual address space 1 GiB at a time
    # The very 1st entry maps 1st GiB 1:1 by pointing to ident_pt_l2 table
    # that specifies addresses of every one of 512 2MiB slots of physical memory
    .quad ident_pt_l2 + 0x67 - OSV_KERNEL_VM_SHIFT
    # The 2nd entry maps 2nd GiB to the same 1st GiB of physical memory by pointing
    # to the same ident_pt_l2 table as the 1st entry above
    # This way we effectively provide correct mapping for the kernel linked
    # to start at 1 GiB + 2 MiB (0x40200000) in virtual memory and point to
    # 2 MiB address (0x200000) where it starts in physical memory
    .quad ident_pt_l2 + 0x67 - OSV_KERNEL_VM_SHIFT
    .rept 510
    .quad 0
    .endr
ident_pt_l2:
    index = 0
    .rept 512
    .quad (index << 21) + 0x1e7
    index = index + 1
    .endr

# 64-bit GDT descriptor loaded by uefi_entry. The base is the physical address
# of the GDT, because it is loaded while still running under the firmware's
# identity map, before OSv's page tables are installed.
.align 8
gdt64_desc:
    .short gdt_end - gdt - 1
    .quad gdt - OSV_KERNEL_VM_SHIFT

.align 8
gdt = . - 8
    .quad 0x00af9b000000ffff # 64-bit code segment
    .quad 0x00cf93000000ffff # 64-bit data segment
    .quad 0x00cf9b000000ffff # 32-bit code segment
gdt_end = .

.align 8
. = . + 4  # make sure tss_ist is aligned on a quad boundary

.bss

.align 16
. = . + 4096*10
init_stack_top = .

.text

.code64
.globl uefi_entry
.hidden uefi_entry
# Kernel entry point reached from the UEFI stub (boot/uefi/stub.cc). We arrive
# in 64-bit long mode with paging enabled under the firmware's identity map;
# %rdi holds the physical address of the osv::boot_info structure. Switch to
# OSv's GDT and boot page tables, then jump to the virtual start64.
uefi_entry:
    mov %rdi, %r12                              # preserve boot_info pointer

    # Load OSv's GDT. Address the descriptor through a register (absolute
    # physical address) rather than a RIP-relative symbol reference, because we
    # still execute unrelocated, before our page tables are active.
    mov $gdt64_desc - OSV_KERNEL_VM_SHIFT, %eax
    lgdt (%rax)

    mov $0x10, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %fs
    mov %eax, %gs
    mov %eax, %ss

    # Set the control registers to exactly the values OSv expects (4-level
    # paging). This clears any firmware-only bits, matching the SMP trampoline
    # below; it assumes the firmware did not enable 5-level paging (LA57).
    mov $BOOT_CR4, %eax
    mov %rax, %cr4

    # Point CR3 at OSv's identity + kernel boot page tables (physical address).
    mov $ident_pt_l4 - OSV_KERNEL_VM_SHIFT, %eax
    mov %rax, %cr3

    # Long mode (EFER.LME) is already on; make sure EFER.NXE is set too. Read /
    # modify / write so LME and LMA are preserved.
    mov $0xc0000080, %ecx
    rdmsr
    or $0x800, %eax
    wrmsr

    mov $BOOT_CR0, %eax
    mov %rax, %cr0

    # Reload CS and jump to the virtual entry now that our mapping is active
    # (virtual 0x40200000 -> physical 0x200000).
    pushq $0x8
    movabs $start64, %rax
    pushq %rax
    lretq

.hidden start64
.global start64
start64:
    .cfi_startproc simple
    .cfi_def_cfa %rsp, 0
    .cfi_undefined %rip
    lea .bss, %rdi
    lea .edata, %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    rep stosb
    mov $OSV_KERNEL_BASE, %rbp
    mov %rbp, elf_header
    mov %r12, osv_boot_info                     # boot_info pointer from the UEFI stub
    lea init_stack_top, %rsp
    call premain
    call main
    .cfi_endproc

# The smp trampoline must be in the lower 1MB, so we manually relocate
# it to address 0 by subtracting smpboot from any offset
.data
.global smpboot
.hidden smpboot
smpboot:
.code16
    lgdtl smpboot_gdt_desc-smpboot
    mov smpboot_cr0-smpboot, %eax
    btr $31, %eax # disable paging
    mov %eax, %cr0
    ljmp $0x18, $1f-smpboot
1:
.code32
    mov $0x10, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %eax, %fs
    mov %eax, %gs
    mov smpboot_cr4-smpboot, %eax
    mov %eax, %cr4
    lea ident_pt_l4, %eax
    sub $OSV_KERNEL_VM_SHIFT, %eax
    mov %eax, %cr3
    mov smpboot_efer-smpboot, %eax
    mov smpboot_efer+4-smpboot, %edx
    btr $10, %eax
    mov $0xc0000080, %ecx
    wrmsr
    mov smpboot_cr0-smpboot, %eax
    #1: jmp 1b
    mov %eax, %cr0 # now with paging
    ljmp $8, $smpboot64

smpboot_gdt_desc:
    .short gdt_end - gdt - 1
    .long gdt - OSV_KERNEL_VM_SHIFT
.global smpboot_cr0
.hidden smpboot_cr0
smpboot_cr0:
    .long 0
.global smpboot_cr4
.hidden smpboot_cr4
smpboot_cr4:
    .long 0
.global smpboot_efer
.hidden smpboot_efer
smpboot_efer:
    .quad 0

.global smpboot_end
.hidden smpboot_end
smpboot_end = .

.bss

.global smpboot_cr3
.hidden smpboot_cr3
smpboot_cr3: .quad 0

.global smp_stack_free
.hidden smp_stack_free
smp_stack_free: .quad 0

# 'ltr' expects an available TSS, then marks it busy, so only one
# can be loaded at a time
trlock: .byte 0

.text
.code64

smpboot64:
    mov smpboot_cr3, %rax
    mov %rax, %cr3
1:
    mov smp_stack_free, %rax
    mov 4096(%rax), %rbx
    lock cmpxchg %rbx, smp_stack_free
    jnz 1b
    lea 4096(%rax), %rsp
    call smp_main
