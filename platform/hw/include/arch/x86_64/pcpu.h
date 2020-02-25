#ifndef _BMK_PCPU_PCPU_H_
#define _BMK_PCPU_PCPU_H_

#define BMK_PCPU_PAGE_SHIFT 12UL
#define BMK_PCPU_PAGE_SIZE (1<<BMK_PCPU_PAGE_SHIFT)
#define BMK_PCPU_L1_SHIFT 7
#define BMK_PCPU_L1_SIZE 128

struct bmk_cpu_info {
	__attribute__ ((aligned(BMK_PCPU_L1_SIZE))) struct bmk_cpu_info *self;
	struct bmk_thread *idle_thread;
	unsigned long cpu;
	/* Interrupt enabling/disabling. */
	unsigned long spldepth;
	/* Base time values at the last call to tscclock_monotonic(). */
	unsigned long long time_base;
	unsigned long long tsc_base;
	__attribute__ ((aligned(BMK_PCPU_L1_SIZE))) char _pad[0];
};

static inline struct bmk_cpu_info *bmk_get_cpu_info(void)
{
	struct bmk_cpu_info *cpu;

	__asm__ __volatile__ ("movq %%gs:0, %0"
			: "=r" (cpu)
			:
	);
	return cpu;
}

static inline void bmk_set_cpu_info(struct bmk_cpu_info *cpu)
{
	unsigned long p = (unsigned long) cpu;

	cpu->self = cpu;
	__asm__ __volatile ("wrmsr" ::
		"c" (0xc0000101),
		"a" ((unsigned)(p)),
		"d" ((unsigned)(p >> 32))
	);
}

static inline void bmk_cpu_relax(void)
{
	__asm__ __volatile__ ("pause" ::: "memory");
}

#endif /* _BMK_PCPU_PCPU_H_ */
