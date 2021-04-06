struct iovec {
    void *iov_base;
    unsigned long iov_len;
};

#include <mini-os/os.h>
#include <mini-os/xenbus.h>
#include <mini-os/events.h>
#include <xen/io/netif.h>
#include <xen/features.h>
#include <mini-os/gnttab.h>
#include <mini-os/time.h>
#include <mini-os/lib.h>
#include <mini-os/semaphore.h>
#include <mini-os/wait.h>

#include <bmk-core/errno.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/sched.h>
#include <bmk-core/string.h>
#include <bmk-core/pgalloc.h>
#include <bmk-core/platform.h>

#include <bmk-rumpuser/core_types.h>
#include <bmk-rumpuser/rumpuser.h>

#include <xen/io/netif.h>

#include "if_virt.h"
#include "if_virt_user.h"

//#define XENNET_DBG
#ifndef XENNET_DBG
#define XENPRINTF(x)
#else
#define XENPRINTF(x) bmk_printf x
#endif

//DECLARE_WAIT_QUEUE_HEAD(xennetback_queue);

#define memset(a, b, c) bmk_memset(a, b, c)

#define NET_TX_RING_SIZE __CONST_RING_SIZE(netif_tx, PAGE_SIZE)
#define NET_RX_RING_SIZE __CONST_RING_SIZE(netif_rx, PAGE_SIZE)

#define TX_BATCH 32
#define GRANT_INVALID_REF 0
#define NB_XMIT_PAGES_BATCH 64

#define get_gfn(x) (PFN_DOWN((uint64_t)(x)))
#define min(a, b) (a > b ? b : a);

/* The order of these values is important */
#define THREADBLK_STATUS_SLEEP 0x0
#define THREADBLK_STATUS_AWAKE 0x1
#define THREADBLK_STATUS_NOTIFY 0x2
struct threadblk {
    struct bmk_block_data header;
    unsigned long status;
    char _pad[48]; /* FIXME: better way to avoid false sharing */
};

struct virtif_user {
    struct threadblk viu_rcvrblk;
    struct threadblk viu_softsblk;
    struct bmk_thread *viu_rcvrthr;
    struct bmk_thread *viu_softsthr;
    void *viu_ifp;
    struct xnetback_instance *xneti;
    void **xni_xstate;
    void* xbusd_dmat;
    struct virtif_sc *viu_vifsc;

    int viu_read;
    int viu_write;
    int viu_dying;
};

/* state of a xnetback instance */
typedef enum {
	CONNECTED,
	DISCONNECTING,
	DISCONNECTED
} xnetback_state_t;

typedef unsigned long vaddr_t;
typedef unsigned long paddr_t;

/* we keep the xnetback instances in a linked list */
struct xnetback_instance {
	SLIST_ENTRY(xnetback_instance) next;
	struct xenbus_device *xni_xbusd; /* our xenstore entry */
	domid_t xni_domid;		/* attached to this domain */
	uint32_t xni_handle;	/* domain-specific handle */
	xnetback_state_t xni_status;

	/* network interface stuff */
	//struct ethercom xni_ec;
	//struct callout xni_restart;
	uint8_t xni_enaddr[6];

	/* remote domain communication stuff */
	unsigned int xni_evtchn; /* our event channel */
	//struct intrhand *xni_ih;
	netif_tx_back_ring_t xni_txring;
	netif_rx_back_ring_t xni_rxring;
	grant_handle_t xni_tx_ring_handle; /* to unmap the ring */
	grant_handle_t xni_rx_ring_handle;
	vaddr_t xni_tx_ring_va; /* to unmap the ring */
	vaddr_t xni_rx_ring_va;

	/* arrays used in xennetback_ifstart(), used for both Rx and Tx */
	gnttab_copy_t     	xni_gop_copy[NB_XMIT_PAGES_BATCH];
	//struct xnetback_xstate	xni_xstate[NB_XMIT_PAGES_BATCH];

	/* event counters */
	//struct evcnt xni_cnt_rx_cksum_blank;
	//struct evcnt xni_cnt_rx_cksum_undefer;
};
static SLIST_HEAD(, xnetback_instance) xnetback_instances;

int VIFHYPER_XN_RING_FULL(int cnt, void *arg, int queued)
{
	struct xnetback_instance *xneti = arg;
	RING_IDX req_prod, rsp_prod_pvt;

	req_prod = xneti->xni_rxring.sring->req_prod;
	rsp_prod_pvt = xneti->xni_rxring.rsp_prod_pvt;
	rmb();

	return	req_prod == xneti->xni_rxring.req_cons + (cnt) ||  \
		xneti->xni_rxring.req_cons - (rsp_prod_pvt + cnt) ==  \
		NET_RX_RING_SIZE;

}

static int
xennetback_copy(gnttab_copy_t *gop, int copycnt,
    const char *dir)
{
	/*
	 * Copy the data and ack it. Delaying it until the mbuf is
	 * freed will stall transmit.
	 */
	if (HYPERVISOR_grant_table_op(GNTTABOP_copy, gop, copycnt) != 0) {
		bmk_printf("GNTTABOP_copy %s failed", dir);
		return BMK_EINVAL;
	}

	for (int i = 0; i < copycnt; i++) {
		if (gop->status != GNTST_okay) {
			bmk_printf("GNTTABOP_copy[%d] %s %d\n",
			    i, dir, gop->status);
			return BMK_EINVAL;
		}
	}

	return 0;
}

void
VIFHYPER_rx_copy_process(struct ifnet *ifp, struct xnetback_instance *xneti,
	int queued, int copycnt)
{
	int notify;

	if (xennetback_copy(xneti->xni_gop_copy, copycnt, "Rx") != 0) {
		/* message already displayed */
		return;
	}

	/* update pointer */
	rmb();
	xneti->xni_rxring.req_cons += queued;
	xneti->xni_rxring.rsp_prod_pvt += queued;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&xneti->xni_rxring, notify);

	/* send event */
	if (notify) {
		xen_rmb();
		XENPRINTF(("%s receive event\n",
		    xneti->xni_if.if_xname));
        	minios_notify_remote_via_evtchn(xneti->xni_evtchn);
	}

}

static void
VIFHYPER_rx_copy_queue(struct xnetback_instance *xneti,
    int *queued, int *copycntp, int flags, int pkthdr_len, struct iovec *dm, int *xst_count, int dm_nsegs)
{
	gnttab_copy_t *gop;
	struct netif_rx_request rxreq;
	netif_rx_response_t *rxresp;
	paddr_t ma;
	size_t goff, segoff, segsize, take, totsize;
	int copycnt = *copycntp, reqcnt = *queued;
	//const bus_dmamap_t dm = xst0->xs_dmamap;
	const uint8_t multiseg = dm_nsegs > 1 ? 1 : 0;

	int rsp_prod_pvt = xneti->xni_rxring.rsp_prod_pvt;

	//bmk_assert(xst0 == &xneti->xni_xstate[reqcnt]);

	RING_COPY_REQUEST(&xneti->xni_rxring,
	    xneti->xni_rxring.req_cons + reqcnt, &rxreq);
	goff = 0;
	rxresp = RING_GET_RESPONSE(&xneti->xni_rxring, rsp_prod_pvt + reqcnt);
	reqcnt++;

	rxresp->id = rxreq.id;
	rxresp->offset = 0;

	if (flags != 0) {
		rxresp->flags = NETRXF_csum_blank;
	} else {
		rxresp->flags = NETRXF_data_validated;
	}

	if (multiseg)
		rxresp->flags |= NETRXF_more_data;

	totsize = pkthdr_len;

	/*
	 * Arrange for the mbuf contents to be copied into one or more
	 * provided memory pages.
	 */
	for (int seg = 0; seg < dm_nsegs; seg++) {
		ma = (paddr_t)dm[seg].iov_base; //dm->dm_segs[seg].ds_addr;
		segsize = dm[seg].iov_len; //dm->dm_segs[seg].ds_len;
		segoff = 0;

		while (segoff < segsize) {
			take = min(PAGE_SIZE - goff, segsize - segoff);
			bmk_assert(take <= totsize);

			/* add copy request */
			gop = &xneti->xni_gop_copy[copycnt++];
			gop->flags = GNTCOPY_dest_gref;
			gop->source.offset = (ma & PAGE_MASK) + segoff;
			gop->source.domid = DOMID_SELF;
			gop->source.u.gmfn = ma >> PAGE_SHIFT;

			gop->dest.u.ref = rxreq.gref;
			gop->dest.offset = goff;
			gop->dest.domid = xneti->xni_domid;

			gop->len = take;

			segoff += take;
			goff += take;
			totsize -= take;

			if (goff == PAGE_SIZE && totsize > 0) {
				rxresp->status = goff;

				/* Take next grant */
				RING_COPY_REQUEST(&xneti->xni_rxring,
				    xneti->xni_rxring.req_cons + reqcnt,
				    &rxreq);
				goff = 0;
				rxresp = RING_GET_RESPONSE(&xneti->xni_rxring,
				    rsp_prod_pvt + reqcnt);
				reqcnt++;

				rxresp->id = rxreq.id;
				rxresp->offset = 0;
				rxresp->flags = NETRXF_more_data;

				(*xst_count)++;
				//xst->xs_m = NULL;
			}
		}
	}
	rxresp->flags &= ~NETRXF_more_data;
	rxresp->status = goff;
	bmk_assert(totsize == 0);

	bmk_assert(copycnt > *copycntp);
	bmk_assert(reqcnt > *queued);
	*copycntp = copycnt;
	*queued = reqcnt;
}
		    
void VIFHYPER_RING_CONSUMPTION(void *arg, unsigned int *unconsumed)
{
	netif_rx_back_ring_t xni_rxring = (netif_rx_back_ring_t)arg;
	*unconsumed = RING_HAS_UNCONSUMED_REQUESTS(&xni_rxring);
}
