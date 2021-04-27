/*	$NetBSD: rumpcomp_user.h,v 1.4 2013/07/04 11:46:51 pooka Exp $	*/

/*
 * Copyright (c) 2013 Antti Kantee.  All Rights Reserved.
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

struct xennetback_user;
struct rump_iovec;

void VIFHYPER_ENTRY(void);
int VIFHYPER_SET_WATCH(void);
int VIFHYPER_SET_START(struct xennetback_user *, void *);
int VIFHYPER_WAKE(struct xennetback_user *);
int VIFHYPER_CREATE(char *, struct xennetback_sc *, uint8_t *,
                    struct xennetback_user **, int8_t *);
void VIFHYPER_DYING(struct xennetback_user *);
void VIFHYPER_DESTROY(struct xennetback_user *);

void VIFHYPER_SEND(struct xennetback_user *, struct rump_iovec *, size_t);
void VIFHYPER_RING_STATUS(struct xennetback_user *, int *);

int VIFHYPER_XN_RING_FULL(int, struct xennetback_user *, int);
void VIFHYPER_RX_COPY_PROCESS(struct xennetback_user *, int, int);
void VIFHYPER_RX_COPY_QUEUE(struct xennetback_user *, int *, int *, int, int, struct iovec *, int *, int);
int VIFHYPER_RING_CONSUMPTION(struct xennetback_user *);
void VIFHYPER_TX_COPY_ABORT(struct xennetback_user *viu, int start, int queued);
int VIFHYPER_TX_M0LEN_FRAGMENT(struct xennetback_user *viu, int m0_len, int req_cons, int *cntp);
void VIFHYPER_TX_RESPONSE(struct xennetback_user *viu, int id, int status);
void VIFHYPER_TX_COPY_PREPARE(struct xennetback_user *viu, int copycnt, int take,
	       int index, int goff, uint64_t ma, int segoff);
int VIFHYPER_COPY(struct xennetback_user *viu, int copycnt, const char *dir);

