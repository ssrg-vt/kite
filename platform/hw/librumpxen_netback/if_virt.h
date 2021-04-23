/*	$NetBSD: if_virt.h,v 1.2 2013/07/04 11:58:11 pooka Exp $	*/

/*
 * NOTE!  This file is supposed to work on !NetBSD platforms.
 */

#ifndef VIRTIF_BASE
#error Define VIRTIF_BASE
#endif

#define LB_SH 64

#define VIF_STRING(x) #x
#define VIF_STRINGIFY(x) VIF_STRING(x)
#define VIF_CONCAT(x, y) x##y
#define VIF_CONCAT3(x, y, z) x##y##z
#define VIF_BASENAME(x, y) VIF_CONCAT(x, y)
#define VIF_BASENAME3(x, y, z) VIF_CONCAT3(x, y, z)

#define VIF_CLONER VIF_BASENAME(VIRTIF_BASE, _cloner)
#define VIF_NAME VIF_STRINGIFY(VIRTIF_BASE)

#define VIFHYPER_SET_WATCH VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _set_watch)
#define VIFHYPER_SET_START VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _set_start)
#define VIFHYPER_BLOCK VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _block)
#define VIFHYPER_WAKE VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _wake)
#define VIFHYPER_CREATE VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _create)
#define VIFHYPER_DYING VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _dying)
#define VIFHYPER_DESTROY VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _destroy)
#define VIFHYPER_SEND VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _send)
#define VIFHYPER_RING_STATUS VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _ring_status)

struct rump_iovec {
    void *iov_base;
    unsigned long iov_len;
    int iov_offset;
    int csum_blank;
};

struct if_clone;
struct ifnet;

struct xennetback_sc;
void xennetback_entry(void);
//void rump_xennetback_soft_start(struct ifnet *);
struct xennetback_sc* rump_xennetback_create(void* viu, char* ifp_name, char* enaddr);
int rump_evthandler(struct xennetback_sc *sc, int mlen, int more_data, int queued); 
void rump_tx_copy_abort(struct xennetback_sc *sc, int queued);
struct rump_iovec* rump_load_mbuf(struct xennetback_sc *sc, int queued, struct rump_iovec *iov, size_t gsize);
void rump_pktenqueue(struct xennetback_sc *sc, int index, int flag);
void rump_xennetback_ifinit(struct xennetback_sc *sc);
void rump_xennetback_destroy(struct xennetback_sc *sc);
