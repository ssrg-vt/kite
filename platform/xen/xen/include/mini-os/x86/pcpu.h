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
	__attribute__ ((aligned(BMK_PCPU_L1_SIZE))) char _pad[0];
};

extern struct bmk_cpu_info bmk_xen_cpu_info;

static inline struct bmk_cpu_info *bmk_get_cpu_info(void)
{
	return &bmk_xen_cpu_info;
}

static inline void bmk_cpu_relax(void)
{
	__asm__ __volatile__ ("pause" ::: "memory");
}

#endif /* _BMK_PCPU_PCPU_H_ */
