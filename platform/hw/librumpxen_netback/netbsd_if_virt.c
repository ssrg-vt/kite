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

#include <net/if.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <net/netisr.h>
#include <net/bpf.h>
#include <net/if_stats.h>
#include <net/if_ether.h>

#include <rump/rump.h>

#include "xennet_checksum.h"

#include "rump_net_private.h"
#include "rump_private.h"

#include <bmk-core/sched.h>
#include <bmk-rumpuser/rumpuser.h>

#include "if_virt.h"
#include "if_virt_user.h"

#ifdef XENDEBUG_NET
#define XENPRINTF(x) printf x
#else
#define XENPRINTF(x)
#endif

#define XN_M_CSUM_SUPPORTED		\
	(M_CSUM_TCPv4 | M_CSUM_UDPv4 | M_CSUM_TCPv6 | M_CSUM_UDPv6)

#define NB_XMIT_PAGES_BATCH 64

struct netif_tx_request;

struct xnetback_xstate {
	bus_dmamap_t xs_dmamap;
	bool xs_loaded;
	struct mbuf *xs_m;
	//struct netif_tx_request xs_tx;
	uint16_t xs_tx_size;		/* Size of data in this Tx fragment */
};

struct virtif_sc {
    struct ethercom sc_ec;
    struct virtif_user *sc_viu;
    struct xnetback_xstate  sc_xstate[NB_XMIT_PAGES_BATCH];
    bus_dma_tag_t sc_dmat;
};

static void xennetback_free_mbufs(struct virtif_sc *sc, int queued);
static void xennetback_ifsoftstart_copy(struct virtif_sc *sc);

static void
xennetback_ifsoftstart_copy(struct virtif_sc *sc)
{
    	struct ifnet *ifp = &sc->sc_ec.ec_if;
	//struct ifnet *ifp = &xneti->xni_if;
	//struct virtif_user *viu;
	struct mbuf *m;
	int queued = 0;
	struct xnetback_xstate *xst;
	int copycnt = 0;
	bool abort;
	int rxresp_flags;
	struct iovec dm[NB_XMIT_PAGES_BATCH];
	int xst_count;

	XENPRINTF(("xennetback_ifsoftstart_copy "));
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
				aprint_normal("xennetback_ifstart: ring full");
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
				VIFHYPER_rx_copy_process(ifp, sc->sc_viu, queued,
				    copycnt);
				xennetback_free_mbufs(sc, queued);
				queued = copycnt = 0;
				goto again;
			}

			/* Now committed to send */
			xst->xs_loaded = true;
			xst->xs_m = m;

			KASSERT(NB_XMIT_PAGES_BATCH <= xst->xs_dmamap->dm_nsegs);
			/* start filling iov */
			for(int seg=0; seg < xst->xs_dmamap->dm_nsegs; seg++) {
            			dm[seg].iov_base = xst->xs_dmamap->dm_segs[seg].ds_addr;;
            			dm[seg].iov_len = xst->xs_dmamap->dm_segs[seg].ds_len;
			}
		
			rxresp_flags = xst->xs_m->m_pkthdr.csum_flags & XN_M_CSUM_SUPPORTED;
			VIFHYPER_rx_copy_queue(sc->sc_viu, &queued, &copycnt,
			    rxresp_flags,
			    xst->xs_m->m_pkthdr.len, dm,
			    &xst_count, xst->xs_dmamap->dm_nsegs);

			for(int xstc=0; xstc < xst_count; xstc++)
				(xst+xstc)->xs_m = NULL;

			if_statinc(ifp, if_opackets);
			bpf_mtap(ifp, m, BPF_D_OUT);
		}
		KASSERT(copycnt <= NB_XMIT_PAGES_BATCH);
		KASSERT(queued <= copycnt);
		if (copycnt > 0) {
			VIFHYPER_rx_copy_process(ifp, sc->sc_viu, queued, copycnt);
			queued = copycnt = 0;
		}
		/*
		 * note that we don't use RING_FINAL_CHECK_FOR_REQUESTS()
		 * here, as the frontend doesn't notify when adding
		 * requests anyway
		 */
		int *unconsumed = 0;
		VIFHYPER_RING_CONSUMPTION(sc->sc_viu, unconsumed);
		if (__predict_false(abort || unconsumed)) {
			/* ring full */
			ifp->if_timer = 1;
			break;
		}
	}
	splx(s);
}

static void xennetback_free_mbufs(struct virtif_sc *sc, int queued)
{
	struct xnetback_xstate *xst;

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
