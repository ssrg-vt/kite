/*-
 * Copyright (c) 2019 Ruslan Nikolaev.  All Rights Reserved.
 *
 * Derived from https://github.com/rusnikola/lfqueue
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

#ifndef __LFRING_H
#define __LFRING_H	1

#include <stdatomic.h>
#include <stdbool.h>

#include "lfconfig.h"

#if LFATOMIC_WIDTH == 32
# define LFRING_MIN	(LF_CACHE_SHIFT - 2)
#elif LFATOMIC_WIDTH == 64
# define LFRING_MIN	(LF_CACHE_SHIFT - 3)
#elif LFATOMIC_WIDTH == 128
# define LFRING_MIN	(LF_CACHE_SHIFT - 4)
#else
# error "Unsupported LFATOMIC_WIDTH."
#endif

#define LFRING_ALIGN	(_Alignof(struct __lfring))
#define LFRING_SIZE(o)	\
	(offsetof(struct __lfring, array) + (sizeof(lfatomic_t) << (o)))

#define LFRING_EMPTY	(~(size_t) 0U)

#if LFRING_MIN != 0
static inline size_t __lfring_map(lfatomic_t idx, size_t order, size_t n)
{
	return (size_t) (((idx & (n - 1)) >> (order - LFRING_MIN)) |
			((idx << LFRING_MIN) & (n - 1)));
}
#else
static inline size_t __lfring_map(lfatomic_t idx, size_t order, size_t n)
{
	return (size_t) (idx & (n - 1));
}
#endif

static inline size_t lfring_pow2(size_t order)
{
	return (size_t) 1U << order;
}

struct __lfring {
	_Alignas(LF_CACHE_BYTES) _Atomic(lfatomic_t) head;
	_Alignas(LF_CACHE_BYTES) _Atomic(lfatomic_t) tail;
	_Alignas(LF_CACHE_BYTES) _Atomic(lfatomic_t) array[1];
};

struct lfring;
typedef bool (*lfring_process_t) (struct lfring *, size_t, void *);

static inline void lfring_init_empty(struct lfring * ring, size_t order)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t i, n = lfring_pow2(order);

	for (i = 0; i != n; i++) {
		atomic_init(&q->array[i], 0);
	}

	atomic_init(&q->head, n);
	atomic_init(&q->tail, n);
}

static inline void lfring_init_full(struct lfring * ring, size_t order)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t i, n = lfring_pow2(order);

	for (i = 0; i != n; i++) {
		atomic_init(&q->array[i], i);
	}

	atomic_init(&q->head, 0);
	atomic_init(&q->tail, n);
}

static inline void lfring_init_fill(struct lfring * ring,
		size_t s, size_t e, size_t order)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t i, n = lfring_pow2(order);

	for (i = 0; i != s; i++) {
		atomic_init(&q->array[__lfring_map(i, order, n)], 0);
	}
	for (; i != e; i++) {
		atomic_init(&q->array[__lfring_map(i, order, n)], i);
	}
	for (; i != n; i++) {
		atomic_init(&q->array[__lfring_map(i, order, n)], (lfatomic_t) -n);
	}
	atomic_init(&q->head, s);
	atomic_init(&q->tail, e);
}

/* An equivalent to lfring_init_empty but avoids the reserved index 0. */
static inline void lfring_init_mark(struct lfring * ring, size_t order)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t i, n = lfring_pow2(order);

	for (i = 0; i != n; i++) {
		atomic_init(&q->array[i], n - 1);
	}

	atomic_init(&q->head, n);
	atomic_init(&q->tail, n);
}

static inline lfatomic_t lfring_head(struct lfring * ring)
{
	struct __lfring * q = (struct __lfring *) ring;
	return atomic_load(&q->head);
}

static inline size_t __lfring_enqueue(struct lfring * ring,
		size_t order, size_t eidx, lfring_process_t process, void * data)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t n = lfring_pow2(order);
	lfatomic_t tail;

start_over:
	tail = atomic_load(&q->tail);

	while (1) {
		lfatomic_t tcycle = tail & ~(n - 1);
		size_t tidx = __lfring_map(tail, order, n);
		lfatomic_t entry = atomic_load(&q->array[tidx]);

		while (1) {
			lfatomic_t ecycle = entry & ~(n - 1);

			if (ecycle == tcycle) {
				/* Advance the tail pointer. */
				if (atomic_compare_exchange_strong(&q->tail, &tail,
						tail + 1)) {
					tail++;
				}
				break;
			}

			/* Wrapping around. */
			if ((lfatomic_t) (ecycle + n) != tcycle) {
				goto start_over;
			}

			/* Process the previous entry. */
			if (process) {
				size_t pidx = __lfring_map(tail - 1, order, n);
				if (!process(ring, (size_t)
						atomic_load(&q->array[pidx]) & (n - 1),
						data)) {
					lfatomic_t old_entry = entry;

					/* Check it is still an empty entry. */
					entry = atomic_load(&q->array[tidx]);
					if (old_entry == entry) {
						return entry & (n - 1);
					}
					continue;
				}
			}

			/* An empty entry. */
			if (atomic_compare_exchange_strong(&q->array[tidx],
					&entry, __LFMERGE(tcycle, eidx))) {
				/* Try to advance the tail pointer. */
				atomic_compare_exchange_weak(&q->tail, &tail, tail + 1);
				return entry & (n - 1);
			}
		}
	}
}

static inline size_t lfring_enqueue(struct lfring * ring,
		size_t order, size_t eidx)
{
	return __lfring_enqueue(ring, order, eidx, NULL, NULL);
}

static inline size_t lfring_dequeue(struct lfring * ring, size_t order,
		bool nonempty)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t n = lfring_pow2(order);
	lfatomic_t head, entry;

start_over:
	head = atomic_load(&q->head);

	do {
		lfatomic_t ecycle, hcycle = head & ~(n - 1);
		size_t hidx = __lfring_map(head, order, n);
		entry = atomic_load(&q->array[hidx]);
		ecycle = entry & ~(n - 1);
		if (ecycle != hcycle) {
			/* Wrapping around. */
			if (!nonempty && (lfatomic_t) (ecycle + n) == hcycle) {
				return LFRING_EMPTY;
			}
			goto start_over;
		}
	} while (!atomic_compare_exchange_weak(&q->head, &head, head + 1));

	return (size_t) (entry & (n - 1));
}

static inline size_t lfring_dequeue_single(struct lfring * ring, size_t order,
		bool nonempty)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t n = lfring_pow2(order);
	lfatomic_t head, entry, hcycle;
	size_t hidx;

	head = nonempty ? atomic_fetch_add(&q->head, 1) :
			atomic_load(&q->head);

	hcycle = head & ~(n - 1);
	hidx = __lfring_map(head, order, n);
	entry = atomic_load(&q->array[hidx]);

	if (!nonempty) {
		lfatomic_t ecycle = entry & ~(n - 1);
		if (ecycle != hcycle)
			return LFRING_EMPTY;
		atomic_fetch_add(&q->head, 1);
	}

	return (size_t) (entry & (n - 1));
}

static inline size_t lfring_dequeue_mark(struct lfring * ring, size_t order)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t hidx, n = lfring_pow2(order);
	lfatomic_t head, entry;
	lfatomic_t ecycle, hcycle;

start_over:
	head = atomic_load(&q->head);

	while (1) {
		hcycle = head & ~(n - 1);
		hidx = __lfring_map(head, order, n);
		entry = atomic_load(&q->array[hidx]);
repeat_entry:
		ecycle = entry & ~(n - 1);
		if (ecycle != hcycle) {
			/* Wrapping around. */
			if ((lfatomic_t) (ecycle + n) != hcycle)
					goto start_over;
			/* Was already reset by a concurrent thread. */
			if ((entry & (n - 1)) == 0) {
				/* Advance the head pointer. */
				if (atomic_compare_exchange_strong(&q->head, &head,
						head + 1)) {
					head++;
				}
				continue;
			}
			/* Reset the entry. */
			if (!atomic_compare_exchange_strong(&q->array[hidx], &entry,
					ecycle))
				goto repeat_entry;
			/* Try to advance the head pointer. */
			atomic_compare_exchange_weak(&q->head, &head, head + 1);
			return 0;
		}
		if (atomic_compare_exchange_weak(&q->head, &head, head + 1))
			return (size_t) (entry & (n - 1));
	}
}

static inline size_t lfring_dequeue_mark_single(struct lfring * ring,
	size_t order)
{
	struct __lfring * q = (struct __lfring *) ring;
	size_t n = lfring_pow2(order);
	lfatomic_t entry, head = atomic_fetch_add(&q->head, 1);
	lfatomic_t ecycle, hcycle = head & ~(n - 1);
	size_t hidx = __lfring_map(head, order, n);

	/* Use the index 0 to mark entries. */
	entry = atomic_load(&q->array[hidx]);
	do {
		ecycle = entry & ~(n - 1);
		if (ecycle == hcycle)
			return (size_t) (entry & (n - 1));
	} while (!atomic_compare_exchange_weak(&q->array[hidx], &entry, ecycle));

	return 0;
}

#endif	/* !__LFRING_H */

/* vi: set tabstop=4: */
