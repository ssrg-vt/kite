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
struct virtif_sc;
int rump_virtif_clone(char *);
void rump_virtif_pktdeliver(struct virtif_sc *, struct iovec *, size_t,
                            unsigned char);
int virtif_entry(struct if_clone *ifc, int num);
void rump_virtif_soft_start(struct ifnet *);
