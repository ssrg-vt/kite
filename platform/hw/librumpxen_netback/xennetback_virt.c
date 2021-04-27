/*      $NetBSD: xennetback_xenbus.c,v 1.105 2020/05/05 17:02:01 bouyer Exp $      */

/*
 * Copyright (c) 2006 Manuel Bouyer.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: xennetback_xenbus.c,v 1.105 2020/05/05 17:02:01 bouyer Exp $");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/queue.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/bus.h>
#include <sys/cprng.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <net/netisr.h>
#include <net/bpf.h>
//#include <net/if_stats.h>
#include <net/if_ether.h>

#include <rump/rump.h>

#include "xennet_checksum.h"

#include "rump_net_private.h"
#include "rump_private.h"

#include <bmk-core/sched.h>
#include <bmk-rumpuser/rumpuser.h>

#include "if_virt.h"
#include "if_virt_user.h"

//#define XENDEBUG_NET
#ifdef XENDEBUG_NET
#define XENPRINTF(x) aprint_normal x
#else
#define XENPRINTF(x)
#endif

#define XN_M_CSUM_SUPPORTED		\
	(M_CSUM_TCPv4 | M_CSUM_UDPv4 | M_CSUM_TCPv6 | M_CSUM_UDPv6)

#define NB_XMIT_PAGES_BATCH 64

//TODO: remove hardcoded values
#define NET_TX_RING_SIZE 256
#define XEN_NETIF_NR_SLOTS_MIN 18

#define NETIF_RSP_DROPPED         -2
#define NETIF_RSP_ERROR           -1
#define NETIF_RSP_OKAY             0
/* No response: used for auxiliary requests (e.g., netif_extra_info_t). */
#define NETIF_RSP_NULL             1

struct netif_tx_request;

struct xnetback_xstate {
	bus_dmamap_t xs_dmamap;
	bool xs_loaded;
	struct mbuf *xs_m;
	//struct netif_tx_request xs_tx;
	uint16_t xs_tx_size;		/* Size of data in this Tx fragment */
};

struct xennetback_sc {
    struct ethercom sc_ec;
    struct xennetback_user *sc_viu;
    struct xnetback_xstate  sc_xstate[NB_XMIT_PAGES_BATCH];
    bus_dma_tag_t sc_dmat;
    /* event counters */
    struct evcnt sc_cnt_rx_cksum_blank;
    struct evcnt sc_cnt_rx_cksum_undefer;
};

static int xennetback_ifioctl(struct ifnet *ifp, u_long cmd, void *data);
static void xennetback_ifstart(struct ifnet *ifp);
static void xennetback_ifwatchdog(struct ifnet * ifp);
static int xennetback_ifinit(struct ifnet *ifp);
static void xennetback_ifstop(struct ifnet *ifp, int disable);

void xennetback_ifsoftstart_copy(struct xennetback_sc *sc);
static void xennetback_free_mbufs(struct xennetback_sc *sc, int queued);
static void xennetback_tx_copy_process(struct xennetback_sc *sc,
		struct tx_req_info *tri, int start, int queued);

struct bmk_thread *softstart_thread;

/* This function should be the entry point for network backend driver */
void xennetback_entry(void)
{
	VIFHYPER_ENTRY();
}

struct xennetback_sc* rump_xennetback_create(void* viu, char* ifp_name, char* enaddr)
{
	struct xennetback_sc *sc = NULL;
	struct ifnet *ifp;
	extern int ifqmaxlen; /* XXX */
	int i;

	sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
	if (sc == NULL)
		return NULL;

	sc->sc_viu = viu;

	ifp = &sc->sc_ec.ec_if;
	ifp->if_softc = sc;
        snprintf(ifp->if_xname, IFNAMSIZ, "%s", ifp_name);

	/* Initialize DMA map, used only for loading PA */
	for (i = 0; i < __arraycount(sc->sc_xstate); i++) {
		if (bus_dmamap_create(sc->sc_dmat,
		    ETHER_MAX_LEN_JUMBO, XEN_NETIF_NR_SLOTS_MIN,
		    PAGE_SIZE, PAGE_SIZE, BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW,
		    &sc->sc_xstate[i].xs_dmamap)
		    != 0) {
			aprint_error_ifnet(ifp,
			    "failed to allocate dma map\n");
			return NULL;
		}
	}

	evcnt_attach_dynamic(&sc->sc_cnt_rx_cksum_blank, EVCNT_TYPE_MISC,
	    NULL, ifp->if_xname, "Rx csum blank");
	evcnt_attach_dynamic(&sc->sc_cnt_rx_cksum_undefer, EVCNT_TYPE_MISC,
	    NULL, ifp->if_xname, "Rx csum undeferred");

	/* create pseudo-interface */
	aprint_verbose_ifnet(ifp, "Ethernet address %s\n",
	    ether_sprintf(enaddr));
	sc->sc_ec.ec_capabilities |= ETHERCAP_VLAN_MTU | ETHERCAP_JUMBO_MTU;
	sc->sc_ec.ec_capenable = sc->sc_ec.ec_capabilities;
	

	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_snd.ifq_maxlen =
	    uimax(ifqmaxlen, NET_TX_RING_SIZE * 2);
	ifp->if_capabilities =
		IFCAP_CSUM_UDPv4_Rx | IFCAP_CSUM_UDPv4_Tx
		| IFCAP_CSUM_TCPv4_Rx | IFCAP_CSUM_TCPv4_Tx
		| IFCAP_CSUM_UDPv6_Rx | IFCAP_CSUM_UDPv6_Tx
		| IFCAP_CSUM_TCPv6_Rx | IFCAP_CSUM_TCPv6_Tx;

	ifp->if_ioctl = xennetback_ifioctl;
	ifp->if_start = xennetback_ifstart;
	ifp->if_watchdog = xennetback_ifwatchdog;
	ifp->if_init = xennetback_ifinit;
	ifp->if_stop = xennetback_ifstop;
	ifp->if_timer = 0;
	IFQ_SET_MAXLEN(&ifp->if_snd, uimax(2 * NET_TX_RING_SIZE, IFQ_MAXLEN));
	IFQ_SET_READY(&ifp->if_snd);

	sc->sc_viu = viu;

	if_attach(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, enaddr);

	return sc;
}

static int
xennetback_ifioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	//struct xnetback_instance *xneti = ifp->if_softc;
	//struct ifreq *ifr = (struct ifreq *)data;
	int s, error;

	s = splnet();
	error = ether_ioctl(ifp, cmd, data);
	if (error == ENETRESET)
		error = 0;
	splx(s);
	return error;
}

static void
xennetback_ifstart(struct ifnet *ifp)
{
	struct xennetback_sc *sc = ifp->if_softc;

	XENPRINTF(("%s\n", __func__));
	/*
	 * The Xen communication channel is much more efficient if we can
	 * schedule batch of packets for the domain. Deferred start by network
	 * stack will enqueue all pending mbufs in the interface's send queue
	 * before it is processed by the soft interrupt handler.
	 */
	VIFHYPER_WAKE(sc->sc_viu);
}

static void
xennetback_ifwatchdog(struct ifnet * ifp)
{
	XENPRINTF(("%s\n", __func__));
	/*
	 * We can get to the following condition: transmit stalls because the
	 * ring is full when the ifq is full too.
	 *
	 * In this case (as, unfortunately, we don't get an interrupt from xen
	 * on transmit) nothing will ever call xennetback_ifstart() again.
	 * Here we abuse the watchdog to get out of this condition.
	 */
	XENPRINTF(("xennetback_ifwatchdog\n"));
	xennetback_ifstart(ifp);
}

static int
xennetback_ifinit(struct ifnet *ifp)
{
	XENPRINTF(("%s\n", __func__));

	int s = splnet();

	if ((ifp->if_flags & IFF_UP) == 0) {
		splx(s);
		return 0;
	}
	splx(s);
	return 0;
}

void
rump_xennetback_ifinit(struct xennetback_sc *sc)
{
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	
	XENPRINTF(("%s\n", __func__));
	
	int s = splnet();

	ifp->if_flags |= IFF_RUNNING;
	splx(s);
}


static void
xennetback_ifstop(struct ifnet *ifp, int disable)
{
	//struct xnetback_instance *xneti = ifp->if_softc;
	XENPRINTF(("%s\n", __func__));
	
	int s = splnet();

	ifp->if_flags &= ~IFF_RUNNING;
	ifp->if_timer = 0;

	//TODO: activate the following code
#if 0
	if (xneti->xni_status == CONNECTED) {
		XENPRINTF(("%s: req_prod 0x%x resp_prod 0x%x req_cons 0x%x "
		    "event 0x%x\n", ifp->if_xname, xneti->xni_txring->req_prod,
		    xneti->xni_txring->resp_prod, xneti->xni_txring->req_cons,
		    xneti->xni_txring->event));
		xennetback_evthandler(ifp->if_softc); /* flush pending RX requests */
	}
#endif
	splx(s);
}

void
rump_xennetback_ifsoftstart_copy(struct xennetback_sc *sc)
{
    	struct ifnet *ifp = &sc->sc_ec.ec_if;
	//struct ifnet *ifp = &xneti->xni_if;
	//struct xennetback_user *viu;
	struct mbuf *m;
	int queued = 0;
	struct xnetback_xstate *xst;
	int copycnt = 0;
	bool abort;
	int rxresp_flags;
	struct iovec dm[NB_XMIT_PAGES_BATCH];
	int xst_count, unconsumed;

	XENPRINTF(("%s\n", __func__));
	int s = splnet();
	if (__predict_false((ifp->if_flags & IFF_RUNNING) == 0)) {
		splx(s);
		return;
	}

	while (!IFQ_IS_EMPTY(&ifp->if_snd)) {
		XENPRINTF(("pkt\n"));

		abort = false;
		KASSERT(queued == 0);
		KASSERT(copycnt == 0);
		while (copycnt < NB_XMIT_PAGES_BATCH) {
			if (__predict_false(VIFHYPER_XN_RING_FULL(1, sc->sc_viu, queued))) {
				/* out of ring space */
				abort = true;
				break;
			}

			IFQ_DEQUEUE(&ifp->if_snd, m);
			if (m == NULL)
				break;

again:
			//xst = &xneti->xni_xstate[queued];
			xst = &sc->sc_xstate[queued];

			/*
			 * For short packets it's always way faster passing
			 * single defragmented packet, even with feature-sg.
			 * Try to defragment first if the result is likely
			 * to fit into a single mbuf.
			 */
			if (m->m_pkthdr.len < MCLBYTES && m->m_next)
				(void)m_defrag(m, M_DONTWAIT);

			if (bus_dmamap_load_mbuf(
			    sc->sc_dmat,
			    xst->xs_dmamap, m, BUS_DMA_NOWAIT) != 0) {
				if (m_defrag(m, M_DONTWAIT) == NULL) {
					m_freem(m);
					//static struct timeval lasttime;
					/*if (ratecheck(&lasttime, &xni_pool_errintvl))
						printf("%s: fail defrag mbuf\n",
						    ifp->if_xname);
						    */
					continue;
				}

				if (__predict_false(bus_dmamap_load_mbuf(
				    sc->sc_dmat,
				    xst->xs_dmamap, m, BUS_DMA_NOWAIT) != 0)) {
					aprint_normal("%s: cannot load mbuf\n",
					    ifp->if_xname);
					m_freem(m);
					continue;
				}
			}
			KASSERT(xst->xs_dmamap->dm_nsegs < NB_XMIT_PAGES_BATCH);
			KASSERTMSG(queued <= copycnt, "queued %d > copycnt %d",
			    queued, copycnt);

			if (__predict_false(VIFHYPER_XN_RING_FULL(
			    xst->xs_dmamap->dm_nsegs, sc->sc_viu, queued))) {
				/* Ring too full to fit the packet */
				bus_dmamap_unload(sc->sc_dmat,
				    xst->xs_dmamap);
				m_freem(m);
				abort = true;
				break;
			}
			if (__predict_false(copycnt + xst->xs_dmamap->dm_nsegs >
			    NB_XMIT_PAGES_BATCH)) {
				/* Batch already too full, flush and retry */
				bus_dmamap_unload(sc->sc_dmat,
				    xst->xs_dmamap);
				VIFHYPER_RX_COPY_PROCESS(sc->sc_viu, queued,
				    copycnt);
				xennetback_free_mbufs(sc, queued);
				queued = copycnt = 0;
				goto again;
			}

			/* Now committed to send */
			xst->xs_loaded = true;
			xst->xs_m = m;

			KASSERT(NB_XMIT_PAGES_BATCH >= xst->xs_dmamap->dm_nsegs);
			/* start filling iov */
			for(int seg=0; seg < xst->xs_dmamap->dm_nsegs; seg++) {
            			dm[seg].iov_base = (void *)xst->xs_dmamap->dm_segs[seg].ds_addr;;
            			dm[seg].iov_len = xst->xs_dmamap->dm_segs[seg].ds_len;
			}
		
			rxresp_flags = xst->xs_m->m_pkthdr.csum_flags & XN_M_CSUM_SUPPORTED;
			VIFHYPER_RX_COPY_QUEUE(sc->sc_viu, &queued, &copycnt,
			    rxresp_flags,
			    xst->xs_m->m_pkthdr.len, dm,
			    &xst_count, xst->xs_dmamap->dm_nsegs);

			for(int xstc=0; xstc < xst_count; xstc++)
				(xst+xstc)->xs_m = NULL;

			//if_statinc(ifp, if_opackets);
			bpf_mtap(ifp, m, BPF_D_OUT);
		}
		KASSERT(copycnt <= NB_XMIT_PAGES_BATCH);
		KASSERT(queued <= copycnt);
		if (copycnt > 0) {
			VIFHYPER_RX_COPY_PROCESS(sc->sc_viu, queued, copycnt);
			//TODO: double check the following line
			xennetback_free_mbufs(sc, queued);
			queued = copycnt = 0;
		}
		/*
		 * note that we don't use RING_FINAL_CHECK_FOR_REQUESTS()
		 * here, as the frontend doesn't notify when adding
		 * requests anyway
		 */
		unconsumed = VIFHYPER_RING_CONSUMPTION(sc->sc_viu);
		if (__predict_false(abort || unconsumed)) {
			/* ring full */
			ifp->if_timer = 1;
			break;
		}
	}
	splx(s);
}

static void xennetback_free_mbufs(struct xennetback_sc *sc, int queued)
{
	struct xnetback_xstate *xst;

	XENPRINTF(("%s\n", __func__));
	/* now that data was copied we can free the mbufs */
	for (int j = 0; j < queued; j++) {
		xst = &sc->sc_xstate[j];
		if (xst->xs_loaded) {
			bus_dmamap_unload(sc->sc_dmat,
			    xst->xs_dmamap);
			xst->xs_loaded = false;
		}
		if (xst->xs_m != NULL) {
			m_freem(xst->xs_m);
			xst->xs_m = NULL;
		}
	}
}

static void
xennetback_tx_copy_abort(struct xennetback_sc *sc, struct tx_req_info *tri, int queued)
{
	struct xnetback_xstate *xst;

	XENPRINTF(("%s\n", __func__));
	for (int i = 0; i < queued; i++) {
		xst = &sc->sc_xstate[queued];

		if (xst->xs_loaded) {
			KASSERT(xst->xs_m != NULL);
			bus_dmamap_unload(sc->sc_dmat,
			    xst->xs_dmamap);
			xst->xs_loaded = false;
			m_freem(xst->xs_m);

			VIFHYPER_TX_RESPONSE(sc->sc_viu, tri[i].tri_id,
			    NETIF_RSP_ERROR);
		}
	}
}

int rump_xennetback_network_tx(struct xennetback_sc *sc, struct tx_req_info *tri,
		int tx_queued)

{
    	struct ifnet *ifp = &sc->sc_ec.ec_if;
	struct mbuf *m, *m0 = NULL, *mlast = NULL;
	int queued = 0, m0_len = 0;
	struct xnetback_xstate *xst;
	const bool discard = ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) !=
	    (IFF_UP | IFF_RUNNING));
	int start = 0;

	XENPRINTF(("%s\n", __func__));
	for(int i=0; i < tx_queued; i++){
		if (__predict_false(discard)) {
			/* interface not up, drop all requests */
			//if_statinc(ifp, if_iqdrops);
			VIFHYPER_TX_RESPONSE(sc->sc_viu, tri[i].tri_id,
			    NETIF_RSP_DROPPED);
			continue;
		}

		/* get a mbuf for this fragment */
		MGETHDR(m, M_DONTWAIT, MT_DATA);
		if (__predict_false(m == NULL)) {
			//static struct timeval lasttime;
mbuf_fail:
			//if (ratecheck(&lasttime, &xni_pool_errintvl))
			aprint_normal("%s: mbuf alloc failed\n",
				    ifp->if_xname);
			xennetback_tx_copy_abort(sc, tri, queued);
			start += queued;
			queued = 0; /*TODO: check*/
			m0 = NULL;
			VIFHYPER_TX_RESPONSE(sc->sc_viu, tri[i].tri_id,
			    NETIF_RSP_DROPPED);
			//if_statinc(ifp, if_ierrors);
			continue;
		}
		m->m_len = m->m_pkthdr.len = tri[i].tri_size;

		if (!m0 && tri[i].tri_more_data) {
			/*
			 * The first fragment of multi-fragment Tx request
			 * contains total size. Need to read whole
			 * chain to determine actual size of the first
			 * (i.e. current) fragment.
			 */
			int cnt;
			m0_len = VIFHYPER_TX_M0LEN_FRAGMENT(sc->sc_viu,
			    tri[i].tri_size, tri[i].tri_req_cons, &cnt);
			m->m_len = m0_len;
			KASSERT(cnt <= XEN_NETIF_NR_SLOTS_MIN);

			if (queued + cnt >= NB_XMIT_PAGES_BATCH) {
				/*
				 * Flush queue if too full to fit this
				 * new packet whole.
				 */
				KASSERT(m0 == NULL);
				xennetback_tx_copy_process(sc, tri,
						start, queued);
				start += queued;
				queued = 0;
			}
		}

		if (m->m_len > MHLEN) {
			MCLGET(m, M_DONTWAIT);
			if (__predict_false((m->m_flags & M_EXT) == 0)) {
				m_freem(m);
				goto mbuf_fail;
			}
			if (__predict_false(m->m_len > MCLBYTES)) {
				/* one more mbuf necessary */
				struct mbuf *mn;
				MGET(mn, M_DONTWAIT, MT_DATA);
				if (__predict_false(mn == NULL)) {
					m_freem(m);
					goto mbuf_fail;
				}
				if (m->m_len - MCLBYTES > MLEN) {
					MCLGET(mn, M_DONTWAIT);
					if ((mn->m_flags & M_EXT) == 0) {
						m_freem(mn);
						m_freem(m);
						goto mbuf_fail;
					}
				}
				mn->m_len = m->m_len - MCLBYTES;
				m->m_len = MCLBYTES;
				m->m_next = mn;
				KASSERT(mn->m_len <= MCLBYTES);
			}
			KASSERT(m->m_len <= MCLBYTES);
		}

		if (m0 || tri[i].tri_more_data) {
			if (m0 == NULL) {
				m0 = m;
				mlast = (m->m_next) ? m->m_next : m;
				KASSERT(mlast->m_next == NULL);
			} else {
				/* Coalesce like m_cat(), but without copy */
				KASSERT(mlast != NULL);
				if (M_TRAILINGSPACE(mlast) >= m->m_pkthdr.len) {
					mlast->m_len +=  m->m_pkthdr.len;
					m_freem(m);
				} else {
					mlast->m_next = m;
					mlast = (m->m_next) ? m->m_next : m;
					KASSERT(mlast->m_next == NULL);
				}
			}
		}

		xst = &sc->sc_xstate[queued];
		xst->xs_m = (m0 == NULL || m == m0) ? m : NULL;
		/* Fill the length of _this_ fragment */
		xst->xs_tx_size = (m == m0) ? m0_len : m->m_pkthdr.len;
		queued++;

		KASSERT(queued <= NB_XMIT_PAGES_BATCH);
		if (__predict_false(m0 &&
		    tri[i].tri_more_data == 0)) {
			/* Last fragment, stop appending mbufs */
			m0 = NULL;
		}
		if (queued == NB_XMIT_PAGES_BATCH) {
			KASSERT(m0 == NULL);
			xennetback_tx_copy_process(sc, tri,
					start, queued);
			start += queued;
			queued = 0;
		}
	}
	if (m0) {
		/* Queue empty, and still unfinished multi-fragment request */
		aprint_normal("%s: dropped unfinished multi-fragment\n",
		    ifp->if_xname);
		xennetback_tx_copy_abort(sc, tri, queued);
		start += queued;
		queued = 0;
		m0 = NULL;
	}
	if (queued > 0)
		xennetback_tx_copy_process(sc, tri,
				start, queued);
	
	/* check to see if we can transmit more packets */
	//if_schedule_deferred_start(ifp);

	return 1;
}

static void
xennetback_tx_copy_process(struct xennetback_sc *sc, struct tx_req_info *tri,
		int start, int queued)
{
	struct xnetback_xstate *xst;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	int copycnt = 0, seg = 0;
	size_t goff = 0, segoff = 0, gsize, take;
	bus_dmamap_t dm = NULL;
	paddr_t ma;

	aprint_normal("Start\n");
	if(queued > 2)
		aprint_normal("queued = %d\n", queued);
	for (int i = 0; i < queued; i++) {
		xst = &sc->sc_xstate[start + i];

		if (xst->xs_m != NULL) {
			KASSERT(xst->xs_m->m_pkthdr.len == tri[i].tri_size);
			if (__predict_false(bus_dmamap_load_mbuf(
			    sc->sc_dmat,
			    xst->xs_dmamap, xst->xs_m, BUS_DMA_NOWAIT) != 0))
				goto abort;
			xst->xs_loaded = true;
			dm = xst->xs_dmamap;
			seg = 0;
			goff = segoff = 0;
		}

		gsize = xst->xs_tx_size;
		goff = 0;
		for (; seg < dm->dm_nsegs && gsize > 0; seg++) {
			bus_dma_segment_t *ds = &dm->dm_segs[seg];
			ma = ds->ds_addr;
			take = uimin(gsize, ds->ds_len);

			KASSERT(copycnt <= NB_XMIT_PAGES_BATCH);
			if (copycnt == NB_XMIT_PAGES_BATCH) {
				if (VIFHYPER_COPY(sc->sc_viu,
				    copycnt, "Tx") != 0)
					goto abort;
				copycnt = 0;
			}

			/* Queue for the copy */
			VIFHYPER_TX_COPY_PREPARE(sc->sc_viu, copycnt++, take,
					start + i, goff, ma, segoff);
			goff += take;
			gsize -= take;
			if (take + segoff < ds->ds_len) {
				segoff += take;
				/* Segment not completely consumed yet */
				break;
			}
			segoff = 0;
		}
		KASSERT(gsize == 0);
		KASSERT(goff == xst->xs_tx_size);
	}
	if (copycnt > 0) {
		if (VIFHYPER_COPY(sc->sc_viu, copycnt, "Tx"))
			goto abort;
		copycnt = 0;
	}

	/* If we got here, the whole copy was successful */
	for (int i = 0; i < queued; i++) {
		xst = &sc->sc_xstate[start + i];

		VIFHYPER_TX_RESPONSE(sc->sc_viu, tri[i].tri_id, NETIF_RSP_OKAY);

		if (xst->xs_m != NULL) {
			KASSERT(xst->xs_loaded);
			bus_dmamap_unload(sc->sc_dmat, xst->xs_dmamap);

			if (tri[i].tri_csum_blank) {
				xennet_checksum_fill(&xst->xs_m);
				//xennet_checksum_fill(ifp, xst->xs_m,
				//    &xneti->xni_cnt_rx_cksum_blank,
				//    &xneti->xni_cnt_rx_cksum_undefer);
			} else if (tri[i].tri_data_validated) {
				xst->xs_m->m_pkthdr.csum_flags =
				    XN_M_CSUM_SUPPORTED;
			}
			m_set_rcvif(xst->xs_m, ifp);

			KERNEL_LOCK(1, NULL);
			if_percpuq_enqueue(ifp->if_percpuq, xst->xs_m);
			KERNEL_UNLOCK_LAST(NULL);
		}
	}

	aprint_normal("End\n");
	return;

abort:
	xennetback_tx_copy_abort(sc, tri, queued);
}

void rump_xennetback_destroy(struct xennetback_sc *sc)
{
	struct ifnet *ifp = &sc->sc_ec.ec_if;

	ether_ifdetach(ifp);
	if_detach(ifp);

	evcnt_detach(&sc->sc_cnt_rx_cksum_blank);
	evcnt_detach(&sc->sc_cnt_rx_cksum_undefer);

	/* Destroy DMA maps */
	for (int i = 0; i < __arraycount(sc->sc_xstate); i++) {
		if (sc->sc_xstate[i].xs_dmamap != NULL) {
			bus_dmamap_destroy(sc->sc_dmat,
			    sc->sc_xstate[i].xs_dmamap);
			sc->sc_xstate[i].xs_dmamap = NULL;
		}
	}
}	
