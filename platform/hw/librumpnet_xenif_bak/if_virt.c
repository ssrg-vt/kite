/*	$NetBSD: if_virt.c,v 1.36 2013/07/04 11:46:51 pooka Exp $	*/

/*
 * Copyright (c) 2008, 2013 Antti Kantee.  All Rights Reserved.
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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: if_virt.c,v 1.36 2013/07/04 11:46:51 pooka Exp $");

#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/cprng.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/sched.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_tap.h>

#include <netinet/in.h>
#include <netinet/in_var.h>

#include <rump/rump.h>

#include "xennet_checksum.h"

#include "rump_net_private.h"
#include "rump_private.h"

#include "if_virt.h"
#include "if_virt_user.h"

#include <bmk-core/sched.h>
#include <bmk-rumpuser/rumpuser.h>

static int virtif_init(struct ifnet *);
static int virtif_ioctl(struct ifnet *, u_long, void *);
static void virtif_start(struct ifnet *);
static void virtif_watchdog(struct ifnet *);
static void virtif_stop(struct ifnet *, int);

struct virtif_sc {
    struct ethercom sc_ec;
    struct virtif_user *sc_viu;
};

static int virtif_clone(struct if_clone *, int, char *);
static int virtif_unclone(struct ifnet *);

struct bmk_thread *softstart_thread;
struct if_clone VIF_CLONER =
    IF_CLONE_INITIALIZER(VIF_NAME, virtif_entry, virtif_unclone);

/* This function should be the entry point for network backend driver */
int virtif_entry(struct if_clone *ifc, int num) {
    int error = 0;

    if ((error = VIFHYPER_SET_WATCH()) != 0) {
        return error;
    }

    return 0;
}

int rump_virtif_clone(char *path) { return virtif_clone(NULL, 0, path); }

static int virtif_clone(struct if_clone *ifc, int num, char *path) {
    struct virtif_sc *sc = NULL;
    struct virtif_user *viu;
    struct ifnet *ifp;
    uint8_t enaddr[ETHER_ADDR_LEN] = {0xb2, 0x0a, 0x00, 0x0b, 0x0e, 0x01};
    char enaddrstr[3 * ETHER_ADDR_LEN];
    int error = 0;
    char vifname[16] = {0};

    if (num >= 0x100)
        return E2BIG;

    enaddr[2] = cprng_fast32() & 0xff;
    enaddr[5] = num;

    sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
    if (sc == NULL)
        return -1;

    if ((error = VIFHYPER_CREATE(path, sc, enaddr, &viu, vifname)) != 0) {
        kmem_free(sc, sizeof(*sc));
        return error;
    }
    sc->sc_viu = viu;

    sc->sc_ec.ec_capabilities |= ETHERCAP_VLAN_MTU;
    ifp = &sc->sc_ec.ec_if;
    snprintf(ifp->if_xname, sizeof(ifp->if_xname), "%s", vifname);
    ifp->if_softc = sc;

    ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
    ifp->if_capabilities = IFCAP_CSUM_TCPv4_Tx | IFCAP_CSUM_UDPv4_Tx;
    ifp->if_init = virtif_init;
    ifp->if_ioctl = virtif_ioctl;
    ifp->if_start = virtif_start;
    ifp->if_watchdog = virtif_watchdog;
    ifp->if_stop = virtif_stop;
    ifp->if_timer = 0;
    sc->sc_viu = viu;
    IFQ_SET_READY(&ifp->if_snd);

    sc->sc_viu = viu;
    if_attach(ifp);
    ether_ifattach(ifp, enaddr);

    ether_snprintf(enaddrstr, sizeof(enaddrstr), enaddr);
    aprint_normal_ifnet(ifp, "Ethernet address %s\n", enaddrstr);

    if (error) {
        virtif_unclone(ifp);
        return error;
    }

    VIFHYPER_SET_START(viu, &sc->sc_ec.ec_if);

    return error;
}

static int virtif_unclone(struct ifnet *ifp) {
    struct virtif_sc *sc = ifp->if_softc;

    VIFHYPER_DYING(sc->sc_viu);

    virtif_stop(ifp, 1);
    if_down(ifp);

    VIFHYPER_DESTROY(sc->sc_viu);

    kmem_free(sc, sizeof(*sc));

    ether_ifdetach(ifp);
    if_detach(ifp);

    return 0;
}

static int virtif_init(struct ifnet *ifp) {
    ifp->if_flags |= IFF_RUNNING;
    return 0;
}

static int virtif_ioctl(struct ifnet *ifp, u_long cmd, void *data) {
    int s, rv;

    s = splnet();
    rv = ether_ioctl(ifp, cmd, data);
    if (rv == ENETRESET)
        rv = 0;
    splx(s);

    return rv;
}

void rump_virtif_soft_start(struct ifnet *ifp) {
    int i = 0, j, offset;
    struct virtif_sc *sc = ifp->if_softc;
    struct rump_iovec io[LB_SH];
    struct mbuf *mbufs_sent[LB_SH];
    struct mbuf *m, *new_m;
    paddr_t xmit_pa;
    int is_full;
    int count = 0;

    int s = splnet();
    if (__predict_false((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) !=
                        IFF_RUNNING)) {
        splx(s);
        return;
    }

    ifp->if_flags |= IFF_OACTIVE;

    while (!IFQ_IS_EMPTY(&ifp->if_snd)) {
        for (i = 0; !IFQ_IS_EMPTY(&ifp->if_snd);) {
            IFQ_POLL(&ifp->if_snd, m);
            if (__predict_false(m == NULL))
                panic("xennetback_ifstart: IFQ_POLL");
            if (__predict_false(i == LB_SH))
                break; /* we filled the array */
            switch (m->m_flags & (M_EXT | M_EXT_CLUSTER)) {
            case M_EXT | M_EXT_CLUSTER:
                KASSERT(m->m_ext.ext_paddr != M_PADDR_INVALID);
                xmit_pa = m->m_ext.ext_paddr;
                offset = m->m_data - m->m_ext.ext_buf;
                break;
            case 0:
                KASSERT(m->m_paddr != M_PADDR_INVALID);
                xmit_pa = m->m_paddr;
                offset = M_BUFOFFSET(m) + (m->m_data - M_BUFADDR(m));
                break;
            default:
                if (__predict_false(!pmap_extract(
                        pmap_kernel(), (vaddr_t)m->m_data, &xmit_pa))) {
                    panic("xennet_start: no pa");
                }
                offset = 0;
                break;
            }
            offset += (xmit_pa & ~PG_FRAME);
            xmit_pa = (xmit_pa & PG_FRAME);
            if (m->m_pkthdr.len != m->m_len ||
                (offset + m->m_pkthdr.len) > PAGE_SIZE) {
                MGETHDR(new_m, M_DONTWAIT, MT_DATA);
                count++;
                if (__predict_false(new_m == NULL)) {
                    aprint_normal("%s: cannot allocate new mbuf\n",
                                  ifp->if_xname);
                    break;
                }
                if (m->m_pkthdr.len > MHLEN) {
                    MCLGET(new_m, M_DONTWAIT);
                    if (__predict_false((new_m->m_flags & M_EXT) == 0)) {
                        aprint_normal("%s: no mbuf cluster\n", ifp->if_xname);
                        m_freem(new_m);
                        count--;
                        break;
                    }
                    xmit_pa = new_m->m_ext.ext_paddr;
                    offset = new_m->m_data - new_m->m_ext.ext_buf;
                } else {
                    xmit_pa = new_m->m_paddr;
                    offset =
                        M_BUFOFFSET(new_m) + (new_m->m_data - M_BUFADDR(new_m));
                }
                offset += (xmit_pa & ~PG_FRAME);
                xmit_pa = (xmit_pa & PG_FRAME);
                m_copydata(m, 0, m->m_pkthdr.len, mtod(new_m, void *));
                new_m->m_len = new_m->m_pkthdr.len = m->m_pkthdr.len;
                IFQ_DEQUEUE(&ifp->if_snd, m);
                m_freem(m);
                m = new_m;
            } else {
                IFQ_DEQUEUE(&ifp->if_snd, m);
            }

            KASSERT(xmit_pa != POOL_PADDR_INVALID);
            KASSERT((offset + m->m_pkthdr.len) <= PAGE_SIZE);
            /* start filling iov */
            io[i].iov_base = (void *)xmit_pa;
            io[i].iov_offset = offset;
            io[i].iov_len = m->m_pkthdr.len;
            io[i].csum_blank =
                (m->m_pkthdr.csum_flags & (M_CSUM_TCPv4 | M_CSUM_UDPv4));
            mbufs_sent[i] = m;
            i++; /* this packet has been queued */
            ifp->if_opackets++;
            bpf_mtap(ifp, m, BPF_D_OUT);
        }
        if (i != 0) {
            VIFHYPER_SEND(sc->sc_viu, io, i);

            /* now we can free the mbufs */
            for (j = 0; j < i; j++) {
                m_freem(mbufs_sent[j]);
            }
        }
        VIFHYPER_RING_STATUS(sc->sc_viu, &is_full);
        if (is_full)
            break;
    }
    splx(s);
    ifp->if_flags &= ~IFF_OACTIVE;
}

static void virtif_start(struct ifnet *ifp) {
    struct virtif_sc *sc = ifp->if_softc;
    VIFHYPER_WAKE(sc->sc_viu);
}

static void virtif_watchdog(struct ifnet *ifp) { virtif_start(ifp); }

static void virtif_stop(struct ifnet *ifp, int disable) {
    ifp->if_flags &= ~IFF_RUNNING;
}

void rump_virtif_pktdeliver(struct virtif_sc *sc, struct iovec *iov,
                            size_t iovlen, unsigned char csum_blank) {
    struct ifnet *ifp = &sc->sc_ec.ec_if;
    struct mbuf *m;
    size_t i;
    int off, olen;

    if ((ifp->if_flags & IFF_RUNNING) == 0)
        return;

    m = m_gethdr(M_NOWAIT, MT_DATA);
    if (m == NULL) {
        aprint_normal("m == NULL packet dropped\n");
        return; /* drop packet */
    }
    m->m_len = m->m_pkthdr.len = 0;

    for (i = 0, off = 0; i < iovlen; i++) {
        olen = m->m_pkthdr.len;
        m_copyback(m, off, iov[i].iov_len, iov[i].iov_base);
        off += iov[i].iov_len;
        if (olen + off != m->m_pkthdr.len) {
            aprint_verbose_ifnet(ifp, "m_copyback failed\n");
            m_freem(m);
            return;
        }
    }
    if (csum_blank == 1) {
        xennet_checksum_fill(&m);
        if (m == NULL) {
            aprint_normal("csum error\n");
            ifp->if_ierrors++;
            return;
        }
    }

#if __NetBSD_Prereq__(7, 99, 31)
    m_set_rcvif(m, ifp);
#else
    m->m_pkthdr.rcvif = ifp;
#endif

    KERNEL_LOCK(1, NULL);
    bpf_mtap(ifp, m, BPF_D_OUT);
    if_percpuq_enqueue(ifp->if_percpuq, m);
    KERNEL_UNLOCK_LAST(NULL);
}
