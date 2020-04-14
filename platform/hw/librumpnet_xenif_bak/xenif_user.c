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

struct iovec {
    void *iov_base;
    unsigned long iov_len;
};

#include <mini-os/netback.h>
#include <mini-os/os.h>
#include <mini-os/xenbus.h>

#include <bmk-core/errno.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/sched.h>
#include <bmk-core/string.h>

#include <bmk-rumpuser/core_types.h>
#include <bmk-rumpuser/rumpuser.h>

#include <xen/io/netif.h>

#include "if_virt.h"
#include "if_virt_user.h"

/*
 * For now, shovel the packets from the interrupt to a
 * thread context via an intermediate set of buffers.  Need
 * to fix this a bit down the road.
 */
#define MAXPKT 2000
struct onepkt {
    unsigned char pkt_data[MAXPKT];
    int pkt_dlen;
    unsigned char pkt_csum;
};

/* The order of these values is important */
#define THREADBLK_STATUS_SLEEP 0x0
#define THREADBLK_STATUS_AWAKE 0x1
#define THREADBLK_STATUS_NOTIFY 0x2
struct threadblk {
    struct bmk_block_data header;
    unsigned long status;
    char _pad[48]; /* FIXME: better way to avoid false sharing */
};

#define NBUF 512
struct virtif_user {
    struct threadblk viu_rcvrblk;
    struct threadblk viu_softsblk;
    struct bmk_thread *viu_rcvrthr;
    struct bmk_thread *viu_softsthr;
    void *viu_ifp;
    struct netback_dev *viu_dev;
    struct virtif_sc *viu_vifsc;

    int viu_read;
    int viu_write;
    int viu_dying;
    struct onepkt viu_pkts[NBUF];
};

static struct xenbus_event_queue be_watch;
static struct bmk_thread *backend_thread;

static void threadblk_callback(struct bmk_thread *prev,
                               struct bmk_block_data *_data) {
    struct threadblk *data = (struct threadblk *)_data;

    /* THREADBLK_STATUS_AWAKE -> THREADBLK_STATUS_SLEEP */
    /* THREADBLK_STATUS_NOTIFY -> THREADBLK_STATUS_AWAKE */
    if (__atomic_fetch_sub(&data->status, 1, __ATOMIC_ACQ_REL) !=
        THREADBLK_STATUS_AWAKE) {
        /* the current state is THREADBLK_STATUS_AWAKE */
        bmk_sched_wake(prev);
    }
}

static int probe_netback_device(const char *vifpath) {
    int err, i, pos, msize;
    unsigned long state;
    char **dir;
    char *msg;
    char *devpath, path[40];

    /* give us a rump kernel context */
    rumpuser__hyp.hyp_schedule();
    rumpuser__hyp.hyp_lwproc_newlwp(0);
    rumpuser__hyp.hyp_unschedule();

    msg = xenbus_ls(XBT_NIL, vifpath, &dir);
    if (msg) {
        bmk_printf("Error in xenbus ls: %s\n", msg);
        bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
        return 1;
    }

    for (pos = 0; dir[pos]; pos++) {
        i = pos;
        msize = bmk_strlen(vifpath) + bmk_strlen(dir[i]) + 2;
        devpath = bmk_memalloc(msize, 0, BMK_MEMWHO_WIREDBMK);
        if (devpath == NULL) {
            bmk_printf("can't malloc xbusd");
            return 1;
        }

        bmk_snprintf(devpath, msize, "%s/%s", vifpath, dir[i]);
        // TODO: check internal list of already registered devices
        bmk_snprintf(path, sizeof(path), "%s/state", devpath);
        state = xenbus_read_integer(path);
        if (state != XenbusStateInitialising) {
            /* device is not new */
            bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
            continue;
        }

        bmk_printf("Probe netback device %s\n", devpath);
        rumpuser__hyp.hyp_schedule();
        err = rump_virtif_clone(devpath);
        rumpuser__hyp.hyp_unschedule();
        if (err) {
            bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
            return err;
        }

        bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
    }

    return 0;
}

static void backend_thread_func(void *ign) {
    char *msg;
    char **dirt, **dirid;
    unsigned int type, id;
    char path[30];
    char vif_found;
    int err;

    for (;;) {
        xenbus_wait_for_watch(&be_watch);

        bmk_printf("Checking for backend changes\n");
        msg = xenbus_ls(XBT_NIL, "backend", &dirt);
        if (msg) {
            bmk_printf("No backend found: %s\n", msg);
            bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
            continue;
        }

        vif_found = 0;
        for (type = 0; dirt[type]; type++) {
            if (bmk_strcmp(dirt[type], "vif") == 0) {
                vif_found = 1;
                break;
            }
        }

        if (vif_found == 0)
            continue;

        msg = xenbus_ls(XBT_NIL, "backend/vif", &dirid);
        if (msg) {
            bmk_printf("Error in xenbus ls: %s\n", msg);
            bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);

            return;
        }

        for (id = 0; dirid[id]; id++) {
            bmk_snprintf(path, sizeof(path), "backend/vif/%s", dirid[id]);
            err = probe_netback_device(path);
            if (err)
                break;
            bmk_memfree(dirid[id], BMK_MEMWHO_WIREDBMK);
        }
        bmk_memfree(dirid, BMK_MEMWHO_WIREDBMK);

        for (type = 0; dirt[type]; type++) {
            bmk_memfree(dirt[type], BMK_MEMWHO_WIREDBMK);
        }
        bmk_memfree(dirt, BMK_MEMWHO_WIREDBMK);
    }
}

int VIFHYPER_SET_WATCH(void) {
    char path[64];
    int dom, nlocks;

    rumpkern_unsched(&nlocks, NULL);
    xenbus_event_queue_init(&be_watch);
    bmk_snprintf(path, sizeof(path), "domid");
    dom = xenbus_read_integer(path);

    if (dom < 0) {
        bmk_printf("Couldn't fetch domid\n");
        return BMK_EINVAL;
    } else
        bmk_printf("Netback domid is %d\n", dom);

    bmk_snprintf(path, sizeof(path), "/local/domain/%d/backend", dom);
    xenbus_watch_path_token(XBT_NIL, path, path, &be_watch);

    backend_thread = bmk_sched_create("backend", NULL, 0, -1,
                                      backend_thread_func, NULL, NULL, 0);
    rumpkern_sched(nlocks, NULL);

    return 0;
}

static void soft_start_thread_func(void *_viu) {
    struct virtif_user *viu = (struct virtif_user *)_viu;

    /* give us a rump kernel context */
    rumpuser__hyp.hyp_schedule();
    rumpuser__hyp.hyp_lwproc_newlwp(0);
    rumpuser__hyp.hyp_unschedule();

    while (1) {
        bmk_sched_blockprepare();
        bmk_sched_block(&viu->viu_softsblk.header);

        size_t counter = 0;
        __atomic_store_n(&viu->viu_softsblk.status, THREADBLK_STATUS_AWAKE,
                         __ATOMIC_RELEASE);
        do {
            rumpuser__hyp.hyp_schedule();
            rump_virtif_soft_start(viu->viu_ifp);
            rumpuser__hyp.hyp_unschedule();
            /* do not monopolize the CPU */
            if (++counter == 1000) {
                bmk_sched_yield();
                counter = 0;
            }
        } while (__atomic_exchange_n(
                     &viu->viu_softsblk.status, THREADBLK_STATUS_AWAKE,
                     __ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
    }
}

int VIFHYPER_SET_START(struct virtif_user *viu, void *ifp) {
    viu->viu_ifp = ifp;
    viu->viu_softsthr = bmk_sched_create("soft_start", NULL, 0, -1,
                                         soft_start_thread_func, viu, NULL, 0);
    return 0;
}

int VIFHYPER_WAKE(struct virtif_user *viu) {
    if (__atomic_exchange_n(&viu->viu_softsblk.status, THREADBLK_STATUS_NOTIFY,
                            __ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
        bmk_sched_wake(viu->viu_softsthr);
    }

    return 0;
}

/*
 * Ok, based on how (the unmodified) netback works, we need to
 * consume the data here.  So store it locally (and revisit some day).
 */
static void myrecv(struct netback_dev *dev, unsigned char *data, int dlen,
                   unsigned char csum_blank) {
    struct virtif_user *viu = netback_get_private(dev);
    int write, nextw;

    /* TODO: we should be at the correct spl already, assert how? */

    write = __atomic_load_n(&viu->viu_write, __ATOMIC_ACQUIRE);
    nextw = (write + 1) % NBUF;
    /* queue full?  drop packet */
    if (nextw == __atomic_load_n(&viu->viu_read, __ATOMIC_ACQUIRE)) {
        return;
    }

    if (dlen > MAXPKT) {
        bmk_printf("myrecv: pkt len %d too big\n", dlen);
        return;
    }

    bmk_memcpy(viu->viu_pkts[write].pkt_data, data, dlen);
    viu->viu_pkts[write].pkt_dlen = dlen;
    viu->viu_pkts[write].pkt_csum = csum_blank;
    __atomic_store_n(&viu->viu_write, nextw, __ATOMIC_RELEASE);

    if (__atomic_exchange_n(&viu->viu_rcvrblk.status, THREADBLK_STATUS_NOTIFY,
                            __ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
        bmk_sched_wake(viu->viu_rcvrthr);
    }
}

static void pusher(void *arg) {
    struct virtif_user *viu = arg;
    struct iovec iov;
    struct onepkt *mypkt;
    int read = __atomic_load_n(&viu->viu_read, __ATOMIC_ACQUIRE);

    /* give us a rump kernel context */
    rumpuser__hyp.hyp_schedule();
    rumpuser__hyp.hyp_lwproc_newlwp(0);
    rumpuser__hyp.hyp_unschedule();

    while (!__atomic_load_n(&viu->viu_dying, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&viu->viu_rcvrblk.status, THREADBLK_STATUS_AWAKE,
                         __ATOMIC_RELEASE);
        if (read == __atomic_load_n(&viu->viu_write, __ATOMIC_ACQUIRE)) {
            bmk_sched_blockprepare();
            bmk_sched_block(&viu->viu_rcvrblk.header);
            continue;
        }
        mypkt = &viu->viu_pkts[read];

        iov.iov_base = mypkt->pkt_data;
        iov.iov_len = mypkt->pkt_dlen;

        rumpuser__hyp.hyp_schedule();
        rump_virtif_pktdeliver(viu->viu_vifsc, &iov, 1, mypkt->pkt_csum);
        rumpuser__hyp.hyp_unschedule();

        read = (read + 1) % NBUF;
        __atomic_store_n(&viu->viu_read, read, __ATOMIC_RELEASE);
    }
}

int VIFHYPER_CREATE(char *path, struct virtif_sc *vif_sc, uint8_t *enaddr,
                    struct virtif_user **viup, int8_t *vifname) {
    struct virtif_user *viu = NULL;
    int rv, nlocks;

    rumpkern_unsched(&nlocks, NULL);

    viu = bmk_memalloc(sizeof(*viu), 0, BMK_MEMWHO_RUMPKERN);
    if (viu == NULL) {
        rv = BMK_ENOMEM;
        goto out;
    }
    bmk_memset(viu, 0, sizeof(*viu));
    viu->viu_rcvrblk.header.callback = threadblk_callback;
    viu->viu_rcvrblk.status = THREADBLK_STATUS_AWAKE;
    viu->viu_softsblk.header.callback = threadblk_callback;
    viu->viu_softsblk.status = THREADBLK_STATUS_AWAKE;
    viu->viu_vifsc = vif_sc;

    viu->viu_rcvrthr =
        bmk_sched_create("xenifp", NULL, 1, -1, pusher, viu, NULL, 0);
    if (viu->viu_rcvrthr == NULL) {
        bmk_platform_halt("fatal thread creation failure\n"); /* XXX */
    }

    viu->viu_dev = netback_init(path, myrecv, enaddr, NULL, viu, vifname);
    if (!viu->viu_dev) {
        VIFHYPER_DYING(viu);
        bmk_sched_join(viu->viu_rcvrthr);
        bmk_memfree(viu, BMK_MEMWHO_RUMPKERN);
        rv = BMK_EINVAL; /* ? */
        goto out;
    }

    rv = 0;

out:
    rumpkern_sched(nlocks, NULL);

    *viup = viu;
    return rv;
}

void VIFHYPER_SEND(struct virtif_user *viu, struct rump_iovec *iov,
                   size_t iovlen) {
    int csum_blank[LB_SH];
    size_t i = 0;

    for (i = 0; i < iovlen; i++) {
        if (netback_prepare_xmit(viu->viu_dev, iov[i].iov_base, iov[i].iov_len,
                                 iov[i].iov_offset, i) == -1) {
            break;
        }

        csum_blank[i] = iov[i].csum_blank;
    }

    netback_xmit(viu->viu_dev, csum_blank, i);
}

void VIFHYPER_RING_STATUS(struct virtif_user *viu, int *is_full) {
    *is_full = netback_rxring_full(viu->viu_dev);
}

void VIFHYPER_DYING(struct virtif_user *viu) {
    __atomic_store_n(&viu->viu_dying, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&viu->viu_rcvrblk.status, THREADBLK_STATUS_NOTIFY,
                            __ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
        bmk_sched_wake(viu->viu_rcvrthr);
    }
}

void VIFHYPER_DESTROY(struct virtif_user *viu) {
    XENBUS_BUG_ON(viu->viu_dying != 1);

    bmk_sched_join(viu->viu_rcvrthr);
    netback_shutdown(viu->viu_dev);
    bmk_memfree(viu, BMK_MEMWHO_RUMPKERN);
}
