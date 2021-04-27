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
#define VIFHYPER_ENTRY VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _entry)
#define VIFHYPER_XN_RING_FULL VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _ring_ful)
#define VIFHYPER_RX_COPY_PROCESS VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _rx_copy_process)
#define VIFHYPER_RX_COPY_QUEUE VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _copy_queue)
#define VIFHYPER_RING_CONSUMPTION VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _ring_consumption)
#define VIFHYPER_TX_RESPONSE VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _tx_response)
#define VIFHYPER_TX_M0LEN_FRAGMENT VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _len_fragment)
#define VIFHYPER_TX_COPY_PREPARE VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _tx_copy_prepare)
#define VIFHYPER_COPY VIF_BASENAME3(rumpcomp_, VIRTIF_BASE, _copy)

struct rump_iovec {
    void *iov_base;
    unsigned long iov_len;
    int iov_offset;
    int csum_blank;
};

struct tx_req_info{
	uint8_t tri_id;
	size_t tri_size;
	int tri_req_cons;
	uint8_t tri_more_data;
	uint8_t tri_csum_blank;
	uint8_t tri_data_validated;
};

struct if_clone;
struct ifnet;

struct xennetback_sc;
void xennetback_entry(void);
//void rump_xennetback_soft_start(struct ifnet *);
struct xennetback_sc* rump_xennetback_create(void* viu, char* ifp_name, char* enaddr);
//int rump_xennetback_network_tx(struct xennetback_sc *sc, int mlen, int more_data, int queued); 
int rump_xennetback_network_tx(struct xennetback_sc *sc, struct tx_req_info *tri, int tx_queued);
struct rump_iovec* rump_xennetback_load_mbuf(struct xennetback_sc *sc, int index, struct rump_iovec *iov, size_t gsize, int *seg);
void rump_xennetback_pktenqueue(struct xennetback_sc *sc, int index, int flag);
void rump_xennetback_ifinit(struct xennetback_sc *sc);
void rump_xennetback_destroy(struct xennetback_sc *sc);
void rump_xennetback_ifsoftstart_copy(struct xennetback_sc *sc);
