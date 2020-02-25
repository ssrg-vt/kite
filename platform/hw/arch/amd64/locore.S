/*-
 * Copyright (c) 2014, 2015 Antti Kantee.  All Rights Reserved.
 * Copyright (c) 2018 Ruslan Nikolaev.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <hw/kernel.h>
#include <hw/multiboot.h>

#define MYMULTIBOOT_FLAGS \
    (MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_AOUT_KLUDGE)

.section .bootstrap, "ax"

.code32

.align 4
.global _minios_hypercall_page, _kvm_pvclock_wc
_minios_hypercall_page:
_kvm_pvclock_wc:
bootstrap:
.long MULTIBOOT_HEADER_MAGIC
.long MYMULTIBOOT_FLAGS
.long -(MULTIBOOT_HEADER_MAGIC+MYMULTIBOOT_FLAGS)
.long bootstrap
.long 0x100000
.long _edata
.long _ebss
.long _start

/* Reserve space for the hypercall page. */
.org bootstrap + 0x1000

/* Space for Xen shared info or KVM PV clock. */
.global _minios_shared_info, _kvm_pvclock_ti
_minios_shared_info:
_kvm_pvclock_ti:
.space 4096

/* 1K boot stacks. */
.space 1024*BMK_MAXCPUS
bootstack:

/* The 16-bit trampoline is moved to 0x7000, so modify all absolute
   addresses appropriately. */
#define TRAMPOLINE_ADDR(x)	(x - trampoline + 0x7000)

/* The 16-bit trampoline must fit in the 512-byte region. */
.code16
ENTRY(trampoline)
	cli				/* Disable interrupts */
	lgdt (TRAMPOLINE_ADDR(gdt32_ptr))
	movl %cr0, %eax
	orb $1, %al			/* Protected Enable */
	movl %eax, %cr0
	.byte 0x66			/* Prefix */
	.byte 0xea			/* Long Jump */
	.long trampoline_32
	.word 0x8			/* CS */

.align 64
cpu_gdt32:
	.quad 0x0000000000000000
	.quad 0x00cf9b000000ffff	/* CS */
	.quad 0x00cf93000000ffff	/* DS */
gdt32_end:

.align 64
gdt32_ptr:
	.word gdt32_end-cpu_gdt32-1
	.quad TRAMPOLINE_ADDR(cpu_gdt32)

/* Do not move since the label is used to calculate the trampoline size. */
.code32
ENTRY(trampoline_32)
	movl $0x10, %eax		/* DS */
	movl %eax, %ds
	movl %eax, %es
	movl %eax, %ss
	movl %eax, %fs
	movl %eax, %gs
	cld
	movl $1, %eax
	lock xadd %eax, (trampoline_num_cpus)
	movl %eax, %esi			/* CPU # */
	shll $10, %eax			/* Each CPU has 1K boot stack */
	negl %eax
	leal bootstack(%eax), %esp	/* Calculate address for current CPU */
	xorl %edi,%edi			/* Multiboot: NULL pointer */
	jmp from_trampoline_32

/*
 * Bootloader entry point.
 *
 * Bootstrap is slightly different from i386.  Multiboot puts us only
 * in 32bit mode, so it's our responsibility to install a page table
 * and switch to long mode.  Notably, we can't call C code until
 * we've switched to long mode.
 */
ENTRY(_start)
	cld
	movl $bootstack, %esp

	/* save BIOS data area values */
	movw BIOS_COM1_BASE, %di
	movw %di, bios_com1_base
	movw BIOS_EBDA_BASE, %di
	movw %di, bios_ebda_base
	movw BIOS_CRTC_BASE, %di
	movw %di, bios_crtc_base

	/* clear console */
	pushl %eax
	movw $' ', %ax
	movl $(CONS_ADDRESS), %edi
	movl $(CONS_WIDTH*CONS_HEIGHT), %ecx
	rep stosw
	popl %eax

	movl %ebx, %edi	/* Store multiboot pointer */
	xorl %esi, %esi	/* CPU: 0 */

	/* only multiboot is supported for now */
	cmpl $MULTIBOOT_BOOTLOADER_MAGIC, %eax
	jne nomultiboot

from_trampoline_32:
	lgdt (gdt64_ptr)
	pushl $0x0
	pushw $0x10
	pushl $1f
	lret

1:	movl $0x18, %eax
	movl %eax, %ds
	movl %eax, %es
	movl %eax, %ss

	xorl %eax, %eax
	movl %eax, %fs
	movl %eax, %gs

	/*
	 * x86_64 switch to long mode
	 */

	/* 1: enable pae and sse */
	movl %cr4, %eax
	orl $(CR4_OSXMMEXCPT|CR4_OSFXSR|CR4_PAE), %eax
	movl %eax, %cr4

	/* 2: enable long mode */
	movl $MSR_EFER, %ecx
	rdmsr
	movl $MSR_EFER_LME, %eax
	wrmsr

	/* 3: load pml4 pointer */
	movl $cpu_pml4, %eax
	movl %eax, %cr3

	/* 4: enable paging */
	movl %cr0, %eax
	orl $(CR0_PG|CR0_WP|CR0_PE), %eax
	movl %eax, %cr0

	/* 5: poetically longjump to longmode */
	pushw $0x08
	pushl $_start64
	lret

	/* NOTREACHED */
	jmp haltme

nomultiboot:
	/* we don't have printf available yet, just output manually */
	mov $nomultimesg, %ebx
	mov $(CONS_ADDRESS), %ecx
1:
	movsbl (%ebx), %eax
	test %al, %al
	je haltme
	orl $0x500, %eax
	movl %eax, (%ecx)
	inc %ebx
	addl $2, %ecx
	jmp 1b

haltme:
	cli
	hlt
	jmp haltme
END(_start)

nomultimesg:
	.asciz "not multibooted, halting!"

#include "pagetable.S"

/*
 * amd64 programmer's manual:
 *
 * "In long mode, segmentation is not used ... except for a few exceptions."
 *
 * Uuuyea, exceptions.
 */

.data
.align 64
.globl cpu_gdt64
cpu_gdt64:
	.quad 0x0000000000000000
	.quad 0x00af9b000000ffff	/* 64bit CS		*/
	.quad 0x00cf9b000000ffff	/* 32bit CS		*/
	.quad 0x00cf93000000ffff	/* DS			*/
.rept BMK_MAXCPUS
	.quad 0x0000000000000000	/* TSS part 1 (via C)	*/
	.quad 0x0000000000000000	/* TSS part 2 (via C)	*/
.endr
gdt64_end:
.align 64

.type gdt64_ptr, @object
gdt64_ptr:
	.word gdt64_end-cpu_gdt64-1
	.quad cpu_gdt64

.align 64
.global boot_num_cpus
boot_num_cpus:
	.long 0

.align 64
trampoline_num_cpus:
	.long 1				/* CPU #0 has started already */

.align 64
.code64

ENTRY(_start64)
	movl %esp, %esp			/* RSP: clean up upper 32 bits */
	xorq %rbp, %rbp

	movl %edi, %edi			/* Multiboot: clean up upper 32 bits */
	movl %esi, %esi			/* CPU #: clean up upper 32 bits */

	lock incl (boot_num_cpus)	/* Increment number of CPUs */

	pushq $0x0
	pushq $0x0

	call x86_boot
	hlt
END(_start64)

ENTRY(amd64_lidt)
	lidt (%rdi)
	ret
END(amd64_lidt)

ENTRY(amd64_ltr)
	ltr %di
	ret
END(amd64_ltr)
