/*-
 * Copyright (c) 2015 Antti Kantee.  All Rights Reserved.
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

/*
 * Historically based on the Xen Mini-OS scheduler by Grzegorz Milos,
 * rewritten to deal with multiple infrequently running threads in the
 * current reincarnation. Heavily modified by Ruslan Nikolaev to support SMP.
 */

#include <bmk-core/core.h>
#include <bmk-core/errno.h>
#include <bmk-core/mainthread.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/platform.h>
#include <bmk-core/pgalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/queue.h>
#include <bmk-core/string.h>
#include <bmk-core/sched.h>
#include <bmk-core/simple_lock.h>

#include <stdatomic.h>
#include <stddef.h>

#include <bmk-core/types.h>
#include <bmk-core/lfring.h>

void *bmk_mainstackbase;
unsigned long bmk_mainstacksize;

/*
 * sleep for how long if there's absolutely nothing to do
 * (default 1s)
 */
#define BLOCKTIME_MAX (1*1000*1000*1000)

#define NAME_MAXLEN 16
#define MAXCPUS 64

/* flags and their meanings + invariants */
#define THR_MUSTJOIN	0x0001
#define THR_EXTSTACK	0x0002

#if !(defined(__i386__) || defined(__x86_64__))
#define _TLS_I
#else
#define _TLS_II
#endif

extern const char _tdata_start[], _tdata_end[];
extern const char _tbss_start[], _tbss_end[];
#define TDATASIZE (_tdata_end - _tdata_start)
#define TBSSSIZE (_tbss_end - _tbss_start)
#define TMEMSIZE \
    (((TDATASIZE + TBSSSIZE + sizeof(void *)-1)/sizeof(void *))*sizeof(void *))
#ifdef _TLS_I
#define TCBOFFSET 0
#else
#define TCBOFFSET TMEMSIZE
#endif
#define TLSAREASIZE (TMEMSIZE + BMK_TLS_EXTRA)

#define bmk_container_of(addr, type, field)	\
	(type *) ((char *) (addr) - offsetof(type, field))

struct bmk_join_data {
	struct bmk_block_data header;
	_Atomic(unsigned long) value;
};

struct bmk_thread {
	/* MD thread control block */
	struct bmk_tcb bt_tcb;

	unsigned int bt_idx;
	int bt_flags;
	unsigned int bt_cpuidx;

	char bt_name[NAME_MAXLEN];

	void *bt_stackbase;
	void *bt_cookie;

	bmk_time_t bt_wakeup_time;
	void (*bt_wake) (struct bmk_thread *);

	int bt_errno;
	int bt_timedout;

	struct bmk_join_data bt_join;
	struct bmk_join_data bt_exit;

	TAILQ_ENTRY(bmk_thread) bt_schedq;
	__attribute__ ((aligned(BMK_PCPU_L1_SIZE))) char _pad[0];
} __attribute__ ((aligned(BMK_PCPU_L1_SIZE)));
__thread struct bmk_thread *bmk_current;
static struct bmk_thread *thread_array;

TAILQ_HEAD(threadqueue, bmk_thread);

/*
 * We have 3 different queues for theoretically runnable threads:
 * 1) runnable threads waiting to be scheduled
 * 2) threads waiting for a timeout to expire (or to be woken up)
 * 3) threads waiting indefinitely for a wakeup
 *
 * Rules: while running, threads are on no schedq.  Threads can block
 *        only themselves (though that needs revisiting for "suspend").
 *        when blocked, threads will move either to blockq or timeq.
 *        When a thread is woken up (possibly by itself of a timeout
 *        expires), the thread will move to the runnable queue.  Wakeups
 *        while a thread is already in the runnable queue or while
 *        running (via interrupt handler) have no effect.
 */

static struct lfring * runq[MAXCPUS+1], * freeq, * zombieq;
static struct threadqueue timeq[MAXCPUS+1];

static void (*scheduler_hook)(void *, void *);

static void
join_callback(struct bmk_thread *prev, struct bmk_block_data *_join)
{
	struct bmk_join_data *join = (struct bmk_join_data *) _join;

	if (atomic_exchange(&join->value, (unsigned long) prev) == 1)
		bmk_sched_wake(prev);
}

static inline void
join_init(struct bmk_join_data *join)
{
	join->header.callback = join_callback;
	atomic_init(&join->value, 1);
}

static inline void
join_wait(struct bmk_join_data *join)
{
	if (atomic_fetch_sub(&join->value, 1) == 1) {
		bmk_sched_blockprepare();
		bmk_sched_block(&join->header);
	}
}

static inline void
join_post(struct bmk_join_data *join)
{
	unsigned long value = atomic_fetch_add(&join->value, 1);

	if (value > 1) {
		struct bmk_thread * thread = (struct bmk_thread *) value;
		bmk_sched_wake(thread);
	}
}

static bmk_simple_lock_t timeq_lock = BMK_SIMPLE_LOCK_INITIALIZER;

#if 0
static void
print_threadinfo(struct bmk_thread *thread)
{

	bmk_printf("thread \"%s\" at %p, flags 0x%x\n",
	    thread->bt_name, thread, thread->bt_flags);
}
#endif

/*
 * Insert thread into timeq at the correct place.
 */
void
bmk_insert_timeq(struct bmk_thread *thread)
{
	struct bmk_thread *iter;
	unsigned int cpuidx = thread->bt_cpuidx;

	bmk_simple_lock_enter(&timeq_lock);

	/*
	 * Currently we require that a thread will block only
	 * once before calling the scheduler.
	 */

	bmk_assert(thread->bt_wakeup_time != BMK_SCHED_BLOCK_INFTIME);

	/* case1: no others */
	if (TAILQ_EMPTY(&timeq[cpuidx])) {
		TAILQ_INSERT_HEAD(&timeq[cpuidx], thread, bt_schedq);
		goto done;
	}

	/* case2: not last in queue */
	TAILQ_FOREACH(iter, &timeq[cpuidx], bt_schedq) {
		if (iter->bt_wakeup_time > thread->bt_wakeup_time) {
			TAILQ_INSERT_BEFORE(iter, thread, bt_schedq);
			goto done;
		}
	}

	/* case3: last in queue with greatest current timeout */
	bmk_assert(TAILQ_LAST(&timeq[cpuidx], threadqueue)->bt_wakeup_time
	    <= thread->bt_wakeup_time);
	TAILQ_INSERT_TAIL(&timeq[cpuidx], thread, bt_schedq);

done:
	bmk_simple_lock_exit(&timeq_lock);
}

static void
stackalloc(void **stack, unsigned long *ss)
{
	*stack = bmk_pgalloc(bmk_stackpageorder);
	*ss = bmk_stacksize;
}

static void
stackfree(struct bmk_thread *thread)
{
	bmk_pgfree(thread->bt_stackbase, bmk_stackpageorder);
}

void
bmk_sched_dumpqueue(void)
{
#if 0
	struct bmk_thread *thr;

	bmk_printf("BEGIN runq dump\n");
	TAILQ_FOREACH(thr, &runq, bt_schedq) {
		print_threadinfo(thr);
	}
	bmk_printf("END runq dump\n");

	bmk_printf("BEGIN timeq dump\n");
	TAILQ_FOREACH(thr, &timeq, bt_schedq) {
		print_threadinfo(thr);
	}
	bmk_printf("END timeq dump\n");

	bmk_printf("BEGIN blockq dump\n");
	TAILQ_FOREACH(thr, &blockq, bt_schedq) {
		print_threadinfo(thr);
	}
	bmk_printf("END blockq dump\n");
#endif
}

static void
sched_switch(struct bmk_thread *prev, struct bmk_thread *next,
	     struct bmk_block_data *data)
{
	if (scheduler_hook)
		scheduler_hook(prev->bt_cookie, next->bt_cookie);

	bmk_platform_cpu_sched_settls(&next->bt_tcb);

	bmk_cpu_sched_switch(&prev->bt_tcb, data, &next->bt_tcb);
}

static void
schedule(struct bmk_block_data *data)
{
	struct bmk_thread *prev, *next, *thread;
	struct bmk_thread *idle_thread;
	struct bmk_cpu_info *info = bmk_get_cpu_info();
	unsigned long cpuidx = info->cpu;
	size_t idx;

	prev = bmk_current;
	for (;;) {
		bmk_time_t curtime, waketime;

		if ((idx = lfring_dequeue_single(runq[cpuidx],
				BMK_MAX_THREADS_ORDER, false))
				!= LFRING_EMPTY) {
			next = &thread_array[idx];
			break;
		}

		if (bmk_numcpus != 1 &&
			(idx = lfring_dequeue(runq[MAXCPUS],
				BMK_MAX_THREADS_ORDER, false))
				!= LFRING_EMPTY) {
			next = &thread_array[idx];
			break;
		}

		curtime = bmk_platform_cpu_clock_monotonic();
		waketime = curtime + BLOCKTIME_MAX;

		/* TODO: Probably need a better strategy to check timeq. */

		/*
		 * Process timeout queue first by moving threads onto
		 * the runqueue if their timeouts have expired.  Since
		 * the timeouts are sorted, we process until we hit the
		 * first one which will not be woked up.
		 */
		bmk_simple_lock_enter(&timeq_lock);
		while ((thread = TAILQ_FIRST(&timeq[cpuidx])) != NULL) {
			if (thread->bt_wakeup_time <= curtime) {
				/*
				 * move thread to runqueue.
				 * threads will run in inverse order of timeout
				 * expiry.  not sure if that matters or not.
				 */
				thread->bt_timedout = BMK_ETIMEDOUT;
				TAILQ_REMOVE(&timeq[cpuidx], thread, bt_schedq);
				thread->bt_wake(thread);
			} else {
				if (thread->bt_wakeup_time < waketime)
					waketime = thread->bt_wakeup_time;
				break;
			}
		}
		if (bmk_numcpus != 1) {
			while ((thread = TAILQ_FIRST(&timeq[MAXCPUS]))
					!= NULL) {
				if (thread->bt_wakeup_time <= curtime) {
					thread->bt_timedout = BMK_ETIMEDOUT;
					TAILQ_REMOVE(&timeq[MAXCPUS],
							thread, bt_schedq);
					thread->bt_wake(thread);
				} else {
					if (thread->bt_wakeup_time < waketime)
						waketime =
							thread->bt_wakeup_time;
					break;
				}
			}
		}
		bmk_simple_lock_exit(&timeq_lock);

		idle_thread = info->idle_thread;
		if (prev != idle_thread) {
			next = idle_thread;
			break;
		}

		/*
		 * Nothing to run, block until waketime or until an interrupt
		 * occurs, whichever happens first.  The call will enable
		 * interrupts "atomically" before actually blocking.
		 */
		//FIXME: need a proper way to sleep across all CPUs
		//bmk_platform_cpu_block(waketime);
	}

	/*
	 * No switch can happen if:
	 *  + timeout expired while we were in here
	 *  + interrupt handler woke us up before anything else was scheduled
	 */
	if (prev != next) {
		sched_switch(prev, next, data);
	}

	/*
	 * Reaper.  This always runs in the context of the first "non-virgin"
	 * thread that was scheduled after the current thread decided to exit.
	 */
	while ((idx = lfring_dequeue(zombieq, BMK_MAX_THREADS_ORDER, false))
			!= LFRING_EMPTY) {
		struct bmk_thread *thread = &thread_array[idx];

		if ((thread->bt_flags & THR_EXTSTACK) == 0)
			stackfree(thread);
		lfring_enqueue(freeq, BMK_MAX_THREADS_ORDER, idx);
	}
}

/*
 * Allocate tls and initialize it.
 * NOTE: does not initialize tcb, see inittcb().
 */
void *
bmk_sched_tls_alloc(void)
{
	char *tlsmem, *p;

	tlsmem = p = bmk_memalloc(TLSAREASIZE, 0, BMK_MEMWHO_WIREDBMK);
#ifdef _TLS_I
	bmk_memset(p, 0, 2*sizeof(void *));
	p += 2 * sizeof(void *);
#endif
	bmk_memcpy(p, _tdata_start, TDATASIZE);
	bmk_memset(p + TDATASIZE, 0, TBSSSIZE);

	return tlsmem + TCBOFFSET;
}

/*
 * Free tls
 */
void
bmk_sched_tls_free(void *mem)
{

	mem = (void *)((unsigned long)mem - TCBOFFSET);
	bmk_memfree(mem, BMK_MEMWHO_WIREDBMK);
}

void *
bmk_sched_gettcb(void)
{

	return (void *)bmk_current->bt_tcb.btcb_tp;
}

static void
inittcb(struct bmk_tcb *tcb, void *tlsarea, unsigned long tlssize)
{

#ifdef _TLS_II
	*(void **)tlsarea = tlsarea;
#endif
	tcb->btcb_tp = (unsigned long)tlsarea;
#if 0
	tcb->btcb_tpsize = tlssize; /* Some platforms may need it. */
#endif
}

static long bmk_curoff;
static void
initcurrent(void *tcb, struct bmk_thread *value)
{
	struct bmk_thread **dst = (void *)((unsigned long)tcb + bmk_curoff);

	*dst = value;
}

static struct bmk_thread *
do_sched_create_withtls(const char *name, void *cookie, int joinable,
	int cpuidx, void (*f)(void *), void *data,
	void *stack_base, unsigned long stack_size, void *tlsarea, bool insert)
{
	size_t idx = lfring_dequeue(freeq, BMK_MAX_THREADS_ORDER, false);
	struct bmk_thread *thread;

	if (idx == LFRING_EMPTY)
		return NULL;

	thread = &thread_array[idx];
	bmk_memset(thread, 0, sizeof(*thread));
	thread->bt_idx = (unsigned int) idx;
	thread->bt_cpuidx = (bmk_numcpus == 1) ? 0 :
		((cpuidx == -1) ? MAXCPUS : (unsigned int) cpuidx);
	if (thread->bt_cpuidx > MAXCPUS)
		bmk_platform_halt("out of range CPU index");
	bmk_strncpy(thread->bt_name, name, sizeof(thread->bt_name)-1);

	if (!stack_base) {
		bmk_assert(stack_size == 0);
		stackalloc(&stack_base, &stack_size);
	} else {
		thread->bt_flags = THR_EXTSTACK;
	}
	thread->bt_stackbase = stack_base;
	if (joinable) {
		thread->bt_flags |= THR_MUSTJOIN;
		join_init(&thread->bt_join);
		join_init(&thread->bt_exit);
	}

	bmk_cpu_sched_create(thread, &thread->bt_tcb, f, data,
	    stack_base, stack_size);

	thread->bt_cookie = cookie;
	thread->bt_wakeup_time = BMK_SCHED_BLOCK_INFTIME;

	inittcb(&thread->bt_tcb, tlsarea, TCBOFFSET);
	initcurrent(tlsarea, thread);

	if (insert)
		lfring_enqueue(runq[thread->bt_cpuidx], BMK_MAX_THREADS_ORDER,
				idx);

	return thread;
}

struct bmk_thread *
bmk_sched_create_withtls(const char *name, void *cookie, int joinable,
	int cpuidx, void (*f)(void *), void *data,
	void *stack_base, unsigned long stack_size, void *tlsarea)
{
	return do_sched_create_withtls(name, cookie, joinable, cpuidx, f,
			data, stack_base, stack_size, tlsarea, true);
}

static struct bmk_thread *
do_sched_create(const char *name, void *cookie, int joinable,
	int cpuidx, void (*f)(void *), void *data,
	void *stack_base, unsigned long stack_size, bool insert)
{
	return do_sched_create_withtls(name, cookie, joinable, cpuidx, f,
		data, stack_base, stack_size, bmk_sched_tls_alloc(), insert);
}

struct bmk_thread *
bmk_sched_create(const char *name, void *cookie, int joinable, int cpuidx,
	void (*f)(void *), void *data,
	void *stack_base, unsigned long stack_size)
{
	return do_sched_create(name, cookie, joinable, cpuidx, f, data,
			stack_base, stack_size, true);
}

static void
exit_callback(struct bmk_thread *prev, struct bmk_block_data *data)
{
	/* Put onto exited list */
	lfring_enqueue(zombieq, BMK_MAX_THREADS_ORDER, prev->bt_idx);
}

static struct bmk_block_data exit_data = { .callback = exit_callback };

void
bmk_sched_exit_withtls(void)
{
	struct bmk_thread *thread = bmk_current;

	if (thread->bt_flags & THR_MUSTJOIN) {
		join_post(&thread->bt_join);
		join_wait(&thread->bt_exit);
	}

	/* bye */
	schedule(&exit_data);
	bmk_platform_halt("schedule() returned for a dead thread!\n");
}

void
bmk_sched_exit(void)
{
	bmk_sched_tls_free((void *)bmk_current->bt_tcb.btcb_tp);
	bmk_sched_exit_withtls();
}

void
bmk_sched_join(struct bmk_thread *joinable)
{
	bmk_assert(joinable->bt_flags & THR_MUSTJOIN);

	join_wait(&joinable->bt_join);
	join_post(&joinable->bt_exit);
}

/*
 * These suspend calls are different from block calls in the that
 * can be used to block other threads.  The only reason we need these
 * was because someone was clever enough to invent _np interfaces for
 * libpthread which allow randomly suspending other threads.
 */
void
bmk_sched_suspend(struct bmk_thread *thread)
{
	bmk_platform_halt("sched_suspend unimplemented");
}

void
bmk_sched_unsuspend(struct bmk_thread *thread)
{
	bmk_platform_halt("sched_unsuspend unimplemented");
}

void
bmk_sched_blockprepare_timeout(bmk_time_t deadline,
			void (*wake) (struct bmk_thread *))
{
	bmk_current->bt_wakeup_time = deadline;
	bmk_current->bt_wake = wake;
}

void
bmk_sched_blockprepare(void)
{
	bmk_current->bt_wakeup_time = BMK_SCHED_BLOCK_INFTIME;
}

int
bmk_sched_block(struct bmk_block_data *data)
{
	bmk_current->bt_timedout = 0;
	schedule(data);
	return bmk_current->bt_timedout;
}

void
bmk_sched_wake(struct bmk_thread *thread)
{
	lfring_enqueue(runq[thread->bt_cpuidx], BMK_MAX_THREADS_ORDER,
			thread->bt_idx);
}

void
bmk_sched_wake_timeq(struct bmk_thread *thread)
{
	int timedout;

	bmk_simple_lock_enter(&timeq_lock);
	timedout = thread->bt_timedout;
	if (!timedout) {
		TAILQ_REMOVE(&timeq[thread->bt_cpuidx], thread, bt_schedq);
	}
	bmk_simple_lock_exit(&timeq_lock);

	if (!timedout)
		bmk_sched_wake(thread);
}

/*
 * Calculate offset of bmk_current early, so that we can use it
 * in thread creation.  Attempt to not depend on allocating the
 * TLS area so that we don't have to have malloc initialized.
 * We will properly initialize TLS for the main thread later
 * when we start the main thread (which is not necessarily the
 * first thread that we create).
 */

void
bmk_sched_init(void)
{
	unsigned long tlsinit;
	struct bmk_tcb tcbinit;
	size_t i, thread_order;
	struct lfring *local_runq;
	unsigned long ncpus = bmk_numcpus;

	if (ncpus > MAXCPUS)
		bmk_platform_halt("too many CPUs");

	for (i = 0; i <= MAXCPUS; i++) {
		struct threadqueue tq_init = TAILQ_HEAD_INITIALIZER(timeq[i]);
		timeq[i] = tq_init;
		runq[i] = NULL;
	}

	thread_order = (8 * sizeof(long) - 1)
			- __builtin_clzl(sizeof(struct bmk_thread));
	thread_array = bmk_pgalloc(BMK_MAX_THREADS_ORDER + thread_order
				- BMK_PCPU_PAGE_SHIFT);

	freeq = bmk_memalloc(LFRING_SIZE(BMK_MAX_THREADS_ORDER),
			LFRING_ALIGN, BMK_MEMWHO_WIREDBMK);
	if (!freeq)
		bmk_platform_halt("cannot allocate freeq");

	/* The thread index 0 is reserved for marked queues, so
	   skip it and avoid ever using it. */
	lfring_init_fill(freeq, 1,
		BMK_MAX_THREADS, BMK_MAX_THREADS_ORDER);

	for (i = 0; i < ncpus; i++) {
		local_runq = bmk_memalloc(LFRING_SIZE(BMK_MAX_THREADS_ORDER),
			LFRING_ALIGN, BMK_MEMWHO_WIREDBMK);
		if (!local_runq)
			bmk_platform_halt("cannot allocate local runq");
		lfring_init_empty(local_runq, BMK_MAX_THREADS_ORDER);
		runq[i] = local_runq;
	}

	if (ncpus != 1) {
		runq[MAXCPUS] = bmk_memalloc(LFRING_SIZE(BMK_MAX_THREADS_ORDER),
				LFRING_ALIGN, BMK_MEMWHO_WIREDBMK);
		if (!runq[MAXCPUS])
			bmk_platform_halt("cannot allocate runq");
		lfring_init_empty(runq[MAXCPUS], BMK_MAX_THREADS_ORDER);
	}

	zombieq = bmk_memalloc(LFRING_SIZE(BMK_MAX_THREADS_ORDER),
			LFRING_ALIGN, BMK_MEMWHO_WIREDBMK);
	if (!zombieq)
		bmk_platform_halt("cannot allocate zombieq");
	lfring_init_empty(zombieq, BMK_MAX_THREADS_ORDER);

	inittcb(&tcbinit, &tlsinit, 0);
	bmk_platform_cpu_sched_settls(&tcbinit);

	/*
	 * Not sure if the membars are necessary, but better to be
	 * Marvin the Paranoid Paradroid than get eaten by 999
	 */
	__asm__ __volatile__("" ::: "memory");
	bmk_curoff = (unsigned long)&bmk_current - (unsigned long)&tlsinit;
	__asm__ __volatile__("" ::: "memory");

	/*
	 * Set TLS back to 0 so that it's easier to catch someone trying
	 * to use it until we get TLS really initialized.
	 */
	tcbinit.btcb_tp = 0;
	bmk_platform_cpu_sched_settls(&tcbinit);
}

static void idle_thread(void *arg)
{
	while (1) {
		schedule(NULL);
	}
}

void __attribute__((noreturn))
bmk_sched_startmain(void (*mainfun)(void *), void *arg)
{
	struct bmk_thread *thread;
	struct bmk_cpu_info *info = bmk_get_cpu_info();
	struct bmk_thread initthread;

	thread = do_sched_create("idle", NULL, 0, (unsigned int) info->cpu,
			idle_thread, NULL, NULL, 0, false);
	info->idle_thread = thread;
	bmk_memset(&initthread, 0, sizeof(initthread));
	bmk_strcpy(initthread.bt_name, "init");

	if (mainfun) {
		stackalloc(&bmk_mainstackbase, &bmk_mainstacksize);
		thread = do_sched_create("main", NULL, 0, -1, mainfun, arg,
				bmk_mainstackbase, bmk_mainstacksize, false);
		if (thread == NULL)
			bmk_platform_halt("failed to create main thread");
	}

	/*
	 * Manually switch to mainthread without going through
	 * bmk_sched (avoids confusion with bmk_current).
	 */
	sched_switch(&initthread, thread, NULL);

	bmk_platform_halt("bmk_sched_init unreachable");
}

void
bmk_sched_set_hook(void (*f)(void *, void *))
{
	scheduler_hook = f;
}

struct bmk_thread *
bmk_sched_init_mainlwp(void *cookie)
{
	bmk_current->bt_cookie = cookie;
	return bmk_current;
}

const char *
bmk_sched_threadname(struct bmk_thread *thread)
{
	return thread->bt_name;
}

/*
 * XXX: this does not really belong here, but libbmk_rumpuser needs
 * to be able to set an errno, so we can't push it into libc without
 * violating abstraction layers.
 */
int *
bmk_sched_geterrno(void)
{
	return &bmk_current->bt_errno;
}

static void
yield_callback(struct bmk_thread *prev, struct bmk_block_data *data)
{
	/* make schedulable and re-insert into the run queue */
	lfring_enqueue(runq[prev->bt_cpuidx], BMK_MAX_THREADS_ORDER,
			prev->bt_idx);
}

static struct bmk_block_data yield_data = { .callback = yield_callback };

void
bmk_sched_yield(void)
{
	schedule(&yield_data);
}

void
bmk_block_queue_callback(struct bmk_thread *prev, struct bmk_block_data *_block)
{
	struct bmk_block_queue *block = (struct bmk_block_queue *) _block;

	if (!lfring_enqueue(block->queue, BMK_MAX_THREADS_ORDER, prev->bt_idx))
		bmk_sched_wake(prev);
}

struct lfring *
bmk_block_queue_alloc(void)
{
	struct lfring *queue = bmk_memalloc(LFRING_SIZE(BMK_MAX_THREADS_ORDER),
		LFRING_ALIGN, BMK_MEMWHO_WIREDBMK);
	if (!queue)
		bmk_platform_halt("cannot allocate a marked queue");
	lfring_init_mark(queue, BMK_MAX_THREADS_ORDER);
	return queue;
}

void
bmk_block_queue_wake(struct lfring *queue)
{
	size_t idx = lfring_dequeue_mark(queue, BMK_MAX_THREADS_ORDER);
	if (idx != 0) {
		struct bmk_thread *thread = &thread_array[idx];
		lfring_enqueue(runq[thread->bt_cpuidx], BMK_MAX_THREADS_ORDER,
				idx);
	}
}

void
bmk_block_queue_wake_single(struct lfring *queue)
{
	size_t idx = lfring_dequeue_mark_single(queue, BMK_MAX_THREADS_ORDER);
	if (idx != 0) {
		struct bmk_thread *thread = &thread_array[idx];
		lfring_enqueue(runq[thread->bt_cpuidx], BMK_MAX_THREADS_ORDER,
				idx);
	}
}
