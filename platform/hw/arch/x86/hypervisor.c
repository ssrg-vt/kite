/*-
 * Copyright (c) 2015 Martin Lucina.  All Rights Reserved.
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

#include <hw/types.h>
#include <hw/kernel.h>

#include <arch/x86/hypervisor.h>

struct hypervisor_entry {
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t min_leaves;
};

static struct hypervisor_entry hypervisors[HYPERVISOR_NUM] = {
	/* Xen: "XenVMMXenVMM" */
	[ HYPERVISOR_XEN ] = { 0x566e6558, 0x65584d4d, 0x4d4d566e, 2 },
	/* VMware: "VMwareVMware" */
	[ HYPERVISOR_VMWARE ] = { 0x61774d56, 0x4d566572, 0x65726177, 0 },
	/* Hyper-V: "Microsoft Hv" */
	[ HYPERVISOR_HYPERV ] = { 0x7263694d, 0x666f736f, 0x76482074, 0 },
	/* KVM: "KVMKVMKVM\0\0\0" */
	[ HYPERVISOR_KVM ] = { 0x4b4d564b, 0x564b4d56, 0x0000004d, 0 }
};

unsigned hypervisor_detect(void)
{
	uint32_t eax, ebx, ecx, edx;
	unsigned i;

	/*
	 * First check for generic "hypervisor present" bit.
	 */
	x86_cpuid(0x0, &eax, &ebx, &ecx, &edx);
	if (eax >= 0x1) {
		x86_cpuid(0x1, &eax, &ebx, &ecx, &edx);
		if (!(ecx & (1 << 31)))
			return HYPERVISOR_NONE;
	}
	else
		return HYPERVISOR_NONE;

	/*
	 * CPUID leaf at 0x40000000 returns hypervisor vendor signature.
	 * Source: https://lkml.org/lkml/2008/10/1/246
	 */
	x86_cpuid(0x40000000, &eax, &ebx, &ecx, &edx);
	if (!(eax >= 0x40000000))
		return HYPERVISOR_NONE;

	for (i = 0; i < HYPERVISOR_NUM; i++) {
		if (ebx == hypervisors[i].ebx &&
				ecx == hypervisors[i].ecx &&
				edx == hypervisors[i].edx)
			return i;
	}
	return HYPERVISOR_NONE;
}

uint32_t hypervisor_base(unsigned id)
{
	uint32_t base, eax, ebx, ecx, edx;

	if (id >= HYPERVISOR_NUM)
		return 0;

	/* Find the base. */
	for (base = 0x40000000; base < 0x40010000; base += 0x100) {
		x86_cpuid(base, &eax, &ebx, &ecx, &edx);
		if ((eax - base) >= hypervisors[id].min_leaves &&
				ebx == hypervisors[id].ebx &&
				ecx == hypervisors[id].ecx &&
				edx == hypervisors[id].edx) {
			return base;
		}
	}

	return 0;
}
