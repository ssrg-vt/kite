/*-
 * Copyright (c) 2014 Antti Kantee.  All Rights Reserved.
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

#include <hw/types.h>
#include <hw/kernel.h>
#include <hw/multiboot.h>

#include <bmk-core/core.h>
#include <bmk-core/mainthread.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/sched.h>
#include <bmk-core/printf.h>
#include <bmk-core/string.h>

#include <bmk-core/platform.h>

#include <bmk-pcpu/pcpu.h>

#define MP_MAGIC 0x5F504D5FU
#define MT_MAGIC 0x504D4350U

uint16_t bios_ebda_base;

struct x86_mp_pointer {
	uint32_t mp_magic;
	uint32_t mp_table;
	uint8_t mp_length;
	uint8_t mp_revision;
	uint8_t mp_checksum;
	uint8_t mp_defconfig;
	uint32_t mp_features;
};

struct x86_mp_table {
	uint32_t mt_magic;
	uint16_t mt_length;
	uint8_t mt_revision;
	uint8_t mt_checksum;
	char mt_oem[8];
	char mt_product[12];
	uint32_t mt_oem_table;
	uint16_t mt_oem_table_size;
	uint16_t mt_entry_count;
	uint32_t mt_lapic_addr;
	uint16_t mt_extended_table_length;
	uint8_t mt_extended_table_checksum;
	uint8_t mt_reserved;
};

#define X86_MT_TYPE_CPU		0
#define X86_MT_TYPE_BUS		1
#define X86_MT_TYPE_IOAPIC	2
#define X86_MT_TYPE_IOA_ASSIGN	3
#define X86_MT_TYPE_LOC_ASSIGN	4
#define X86_MT_TYPE_COUNT	5

static const uint8_t x86_mt_length[X86_MT_TYPE_COUNT] = { 20, 8, 8, 8, 8 };

struct x86_mt_entry {
	uint8_t me_type;
};

struct x86_mt_cpu {
	uint8_t mc_type;
	uint8_t mc_lapic_id;
	uint8_t mc_lapic_version;
	uint8_t mc_enabled:1;
	uint8_t mc_bootstrap:1;
	uint8_t mc_reserved:6;
	uint32_t mc_signature;
	uint32_t mc_feature;
};

struct x86_mt_ioapic {
	uint8_t mi_type;
	uint8_t mi_ioapic_id;
	uint8_t mi_ioapic_version;
	uint8_t mi_enabled:1;
	uint8_t mi_reserved:7;
	uint32_t mi_address;
};

typedef struct { volatile uint32_t reg; uint32_t pad[3]; } lapic_reg_t;

struct x86_lapic_regs {
	lapic_reg_t lr_reserved1[2];
	lapic_reg_t lr_id;
	lapic_reg_t lr_version;
	lapic_reg_t lr_reserved2[4];
	lapic_reg_t lr_taskprio;
	lapic_reg_t lr_abitrprio;
	lapic_reg_t lr_procprio;
	lapic_reg_t lr_eoi;
	lapic_reg_t lr_reserved3;
	lapic_reg_t lr_logicdest;
	lapic_reg_t lr_destfmt;
	lapic_reg_t lr_spuriousint;
	lapic_reg_t lr_isr[8];
	lapic_reg_t lr_tmr[8];
	lapic_reg_t lr_irr[8];
	lapic_reg_t lr_error;
	lapic_reg_t lr_reserved4[7];
	lapic_reg_t lr_cmd[2];
	lapic_reg_t lr_lvt_timer;
	lapic_reg_t lr_reserved5;
	lapic_reg_t lr_lvt_perf;
	lapic_reg_t lr_lvt_lint[2];
	lapic_reg_t lr_lvt_error;
	lapic_reg_t lr_inittimer;
	lapic_reg_t lr_currtimer;
	lapic_reg_t lr_reserved6[4];
	lapic_reg_t lr_timer_divide;
	lapic_reg_t lr_reserved7;
};

/* Default address. */
static struct x86_lapic_regs * x86_lapic = (struct x86_lapic_regs *) 0xFEE00000;
extern volatile uint32_t boot_num_cpus;

#define X86_LAPIC_INIT_IPI	0x00004500
#define X86_LAPIC_SIPI		0x00004600

static struct x86_mp_pointer * x86_mp_locate(void * _start, void * _end)
{
	struct x86_mp_pointer * start = (struct x86_mp_pointer *) _start;
	struct x86_mp_pointer * end = (struct x86_mp_pointer *) _end;

	do {
		if (start->mp_magic == MP_MAGIC &&
				start->mp_table != 0 &&
				start->mp_length == 1)
			return start;
		start++;
	} while (start != end);
	return NULL;
}

static struct x86_mp_table * x86_mt_locate(void)
{
	struct x86_mp_pointer * mp;
	struct x86_mp_table * mt;
	char * ebda;

	ebda = (char *) ((uintptr_t) bios_ebda_base << 4);
	mp = x86_mp_locate(ebda, ebda + 1024);
	if (!mp) {
		mp = x86_mp_locate((void *) 0x9FC00, (void *) 0xA0000);
		if (!mp) {
			mp = x86_mp_locate((void *) 0xF0000, (void *) 0x100000);
		}
	}
	if (mp) {
		mt = (struct x86_mp_table *) ((uintptr_t) mp->mp_table);
		if (mt->mt_magic == MT_MAGIC) {
			bmk_printf("MP table located %p\n", mt);
			x86_lapic = (struct x86_lapic_regs *) ((uintptr_t) mt->mt_lapic_addr);
			return mt;
		}
	}
	bmk_printf("No MP table found\n");
	return NULL;
}

void trampoline(void);
void trampoline_32(void);

static void x86_mp_delay(bmk_time_t delay)
{
	bmk_time_t start = bmk_platform_cpu_clock_monotonic();
	while (bmk_platform_cpu_clock_monotonic() - start < delay)
		;
}

static void x86_mp_init(void)
{
	struct x86_mp_table * mt = x86_mt_locate();
	struct x86_mt_entry * entry;
	struct x86_mt_cpu * cpu;
	struct x86_mt_ioapic * ioapic;
	uint32_t count;
	uint32_t total_cpus = 0;
	void *backup;

	if (!mt)
		return;

	entry = (struct x86_mt_entry *) (mt + 1);
	count = mt->mt_entry_count;
	for (; count != 0; count--) {
		if (entry->me_type > X86_MT_TYPE_COUNT) {
			bmk_printf("cannot parse MP table: entry type 0x%x\n",
					entry->me_type);
			return;
		}
		switch (entry->me_type) {
			case X86_MT_TYPE_CPU:
				total_cpus++;
				cpu = (struct x86_mt_cpu *) entry;
				bmk_printf("CPU: %x, %x, LAPIC %x\n", cpu->mc_enabled, cpu->mc_bootstrap, cpu->mc_lapic_id);
				break;
			case X86_MT_TYPE_IOAPIC:
				ioapic = (struct x86_mt_ioapic *) entry;
				bmk_printf("IO APIC %x\n", ioapic->mi_ioapic_id);
				break;
			default:
				break;
		}
		entry = (struct x86_mt_entry *)
			((char *) entry + x86_mt_length[entry->me_type]);
	}
	if (total_cpus > 1) {
		unsigned long trampoline_size = (unsigned long)
			((char *) &trampoline_32 - (char *) &trampoline);

		/* Initialize number of CPUs > 1. */
		bmk_numcpus = total_cpus;
		/* Copy the trampoline code */
		backup = bmk_xmalloc_bmk(trampoline_size);
		bmk_memcpy(backup, (void *) 0x7000, trampoline_size);
		bmk_memcpy((void *) 0x7000, &trampoline, trampoline_size);
		/* Spurious interrupt: EN=1 */
		x86_lapic->lr_spuriousint.reg |= (0x1U << 8);
		/* INIT: DSH=11 00 TM=0 LV=1 0 DS=0 DM=0 DMODE=101
		         VECTOR=0000. */
		x86_lapic->lr_cmd[0].reg = 0x000C4500U;
		/* Wait until DS=0 */
		while (x86_lapic->lr_cmd[0].reg & 0x1000U) ;
		x86_mp_delay(10000000);
		/* SIPI: DSH=11 00 TM=0 LV=1 0 DS=0 DM=0 DMODE=110
		         VECTOR=0007 (i.e. the trampoline 0x7000) */
		x86_lapic->lr_cmd[0].reg = 0x000C4607U;
		/* Wait until DS=0 */
		while (x86_lapic->lr_cmd[0].reg & 0x1000U) ;
		x86_mp_delay(200000);
		/* SIPI: DSH=11 00 TM=0 LV=1 0 DS=0 DM=0 DMODE=110
		         VECTOR=0007 (i.e. the trampoline 0x7000) */
		x86_lapic->lr_cmd[0].reg = 0x000C4607U;
		/* Wait until DS=0 */
		while (x86_lapic->lr_cmd[0].reg & 0x1000U) ;
		x86_mp_delay(200000);
		/* Wait until all CPUs wake up */
		while (boot_num_cpus != total_cpus) ;
		/* Restore the trampoline area */
		bmk_memcpy((void *) 0x7000, backup, trampoline_size);
		bmk_memfree(backup, BMK_MEMWHO_WIREDBMK);
	}
	bmk_printf("all %u CPUs are awake\n", total_cpus);
}

static volatile int main_cpu_ready = 0;

struct bmk_cpu_info x86_cpu_info[BMK_MAXCPUS];

void
x86_boot(struct multiboot_info *mbi, unsigned long cpu)
{
	x86_cpu_info[cpu].cpu = cpu;
	x86_cpu_info[cpu].spldepth = 1;
	bmk_set_cpu_info(&x86_cpu_info[cpu]);

	/* Other CPUs jumping from the trampoline. */
	if (cpu) {
		/* Initialize interrupts. */
		cpu_init_notmain(cpu);
		spl0();

		/* Wait until the main CPU completes. */
		while (!main_cpu_ready) {}

		/* Go to the scheduler. */
		bmk_sched_startmain(NULL, NULL);
		return;
	}

	/* Main bootstrapping CPU. */
	cons_init();
	bmk_printf("rump kernel bare metal bootstrap\n\n");

	cpu_init();

	multiboot(mbi);

	x86_mp_init();

	bmk_sched_init();
	intr_init();

	spl0();

	bmk_sched_startmain(bmk_mainthread, multiboot_cmdline);
}

void
bmk_platform_ready(void)
{
	main_cpu_ready = 1;
}
