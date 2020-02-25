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

#define _lwp_park ___lwp_park60
#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/lwpctl.h>
#include <sys/lwp.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/tls.h>

#include <assert.h>
#include <errno.h>
#include <lwp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rump/rump.h>

#include <bmk-core/core.h>
#include <bmk-core/sched.h>
#include <bmk-core/simple_lock.h>

#include <rumprun-base/makelwp.h>

#include "rumprun-private.h"

#define RL_MASK_UNPARK	0x1
#define RL_MASK_PARK	0x2

struct rumprun_lwp {
	struct bmk_block_data rl_header;
	unsigned long rl_value;
	void (*rl_wake) (struct bmk_thread *);
	struct bmk_thread *rl_thread;
	int rl_lwpid;
	char rl_name[MAXCOMLEN+1];
	void (*rl_start)(void *);
	void *rl_arg;

	struct lwpctl rl_lwpctl;

	TAILQ_ENTRY(rumprun_lwp) rl_entries;
};
static TAILQ_HEAD(, rumprun_lwp) all_lwp = TAILQ_HEAD_INITIALIZER(all_lwp);
static __thread struct rumprun_lwp *me;

static bmk_simple_lock_t lwp_lock = BMK_SIMPLE_LOCK_INITIALIZER;

#define FIRST_LWPID 1
static int curlwpid = FIRST_LWPID;

static struct rumprun_lwp mainthread = {
	.rl_lwpid = FIRST_LWPID,
};

static void rumprun_makelwp_tramp(void *);

static ptrdiff_t meoff;
static void
assignme(void *tcb, struct rumprun_lwp *value)
{
	struct rumprun_lwp **dst = (void *)((uintptr_t)tcb + meoff);

	*dst = value;
}

int
_lwp_ctl(int ctl, struct lwpctl **data)
{

	*data = (struct lwpctl *)&me->rl_lwpctl;
	return 0;
}

static void
_lwp_park_callback(struct bmk_thread *prev, struct bmk_block_data *_data)
{
	struct rumprun_lwp *rl = (struct rumprun_lwp *) _data;

	if (rl->rl_wake == bmk_sched_wake_timeq)
		bmk_insert_timeq(prev);

	/* Unset RL_MASK_PARK bit; already unset if RL_MASK_UNPARK is set,
	   but then the fetched value does not equal to RL_MASK_PARK. */
	if (__atomic_fetch_xor(&rl->rl_value, RL_MASK_PARK,
			__ATOMIC_ACQ_REL) != RL_MASK_PARK)
		rl->rl_wake(prev);
}

int
rumprun_makelwp(void (*start)(void *), void *arg, void *private,
	void *stack_base, size_t stack_size, unsigned long flag, lwpid_t *lid)
{
	struct rumprun_lwp *rl;
	struct lwp *curlwp, *newlwp;

	rl = calloc(1, sizeof(*rl));
	if (rl == NULL)
		return errno;
	assignme(private, rl);

	curlwp = rump_pub_lwproc_curlwp();
	if ((errno = rump_pub_lwproc_newlwp(getpid())) != 0) {
		free(rl);
		return errno;
	}
	newlwp = rump_pub_lwproc_curlwp();
	rl->rl_header.callback = _lwp_park_callback;
	rl->rl_value = RL_MASK_PARK;
	rl->rl_start = start;
	rl->rl_arg = arg;
	rl->rl_lwpid = ++curlwpid;
	rl->rl_thread = bmk_sched_create_withtls("lwp", rl, 0, -1,
	    rumprun_makelwp_tramp, newlwp, stack_base, stack_size, private);
	if (rl->rl_thread == NULL) {
		free(rl);
		rump_pub_lwproc_releaselwp();
		rump_pub_lwproc_switch(curlwp);
		return EBUSY; /* ??? */
	}
	rump_pub_lwproc_switch(curlwp);

	*lid = rl->rl_lwpid;
	bmk_simple_lock_enter(&lwp_lock);
	TAILQ_INSERT_TAIL(&all_lwp, rl, rl_entries);
	bmk_simple_lock_exit(&lwp_lock);

	return 0;
}

static void
rumprun_makelwp_tramp(void *arg)
{

	rump_pub_lwproc_switch(arg);
	(me->rl_start)(me->rl_arg);
}

static struct rumprun_lwp *
lwpid2rl(lwpid_t lid)
{
	struct rumprun_lwp *rl;

	if (lid == 0)
		return &mainthread;
	bmk_simple_lock_enter(&lwp_lock);
	TAILQ_FOREACH(rl, &all_lwp, rl_entries) {
		if (rl->rl_lwpid == lid) {
			bmk_simple_lock_exit(&lwp_lock);
			return rl;
		}
	}
	bmk_simple_lock_exit(&lwp_lock);
	return NULL;
}

int
_lwp_unpark(lwpid_t lid, const void *hint)
{
	struct rumprun_lwp *rl;

	if ((rl = lwpid2rl(lid)) == NULL) {
		return -1;
	}

	/* Will only wake up if the callback is complete (scheduled out). */
	if (__atomic_exchange_n(&rl->rl_value, RL_MASK_UNPARK,
				__ATOMIC_ACQ_REL) == 0)
		rl->rl_wake(rl->rl_thread);
	return 0;
}

static void
_lwp_wake_timeq(struct bmk_thread *thread)
{
	struct bmk_tcb *tcb = (struct bmk_tcb *) thread; /* A better way? */
	struct rumprun_lwp *rl;

	/* Get the 'me' variable. */
	rl = *((struct rumprun_lwp **) ((uintptr_t) tcb->btcb_tp + meoff));

	/* Will only wake up if the callback is complete (scheduled out). */
	if (__atomic_exchange_n(&rl->rl_value, RL_MASK_UNPARK,
				__ATOMIC_ACQ_REL) == 0)
		bmk_sched_wake(thread);
}

ssize_t
_lwp_unpark_all(const lwpid_t *targets, size_t ntargets, const void *hint)
{
	ssize_t rv;

	if (targets == NULL)
		return 1024;

	rv = ntargets;
	while (ntargets--) {
		if (_lwp_unpark(*targets, NULL) != 0)
			rv--;
		targets++;
	}
	//assert(rv >= 0);
	return rv;
}

/*
 * called by the scheduler when a context switch is made
 * nb. cookie is null when non-lwp threads are being run
 */
static void
schedhook(void *prevcookie, void *nextcookie)
{
	struct rumprun_lwp *prev, *next;

	prev = prevcookie;
	next = nextcookie;

	if (prev && prev->rl_lwpctl.lc_curcpu != LWPCTL_CPU_EXITED) {
		prev->rl_lwpctl.lc_curcpu = LWPCTL_CPU_NONE;
	}
	if (next) {
		next->rl_lwpctl.lc_curcpu = 0;
		next->rl_lwpctl.lc_pctr++;
	}
}

void
rumprun_lwp_init(void)
{
	void *tcb = bmk_sched_gettcb();

	bmk_sched_set_hook(schedhook);

	meoff = (uintptr_t)&me - (uintptr_t)tcb;
	assignme(tcb, &mainthread);
	mainthread.rl_thread = bmk_sched_init_mainlwp(&mainthread);
	mainthread.rl_header.callback = _lwp_park_callback;
	mainthread.rl_value = RL_MASK_PARK;

	TAILQ_INSERT_TAIL(&all_lwp, me, rl_entries);
}

int
_lwp_park(clockid_t clock_id, int flags, const struct timespec *ts,
	lwpid_t unpark, const void *hint, const void *unparkhint)
{
	int rv;

	if (unpark)
		_lwp_unpark(unpark, unparkhint);

	/* Unparked: clean the RL_MASK_UNPARK bit. */
	if (__atomic_exchange_n(&me->rl_value, RL_MASK_PARK, __ATOMIC_ACQ_REL)
			== RL_MASK_UNPARK) {
		rv = EALREADY;
		goto done;
	}

	if (ts) {
		bmk_time_t nsecs = ts->tv_sec*1000*1000*1000 + ts->tv_nsec;

		if (flags & TIMER_ABSTIME) {
			nsecs -= bmk_platform_cpu_clock_epochoffset();
		} else {
			nsecs += bmk_platform_cpu_clock_monotonic();
		}
		bmk_sched_blockprepare_timeout(nsecs, _lwp_wake_timeq);
		me->rl_wake = bmk_sched_wake_timeq;
	} else {
		bmk_sched_blockprepare();
		me->rl_wake = bmk_sched_wake;
	}

	rv = bmk_sched_block(&me->rl_header);

	/* Clean the RL_MASK_UNPARK bit. */
	__atomic_store_n(&me->rl_value, RL_MASK_PARK, __ATOMIC_SEQ_CST);

	bmk_assert(rv == 0 || rv == ETIMEDOUT);

done:
	if (rv) {
		errno = rv;
		rv = -1;
	}

	return rv;
}

int
_lwp_exit(void)
{

	me->rl_lwpctl.lc_curcpu = LWPCTL_CPU_EXITED;
	rump_pub_lwproc_releaselwp();
	bmk_simple_lock_enter(&lwp_lock);
	TAILQ_REMOVE(&all_lwp, me, rl_entries);
	bmk_simple_lock_exit(&lwp_lock);

	/* could just assign it here, but for symmetry! */
	assignme(bmk_sched_gettcb(), NULL);

	bmk_sched_exit_withtls();

	return 0;
}

int
_lwp_continue(lwpid_t lid)
{
	struct rumprun_lwp *rl;

	if ((rl = lwpid2rl(lid)) == NULL) {
		return ESRCH;
	}

	bmk_sched_unsuspend(rl->rl_thread);
	return 0;
}

int
_lwp_suspend(lwpid_t lid)
{
	struct rumprun_lwp *rl;

	if ((rl = lwpid2rl(lid)) == NULL) {
		return ESRCH;
	}

	bmk_sched_suspend(rl->rl_thread);
	return 0;
}

int
_lwp_wakeup(lwpid_t lid)
{
	struct rumprun_lwp *rl;
	unsigned long value;

	if ((rl = lwpid2rl(lid)) == NULL)
		return ESRCH;

	/* Unpark only if the callback is complete (scheduled out). */
	value = 0;
	if (!__atomic_compare_exchange_n(&rl->rl_value, &value,
			RL_MASK_UNPARK, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return ENODEV;

	rl->rl_wake(rl->rl_thread);
	return 0;
}

int
_lwp_setname(lwpid_t lid, const char *name)
{
	struct rumprun_lwp *rl;

	if ((rl = lwpid2rl(lid)) == NULL)
		return ESRCH;
	strlcpy(rl->rl_name, name, sizeof(rl->rl_name));

	return 0;
}

lwpid_t
_lwp_self(void)
{

	return me->rl_lwpid;
}

/* XXX: messy.  see sched.h, libc, libpthread, and all over */
int _sys_sched_yield(void);
int
_sys_sched_yield(void)
{

	bmk_sched_yield();
	return 0;
}
__weak_alias(sched_yield,_sys_sched_yield);

struct tls_tcb *
_rtld_tls_allocate(void)
{

	return bmk_sched_tls_alloc();
}

void
_rtld_tls_free(struct tls_tcb *arg)
{

	return bmk_sched_tls_free(arg);
}

void *
_lwp_getprivate(void)
{

	return bmk_sched_gettcb();
}

void _lwpnullop(void);
void _lwpnullop(void) { }

int _lwpsuccess(void);
int _lwpsuccess(void) { return 0; }

void _lwpabort(void);
void __dead
_lwpabort(void)
{

	printf("_lwpabort() called\n");
	_exit(1);
}

__strong_alias(_sys_setcontext,_lwpabort);
__strong_alias(_lwp_kill,_lwpabort);

__strong_alias(__libc_static_tls_setup,_lwpnullop);

int rasctl(void);
int rasctl(void) { return ENOSYS; }

/*
 * There is ongoing work to support these in the rump kernel,
 * so I will just stub them out for now.
 */
__strong_alias(_sched_getaffinity,_lwpnullop);
__strong_alias(_sched_getparam,_lwpnullop);
__strong_alias(_sched_setaffinity,_lwpnullop);
__strong_alias(_sched_setparam,_lwpnullop);

/*
 * Technically, specifying a lower >0 protection level is an error,
 * but we don't flag that error for now.
 */
__strong_alias(_sched_protect,_lwpsuccess);
