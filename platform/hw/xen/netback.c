/* Xen PV netback driver for Rumprun driver domain.
 * Copyright (c) 2019-2020 A K M Fazla Mehrab, Virginia Tech.
 * Based on netfront.c from Rumprun.
 *
 */
#include <mini-os/os.h>
#include <mini-os/xenbus.h>
#include <mini-os/events.h>
#include <xen/io/netif.h>
#include <xen/features.h>
#include <mini-os/gnttab.h>
#include <mini-os/time.h>
#include <mini-os/netback.h>
#include <mini-os/lib.h>
#include <mini-os/semaphore.h>

#include <bmk-core/memalloc.h>
#include <bmk-core/pgalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/string.h>
#include <bmk-core/platform.h>

DECLARE_WAIT_QUEUE_HEAD(netback_queue);

#define memset(a, b, c) bmk_memset(a, b, c)

#define NET_TX_RING_SIZE __CONST_RING_SIZE(netif_tx, PAGE_SIZE)
#define NET_RX_RING_SIZE __CONST_RING_SIZE(netif_rx, PAGE_SIZE)

#define GRANT_INVALID_REF 0
#define LB_SH 64

#define get_gfn(x) (PFN_DOWN((uint64_t)(x)))

struct net_buffer {
    void *page;
    grant_ref_t gref;
};

struct netback_dev {
    domid_t back_id;
    domid_t front_id;
    uint32_t handle;

    struct Queue *rx_list;
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

    void (*netif_rx)(struct netback_dev *, unsigned char *data, int len,
                     unsigned char csum);
    void *netback_priv;
    struct gntmap map_entry;
};

struct Queue {
    int front, rear, size;
    unsigned capacity;
    int *array;
};

gnttab_copy_t gop[LB_SH];
unsigned short rsp_id[LB_SH];
static spinlock_t netback_lock = SPIN_LOCK_UNLOCKED;

void init_rx_buffers(struct netback_dev *dev);
void network_tx(struct netback_dev *dev);
static void network_rx_buf_gc(struct netback_dev *dev);
static void netback_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data);

static inline struct Queue *create_queue(unsigned capacity) {
    struct Queue *queue = (struct Queue *)bmk_memcalloc(1, sizeof(struct Queue),
                                                        BMK_MEMWHO_WIREDBMK);
    queue->capacity = capacity;
    queue->front = queue->size = 0;
    queue->rear = capacity - 1;
    queue->array = (int *)bmk_memalloc(1, queue->capacity * sizeof(int),
                                       BMK_MEMWHO_WIREDBMK);

    return queue;
}

static inline int is_full(struct Queue *queue) {
    return (queue->size == queue->capacity);
}

static inline int is_empty(struct Queue *queue) { return (queue->size == 0); }

static inline void add_id_to_list(int item, struct Queue *queue) {
    if (is_full(queue))
        return;
    queue->rear = (queue->rear + 1) % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
}

static inline int get_id_from_list(struct Queue *queue) {
    if (is_empty(queue))
        return -1;
    int item = queue->array[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size = queue->size - 1;

    return item;
}

static inline void destroy_queue(struct Queue *queue) {
    bmk_memfree(queue, BMK_MEMWHO_WIREDBMK);
}

static void netback_tx_response(struct netback_dev *dev, int id, int status) {
    RING_IDX resp_prod;
    struct netif_tx_response *txresp;
    int do_event;

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

void network_tx(struct netback_dev *dev) {
    netif_tx_request_t txreqs[MAP_BATCH];
    RING_IDX req_cons;
    void *pages[MAP_BATCH];
    uint32_t grefs[MAP_BATCH];
    uint32_t id = (uint32_t)dev->front_id;
    int i = 0, j, receive_pending;
    uint8_t csum_blank;

    req_cons = dev->tx.req_cons;
    rmb();

    while (1) {
        rmb(); /* be sure to read the request before updating */
        dev->tx.req_cons = req_cons;
        wmb();
        RING_FINAL_CHECK_FOR_REQUESTS(&dev->tx, receive_pending);
        if (receive_pending == 0) {
            break;
        }

        RING_COPY_REQUEST(&dev->tx, req_cons, &txreqs[i]);
        rmb();
        req_cons++;

        grefs[i] = txreqs[i].gref;

        if (++i == MAP_BATCH) {
            if (gntmap_map_grant_refs_n(&dev->map_entry, i, &id, 0, grefs, 0,
                                        pages) != 0) {
                bmk_printf("page == NULL\n");
                for (j = 0; j < i; j++)
                    netback_tx_response(dev, txreqs[j].id, NETIF_RSP_DROPPED);
                return;
            }

            for (j = 0; j < i; j++) {
                if ((txreqs[j].flags & NETTXF_csum_blank) != 0)
                    csum_blank = 1;
                else
                    csum_blank = 0;

                dev->netif_rx(dev, (unsigned char *)pages[j] + txreqs[j].offset,
                              txreqs[j].size, csum_blank);
            }

            if (gntmap_munmap_n(&dev->map_entry, (unsigned long *)pages, i) !=
                0)
                bmk_printf("UNMAPED FAILED\n");

            for (j = 0; j < i; j++)
                netback_tx_response(dev, txreqs[j].id, NETIF_RSP_OKAY);
            i = 0;
        }
    }

    if (i != 0) {
        if (gntmap_map_grant_refs_n(&dev->map_entry, i, &id, 0, grefs, 0,
                                    pages) != 0) {
            bmk_printf("page == NULL\n");
            for (j = 0; j < i; j++)
                netback_tx_response(dev, txreqs[j].id, NETIF_RSP_DROPPED);
            return;
        }

        for (j = 0; j < i; j++) {
            if ((txreqs[j].flags & NETTXF_csum_blank) != 0)
                csum_blank = 1;
            else
                csum_blank = 0;

            dev->netif_rx(dev, (unsigned char *)pages[j] + txreqs[j].offset,
                          txreqs[j].size, csum_blank);
        }

        if (gntmap_munmap_n(&dev->map_entry, (unsigned long *)pages, i) != 0)
            bmk_printf("UNMAPED FAILED\n");

        for (j = 0; j < i; j++)
            netback_tx_response(dev, txreqs[j].id, NETIF_RSP_OKAY);
    }

    rmb(); /* be sure to read the request before updating pointer */
    dev->tx.req_cons = req_cons;
    wmb();
}

static void network_rx_buf_gc(struct netback_dev *dev) {
    RING_IDX cons, prod;
    unsigned short id;

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

            add_id_to_list(id, dev->rx_list);
            up(&dev->rx_sem);
        }

        dev->rx.req_cons = prod;
        mb();
    } while ((cons == prod) && (prod != dev->rx.sring->req_prod));
}

static void netback_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data) {
    struct netback_dev *dev = data;

    spin_lock(&netback_lock);
    network_rx_buf_gc(dev);
    network_tx(dev);
    spin_unlock(&netback_lock);
}

static void netback_rx_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data) {
    struct netback_dev *dev = data;

    bmk_printf("netback_rx_handler\n");
    spin_lock(&netback_lock);
    network_rx_buf_gc(dev);
    spin_unlock(&netback_lock);
}

static void netback_tx_handler(evtchn_port_t port, struct pt_regs *regs,
                            void *data) {
    struct netback_dev *dev = data;

    spin_lock(&netback_lock);
    network_tx(dev);
    spin_unlock(&netback_lock);
}

static void free_netback(struct netback_dev *dev) {
    int i;

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
    destroy_addr_queue();
    gntmap_fini(&dev->tx_map);
    gntmap_fini(&dev->rx_map);

    bmk_memfree(dev, BMK_MEMWHO_WIREDBMK);
}

struct netback_dev *
netback_init(char *_nodename,
             void (*thenetif_rx)(struct netback_dev *, unsigned char *data,
                                 int len, unsigned char csum_blank),
             unsigned char rawmac[6], char **ip, void *priv, char *vifname) {
    xenbus_transaction_t xbt;
    char *err, *message = NULL;
    int i, revtchn, tevtchn, rc, retry = 0;
    char path[256];
    uint64_t rx_copy;
    uint32_t id;
    struct netif_tx_sring *txs = NULL;
    struct netif_rx_sring *rxs = NULL;
    struct netback_dev *dev;
    uint8_t split_channel = 0;

    dev = bmk_memcalloc(1, sizeof(*dev), BMK_MEMWHO_WIREDBMK);
    bmk_snprintf(path, sizeof(path), "domid");
    dev->back_id = xenbus_read_integer(path);

    dev->netback_priv = priv;

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

    bmk_snprintf(path, sizeof(path), "%s/frontend-id", dev->nodename);
    dev->front_id = xenbus_read_integer(path);
    bmk_snprintf(path, sizeof(path), "%s/handle", dev->nodename);
    dev->handle = xenbus_read_integer(path);

    dev->netif_rx = thenetif_rx;

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
    dev->tx_ring_ref = xenbus_read_integer(path);
    if (dev->tx_ring_ref < 0) {
        bmk_printf("Reading tx-ring-ref failed\n");
        goto error;
    }

    bmk_snprintf(path, sizeof(path), "%s/rx-ring-ref", dev->frontend);
    dev->rx_ring_ref = xenbus_read_integer(path);
    if (dev->rx_ring_ref < 0) {
        bmk_printf("Reading rx-ring-ref failed\n");
        goto error;
    }

    bmk_snprintf(path, sizeof(path), "%s/event-channel", dev->frontend);
    revtchn = xenbus_read_integer(path);
    if (revtchn < 0) {
	bmk_printf("Using split event channel\n");
	bmk_snprintf(path, sizeof(path), "%s/event-channel-tx", dev->frontend);
	tevtchn = xenbus_read_integer(path);
	if (tevtchn < 0) {
		bmk_printf("Reading event-channel-tx failed\n");
		goto error;
	}

	bmk_snprintf(path, sizeof(path), "%s/event-channel-rx", dev->frontend);
	revtchn = xenbus_read_integer(path);
	if (revtchn < 0) {
		bmk_printf("Reading event-channel-rx failed\n");
		goto error;
	}

	split_channel = 1;
    }

    bmk_snprintf(path, sizeof(path), "%s/request-rx-copy", dev->frontend);
    rx_copy = xenbus_read_integer(path);
    if (rx_copy == 0) {
        bmk_printf("rx-copy is not supported by frontend\n");
        goto error;
    } else if (rx_copy < 0) {
        bmk_printf("Reading rx-copy failed\n");
        goto error;
    }

    if (create_addr_queue() == NULL)
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

    if (split_channel == 0) {
	    rc = minios_evtchn_bind_interdomain(dev->front_id, revtchn, netback_handler,
                                        dev, &dev->evtchn);
	    if (rc)
		    goto error2;

	    dev->revtchn = dev->tevtchn = dev->evtchn;
	    minios_unmask_evtchn(dev->evtchn);
	    minios_notify_remote_via_evtchn(dev->evtchn);
    } else {
	    rc = minios_evtchn_bind_interdomain(dev->front_id, revtchn,
			netback_rx_handler, dev, &dev->revtchn);
	    if (rc)
		    goto error2;

	    rc = minios_evtchn_bind_interdomain(dev->front_id, tevtchn,
			    netback_tx_handler, dev, &dev->tevtchn);
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

void netback_shutdown(struct netback_dev *dev) {
    char *err = NULL;
    XenbusState state;
    char path[256];
    char nodename[256];
    int len;

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

int netback_prepare_xmit(struct netback_dev *dev, unsigned char *data, int len,
                         int offset, unsigned int index) {
    int id;
    struct net_buffer *buf;

    if (len > PAGE_SIZE)
        bmk_platform_halt("len > PAGE_SIZE\n");

    down(&dev->rx_sem);

    if (index > LB_SH)
        bmk_platform_halt("len > PAGE_SIZE\n");

    gop[index].flags = GNTCOPY_dest_gref;
    gop[index].source.offset = offset;
    gop[index].source.domid = DOMID_SELF;
    gop[index].source.u.gmfn = get_gfn(data);

    bmk_platform_splhigh();
    spin_lock(&netback_lock);
    id = get_id_from_list(dev->rx_list);
    spin_unlock(&netback_lock);
    bmk_platform_splx(0);
    if (id == -1) {
        return -1;
    }

    if (id >= NET_RX_RING_SIZE)
        bmk_platform_halt("id >= NET_RX_RING_SIZE\n");

    rsp_id[index] = id;
    buf = &dev->rx_buffers[id];

    gop[index].dest.u.ref = buf->gref;
    gop[index].dest.offset = 0;
    gop[index].len = len;
    gop[index].dest.domid = dev->front_id;

    return 0;
}

void netback_xmit(struct netback_dev *dev, int *csum_blank, int count) {
    netif_rx_response_t *rxresp;
    RING_IDX rsp_prod;
    int i, notify;

    rmb();
    rsp_prod = dev->rx.rsp_prod_pvt;

    if (HYPERVISOR_grant_table_op(GNTTABOP_copy, &gop, count) != 0) {
        bmk_printf("GNTTABOP_copy failed\n");
    }

    for (i = 0; i < count; i++) {
        rxresp = RING_GET_RESPONSE(&dev->rx, rsp_prod);
        rsp_prod++;
        rxresp->id = rsp_id[i];
        rxresp->offset = 0;
        rxresp->status = gop[i].len;

        if (csum_blank[i] != 0)
            rxresp->flags = NETRXF_csum_blank;
        else
            rxresp->flags = 0;

        if (gop[i].status != GNTST_okay) {
            bmk_printf("GOP Status = %d, id = %d\n", gop[i].status, rsp_id[i]);
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
    spin_lock(&netback_lock);
    network_rx_buf_gc(dev);
    spin_unlock(&netback_lock);
    bmk_platform_splx(0);
}

int netback_rxring_full(struct netback_dev *dev) {
    return RING_HAS_UNCONSUMED_REQUESTS(&dev->rx);
}

void *netback_get_private(struct netback_dev *dev) { return dev->netback_priv; }
