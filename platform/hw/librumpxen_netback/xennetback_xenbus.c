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

#define XENNET_PAGE_MASK (PAGE_SIZE - 1)

#define	ETHER_MAX_LEN_JUMBO 9018 /* maximum jumbo frame len, including CRC */
#define	ETHER_ADDR_LEN	6	/* length of an Ethernet address */

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

struct xennetback_user {
    struct threadblk viu_rcvrblk;
    struct threadblk viu_softsblk;
    struct bmk_thread *viu_rcvrthr;
    struct bmk_thread *viu_softsthr;
    void *viu_ifp;
    struct xnetback_instance *viu_xneti;
    void **xni_xstate;
    void* xbusd_dmat;
    struct xennetback_sc *viu_vifsc;

    int viu_read;
    int viu_write;
    int viu_dying;
};

/* state of a xnetback instance */
typedef enum {
	WAITING,
	CONNECTED,
	RUN,
	DISCONNECTING,
	DISCONNECTED
} xnetback_state_t;

typedef unsigned long vaddr_t;
typedef unsigned long paddr_t;

struct xennetback_watch {
	SLIST_ENTRY(xennetback_watch) next;
	char *path;
	struct xnetback_instance *xneti;
	void (*cbfun)(char *path, struct xnetback_instance *xneti);
};
SLIST_HEAD(, xennetback_watch) xennetback_watches;

/* we keep the xnetback instances in a linked list */
struct xnetback_instance {
	SLIST_ENTRY(xnetback_instance) next;
	struct xenbus_device *xni_xbusd; /* our xenstore entry */
	domid_t xni_domid;		/* attached to this domain */
	uint32_t xni_handle;	/* domain-specific handle */
	xnetback_state_t xni_status;
	xnetback_state_t xni_evt_status;

	/* network interface stuff */
	//struct ethercom xni_ec;
	//struct callout xni_restart;
	uint8_t xni_enaddr[ETHER_ADDR_LEN];
	char xni_name[16];

	/* remote domain communication stuff */
	unsigned int xni_evtchn; /* our event channel */
	//struct intrhand *xni_ih;
	netif_tx_back_ring_t xni_txring;
	netif_rx_back_ring_t xni_rxring;
	grant_handle_t xni_tx_ring_handle; /* to unmap the ring */
	grant_handle_t xni_rx_ring_handle;
	void* xni_tx_ring_va; /* to unmap the ring */
	void* xni_rx_ring_va;

	struct gntmap xni_map_entry;
	struct gntmap xni_tx_ring_map;
	struct gntmap xni_rx_ring_map;

	/* arrays used in xennetback_ifstart(), used for both Rx and Tx */
	gnttab_copy_t     	xni_gop_copy[NB_XMIT_PAGES_BATCH];
	//struct xnetback_xstate	xni_xstate[NB_XMIT_PAGES_BATCH];
	//struct netif_tx_request xni_tx[NB_XMIT_PAGES_BATCH];
	struct netif_tx_request xni_tx[NET_TX_RING_SIZE];

	/* event counters */
	//struct evcnt xni_cnt_rx_cksum_blank;
	//struct evcnt xni_cnt_rx_cksum_undefer;
        void *xni_priv;
	spinlock_t xni_lock;
	struct xennetback_watch *xbdw_front;
	struct xennetback_watch *xbdw_back;

	struct bmk_thread *xni_thread;
	struct threadblk xni_thread_blk;
};

static SLIST_HEAD(, xnetback_instance) xnetback_instances;

static struct bmk_thread *xennetback_watch_thread;
static struct xenbus_event_queue watch_queue;

struct xennetback_watch *xbdw_vbd;

static void threadblk_callback(struct bmk_thread *prev, struct bmk_block_data *_data);
static void xennetback_sched_wake(struct threadblk *tblk, struct bmk_thread *thread);
static int xennetback_init_watches(void); 
static void xbdw_thread_func(void *ign);
static void xennetback_instance_search(char *backend_root, struct xnetback_instance *xneti);
static int xennetback_probe_device(const char *vbdpath);
static int xennetback_xenbus_create(char *xbusd_path);
static struct xnetback_instance* xennetback_lookup(domid_t dom , uint32_t handle);
static void* xennetback_get_private(struct xnetback_instance *dev);
static inline void xennetback_tx_response(struct xnetback_instance *xneti, int id, int status);
static const char* xennetback_tx_check_packet(const netif_tx_request_t *txreq);
static int xennetback_copy(gnttab_copy_t *gop, int copycnt, const char *dir);
static int xennetback_tx_m0len_fragment(struct xnetback_instance *xneti, int m0_len, int req_cons, int *cntp);
static void xennetback_network_tx(struct xnetback_instance *xneti);
static void xennetback_frontend_changed(char *path, struct xnetback_instance* xneti);
static void xennetback_evthandler(evtchn_port_t port, struct pt_regs *regs, void *data);
static void xennetback_thread(void *arg);
static void xbdback_wakeup_thread(struct xnetback_instance *xneti);
static int xennetback_xenbus_destroy(void *arg);
static void xennetback_disconnect(struct xnetback_instance *xneti);
static void soft_start_thread_func(void *_viu); 

static void threadblk_callback(struct bmk_thread *prev,
        struct bmk_block_data *_data)
{
	struct threadblk *data = (struct threadblk *)_data;

	XENPRINTF(("%s\n", __func__));

         /* THREADBLK_STATUS_AWAKE -> THREADBLK_STATUS_SLEEP */
	 /* THREADBLK_STATUS_NOTIFY -> THREADBLK_STATUS_AWAKE */
	 if (__atomic_fetch_sub(&data->status, 1, __ATOMIC_ACQ_REL) !=
	     THREADBLK_STATUS_AWAKE) {
	     /* the current state is THREADBLK_STATUS_AWAKE */
	     bmk_sched_wake(prev);
	 }
}

static void xennetback_sched_wake(struct threadblk *tblk, struct bmk_thread *thread)
{
	if (__atomic_exchange_n(&tblk->status, THREADBLK_STATUS_NOTIFY,
				__ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
		bmk_sched_wake(thread);
	}
}

void VIFHYPER_ENTRY(void)
{
	int nlocks;

	XENPRINTF(("%s\n", __func__));

	rumpkern_unsched(&nlocks, NULL);
	xennetback_init_watches();
	rumpkern_sched(nlocks, NULL);
}

static int xennetback_init_watches(void) {
	char path[64];
	int dom;

	XENPRINTF(("%s\n", __func__));
	
	SLIST_INIT(&xennetback_watches);

    	bmk_snprintf(path, sizeof(path), "domid");
    	dom = xenbus_read_integer(path);

    	if (dom < 0) {
        	bmk_printf("Couldn't fetch backend domid\n");
        	return BMK_EINVAL;
    	} else
        	bmk_printf("Backend domid is %d\n", dom);

	xbdw_vbd = bmk_memcalloc(1, sizeof(xbdw_vbd), BMK_MEMWHO_WIREDBMK);
	xbdw_vbd->path = bmk_memcalloc(1, sizeof(path), BMK_MEMWHO_WIREDBMK);
    	bmk_snprintf(xbdw_vbd->path, sizeof(path), "/local/domain/%d/backend", dom);
	xbdw_vbd->cbfun = xennetback_instance_search;
	xbdw_vbd->xneti = NULL;
	SLIST_INSERT_HEAD(&xennetback_watches, xbdw_vbd, next);

    	xenbus_event_queue_init(&watch_queue);
    	bmk_snprintf(path, sizeof(path), "/local/domain");
	xenbus_watch_path_token(XBT_NIL, path, path, &watch_queue);
 	xennetback_watch_thread = bmk_sched_create("xennetback_watch", NULL, 0, -1,
		                                       xbdw_thread_func, NULL, NULL, 0);

	return 0;
}

static void xbdw_thread_func(void *ign) 
{
	char **ret;
	struct xennetback_watch *xbdw;

	XENPRINTF(("%s\n", __func__));
		
	/* give us a rump kernel context */
    	rumpuser__hyp.hyp_schedule();
    	rumpuser__hyp.hyp_lwproc_newlwp(0);
    	rumpuser__hyp.hyp_unschedule();

	for(;;) {
		ret = xenbus_wait_for_watch_return(&watch_queue);
		if(ret == NULL)
			continue;

		SLIST_FOREACH(xbdw, &xennetback_watches, next) {
			if (bmk_strcmp(xbdw->path, *ret) == 0) {
				XENPRINTF(("Event match for path %s\n", *ret));
				xbdw->cbfun(*ret, xbdw->xneti);
				continue;
			}
		}
	}
}

static void
xennetback_instance_search(char *backend_root, struct xnetback_instance *xneti){
	char *msg;
    	char **dirt, **dirid;
    	unsigned int type, id;
    	char path[30];
    	char vbd_found = 0;
    	int err;

	XENPRINTF(("%s\n", __func__));

        bmk_printf("Checking for backend changes\n");
        msg = xenbus_ls(XBT_NIL, "backend", &dirt);
        if (msg) {
            bmk_printf("No backend found: %s\n", msg);
            bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
            return;
        }

        for (type = 0; dirt[type]; type++) {
            if (bmk_strcmp(dirt[type], "vif") == 0) {
                vbd_found = 1;
                break;
            }
        }

        if (vbd_found == 0)
            return;

        msg = xenbus_ls(XBT_NIL, "backend/vif", &dirid);
        if (msg) {
            bmk_printf("Error in xenbus ls: %s\n", msg);
            bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);

            return;
        }

        for (id = 0; dirid[id]; id++) {
            bmk_snprintf(path, sizeof(path), "backend/vif/%s", dirid[id]);
            err = xennetback_probe_device(path);
            if (err)
                break;
            bmk_memfree(dirid[id], BMK_MEMWHO_WIREDBMK);
        }
        bmk_memfree(dirid, BMK_MEMWHO_WIREDBMK);
/*
        for (type = 0; dirt[type]; type++) {
            bmk_memfree(dirt[type], BMK_MEMWHO_WIREDBMK);
        }
        bmk_memfree(dirt, BMK_MEMWHO_WIREDBMK);
*/
}

static int 
xennetback_probe_device(const char *vbdpath) {
    int err, i, pos, msize;
    unsigned long state;
    char **dir;
    char *msg;
    char *devpath, path[40];

    XENPRINTF(("%s\n", __func__));

    msg = xenbus_ls(XBT_NIL, vbdpath, &dir);
    if (msg) {
        bmk_printf("Error in xenbus ls: %s\n", msg);
        bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
        return 1;
    }

    for (pos = 0; dir[pos]; pos++) {
        i = pos;
        msize = bmk_strlen(vbdpath) + bmk_strlen(dir[i]) + 2;
        devpath = bmk_memalloc(msize, 0, BMK_MEMWHO_WIREDBMK);
        if (devpath == NULL) {
            bmk_printf("%s: can't malloc devpath", __func__);
            return 1;
        }

        bmk_snprintf(devpath, msize, "%s/%s", vbdpath, dir[i]);
        bmk_snprintf(path, sizeof(path), "%s/state", devpath);
        state = xenbus_read_integer(path);
        if (state != XenbusStateInitialising) {
            /* device is not new */
	    bmk_printf("%s state is %lu\n", devpath, state);
            bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
            continue;
        }

	err = xennetback_xenbus_create(devpath);
        if (err) {
            bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
            return err;
        }

        bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
    }

    return 0;
}

static int
xennetback_xenbus_create(char *xbusd_path)
{
	struct xennetback_user *viu = NULL;
	struct xnetback_instance *xneti;
	struct xenbus_device *xbusd;
	long domid, handle;
	char *e, *p;
	char *mac;
	int i, retry = 0;
	char *message;
	char path[64];
	xenbus_transaction_t xbt;
	long len;

	xbusd = (struct xenbus_device*)bmk_memcalloc(1, sizeof(xbusd),
			BMK_MEMWHO_WIREDBMK);
	if(xbusd == NULL) {
		bmk_printf("%s: cannot allocate xbusd for device at %s\n",
				__func__, path);
		return -1;
	}

	xbusd->xbusd_path = xbusd_path;

	len = bmk_strlen(xbusd_path) + 1;
	bmk_printf("PATH = %s\n", xbusd_path);
	xbusd->xbusd_path = bmk_memcalloc(1, sizeof(char)*len,
			BMK_MEMWHO_WIREDBMK);
	if(xbusd->xbusd_path == NULL) {
		bmk_printf("%s: cannot allocate xbusd_path for device at %s\n",
				__func__, path);
		bmk_memfree(xbusd, BMK_MEMWHO_WIREDBMK);
		return -1;
	}
	bmk_memset(xbusd->xbusd_path, 0, sizeof(char)*len);
	bmk_snprintf(xbusd->xbusd_path, len, "%s", xbusd_path);

	XENPRINTF(("%s: path %s\n", __func__, xbusd->xbusd_path));

	bmk_snprintf(path, sizeof(path), "%s/frontend-id", xbusd->xbusd_path);
	domid = xenbus_read_integer(path);

	if (domid < 0) {
		bmk_printf("xvif: can't read %s/frontend-id: %ld\n",
		    xbusd->xbusd_path, domid);
		return domid;
	}

	bmk_snprintf(path, sizeof(path), "%s/handle", xbusd->xbusd_path);
	handle = xenbus_read_integer(path);

	if (handle < 0) {
		bmk_printf("xvif: can't read %s/handle: %ld\n",
		    xbusd->xbusd_path, handle);
		return handle;
	}

	xneti = bmk_memcalloc(1, sizeof(*xneti), BMK_MEMWHO_WIREDBMK);
	if(xneti == NULL) {
		bmk_printf("%s: cannot allocate xneti for device at %s\n",
				__func__, path);
		goto fail1;
	}

	xneti->xni_domid = domid;
	xneti->xni_handle = handle;
	xneti->xni_status = DISCONNECTED;

	/* Need to keep the lock for lookup and the list update */
	if (xennetback_lookup(domid, handle) != NULL) {
		bmk_printf("xennetback: backend exists\n");
		return BMK_EINVAL;
	}

	spin_lock_init(&xneti->xni_lock);
	xneti->xni_thread_blk.header.callback = threadblk_callback;
	xneti->xni_thread_blk.status = THREADBLK_STATUS_AWAKE;
	SLIST_INSERT_HEAD(&xnetback_instances, xneti, next);

	xbusd->xbusd_u.b.b_cookie = xneti;
	xbusd->xbusd_u.b.b_detach = xennetback_xenbus_destroy;

	bmk_snprintf(path, sizeof(path), "%s/frontend", xbusd->xbusd_path);
	message = xenbus_read(XBT_NIL, path, &xbusd->xbusd_otherend);
	if(message)
		bmk_platform_halt("Cannot read frontend path\n");

	XENPRINTF(("%s: other end path %s\n", __func__, xbusd->xbusd_otherend));

	xneti->xbdw_front = bmk_memcalloc(1, sizeof(xneti->xbdw_front),
			BMK_MEMWHO_WIREDBMK);
	if (xneti->xbdw_front == NULL)
		goto fail;

	xneti->xbdw_front->path = bmk_memcalloc(1, sizeof(path),
			BMK_MEMWHO_WIREDBMK);
	if (xneti->xbdw_front->path == NULL)
		goto fail2;

	bmk_snprintf(xneti->xbdw_front->path, sizeof(path), "%s/state",
			xbusd->xbusd_otherend);
	xneti->xbdw_front->cbfun = xennetback_frontend_changed;
	xneti->xbdw_front->xneti = xneti;
	SLIST_INSERT_HEAD(&xennetback_watches, xneti->xbdw_front, next);

	xneti->xni_xbusd = xbusd;

	bmk_snprintf(xneti->xni_name, sizeof(xneti->xni_name), "xvif%di%d",
	    (int)domid, (int)handle);

	XENPRINTF(("xennet_name = %s\n", xneti->xni_name));

	/* read mac address */
	bmk_snprintf(path, sizeof(path), "%s/mac", xbusd->xbusd_path);
	message = xenbus_read(XBT_NIL, path, &mac);

	if (message) {
		bmk_printf("can't read %s/mac\n", xbusd->xbusd_path);
		goto fail;
	}

	for (i = 0, p = mac; i < ETHER_ADDR_LEN; i++) {
		xneti->xni_enaddr[i] = bmk_strtoul(p, &e, 16);
		if ((e[0] == '\0' && i != 5) && e[0] != ':') {
			bmk_printf("%s is not a valid mac address\n", mac);
			//err = BMK_EINVAL;
			goto fail;
		}
		p = &e[1];
	}

	/* we can't use the same MAC addr as our guest */
	xneti->xni_enaddr[3]++;

	viu = bmk_memalloc(sizeof(*viu), 0, BMK_MEMWHO_RUMPKERN);
	if (viu == NULL)
		goto fail;

	bmk_memset(viu, 0, sizeof(*viu));
	viu->viu_rcvrblk.header.callback = threadblk_callback;
	viu->viu_rcvrblk.status = THREADBLK_STATUS_AWAKE;
	viu->viu_softsblk.header.callback = threadblk_callback;
	viu->viu_softsblk.status = THREADBLK_STATUS_AWAKE;

	viu->viu_xneti = xneti;

	rumpuser__hyp.hyp_schedule();
	viu->viu_vifsc = rump_xennetback_create(viu, xneti->xni_name,
			xneti->xni_enaddr);
	rumpuser__hyp.hyp_unschedule();

	xneti->xni_priv = viu;

	do {
		message = xenbus_transaction_start(&xbt);
		if (message) {
			bmk_printf("%s: can't start transaction\n",
			    xbusd->xbusd_path);
			bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
			goto fail;
		}

		message = xenbus_printf(xbt, xbusd->xbusd_path,
		    "vifname", "%s", xneti->xni_name);
		if (message) {
			bmk_printf("failed to write %s/vifname\n",
			    xbusd->xbusd_path);
			bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
			goto abort_xbt;
		}

		message = xenbus_printf(xbt, xbusd->xbusd_path,
		    "feature-rx-copy", "%d", 1);
		if (message) {
			bmk_printf("failed to write %s/feature-rx-copy\n",
			    xbusd->xbusd_path);
			bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
			goto abort_xbt;
		}

		message = xenbus_printf(xbt, xbusd->xbusd_path,
		    "feature-ipv6-csum-offload", "%d", 1);
		if (message) {
			bmk_printf("failed to write %s/feature-ipv6-csum-offload\n",
			    xbusd->xbusd_path);
			bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
			goto abort_xbt;
		}

		message = xenbus_printf(xbt, xbusd->xbusd_path,
		    "feature-sg", "%d", 1);
		if (message) {
			bmk_printf("failed to write %s/feature-sg\n",
			    xbusd->xbusd_path);
			bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
			goto abort_xbt;
		}

		message = xenbus_transaction_end(xbt, 0, &retry);
	} while (retry);
	if (message) {
		bmk_printf("%s: can't end transaction\n",
		    xbusd->xbusd_path);
		bmk_memfree(message, BMK_MEMWHO_WIREDBMK);
	}

	bmk_snprintf(path, sizeof(path), "%s/state", xbusd->xbusd_path);
	message = xenbus_switch_state(XBT_NIL, path, XenbusStateInitWait);
	if (message) {
		bmk_printf("failed to switch state on %s\n",
		    xbusd->xbusd_path);
		goto fail;
	}

    	viu->viu_softsthr = bmk_sched_create("soft_start", NULL, 0, -1,
			soft_start_thread_func, viu, NULL, 0);


	return 0;

abort_xbt:
	xenbus_transaction_end(xbt, 1, &retry);
fail2:
	bmk_memfree(xneti->xbdw_front, BMK_MEMWHO_WIREDBMK);
fail:
        bmk_memfree(xneti, BMK_MEMWHO_WIREDBMK);
fail1:
        bmk_memfree(xbusd, BMK_MEMWHO_WIREDBMK);
	return -1;
}

/*
 * Signal a xbdback thread to disconnect. Done in 'xenwatch' thread context.
 */
static void
xennetback_disconnect(struct xnetback_instance *xneti)
{
	XENPRINTF(("%s\n", __func__));

	spin_lock(&xneti->xni_lock);
	XENPRINTF(("xennetback_disconnect: inside spinlock"));
	if (xneti->xni_evt_status == DISCONNECTED) {
		spin_unlock(&xneti->xni_lock);
		return;
	}
	minios_unbind_evtchn(xneti->xni_evtchn);

	/* signal thread that we want to disconnect, then wait for it */
	xneti->xni_evt_status = DISCONNECTING;
	xennetback_sched_wake(&xneti->xni_thread_blk, xneti->xni_thread);

	while (xneti->xni_evt_status != DISCONNECTED) {
		bmk_sched_blockprepare();
		bmk_sched_block(&xneti->xni_thread_blk.header);
	}

	XENPRINTF(("xbdback_disconnect: outside spinlock"));
	spin_unlock(&xneti->xni_lock);

	xenbus_switch_state(XBT_NIL, xneti->xni_xbusd->xbusd_path,
			XenbusStateClosing);
}

int
xennetback_xenbus_destroy(void *arg)
{
	struct xnetback_instance *xneti = arg;
    	struct xennetback_user *viu = xennetback_get_private(xneti);

	XENPRINTF(("%s\n", __func__));

	/* give us a rump kernel context */
	rumpuser__hyp.hyp_schedule();
	rumpuser__hyp.hyp_lwproc_newlwp(0);
	rumpuser__hyp.hyp_unschedule();

	XENPRINTF(("%s: status %d\n", __func__, xneti->xni_status));

	bmk_printf("%s disconnecting\n", xneti->xni_name);

	xennetback_disconnect(xneti);

	/* unregister watch */
	if (xneti->xbdw_front) {
		SLIST_REMOVE(&xennetback_watches, xneti->xbdw_front, xennetback_watch, next);
		bmk_memfree(xneti->xbdw_front->path, BMK_MEMWHO_WIREDBMK);
		bmk_memfree(xneti->xbdw_front, BMK_MEMWHO_WIREDBMK);
		xneti->xbdw_front = NULL;
	}

	gntmap_destroy_addr_list();

	/* unmap ring */
	gntmap_fini(&xneti->xni_tx_ring_map);
	gntmap_fini(&xneti->xni_rx_ring_map);

	/* close device */
    	rumpuser__hyp.hyp_schedule();
	rump_xennetback_destroy(viu->viu_vifsc);
    	rumpuser__hyp.hyp_unschedule();

	spin_lock(&xneti->xni_lock);
	SLIST_REMOVE(&xnetback_instances, xneti, xnetback_instance, next);
	spin_unlock(&xneti->xni_lock);

	bmk_memfree(xneti->xni_xbusd->xbusd_path, BMK_MEMWHO_WIREDBMK);
	bmk_memfree(xneti->xni_xbusd, BMK_MEMWHO_WIREDBMK);
	bmk_memfree(xneti, BMK_MEMWHO_WIREDBMK);

	return 0;
}

static int
xennetback_connect(struct xnetback_instance *xneti)
{
	int err;
	netif_tx_sring_t *tx_ring;
	netif_rx_sring_t *rx_ring;
	int tx_ring_ref, rx_ring_ref;
	long revtchn, rx_copy;
	char path[64];
	struct xenbus_device *xbusd = xneti->xni_xbusd;
	struct xennetback_user *viu = xennetback_get_private(xneti);

	bmk_assert(xbusd != NULL);
	XENPRINTF(("%s: %s\n", __func__, xbusd->xbusd_path));

	/* read communication information */
	bmk_snprintf(path, sizeof(path), "%s/tx-ring-ref", xbusd->xbusd_otherend);
	tx_ring_ref = xenbus_read_integer(path);
	if (tx_ring_ref < 0) {
		bmk_printf("Failed reading %s/tx-ring-ref", xbusd->xbusd_otherend);
		return -1;
	}

	bmk_snprintf(path, sizeof(path), "%s/rx-ring-ref", xbusd->xbusd_otherend);
	rx_ring_ref = xenbus_read_integer(path);
	if (rx_ring_ref < 0) {
		bmk_printf("Failed reading %s/rx-ring-ref", xbusd->xbusd_otherend);
		return -1;
	}

	bmk_snprintf(path, sizeof(path), "%s/event-channel", xbusd->xbusd_otherend);
	revtchn = xenbus_read_integer(path);
	if (revtchn < 0) {
		bmk_printf("Failed reading %s/event-channel", xbusd->xbusd_otherend);
		return -1;
	}

	bmk_snprintf(path, sizeof(path), "%s/request-rx-copy", xbusd->xbusd_otherend);
	rx_copy = xenbus_read_integer(path);
	if (rx_copy < 0) {
		bmk_printf("Failed reading %s/request-rx-copy", xbusd->xbusd_otherend);
		return -1;
	}

	if (gntmap_create_addr_list() == NULL)
		return -1;

	gntmap_init(&xneti->xni_map_entry);
	gntmap_init(&xneti->xni_tx_ring_map);
	gntmap_init(&xneti->xni_rx_ring_map);

	xneti->xni_tx_ring_va = gntmap_map_grant_refs(&xneti->xni_tx_ring_map,
			1,
			(uint32_t*)&xneti->xni_domid,
			1,
			&tx_ring_ref,
			1);

    	if (xneti->xni_tx_ring_va == NULL) {
        	goto err1;
    	}
	tx_ring = xneti->xni_tx_ring_va;
	BACK_RING_INIT(&xneti->xni_txring, tx_ring, PAGE_SIZE);

	xneti->xni_rx_ring_va = gntmap_map_grant_refs(&xneti->xni_rx_ring_map,
			1,
			(uint32_t*)&xneti->xni_domid,
			1,
			&rx_ring_ref,
			1);

    	if (xneti->xni_rx_ring_va == NULL) {
        	goto err1;
    	}
	rx_ring = xneti->xni_rx_ring_va;
	BACK_RING_INIT(&xneti->xni_rxring, rx_ring, PAGE_SIZE);

	xneti->xni_evt_status = WAITING;
	xneti->xni_thread = bmk_sched_create(xneti->xni_name, NULL, 0, -1,
			xennetback_thread, xneti, NULL, 0);
	if(xneti->xni_thread == NULL)
		goto err2;

	err = minios_evtchn_bind_interdomain(xneti->xni_domid, revtchn,
			xennetback_evthandler, xneti, &xneti->xni_evtchn);
	if (err) {
		bmk_printf("Can't get event channel: %d\n", err);
		goto err2;
	}
	wmb();
	xneti->xni_status = CONNECTED;
	wmb();

    	rumpuser__hyp.hyp_schedule();
	rump_xennetback_ifinit(viu->viu_vifsc);
    	rumpuser__hyp.hyp_unschedule();

	/* enable the xennetback event handler machinery */
	minios_unmask_evtchn(xneti->xni_evtchn);
	minios_notify_remote_via_evtchn(xneti->xni_evtchn);

	return 0;

err2:
	gntmap_destroy_addr_list();
	/* unmap rings */
	if(xneti->xni_tx_ring_va)
		gntmap_munmap(&xneti->xni_tx_ring_map,
				(unsigned long)xneti->xni_tx_ring_va, 1);
	gntmap_fini(&xneti->xni_tx_ring_map);

	if(xneti->xni_rx_ring_va)
		gntmap_munmap(&xneti->xni_rx_ring_map,
				(unsigned long)xneti->xni_rx_ring_va, 1);
	gntmap_fini(&xneti->xni_rx_ring_map);
err1:
	return -1;
}

static void
xennetback_frontend_changed(char *path, struct xnetback_instance* xneti)
{
	struct xenbus_device *xbusd = xneti->xni_xbusd;
	char state_path[64];

	XENPRINTF(("%s\n", __func__));

	int new_state = xenbus_read_integer(path);
	XENPRINTF(("%s: new state %d\n", xneti->xni_name, new_state));

	bmk_snprintf(state_path, sizeof(state_path), "%s/state",
			xbusd->xbusd_path);

	switch(new_state) {
	case XenbusStateInitialising:
	case XenbusStateInitialised:
		break;

	case XenbusStateConnected:
		if (xneti->xni_status == CONNECTED)
			break;
		if (xennetback_connect(xneti) == 0)
			xenbus_switch_state(XBT_NIL, state_path,
					XenbusStateConnected);
		break;

	case XenbusStateClosing:
		xneti->xni_status = DISCONNECTING;
		//xneti->xni_if.if_flags &= ~IFF_RUNNING;
		//xneti->xni_if.if_timer = 0;
		xenbus_switch_state(XBT_NIL, state_path, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		/* otherend_changed() should handle it for us */
		bmk_printf("xennetback_frontend_changed: closed\n");
		xennetback_xenbus_destroy(xneti);
		break;
	case XenbusStateUnknown:
	case XenbusStateInitWait:
	default:
		bmk_printf("Invalid frontend state %d\n", new_state);
		break;
	}
	return;
}

static struct xnetback_instance *
xennetback_lookup(domid_t dom , uint32_t handle)
{
	struct xnetback_instance *xneti;

	SLIST_FOREACH(xneti, &xnetback_instances, next) {
		if (xneti->xni_domid == dom && xneti->xni_handle == handle)
			return xneti;
	}
	return NULL;
}

static void 
xennetback_evthandler(evtchn_port_t port, struct pt_regs *regs,
		                            void *data)
{
	struct xnetback_instance *xneti = data;

	xbdback_wakeup_thread(xneti);

	return;
}

/*
 * Wake up the per xbdback instance thread.
 */
static void
xbdback_wakeup_thread(struct xnetback_instance *xneti)
{

	bmk_platform_splhigh();
	spin_lock(&xneti->xni_lock);
	/* only set RUN state when we are WAITING for work */
	if (xneti->xni_evt_status == WAITING)
	       xneti->xni_evt_status = RUN;
	xennetback_sched_wake(&xneti->xni_thread_blk, xneti->xni_thread);
	spin_unlock(&xneti->xni_lock);
	bmk_platform_splx(0);
}

/*
 * Main thread routine for one xbdback instance. Woken up by
 * xbdback_evthandler when a domain has I/O work scheduled in a I/O ring.
 */
static void
xennetback_thread(void *arg)
{
	struct xnetback_instance *xneti = arg;

	/* give us a rump kernel context */
    	rumpuser__hyp.hyp_schedule();
    	rumpuser__hyp.hyp_lwproc_newlwp(0);
	rumpuser__hyp.hyp_unschedule();

	for (;;) {
		XENPRINTF(("%s\n", __func__));
		bmk_platform_splhigh();
		spin_lock(&xneti->xni_lock);
		XENPRINTF(("xennetback_thread: inside spinlock\n"));
		switch (xneti->xni_evt_status) {
		case WAITING:
			spin_unlock(&xneti->xni_lock);
			bmk_platform_splx(0);
			bmk_sched_blockprepare();
			bmk_sched_block(&xneti->xni_thread_blk.header);
			XENPRINTF(("xennetback_thread: wait: unblocked\n"));

			__atomic_store_n(&xneti->xni_thread_blk.status, THREADBLK_STATUS_AWAKE,
				__ATOMIC_RELEASE);
			break;
		case RUN:
			XENPRINTF(("xennetback_thread: run: outside spinlock\n"));
			xneti->xni_evt_status = WAITING; /* reset state */
			spin_unlock(&xneti->xni_lock);
			bmk_platform_splx(0);

			do {
				xennetback_network_tx(xneti);

	      		} while (__atomic_exchange_n(
				&xneti->xni_thread_blk.status, THREADBLK_STATUS_AWAKE,
                     		__ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
			break;
		case DISCONNECTING:
			XENPRINTF(("xennetdback_thread: disconnecting\n"));
			/* there are pending I/Os. Wait for them. */
			bmk_sched_blockprepare();
			bmk_sched_block(&xneti->xni_thread_blk.header);
			__atomic_store_n(&xneti->xni_thread_blk.status, THREADBLK_STATUS_AWAKE,
					__ATOMIC_RELEASE);

			XENPRINTF(("xennetback_thread: disconnect outside spinlock\n"));
			spin_unlock(&xneti->xni_lock);
			break;
			
			spin_unlock(&xneti->xni_lock);
			bmk_sched_exit();
			return;
		default:
			bmk_printf("%s: invalid state %d",
			    xneti->xni_name, xneti->xni_evt_status);
			bmk_platform_halt(NULL);
		}
	}
}

static void *
xennetback_get_private(struct xnetback_instance *xneti)
{ 
	return xneti->xni_priv;
}

static inline void
xennetback_tx_response(struct xnetback_instance *xneti, int id, int status)
{
	RING_IDX resp_prod;
	netif_tx_response_t *txresp;
	int do_event;

	XENPRINTF(("%s\n", __func__));

	resp_prod = xneti->xni_txring.rsp_prod_pvt;
	txresp = RING_GET_RESPONSE(&xneti->xni_txring, resp_prod);

	txresp->id = id;
	txresp->status = status;
	xneti->xni_txring.rsp_prod_pvt++;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&xneti->xni_txring, do_event);
	if (do_event) {
		XENPRINTF(("%s send event\n", xneti->xni_name));
        	minios_notify_remote_via_evtchn(xneti->xni_evtchn);
	}
}

static const char *
xennetback_tx_check_packet(const netif_tx_request_t *txreq)
{
	XENPRINTF(("%s\n", __func__));

	if ((txreq->flags & NETTXF_more_data) == 0 &&
	    txreq->offset + txreq->size > PAGE_SIZE)
		return "crossing page boundary";

	if (txreq->size > ETHER_MAX_LEN_JUMBO)
		return "bigger then jumbo";

	return NULL;
}

static int
xennetback_copy(gnttab_copy_t *gop, int copycnt, const char *dir)
{
	XENPRINTF(("%s\n", __func__));

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
			bmk_printf("GNTTABOP_copy[%d] %s status %d\n",
			    i, dir, gop->status);
			return BMK_EINVAL;
		}
	}

	return 0;
}

static int
xennetback_tx_m0len_fragment(struct xnetback_instance *xneti,
    int m0_len, int req_cons, int *cntp)
{
	netif_tx_request_t *txreq;

	XENPRINTF(("%s\n", __func__));

	/* This assumes all the requests are already pushed into the ring */ 
	*cntp = 1;
	do {
		txreq = RING_GET_REQUEST(&xneti->xni_txring, req_cons);
		bmk_assert(m0_len > txreq->size);
		m0_len -= txreq->size;
		req_cons++;
		(*cntp)++;
	} while (txreq->flags & NETTXF_more_data);

	return m0_len;
}

static void 
xennetback_network_tx(struct xnetback_instance *xneti)

{
	netif_tx_request_t txreq;
	int receive_pending;
	RING_IDX req_cons;
	unsigned int queued = 0;
	struct tx_req_info tri[NET_TX_RING_SIZE];
    	struct xennetback_user *viu = xennetback_get_private(xneti);

	XENPRINTF(("%s\n", __func__));
	req_cons = xneti->xni_txring.req_cons;
	while (1) {
		xen_rmb(); /* be sure to read the request before updating */
		xneti->xni_txring.req_cons = req_cons;
		xen_wmb();
		RING_FINAL_CHECK_FOR_REQUESTS(&xneti->xni_txring,
		    receive_pending);
		if (receive_pending == 0)
			break;
		RING_COPY_REQUEST(&xneti->xni_txring, req_cons,
		    &txreq);
		xen_rmb();
		XENPRINTF(("%s pkt size %d\n", xneti->xni_name,
		    txreq.size));
		tri[queued].tri_id = txreq.id;
		tri[queued].tri_size = txreq.size;
		
		req_cons++;
		tri[queued].tri_req_cons = req_cons;

		/*
		 * Do some sanity checks, and queue copy of the data.
		 */
		const char *msg = xennetback_tx_check_packet(&txreq);
		if (msg != NULL) {
			bmk_printf("%s: packet with size %d is %s\n",
			    xneti->xni_name, txreq.size, msg);
			xennetback_tx_response(xneti, txreq.id,
			    NETIF_RSP_ERROR);
			continue;
		}

		tri[queued].tri_more_data = txreq.flags & NETTXF_more_data;
		tri[queued].tri_csum_blank = txreq.flags & NETTXF_csum_blank;
		tri[queued].tri_data_validated = txreq.flags & NETTXF_data_validated;
		XENPRINTF(("%s pkt offset %d size %d id %d req_cons %d\n",
		    xneti->xni_name, txreq.offset,
		    txreq.size, txreq.id, req_cons));

		xneti->xni_tx[queued] = txreq;
		queued++;

		bmk_assert(queued <= NET_TX_RING_SIZE);
		if (queued == NET_TX_RING_SIZE) {
			rumpuser__hyp.hyp_schedule();
			rump_xennetback_network_tx(viu->viu_vifsc,
				tri, queued);
			rumpuser__hyp.hyp_unschedule();
			//bmk_assert(m0 == NULL);
			queued = 0;
		}
	}

	if (queued > 0) {		
		rumpuser__hyp.hyp_schedule();
		rump_xennetback_network_tx(viu->viu_vifsc, tri, queued);
		rumpuser__hyp.hyp_unschedule();	
	}
}

void
VIFHYPER_RX_COPY_PROCESS(struct xennetback_user *viu,
	int queued, int copycnt)
{
	struct xnetback_instance *xneti = viu->viu_xneti;
	int notify, nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);

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
		XENPRINTF(("%s receive event\n", xneti->xni_name));
        	minios_notify_remote_via_evtchn(xneti->xni_evtchn);
	}
	rumpkern_sched(nlocks, NULL);
}

void
VIFHYPER_RX_COPY_QUEUE(struct xennetback_user *viu,
    int *queued, int *copycntp, int flags, int pkthdr_len, struct iovec *dm, int *xst_count, int dm_nsegs)
{
	struct xnetback_instance *xneti = viu->viu_xneti;
	gnttab_copy_t *gop;
	struct netif_rx_request rxreq;
	netif_rx_response_t *rxresp;
	paddr_t ma;
	size_t goff, segoff, segsize, take, totsize;
	int copycnt = *copycntp, reqcnt = *queued;
	//const bus_dmamap_t dm = xst0->xs_dmamap;
	const uint8_t multiseg = dm_nsegs > 1 ? 1 : 0;
	int nlocks;

	int rsp_prod_pvt = xneti->xni_rxring.rsp_prod_pvt;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);

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
			gop->source.offset = (ma & XENNET_PAGE_MASK) + segoff;
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
	rumpkern_sched(nlocks, NULL);
}
		    
int VIFHYPER_RING_CONSUMPTION(struct xennetback_user *viu)
{
	int unconsumed, nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);

	struct xnetback_instance *xneti = viu->viu_xneti;
	unconsumed = RING_HAS_UNCONSUMED_REQUESTS(&xneti->xni_rxring);

	rumpkern_sched(nlocks, NULL);

	return unconsumed;
}

int VIFHYPER_XN_RING_FULL(int cnt, struct xennetback_user *viu, int queued)
{
	struct xnetback_instance *xneti = viu->viu_xneti;
	RING_IDX req_prod, rsp_prod_pvt;
	int nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);

	req_prod = xneti->xni_rxring.sring->req_prod;
	rsp_prod_pvt = xneti->xni_rxring.rsp_prod_pvt;
	rmb();

	rumpkern_sched(nlocks, NULL);
	return	req_prod == xneti->xni_rxring.req_cons + (cnt) ||  \
		xneti->xni_rxring.req_cons - (rsp_prod_pvt + cnt) ==  \
		NET_RX_RING_SIZE;
}

static void soft_start_thread_func(void *_viu)
{
    struct xennetback_user *viu = (struct xennetback_user *)_viu;

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
	    rump_xennetback_ifsoftstart_copy(viu->viu_vifsc);
            rumpuser__hyp.hyp_unschedule();
            /* do not monopolize the CPU */ 
            if (++counter == 10) {
                	bmk_sched_yield();
                counter = 0;
            }
        } while (__atomic_exchange_n(
                     &viu->viu_softsblk.status, THREADBLK_STATUS_AWAKE,
                     __ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
    }
}

int VIFHYPER_WAKE(struct xennetback_user *viu)
{
	int nlocks;
	XENPRINTF(("%s\n", __func__));

	rumpkern_unsched(&nlocks, NULL);
	if (__atomic_exchange_n(&viu->viu_softsblk.status, THREADBLK_STATUS_NOTIFY,
                            __ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
        	bmk_sched_wake(viu->viu_softsthr);
	}
	rumpkern_sched(nlocks, NULL);

	return 0;
}

void VIFHYPER_TX_RESPONSE(struct xennetback_user *viu, int id, int status)
{
	struct xnetback_instance *xneti = viu->viu_xneti;
	int nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);
	xennetback_tx_response(xneti, id, status);
	rumpkern_sched(nlocks, NULL);
}

int VIFHYPER_TX_M0LEN_FRAGMENT(struct xennetback_user *viu,
    int m0_len, int req_cons, int *cntp)
{
	struct xnetback_instance *xneti = viu->viu_xneti;
	int ret, nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);
	ret = xennetback_tx_m0len_fragment(xneti, m0_len, req_cons, cntp);
	rumpkern_sched(nlocks, NULL);

	return ret;
}

void VIFHYPER_TX_COPY_PREPARE(struct xennetback_user *viu, int copycnt, int take,
	       int index, int goff, uint64_t ma, int segoff)
{	
	struct xnetback_instance *xneti = viu->viu_xneti;
	gnttab_copy_t *gop;
	int nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);
	/* Queue for the copy */
	gop = &xneti->xni_gop_copy[copycnt];
	bmk_memset(gop, 0, sizeof(*gop));
	gop->flags = GNTCOPY_source_gref;
	gop->len = take;

	gop->source.u.ref = xneti->xni_tx[index].gref;
	gop->source.offset = xneti->xni_tx[index].offset + goff;
	gop->source.domid = xneti->xni_domid;

	gop->dest.offset = (ma & XENNET_PAGE_MASK) + segoff;
	bmk_assert(gop->dest.offset <= PAGE_SIZE);
	gop->dest.domid = DOMID_SELF;
	gop->dest.u.gmfn = ma >> PAGE_SHIFT;
	rumpkern_sched(nlocks, NULL);

}

int VIFHYPER_COPY(struct xennetback_user *viu, int copycnt, const char *dir)
{
	struct xnetback_instance *xneti = viu->viu_xneti;
	int ret, nlocks;

	XENPRINTF(("%s\n", __func__));
	rumpkern_unsched(&nlocks, NULL);
	ret = xennetback_copy(xneti->xni_gop_copy, copycnt, dir);
	rumpkern_sched(nlocks, NULL);

	return ret;
}
