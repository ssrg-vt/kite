#ifndef _MINIOS_SEMAPHORE_H_
#define _MINIOS_SEMAPHORE_H_

#include <bmk-core/sched.h>

struct semaphore {
	struct bmk_block_queue block;
	long count;
};

/*
 * the semaphore definition
 */
struct rw_semaphore {
	signed long count;
	/* not implemented */
};

static inline void init_SEMAPHORE(struct semaphore *sem, long count)
{
	sem->count = count;
	sem->block.header.callback = bmk_block_queue_callback;
	sem->block.queue = bmk_block_queue_alloc();
}

#define init_MUTEX(sem) init_SEMAPHORE(sem, 1)

static inline int trydown(struct semaphore *sem)
{
	long count = __atomic_load_n(&sem->count, __ATOMIC_SEQ_CST);

	do {
		if (count <= 0)
			return 0;
	} while (!__atomic_compare_exchange_n(&sem->count, &count, count - 1,
			1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

    return 1;
}

static inline void down(struct semaphore *sem)
{
	if (__atomic_fetch_sub(&sem->count, 1, __ATOMIC_SEQ_CST) <= 0) {
		bmk_sched_blockprepare();
		bmk_sched_block(&sem->block.header);
	}
}

static inline void up(struct semaphore *sem)
{
	if (__atomic_fetch_add(&sem->count, 1, __ATOMIC_SEQ_CST) < 0)
		bmk_block_queue_wake(sem->block.queue);
}

/* FIXME! Thre read/write semaphores are unimplemented! */
static inline void init_rwsem(struct rw_semaphore *sem)
{
	sem->count = 1;
}

static inline void down_read(struct rw_semaphore *sem)
{
}


static inline void up_read(struct rw_semaphore *sem)
{
}

static inline void up_write(struct rw_semaphore *sem)
{
}

static inline void down_write(struct rw_semaphore *sem)
{
}

#endif /* _MINIOS_SEMAPHORE_H */
