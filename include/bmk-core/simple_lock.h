#pragma once

#include <bmk-pcpu/pcpu.h>

typedef struct bmk_simple_lock_s {
	__attribute__ ((aligned(BMK_PCPU_L1_SIZE))) long spinlock;
	__attribute__ ((aligned(BMK_PCPU_L1_SIZE))) char _pad[0];
} bmk_simple_lock_t;

#define BMK_SIMPLE_LOCK_INITIALIZER { 0 }

static inline void bmk_simple_lock_init(bmk_simple_lock_t * lock)
{
	lock->spinlock = 0;
}

static inline void bmk_simple_lock_enter(bmk_simple_lock_t * lock)
{
	do {
		while (__atomic_load_n(&lock->spinlock, __ATOMIC_ACQUIRE))
			bmk_cpu_relax();
	} while (__atomic_exchange_n(&lock->spinlock, 1, __ATOMIC_SEQ_CST));
}

static inline void bmk_simple_lock_exit(bmk_simple_lock_t * lock)
{
	__atomic_store_n(&lock->spinlock, 0, __ATOMIC_SEQ_CST);
}
