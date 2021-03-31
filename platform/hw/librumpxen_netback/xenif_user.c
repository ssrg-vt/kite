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

DECLARE_WAIT_QUEUE_HEAD(xennetback_queue);

#define memset(a, b, c) bmk_memset(a, b, c)

#define NET_TX_RING_SIZE __CONST_RING_SIZE(netif_tx, PAGE_SIZE)
#define NET_RX_RING_SIZE __CONST_RING_SIZE(netif_rx, PAGE_SIZE)

#define TX_BATCH 32
#define GRANT_INVALID_REF 0
#define get_gfn(x) (PFN_DOWN((uint64_t)(x)))

/* The order of these values is important */
#define THREADBLK_STATUS_SLEEP 0x0
#define THREADBLK_STATUS_AWAKE 0x1
#define THREADBLK_STATUS_NOTIFY 0x2
struct threadblk {
    struct bmk_block_data header;
    unsigned long status;
    char _pad[48]; /* FIXME: better way to avoid false sharing */
};

typedef enum {WAITING, RUN, RUN_TX, RUN_RX, DISCONNECTING, DISCONNECTED} xennet_state_t;

struct net_buffer {
    void *page;
    grant_ref_t gref;
};

struct xennetback_dev {
    domid_t back_id;
    domid_t front_id;
    uint32_t handle;

    struct Queue *rx_list;
    struct Queue *tx_list;
    struct semaphore rx_sem;

    struct net_buffer rx_buffers[NET_RX_RING_SIZE];

    netif_tx_back_ring_t tx;
    netif_rx_back_ring_t rx;
    grant_ref_t tx_ring_ref;
    grant_ref_t rx_ring_ref;

    evtchn_port_t evtchn;
    evtchn_port_t revtchn;
    evtchn_port_t tevtchn;

    struct gntmap rx_map;
    struct gntmap tx_map;

    char nodename[64];

    char *frontend;
    char *mac;

    struct xenbus_event_queue events;

    void *xennetback_priv;
    struct gntmap map_entry;

    spinlock_t xennet_lock;
    xennet_state_t xennet_status;
    struct bmk_thread *xennet_thread;
    struct threadblk xennet_thread_blk;
};

struct Queue {
    unsigned int front, rear, size, capacity;
    unsigned int *array;
};

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

#define NBUF 512
struct virtif_user {
    struct threadblk viu_rcvrblk;
    struct threadblk viu_softsblk;
    struct bmk_thread *viu_rcvrthr;
    struct bmk_thread *viu_softsthr;
    void *viu_ifp;
    struct xennetback_dev *viu_dev;
    struct virtif_sc *viu_vifsc;

    int viu_read;
    int viu_write;
    int viu_dying;
    struct onepkt viu_pkts[NBUF];
};

static struct xenbus_event_queue be_watch;
static struct bmk_thread *backend_thread;
gnttab_copy_t rx_gop[NET_TX_RING_SIZE];
gnttab_copy_t tx_gop[NET_TX_RING_SIZE];
unsigned short rsp_id[NET_TX_RING_SIZE];
static spinlock_t xennetback_lock = SPIN_LOCK_UNLOCKED;

static void network_tx(struct xennetback_dev *dev);
static void network_rx_buf_gc(struct xennetback_dev *dev);
static void xennetback_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data);

static struct xennetback_dev *xennetback_init(char *nodename, unsigned char rawmac[6], char **ip, void *priv, char* vifname);
static int xennetback_prepare_xmit(struct xennetback_dev *dev, unsigned char* data, unsigned int len, int offset, unsigned int index);
static void xennetback_xmit(struct xennetback_dev *dev, int* csum_blank, int count); 
static int xennetback_rxring_full(struct xennetback_dev *dev); 
static void xennetback_shutdown(struct xennetback_dev *dev);

static void *xennetback_get_private(struct xennetback_dev *);

extern struct wait_queue_head xennetback_queue;


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

static inline struct Queue *create_queue(unsigned capacity) {
    struct Queue *queue = (struct Queue *)bmk_memcalloc(1, sizeof(struct Queue),
                                                        BMK_MEMWHO_WIREDBMK);
    queue->capacity = capacity;
    queue->front = queue->size = 0;
    queue->rear = capacity - 1;
    queue->array = (int *)bmk_memalloc(1, queue->capacity * sizeof(long),
                                       BMK_MEMWHO_WIREDBMK);

    return queue;
}

static inline int is_full(struct Queue *queue) {
    return (queue->size == queue->capacity);
}

static inline int is_empty(struct Queue *queue) { return (queue->size == 0); }

static inline void add_to_list(long item, struct Queue *queue) {
    if (is_full(queue)) 
	    return;

    queue->rear = (queue->rear + 1) % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
}

static inline long get_from_list(struct Queue *queue) {
    if (is_empty(queue))
	    return -1;

    int item = queue->array[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size = queue->size - 1;

    bmk_assert(item != -1);

    return item;
}

static inline void destroy_queue(struct Queue *queue) {
    bmk_memfree(queue, BMK_MEMWHO_WIREDBMK);
}

static int probe_xennetback_device(const char *vifpath) {
    int err, i, pos, msize;
    unsigned long state;
    char **dir;
    char *msg;
    char *devpath, path[40];

    XENPRINTF(("%s\n", __func__));

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

    XENPRINTF(("%s\n", __func__));

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
            err = probe_xennetback_device(path);
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

    XENPRINTF(("%s\n", __func__));

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

    XENPRINTF(("%s\n", __func__));

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

    XENPRINTF(("%s\n", __func__));

    viu->viu_ifp = ifp;
    viu->viu_softsthr = bmk_sched_create("soft_start", NULL, 0, -1,
                                         soft_start_thread_func, viu, NULL, 0);
    return 0;
}

int VIFHYPER_WAKE(struct virtif_user *viu) {

    XENPRINTF(("%s\n", __func__));

    if (__atomic_exchange_n(&viu->viu_softsblk.status, THREADBLK_STATUS_NOTIFY,
                            __ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
        bmk_sched_wake(viu->viu_softsthr);
    }

    return 0;
}

int VIFHYPER_CREATE(char *path, struct virtif_sc *vif_sc, uint8_t *enaddr,
                    struct virtif_user **viup, int8_t *vifname) {
    struct virtif_user *viu = NULL;
    int rv, nlocks;

    XENPRINTF(("%s\n", __func__));

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

    viu->viu_dev = xennetback_init(path, enaddr, NULL, viu, vifname);
    if (!viu->viu_dev) {
        VIFHYPER_DYING(viu);
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
    int csum_blank[NET_TX_RING_SIZE];
    size_t i = 0;

    XENPRINTF(("%s\n", __func__));

    for (i = 0; i < iovlen; i++) {
        if (xennetback_prepare_xmit(viu->viu_dev, iov[i].iov_base, iov[i].iov_len,
                                 iov[i].iov_offset, i) == -1) {
            break;
        }

        csum_blank[i] = iov[i].csum_blank;
    }

    xennetback_xmit(viu->viu_dev, csum_blank, i);
}

void VIFHYPER_RING_STATUS(struct virtif_user *viu, int *is_full) {
    XENPRINTF(("%s\n", __func__));

    *is_full = xennetback_rxring_full(viu->viu_dev);
}

void VIFHYPER_DYING(struct virtif_user *viu) {
    int nlocks;

    XENPRINTF(("%s\n", __func__));

    rumpkern_unsched(&nlocks, NULL);
    __atomic_store_n(&viu->viu_dying, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&viu->viu_rcvrblk.status, THREADBLK_STATUS_NOTIFY,
                            __ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
    }
    rumpkern_sched(nlocks, NULL);
}

void VIFHYPER_DESTROY(struct virtif_user *viu) {
    int nlocks;

    XENPRINTF(("%s\n", __func__));

    rumpkern_unsched(&nlocks, NULL);
    XENBUS_BUG_ON(viu->viu_dying != 1);

    xennetback_shutdown(viu->viu_dev);
    bmk_memfree(viu, BMK_MEMWHO_RUMPKERN);
    rumpkern_sched(nlocks, NULL);
}

static void xennetback_tx_response(struct xennetback_dev *dev, int id, int status) {
    RING_IDX resp_prod;
    struct netif_tx_response *txresp;
    int do_event;

    XENPRINTF(("%s\n", __func__));

    resp_prod = dev->tx.rsp_prod_pvt;
    txresp = RING_GET_RESPONSE(&dev->tx, resp_prod);

    txresp->id = id;
    txresp->status = status;
    dev->tx.rsp_prod_pvt++;

    RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&dev->tx, do_event);
    if (do_event) {
        minios_notify_remote_via_evtchn(dev->tevtchn);
    }
}

static void network_tx(struct xennetback_dev *dev) {
    netif_tx_request_t txreqs[TX_BATCH];
    RING_IDX req_cons;
    void *pages[TX_BATCH];
    void *map_pages[TX_BATCH];
    uint32_t grefs[TX_BATCH];
    uint32_t id = (uint32_t)dev->front_id;
    int i = 0, j, receive_pending;
    uint8_t csum_blank[TX_BATCH];
    struct iovec iov[TX_BATCH];
    long addr;
    int map_count = 0;
    int copy_count = 0;

    struct virtif_user *viu = xennetback_get_private(dev);

    XENPRINTF(("%s\n", __func__));

    req_cons = dev->tx.req_cons;
    rmb();

label:
    i = map_count = copy_count = 0;

    while (1) {
    	rmb(); /* be sure to read the request before updating */
        dev->tx.req_cons = req_cons;
        wmb();
	receive_pending = RING_HAS_UNCONSUMED_REQUESTS(&dev->tx);
        if (receive_pending == 0) {
            break;
        }
	receive_pending--;

next:
	rmb();
        RING_COPY_REQUEST(&dev->tx, req_cons, &txreqs[i]);
        req_cons++;

	if (txreqs[i].flags & NETTXF_more_data) {
		bmk_printf("MORE DATA i=%d size=%d\n", i, txreqs[i].size);
		goto next;
	}

	addr = get_from_list(dev->tx_list);
	if(addr == -1)
		pages[i] = bmk_pgalloc_align(0, BMK_PCPU_PAGE_SIZE);
	else 
		pages[i] = (void*)addr;

	if(pages[i] == NULL) {
		bmk_printf("%s: Cannot allocate memory\n", __func__);
		bmk_platform_halt(NULL);
    	}

	if ((txreqs[i].size > 128) && 0) {
		grefs[map_count] = txreqs[i].gref;
		map_count++;
	}
	else {
		bmk_assert((txreqs[i].size + txreqs[i].offset) < PAGE_SIZE);

		tx_gop[copy_count].source.u.ref = txreqs[i].gref;
		tx_gop[copy_count].source.domid = dev->front_id; 
		tx_gop[copy_count].source.offset = txreqs[i].offset;
		tx_gop[copy_count].dest.u.gmfn = get_gfn(pages[i]);
		tx_gop[copy_count].dest.domid = DOMID_SELF;
		tx_gop[copy_count].dest.offset = 0;
		tx_gop[copy_count].len = txreqs[i].size;
		tx_gop[copy_count].flags = GNTCOPY_source_gref;

		iov[i].iov_base = (unsigned char *)pages[i];
		copy_count++;
	}

	iov[i].iov_len = txreqs[i].size;
	if ((txreqs[i].flags & NETTXF_csum_blank) != 0)
		csum_blank[i] = 1;
	else
                csum_blank[i] = 0;

    	rmb(); /* be sure to read the request before updating pointer */
	dev->tx.req_cons = req_cons;
	wmb();

	if (++i >= TX_BATCH)
		break;
    }

    /* Do copy for tx_size smaller than 128*/
    if (copy_count != 0) {
	if (HYPERVISOR_grant_table_op(GNTTABOP_copy, &tx_gop, copy_count) != 0) {
        	bmk_printf("TX GNTTABOP_copy failed\n");
    	}

        for (j = 0; j < copy_count; j++) {
    	    if (tx_gop[j].status != GNTST_okay) {
		    bmk_printf("TX GOP Status = %d, id = %d\n", tx_gop[j].status, j);
		    xennetback_tx_response(dev, txreqs[j].id, NETIF_RSP_DROPPED);
	    }
	}
    }
    
    /* Do map for tx_size of 128 or bigger, and construct iov*/
    if (map_count != 0) {
	    if (gntmap_map_grant_refs_n(&dev->map_entry, map_count, &id, 0,
				    grefs, 0, map_pages) != 0) {
		    bmk_printf("TX GNTTABOP_map failed\n");
		    for (j = 0; j < map_count; j++)
			xennetback_tx_response(dev, txreqs[j].id, NETIF_RSP_DROPPED);
		    return;
	    }
	    else {
		    for (j = 0; j < i; j++) {
			if (txreqs[j].size > 128) {
				bmk_memcpy(pages[j], (unsigned char *)map_pages[j]
						+ txreqs[j].offset, txreqs[j].size);
		    		iov[j].iov_base = (unsigned char *)pages[j];
			}
		    }
	    }
	
	    /* unmap map grefs */
	    if (gntmap_munmap_n(&dev->map_entry, (unsigned long *)map_pages, map_count) != 0)
		    bmk_printf("UNMAPED FAILED\n");
    }

    /* submit all iovs */
    for (j = 0; j < i; j++) {
	rumpuser__hyp.hyp_schedule();
	rump_virtif_pktenque(viu->viu_vifsc, &iov[j], 1, csum_blank[j]);
	rumpuser__hyp.hyp_unschedule();
    }
   
    /* sending response just after ecah corresponding pktdeliver reduces
     * performance. Therefore, we first do all pktdeliver and then send responses. 
     */
    for (j = 0; j < i; j++) {
	xennetback_tx_response(dev, txreqs[j].id, NETIF_RSP_OKAY);
    	add_to_list((long)pages[j], dev->tx_list);
    }

    rmb(); /* be sure to read the request before updating pointer */
    dev->tx.req_cons = req_cons;
    wmb();

    network_rx_buf_gc(dev); 

    RING_FINAL_CHECK_FOR_REQUESTS(&dev->tx, receive_pending);
    if (receive_pending != 0)
	    goto label;
}

static void network_rx_buf_gc(struct xennetback_dev *dev) {
    RING_IDX cons, prod;
    unsigned short id;

    XENPRINTF(("%s\n", __func__));

    do {
        prod = dev->rx.sring->req_prod;
        rmb(); /* Ensure we see responses up to 'rp'. */

        for (cons = dev->rx.req_cons; cons != prod; cons++) {
            netif_rx_request_t rxreq;
            struct net_buffer *buf;

            RING_COPY_REQUEST(&dev->rx, cons, &rxreq);

            id = rxreq.id;
            XENBUS_BUG_ON(id >= NET_RX_RING_SIZE);

            buf = &dev->rx_buffers[id];
            buf->gref = rxreq.gref;

            add_to_list(id, dev->rx_list);
            up(&dev->rx_sem);
        }

        dev->rx.req_cons = prod;
        mb();
    } while ((cons == prod) && (prod != dev->rx.sring->req_prod));
}

static void xennet_sched_wake(struct threadblk *tblk, struct bmk_thread *thread)
{
	if (__atomic_exchange_n(&tblk->status, THREADBLK_STATUS_NOTIFY,
				__ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
		bmk_sched_wake(thread);
	}
}

static void xennetback_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data) {
	struct xennetback_dev *dev = data;
	bmk_platform_splhigh();
	spin_lock(&dev->xennet_lock);
	/* only set RUN state when we are WAITING for work */
	if (dev->xennet_status == WAITING)
	       dev->xennet_status = RUN;
	xennet_sched_wake(&dev->xennet_thread_blk, dev->xennet_thread);
	spin_unlock(&dev->xennet_lock);
	bmk_platform_splx(0);
}

static void xennetback_tx_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data) {
	struct xennetback_dev *dev = data;
	bmk_platform_splhigh();
	spin_lock(&dev->xennet_lock);
	/* only set RUN state when we are WAITING for work */
	if (dev->xennet_status == WAITING)
	       dev->xennet_status = RUN_TX;
	xennet_sched_wake(&dev->xennet_thread_blk, dev->xennet_thread);
	spin_unlock(&dev->xennet_lock);
	bmk_platform_splx(0);
}

static void xennetback_rx_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data) {
	struct xennetback_dev *dev = data;
	bmk_platform_splhigh();
	spin_lock(&dev->xennet_lock);
	/* only set RUN state when we are WAITING for work */
	if (dev->xennet_status == WAITING)
	       dev->xennet_status = RUN_RX;
	xennet_sched_wake(&dev->xennet_thread_blk, dev->xennet_thread);
	spin_unlock(&dev->xennet_lock);
	bmk_platform_splx(0);
}
/*
 * Main thread routine for one xbdback instance. Woken up by
 * xbdback_evthandler when a domain has I/O work scheduled in a I/O ring.
 */
static void
xennet_thread(void *arg)
{
	struct xennetback_dev *dev = arg;

	/* give us a rump kernel context */
    	rumpuser__hyp.hyp_schedule();
    	rumpuser__hyp.hyp_lwproc_newlwp(0);
	rumpuser__hyp.hyp_unschedule();

	for (;;) {
		XENPRINTF(("%s\n", __func__));
		bmk_platform_splhigh();
		spin_lock(&dev->xennet_lock);
		XENPRINTF(("xbdback_thread: inside spinlock\n"));
		switch (dev->xennet_status) {
		case WAITING:
			spin_unlock(&dev->xennet_lock);
			bmk_platform_splx(0);
			bmk_sched_blockprepare();
			bmk_sched_block(&dev->xennet_thread_blk.header);
			XENPRINTF(("xbdback_thread: wait: unblocked\n"));

			__atomic_store_n(&dev->xennet_thread_blk.status, THREADBLK_STATUS_AWAKE,
				__ATOMIC_RELEASE);
			break;
		case RUN:
			XENPRINTF(("xbdback_thread: run: outside spinlock\n"));
			dev->xennet_status = WAITING; /* reset state */
			spin_unlock(&dev->xennet_lock);
			bmk_platform_splx(0);

			do {
			    	network_rx_buf_gc(dev);
				network_tx(dev);
	      		} while (__atomic_exchange_n(
				&dev->xennet_thread_blk.status, THREADBLK_STATUS_AWAKE,
                     		__ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
			break;
		case RUN_TX:
			XENPRINTF(("xbdback_thread: run: outside spinlock\n"));
			dev->xennet_status = WAITING; /* reset state */
			spin_unlock(&dev->xennet_lock);
			bmk_platform_splx(0);

			do {
				network_tx(dev);
			} while (__atomic_exchange_n(
				&dev->xennet_thread_blk.status, THREADBLK_STATUS_AWAKE,
				__ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
			break;
		case RUN_RX:
			XENPRINTF(("xbdback_thread: run: outside spinlock\n"));
			bmk_printf("Network rx buf gc\n");
			dev->xennet_status = WAITING; /* reset state */
			spin_unlock(&dev->xennet_lock);
			bmk_platform_splx(0);

			do {
			    	network_rx_buf_gc(dev);
	      		} while (__atomic_exchange_n(
				&dev->xennet_thread_blk.status, THREADBLK_STATUS_AWAKE,
                     		__ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
			break;

		case DISCONNECTING:
			XENPRINTF(("xbdback_thread: disconnecting\n"));
			break;
		default:
			bmk_printf("%s: invalid state %d",
					dev->nodename, dev->xennet_status);
			bmk_platform_halt(NULL);
		}
	}
}

static void free_netback(struct xennetback_dev *dev) {
    unsigned int i;

    XENPRINTF(("%s\n", __func__));

    for (i = 0; i < NET_TX_RING_SIZE; i++)
        down(&dev->rx_sem);

    minios_mask_evtchn(dev->evtchn);

    bmk_memfree(dev->mac, BMK_MEMWHO_WIREDBMK);
    bmk_memfree(dev->frontend, BMK_MEMWHO_WIREDBMK);

    minios_unbind_evtchn(dev->evtchn);

    for (i = 0; i < NET_RX_RING_SIZE; i++) {
        gnttab_end_access(dev->rx_buffers[i].gref);
        bmk_pgfree_one(dev->rx_buffers[i].page);
    }

    destroy_queue(dev->rx_list);
    gntmap_destroy_addr_list();
    gntmap_fini(&dev->tx_map);
    gntmap_fini(&dev->rx_map);

    bmk_memfree(dev, BMK_MEMWHO_WIREDBMK);
}

static struct xennetback_dev *
xennetback_init(char *_nodename,
             unsigned char rawmac[6], char **ip, void *priv, char *vifname) {
    xenbus_transaction_t xbt;
    char *err, *message = NULL;
    unsigned int i;
    int revtchn, tevtchn, rc, retry = 0, val;
    char path[256];
    uint32_t id;
    struct netif_tx_sring *txs = NULL;
    struct netif_rx_sring *rxs = NULL;
    struct xennetback_dev *dev;
    uint8_t split_channel = 0;

    XENPRINTF(("%s\n", __func__));

    dev = bmk_memcalloc(1, sizeof(*dev), BMK_MEMWHO_WIREDBMK);
    bmk_snprintf(path, sizeof(path), "domid");
    dev->back_id = xenbus_read_integer(path);

    dev->xennetback_priv = priv;

    if (!_nodename) {
        bmk_printf("No backend path found\n");
        bmk_memfree(dev, BMK_MEMWHO_WIREDBMK);
        return NULL;
    } else {
        bmk_printf("Intializing backend of path %s\n", _nodename);
        bmk_strncpy(dev->nodename, _nodename, sizeof(dev->nodename) - 1);
    }

    bmk_printf("net TX ring size %llu\n", NET_TX_RING_SIZE);
    bmk_printf("net RX ring size %llu\n", NET_RX_RING_SIZE);
    init_SEMAPHORE(&dev->rx_sem, NET_TX_RING_SIZE);

    dev->rx_list = create_queue(NET_RX_RING_SIZE);
    for (i = 0; i < NET_RX_RING_SIZE; i++) {
        /* TODO: that's a lot of memory */
        dev->rx_buffers[i].page = bmk_pgalloc_one();
    }

    dev->tx_list = create_queue(NET_TX_RING_SIZE);

    bmk_snprintf(path, sizeof(path), "%s/frontend-id", dev->nodename);
    dev->front_id = xenbus_read_integer(path);
    bmk_snprintf(path, sizeof(path), "%s/handle", dev->nodename);
    dev->handle = xenbus_read_integer(path);

    xenbus_event_queue_init(&dev->events);

again:
    err = xenbus_transaction_start(&xbt);
    if (err) {
        bmk_printf("starting transaction\n");
        bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
    }

    bmk_snprintf(vifname, sizeof(vifname), "xvif%d%d", dev->front_id,
                 dev->handle);
    err = xenbus_printf(xbt, dev->nodename, "vifname", "%s", vifname);
    if (err) {
        message = "writing vifname";
        goto abort_transaction;
    }

    err = xenbus_printf(xbt, dev->nodename, "feature-rx-copy", "%u", 1);
    if (err) {
        message = "writing feature-rx-copy";
        goto abort_transaction;
    }

    err = xenbus_printf(xbt, dev->nodename, "feature-rx-flip", "%u", 1);
    if (err) {
        message = "writing feature-rx-flip";
        goto abort_transaction;
    }

    err = xenbus_printf(xbt, dev->nodename, "feature-split-event-channels", "%u", 1);
    if (err) {
    	message = "writing feature-split-event-channels";
        goto abort_transaction;
    }

    err = xenbus_printf(xbt, dev->nodename, "feature-sg", "%u", 1);
    if (err) {
    	message = "writing feature-sg";
        goto abort_transaction;
    }

    bmk_snprintf(path, sizeof(path), "%s/state", dev->nodename);
    err = xenbus_switch_state(xbt, path, XenbusStateInitWait);
    if (err) {
        message = "switching state";
        goto abort_transaction;
    }

    err = xenbus_transaction_end(xbt, 0, &retry);
    if (err)
        bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
    if (retry) {
        goto again;
        bmk_printf("completing transaction\n");
    }

    goto done;

abort_transaction:
    bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
    err = xenbus_transaction_end(xbt, 1, &retry);
    bmk_printf("Abort transaction %s\n", message);
    goto error;

done:
    bmk_snprintf(path, sizeof(path), "%s/frontend", dev->nodename);
    message = xenbus_read(XBT_NIL, path, &dev->frontend);
    bmk_snprintf(path, sizeof(path), "%s/mac", dev->nodename);
    message = xenbus_read(XBT_NIL, path, &dev->mac);

    if ((dev->frontend == NULL) || (dev->mac == NULL)) {
        bmk_printf("%s: frontend/mac failed\n", __func__);
        goto error;
    }

    {
        XenbusState state;
        int len = bmk_strlen(dev->frontend) + 1 + 5 + 1;
        bmk_snprintf(path, sizeof(char) * len, "%s/state", dev->frontend);
        xenbus_watch_path_token(XBT_NIL, path, path, &dev->events);

        err = NULL;
        state = xenbus_read_integer(path);
        while (err == NULL && state < XenbusStateConnected)
            err = xenbus_wait_for_state_change(path, &state, &dev->events);
        if (state != XenbusStateConnected) {
            bmk_printf("frontend not avalable, state=%d\n", state);
            xenbus_unwatch_path_token(XBT_NIL, path, path);
            goto error;
        }

        if (ip) {
            bmk_snprintf(path, sizeof(path), "%s/ip", dev->frontend);
            xenbus_read(XBT_NIL, path, ip);
        }
    }

    if (rawmac) {
        char *p;

        for (p = dev->mac, i = 0; i < 6; i++) {
            unsigned long v;
            char *ep;

            v = bmk_strtoul(p, &ep, 16);
            if (v > 255 || (*ep && *ep != ':')) {
                bmk_printf("invalid mac string %s\n", dev->mac);
                bmk_platform_halt(NULL);
            }
            rawmac[i] = v;
            p = ep + 1;
        }
    }

    /* we can't use the same MAC addr as our guest */
    rawmac[3]++;

    bmk_printf("netback: node=%s frontend=%s\n", dev->nodename, dev->frontend);
    bmk_printf("netback: MAC %s\n", dev->mac);
    id = dev->front_id;

    bmk_snprintf(path, sizeof(path), "%s/tx-ring-ref", dev->frontend);
    val = xenbus_read_integer(path);
    if (val < 0) {
        bmk_printf("Reading tx-ring-ref failed\n");
        goto error;
    }
    dev->tx_ring_ref = val;

    bmk_snprintf(path, sizeof(path), "%s/rx-ring-ref", dev->frontend);
    val = xenbus_read_integer(path);
    if (val < 0) {
        bmk_printf("Reading rx-ring-ref failed\n");
        goto error;
    }
    dev->rx_ring_ref = val;

    bmk_snprintf(path, sizeof(path), "%s/event-channel", dev->frontend);
    val = xenbus_read_integer(path);
    if (val < 0) {
       bmk_printf("Using split event channel\n");
       bmk_snprintf(path, sizeof(path), "%s/event-channel-tx", dev->frontend);
       val = xenbus_read_integer(path);
       if (val < 0) {
               bmk_printf("Reading event-channel-tx failed\n");
               goto error;
       }
       tevtchn = val;

       bmk_snprintf(path, sizeof(path), "%s/event-channel-rx", dev->frontend);
       val = xenbus_read_integer(path);
       if (val < 0) {
               bmk_printf("Reading event-channel-rx failed\n");
               goto error;
       }
       revtchn = val;
       split_channel = 1;
    } else
    	revtchn = val;

    bmk_snprintf(path, sizeof(path), "%s/request-rx-copy", dev->frontend);
    val = xenbus_read_integer(path);
    if (val == 0) {
        bmk_printf("rx-copy is not supported by frontend\n");
        goto error;
    } else if (val < 0) {
        bmk_printf("Reading rx-copy failed\n");
        goto error;
    }

    if (gntmap_create_addr_list() == NULL)
        goto error;

    gntmap_init(&dev->map_entry);
    gntmap_init(&dev->tx_map);
    gntmap_init(&dev->rx_map);

    txs = gntmap_map_grant_refs(&dev->tx_map, 1, &id, 1, &dev->tx_ring_ref, 1);
    if (txs == NULL) {
        goto error2;
    }
    BACK_RING_INIT(&dev->tx, txs, PAGE_SIZE);

    rxs = gntmap_map_grant_refs(&dev->rx_map, 1, &id, 1, &dev->rx_ring_ref, 1);
    if (rxs == NULL) {
        goto error2;
    }
    BACK_RING_INIT(&dev->rx, rxs, PAGE_SIZE);

    dev->xennet_thread = bmk_sched_create(dev->nodename, NULL, 0, -1,
		    xennet_thread, dev, NULL, 0);
    if(dev->xennet_thread == NULL)
	    goto error2;
    dev->xennet_status = WAITING;

    spin_lock_init(&dev->xennet_lock);
    dev->xennet_thread_blk.header.callback = threadblk_callback;
    dev->xennet_thread_blk.status = THREADBLK_STATUS_AWAKE;

    if (split_channel == 0) {
    	rc = minios_evtchn_bind_interdomain(dev->front_id, revtchn, xennetback_handler,
                                        dev, &dev->evtchn);
    	if (rc)
        	goto error2;

	dev->revtchn = dev->tevtchn = dev->evtchn;
    	minios_unmask_evtchn(dev->evtchn);
    	minios_notify_remote_via_evtchn(dev->evtchn);
    } else {
       rc = minios_evtchn_bind_interdomain(dev->front_id, revtchn,
                       xennetback_rx_handler, dev, &dev->revtchn);
       if (rc)
               goto error2;

       rc = minios_evtchn_bind_interdomain(dev->front_id, tevtchn,
                       xennetback_tx_handler, dev, &dev->tevtchn);
       if (rc)
               goto error2;

       minios_unmask_evtchn(dev->revtchn);
       minios_unmask_evtchn(dev->tevtchn);
       minios_notify_remote_via_evtchn(dev->revtchn);
       minios_notify_remote_via_evtchn(dev->tevtchn);
    }

    bmk_snprintf(path, sizeof(path), "%s/state", dev->nodename);
    err = xenbus_switch_state(XBT_NIL, path, XenbusStateConnected);
    if (err) {
        bmk_printf("Switch State Failed\n");
        goto error2;
    }

    return dev;

error2:
    if (txs)
        gntmap_munmap(&dev->tx_map, (unsigned long)txs, 1);
    if (rxs)
        gntmap_munmap(&dev->rx_map, (unsigned long)rxs, 1);
    gntmap_fini(&dev->tx_map);
    gntmap_fini(&dev->rx_map);

error:
    bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
    bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
    free_netback(dev);
    return NULL;
}

static void xennetback_shutdown(struct xennetback_dev *dev) {
    char *err = NULL;
    XenbusState state;
    char path[256];
    char nodename[256];
    int len;

    XENPRINTF(("%s\n", __func__));

    bmk_printf("close network: backend at %s\n", dev->frontend);

    len = bmk_strlen(dev->frontend) + 1 + 5 + 1;
    bmk_snprintf(path, sizeof(char) * len, "%s/state", dev->frontend);

    len = bmk_strlen(dev->nodename) + 1 + 5 + 1;
    bmk_snprintf(nodename, sizeof(char) * len, "%s/state", dev->nodename);

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateClosing)) !=
        NULL) {
        bmk_printf("shutdown_netback: error changing state to %d: %s\n",
                   XenbusStateClosing, err);
        goto close;
    }

    state = xenbus_read_integer(path);
    while (err == NULL && state < XenbusStateClosing)
        err = xenbus_wait_for_state_change(path, &state, &dev->events);

    if (err)
        bmk_memfree(err, BMK_MEMWHO_WIREDBMK);

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateClosed)) !=
        NULL) {
        bmk_printf("shutdown_netback: error changing state to %d: %s\n",
                   XenbusStateClosed, err);
        goto close;
    }

    state = xenbus_read_integer(path);
    while (state < XenbusStateClosed) {
        err = xenbus_wait_for_state_change(path, &state, &dev->events);
        if (err)
            bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
    }

    if ((err = xenbus_switch_state(XBT_NIL, nodename,
                                   XenbusStateInitialising)) != NULL) {
        bmk_printf("shutdown_netback: error changing state to %d: %s\n",
                   XenbusStateInitialising, err);
        goto close;
    }

    err = NULL;
    state = xenbus_read_integer(path);
    while (err == NULL &&
           (state < XenbusStateInitWait || state >= XenbusStateClosed))
        err = xenbus_wait_for_state_change(path, &state, &dev->events);

close:
    if (err)
        bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
    xenbus_unwatch_path_token(XBT_NIL, path, path);

    bmk_snprintf(path, sizeof(path), "%s/vifname", nodename);
    xenbus_rm(XBT_NIL, path);

    if (!err)
        free_netback(dev);
}

static int xennetback_prepare_xmit(struct xennetback_dev *dev, unsigned char *data, unsigned int len,
                         int offset, unsigned int index) {
    int id;
    struct net_buffer *buf;

    XENPRINTF(("%s\n", __func__));

    bmk_assert (len <= PAGE_SIZE);

    down(&dev->rx_sem);

    bmk_assert (index < NET_TX_RING_SIZE);

    rx_gop[index].flags = GNTCOPY_dest_gref;
    rx_gop[index].source.offset = offset;
    rx_gop[index].source.domid = DOMID_SELF;
    rx_gop[index].source.u.gmfn = get_gfn(data);

    bmk_platform_splhigh();
    spin_lock(&xennetback_lock);
    id = get_from_list(dev->rx_list);
    spin_unlock(&xennetback_lock);
    bmk_platform_splx(0);
    if (id == -1) {
        return -1;
    }

    if ((unsigned int)id >= NET_RX_RING_SIZE)
        bmk_platform_halt("id >= NET_RX_RING_SIZE\n");

    rsp_id[index] = id;
    buf = &dev->rx_buffers[id];

    rx_gop[index].dest.u.ref = buf->gref;
    rx_gop[index].dest.offset = 0;
    rx_gop[index].len = len;
    rx_gop[index].dest.domid = dev->front_id;

    return 0;
}

static void xennetback_xmit(struct xennetback_dev *dev, int *csum_blank, int count) {
    netif_rx_response_t *rxresp;
    RING_IDX rsp_prod;
    int i, notify;

    XENPRINTF(("%s\n", __func__));

    rmb();
    rsp_prod = dev->rx.rsp_prod_pvt;

    if (HYPERVISOR_grant_table_op(GNTTABOP_copy, &rx_gop, count) != 0) {
        bmk_printf("GNTTABOP_copy failed\n");
    }

    for (i = 0; i < count; i++) {
        rxresp = RING_GET_RESPONSE(&dev->rx, rsp_prod);
        rsp_prod++;
        rxresp->id = rsp_id[i];
        rxresp->offset = 0;
        rxresp->status = rx_gop[i].len;

        if (csum_blank[i] != 0)
            rxresp->flags = NETRXF_csum_blank;
        else
            rxresp->flags = 0;

        if (rx_gop[i].status != GNTST_okay) {
            bmk_printf("GOP Status = %d, id = %d\n", rx_gop[i].status, rsp_id[i]);
            rxresp->status = NETIF_RSP_ERROR;
        }
    }

    if (dev->rx.rsp_prod_pvt + count != rsp_prod)
        bmk_platform_halt("Sum doesn't match\n");
    dev->rx.rsp_prod_pvt = rsp_prod;
    RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&dev->rx, notify);

    rmb();
    if (notify)
        minios_notify_remote_via_evtchn(dev->revtchn);

    bmk_platform_splhigh();
    spin_lock(&xennetback_lock);
    network_rx_buf_gc(dev); 
    spin_unlock(&xennetback_lock);
    bmk_platform_splx(0);
}

static int xennetback_rxring_full(struct xennetback_dev *dev) {
   XENPRINTF(("%s\n", __func__));

   return RING_HAS_UNCONSUMED_REQUESTS(&dev->rx);
}

static void *xennetback_get_private(struct xennetback_dev *dev) { return dev->xennetback_priv; }
