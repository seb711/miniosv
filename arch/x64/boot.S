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
    # The 2nd entry is the kernel window (virtual 1 GiB + 2 MiB = 0x40200000). It
    # points to kernel_pt_l2, whose 2 MiB entries uefi_entry rewrites at runtime
    # to the physical base the UEFI stub actually loaded the kernel at - the
    # kernel is no longer pinned to a fixed physical address. The values below
    # assume the build-time physical base (load at 0x200000, i.e.
    # OSV_KERNEL_VM_SHIFT); uefi_entry corrects them for the real base.
    .quad kernel_pt_l2 + 0x67 - OSV_KERNEL_VM_SHIFT
    .rept 510
    .quad 0
    .endr
ident_pt_l2:
    # Identity map of the low 1 GiB (physical == virtual), 2 MiB at a time. These
    # are leaf page entries (real physical addresses), not relocated at runtime.
    index = 0
    .rept 512
    .quad (index << 21) + 0x1e7
    index = index + 1
    .endr
kernel_pt_l2:
    # The kernel window's leaf entries. Pre-filled assuming the build-time
    # physical base; uefi_entry adds (real_base - 0x200000) to each at runtime so
    # that virtual 0x40200000 maps to wherever the kernel was loaded.
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
.global gdt
.hidden gdt
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

    # The UEFI stub loads the kernel at a firmware-chosen physical base, not the
    # fixed 0x200000 the boot page tables were built for. Compute the relocation
    # delta = (our true physical address) - (the build-time physical address):
    # we run under the firmware's identity map here, so a RIP-relative lea
    # yields our real physical address, and "uefi_entry - OSV_KERNEL_VM_SHIFT" is
    # the address the build assumed. The delta equals (load_base - 0x200000).
    lea uefi_entry(%rip), %rax
    mov $uefi_entry - OSV_KERNEL_VM_SHIFT, %ecx
    sub %rcx, %rax
    mov %rax, %r13                              # %r13 = relocation delta

    # Relocate the boot page tables by the delta: the subtable pointers (L4[0],
    # L3[0], L3[1]) and every kernel-window leaf entry. ident_pt_l2's leaves are
    # a true low-memory identity map and must stay unrelocated.
    lea ident_pt_l4(%rip), %rbx
    add %r13, (%rbx)                            # L4[0] -> phys(ident_pt_l3)
    lea ident_pt_l3(%rip), %rbx
    add %r13, (%rbx)                            # L3[0] -> phys(ident_pt_l2)
    add %r13, 8(%rbx)                           # L3[1] -> phys(kernel_pt_l2)
    lea kernel_pt_l2(%rip), %rbx
    mov $512, %ecx
1:  add %r13, (%rbx)                            # virtual 0x40200000.. -> load_base..
    add $8, %rbx
    dec %ecx
    jnz 1b

    # Load OSv's GDT. The descriptor's base field was linked for the build-time
    # physical address, so relocate it by the same delta before lgdt.
    lea gdt64_desc(%rip), %rax
    add %r13, 2(%rax)
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

    # Point CR3 at OSv's (now relocated) boot page tables. RIP-relative, since
    # we are still under the firmware identity map and the tables live in the
    # kernel image at its true physical base.
    lea ident_pt_l4(%rip), %rax
    mov %rax, %cr3

    # Long mode (EFER.LME) is already on; make sure EFER.NXE is set too. Read /
    # modify / write so LME and LMA are preserved.
    mov $0xc0000080, %ecx
    rdmsr
    or $0x800, %eax
    wrmsr

    mov $BOOT_CR0, %eax
    mov %rax, %cr0

    # Switch to the kernel's own init stack before the stack-using lretq below.
    # We arrived on the firmware's stack, which OSv's boot page tables (just
    # mapped via CR3) only cover if it happens to live in the low 2 GiB they
    # map - UEFI firmware on multi-GiB guests (e.g. AWS) places it higher, so
    # the lretq's pushes would fault on an unmapped address. init_stack_top is
    # in the kernel image (.bss), which the boot tables do map.
    movabs $init_stack_top, %rsp

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
    # Publish the runtime kernel relocation (must come after zeroing .bss, where
    # kernel_vm_shift lives). The virt->phys shift is the build-time shift minus
    # the relocation delta computed in uefi_entry; elf_header is the physical
    # address of the ELF header reported by the UEFI stub.
    mov $OSV_KERNEL_VM_SHIFT, %eax
    sub %r13, %rax
    mov %rax, kernel_vm_shift
    mov 40(%r12), %rax                          # boot_info->kernel_phys_base
    mov %rax, elf_header
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
    # Transient CR3 = physical address of the boot page tables. The kernel is
    # loaded at a firmware-chosen base, so this address is computed at runtime
    # (smp.cc) rather than from the build-time OSV_KERNEL_VM_SHIFT.
    mov smpboot_boot_cr3-smpboot, %eax
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
# GDT base (physical address of gdt). Filled at runtime by smp.cc because the
# kernel's physical load base is firmware-chosen, not OSV_KERNEL_VM_SHIFT.
.global smpboot_gdt_base
.hidden smpboot_gdt_base
smpboot_gdt_base:
    .long 0
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
# Physical address of ident_pt_l4 (transient CR3 for the AP trampoline), filled
# at runtime by smp.cc.
.global smpboot_boot_cr3
.hidden smpboot_boot_cr3
smpboot_boot_cr3:
    .long 0

.global smpboot_end
.hidden smpboot_end
smpboot_end = .

.bss

.global smpboot_cr3
.hidden smpboot_cr3
smpboot_cr3: .quad 0

# Runtime virtual->physical shift for the kernel image (OSV_KERNEL_VM_BASE minus
# the firmware-chosen physical load base). Set by start64; consumed by the
# phys/virt helpers in core/mmu.cc. Mirrors the aarch64 symbol of the same name.
.global kernel_vm_shift
.hidden kernel_vm_shift
kernel_vm_shift: .quad 0

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
