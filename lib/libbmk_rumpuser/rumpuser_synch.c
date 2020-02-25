/*-
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

#include <bmk-core/core.h>
#include <bmk-core/queue.h>
#include <bmk-core/sched.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/errno.h>
#include <stdatomic.h>
#include <stdint.h>

#include <bmk-rumpuser/core_types.h>
#include <bmk-rumpuser/rumpuser.h>

/* RUMPUSER_SYNCH_DEBUG is not very precise but should help
   in debugging of incorrect usage of conditional variables. */
/* #define RUMPUSER_SYNCH_DEBUG */

TAILQ_HEAD(waithead, waiter);
struct waiter {
	struct bmk_thread *thread;
	void (*wake) (struct bmk_thread *);
	TAILQ_ENTRY(waiter) entries;
};

int
rumpuser_thread_create(void *(*f)(void *), void *arg, const char *thrname,
	int joinable, int pri, int cpuidx, void **tptr)
{
	struct bmk_thread *thread;

	thread = bmk_sched_create(thrname, NULL, joinable, cpuidx,
			(void (*)(void *))f, arg, NULL, 0);
	if (!thread)
		return BMK_EINVAL;

	*tptr = thread;
	return 0;
}

void
rumpuser_thread_exit(void)
{
	bmk_sched_exit();
}

int
rumpuser_thread_join(void *p)
{
	bmk_sched_join(p);
	return 0;
}

struct rumpuser_mtx {
	struct bmk_block_queue block;
	struct lwp *owner;
	int flags;
	_Alignas(BMK_PCPU_L1_SIZE) _Atomic(unsigned long) counter;
	_Alignas(BMK_PCPU_L1_SIZE) char _pad[0];
};

void
rumpuser_mutex_init(struct rumpuser_mtx **mtxp, int flags)
{
	struct rumpuser_mtx *mtx;

	mtx = bmk_memcalloc(1, sizeof(*mtx), BMK_MEMWHO_WIREDBMK);
	mtx->block.header.callback = bmk_block_queue_callback;
	mtx->block.queue = bmk_block_queue_alloc();
	mtx->owner = NULL;
	mtx->flags = flags;
	*mtxp = mtx;
}

int
rumpuser_mutex_spin_p(struct rumpuser_mtx *mtx)
{
	return (mtx->flags & RUMPUSER_MTX_SPIN) != 0;
}

void
rumpuser_mutex_enter(struct rumpuser_mtx *mtx)
{
	struct lwp *owner = rumpuser_curlwp();
	int nlocks;

	if (mtx->flags & RUMPUSER_MTX_SPIN) {
		rumpuser_mutex_enter_nowrap(mtx);
		return;
	}

	bmk_assert(mtx->flags & RUMPUSER_MTX_KMUTEX);
	if (atomic_fetch_add(&mtx->counter, 1) != 0) {
		rumpkern_unsched(&nlocks, NULL);
		bmk_sched_blockprepare();
		bmk_sched_block(&mtx->block.header);
		rumpkern_sched(nlocks, NULL);
	}
	mtx->owner = owner;
}

/* A version of mutex_enter that avoids rumpkern_unsched/rumpkern_sched. */
static void
cv_mutex_enter(struct rumpuser_mtx *mtx)
{
	struct lwp *owner = rumpuser_curlwp();

	if (mtx->flags & RUMPUSER_MTX_SPIN) {
		rumpuser_mutex_enter_nowrap(mtx);
		return;
	}

	bmk_assert(mtx->flags & RUMPUSER_MTX_KMUTEX);
	if (atomic_fetch_add(&mtx->counter, 1) != 0) {
		bmk_sched_blockprepare();
		bmk_sched_block(&mtx->block.header);
	}
	mtx->owner = owner;
}

void
rumpuser_mutex_enter_nowrap(struct rumpuser_mtx *mtx)
{
	struct lwp *owner = rumpuser_curlwp();
	unsigned long counter;

	do {
		while ((counter = atomic_load(&mtx->counter)) != 0)
			bmk_cpu_relax();
	} while (!atomic_compare_exchange_weak(&mtx->counter, &counter, 1));

	mtx->owner = owner;
}

int
rumpuser_mutex_tryenter(struct rumpuser_mtx *mtx)
{
	struct lwp *owner = rumpuser_curlwp();
	unsigned long counter;

	counter = atomic_load(&mtx->counter);
	do {
		if (counter != 0)
			return BMK_EBUSY;
	} while (!atomic_compare_exchange_weak(&mtx->counter, &counter, 1));

	mtx->owner = owner;
	return 0;
}

void
rumpuser_mutex_exit(struct rumpuser_mtx *mtx)
{
	mtx->owner = NULL;
	if (atomic_fetch_sub(&mtx->counter, 1) != 1)
		bmk_block_queue_wake_single(mtx->block.queue);
}

void
rumpuser_mutex_destroy(struct rumpuser_mtx *mtx)
{
	bmk_assert(mtx->owner == NULL);
	bmk_memfree(mtx->block.queue, BMK_MEMWHO_WIREDBMK);
	bmk_memfree(mtx, BMK_MEMWHO_WIREDBMK);
}

void
rumpuser_mutex_owner(struct rumpuser_mtx *mtx, struct lwp **lp)
{
	*lp = mtx->owner;
}

enum lock_rw_type { LOCK_RW_UNKNOWN, LOCK_RW_WRITER, LOCK_RW_READER };

struct rumpuser_rw {
	struct rumpuser_mtx *access_mtx;
	struct rumpuser_mtx *wait_mtx;
	_Atomic(unsigned long) readers;
	_Atomic(unsigned long) type;
};

void
rumpuser_rw_init(struct rumpuser_rw **rwp)
{
	struct rumpuser_rw *rw;

	rw = bmk_memcalloc(1, sizeof(*rw), BMK_MEMWHO_WIREDBMK);
	atomic_init(&rw->readers, 0);
	atomic_init(&rw->type, LOCK_RW_UNKNOWN);
	rumpuser_mutex_init(&rw->access_mtx, RUMPUSER_MTX_KMUTEX);
	rumpuser_mutex_init(&rw->wait_mtx, RUMPUSER_MTX_KMUTEX);
	*rwp = rw;
}

void
rumpuser_rw_enter(int enum_rumprwlock, struct rumpuser_rw *rw)
{
	enum rumprwlock type = enum_rumprwlock;

	rumpuser_mutex_enter(rw->wait_mtx);

	switch (type) {
	case RUMPUSER_RW_WRITER:
		rumpuser_mutex_enter(rw->access_mtx);
		atomic_store(&rw->type, LOCK_RW_WRITER);
		break;
	case RUMPUSER_RW_READER:
		if (atomic_fetch_add(&rw->readers, 1) == 0) {
			rumpuser_mutex_enter(rw->access_mtx);
			atomic_store(&rw->type, LOCK_RW_READER);
		}
		break;
	}

	rumpuser_mutex_exit(rw->wait_mtx);
}

int
rumpuser_rw_tryenter(int enum_rumprwlock, struct rumpuser_rw *rw)
{
	enum rumprwlock type = enum_rumprwlock;
	unsigned long readers;
	int rc;

	rc = rumpuser_mutex_tryenter(rw->wait_mtx);
	if (rc != 0)
		goto error2;

	switch (type) {
	case RUMPUSER_RW_WRITER:
		rc = rumpuser_mutex_tryenter(rw->access_mtx);
		if (rc != 0)
			goto error1;
		atomic_store(&rw->type, LOCK_RW_WRITER);
		break;
	case RUMPUSER_RW_READER:
		readers = atomic_load(&rw->readers);
		do {
			if (readers == 0) {
				rc = rumpuser_mutex_tryenter(rw->access_mtx);
				if (rc != 0)
					goto error1;
				atomic_store(&rw->readers, 1);
				atomic_store(&rw->type, LOCK_RW_READER);
			}
		 } while (!atomic_compare_exchange_strong(&rw->readers,
				&readers, readers + 1));
		break;
	}

error1:
	rumpuser_mutex_exit(rw->wait_mtx);
error2:
	return rc;
}

void
rumpuser_rw_exit(struct rumpuser_rw *rw)
{
	if (atomic_load(&rw->type) == LOCK_RW_WRITER ||
			atomic_fetch_sub(&rw->readers, 1) == 1) {
		atomic_store(&rw->type, LOCK_RW_UNKNOWN);
		rumpuser_mutex_exit(rw->access_mtx);
	}
}

void
rumpuser_rw_destroy(struct rumpuser_rw *rw)
{
	rumpuser_mutex_destroy(rw->wait_mtx);
	rumpuser_mutex_destroy(rw->access_mtx);
	bmk_memfree(rw, BMK_MEMWHO_WIREDBMK);
}

void
rumpuser_rw_held(int enum_rumprwlock, struct rumpuser_rw *rw, int *rvp)
{
	enum rumprwlock type = enum_rumprwlock;

	switch (type) {
	case RUMPUSER_RW_WRITER:
		*rvp = atomic_load(&rw->type) == LOCK_RW_WRITER;
		break;
	case RUMPUSER_RW_READER:
		*rvp = atomic_load(&rw->type) == LOCK_RW_READER;
		break;
	}
}

void
rumpuser_rw_downgrade(struct rumpuser_rw *rw)
{
	atomic_store(&rw->type, LOCK_RW_READER);
	if (atomic_fetch_add(&rw->readers, 1) != 0)
		rumpuser_mutex_exit(rw->access_mtx);
}

int
rumpuser_rw_tryupgrade(struct rumpuser_rw *rw)
{
	unsigned long readers = 1;
	if (atomic_compare_exchange_strong(&rw->readers, &readers, 0)) {
		atomic_store(&rw->type, LOCK_RW_WRITER);
		return 0;
	}
	return BMK_EBUSY;
}

struct rumpuser_cv {
	struct waithead waiters;
#ifdef RUMPUSER_SYNCH_DEBUG
	struct rumpuser_mtx *cv_mtx;
#endif
};

struct block_cv {
	struct bmk_block_data header;
	struct rumpuser_mtx *mtx;
};

static void
cv_callback(struct bmk_thread *prev, struct bmk_block_data *_bcv)
{
	struct block_cv *bcv = (struct block_cv *) _bcv;
	rumpuser_mutex_exit(bcv->mtx);
}

static void
cv_callback_timeout(struct bmk_thread *prev, struct bmk_block_data *_bcv)
{
	struct block_cv *bcv = (struct block_cv *) _bcv;
	bmk_insert_timeq(prev);
	rumpuser_mutex_exit(bcv->mtx);
}

static void
cv_sched_enter(int nlocks, struct rumpuser_mtx *mtx)
{
	const int mask = RUMPUSER_MTX_KMUTEX | RUMPUSER_MTX_SPIN;

	/* For a spin mutex, reacquire the CPU context first. */
	if ((mtx->flags & mask) == mask) {
		rumpkern_sched(nlocks, mtx);
		rumpuser_mutex_enter_nowrap(mtx);
	} else {
		cv_mutex_enter(mtx);
		rumpkern_sched(nlocks, mtx);
	}
}

void
rumpuser_cv_init(struct rumpuser_cv **cvp)
{
	struct rumpuser_cv *cv;

	cv = bmk_memcalloc(1, sizeof(*cv), BMK_MEMWHO_WIREDBMK);
	TAILQ_INIT(&cv->waiters);
#ifdef RUMPUSER_SYNCH_DEBUG
	cv->cv_mtx = NULL;
#endif
	*cvp = cv;
}

void
rumpuser_cv_destroy(struct rumpuser_cv *cv)
{
	bmk_assert(*(volatile void **) &cv->waiters.tqh_first == NULL);
	bmk_memfree(cv, BMK_MEMWHO_WIREDBMK);
}

void
rumpuser_cv_wait(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx)
{
	struct waiter w;
	struct block_cv bcv;
	int nlocks;

#ifdef RUMPUSER_SYNCH_DEBUG
	cv->cv_mtx = mtx;
#endif
	rumpkern_unsched(&nlocks, mtx);

	bcv.header.callback = cv_callback;
	bcv.mtx = mtx;
	w.thread = bmk_current;
	w.wake = bmk_sched_wake;

	TAILQ_INSERT_TAIL(&cv->waiters, &w, entries);

	bmk_sched_blockprepare();
	bmk_sched_block(&bcv.header);

	cv_sched_enter(nlocks, mtx);
}

void
rumpuser_cv_wait_nowrap(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx)
{
	struct waiter w;
	struct block_cv bcv;

#ifdef RUMPUSER_SYNCH_DEBUG
	cv->cv_mtx = mtx;
#endif
	bcv.header.callback = cv_callback;
	bcv.mtx = mtx;
	w.thread = bmk_current;
	w.wake = bmk_sched_wake;

	TAILQ_INSERT_TAIL(&cv->waiters, &w, entries);

	bmk_sched_blockprepare();
	bmk_sched_block(&bcv.header);

	rumpuser_mutex_enter_nowrap(mtx);
}

int
rumpuser_cv_timedwait(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx,
	int64_t sec, int64_t nsec)
{
	struct waiter w;
	struct block_cv bcv;
	bmk_time_t time;
	int nlocks, result;

#ifdef RUMPUSER_SYNCH_DEBUG
	cv->cv_mtx = mtx;
#endif
	rumpkern_unsched(&nlocks, mtx);

	bcv.header.callback = cv_callback_timeout;
	bcv.mtx = mtx;
	w.thread = bmk_current;
	w.wake = bmk_sched_wake_timeq;
	time = bmk_platform_cpu_clock_monotonic() +
			sec * UINT64_C(1000000000) + nsec;

	TAILQ_INSERT_TAIL(&cv->waiters, &w, entries);

	bmk_sched_blockprepare_timeout(time, bmk_sched_wake);
	result = bmk_sched_block(&bcv.header);

	cv_sched_enter(nlocks, mtx);
	if (w.thread)
		TAILQ_REMOVE(&cv->waiters, &w, entries);

	return result;
}

void
rumpuser_cv_signal(struct rumpuser_cv *cv)
{
	struct bmk_thread *thread;
	struct waiter *w;

#ifdef RUMPUSER_SYNCH_DEBUG
	bmk_assert(!cv->cv_mtx || cv->cv_mtx->owner == rumpuser_curlwp());
#endif
	if ((w = TAILQ_FIRST(&cv->waiters)) != NULL) {
		TAILQ_REMOVE(&cv->waiters, w, entries);
		thread = w->thread;
		w->thread = NULL;
		w->wake(thread);
	}
}

void
rumpuser_cv_broadcast(struct rumpuser_cv *cv)
{
	struct bmk_thread *thread;
	struct waiter *w;

#ifdef RUMPUSER_SYNCH_DEBUG
	bmk_assert(!cv->cv_mtx || cv->cv_mtx->owner == rumpuser_curlwp());
#endif
	while ((w = TAILQ_FIRST(&cv->waiters)) != NULL) {
		TAILQ_REMOVE(&cv->waiters, w, entries);
		thread = w->thread;
		w->thread = NULL;
		w->wake(thread);
	}
}

void
rumpuser_cv_has_waiters(struct rumpuser_cv *cv, int *rvp)
{
	*rvp = *(volatile void **) &cv->waiters.tqh_first != NULL;
}

static __thread _Atomic(struct lwp *) current_lwp = ATOMIC_VAR_INIT(NULL);

struct lwp *
rumpuser_curlwp(void)
{
	return atomic_load(&current_lwp);
}

void
rumpuser_curlwpop(int enum_rumplwpop, struct lwp *l)
{
	enum rumplwpop op = enum_rumplwpop;

	switch (op) {
	case RUMPUSER_LWP_SET:
		bmk_assert(atomic_load(&current_lwp) == NULL);
		atomic_store(&current_lwp, l);
		break;
	case RUMPUSER_LWP_CLEAR:
		bmk_assert(atomic_load(&current_lwp) == l);
		atomic_store(&current_lwp, NULL);
		break;
	case RUMPUSER_LWP_CREATE:
	case RUMPUSER_LWP_DESTROY:
		break;
	}
}
