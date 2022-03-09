/*      $NetBSD: xbdback_xenbus.c,v 1.63 2016/12/26 08:16:28 skrll Exp $      */

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
 *
 */

#include <sys/types.h>
#include <sys/atomic.h>

#include <mini-os/os.h>
#include <mini-os/xenbus.h>
#include <mini-os/events.h>
#include <xen/io/blkif.h>
#include <xen/io/protocols.h>
#include <xen/features.h>
#include <mini-os/gnttab.h>
#include <mini-os/gntmap.h>
#include <mini-os/time.h>
#include <mini-os/lib.h>
#include <mini-os/semaphore.h>

#include <bmk-core/core.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/pgalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/string.h>
#include <bmk-core/platform.h>
#include <bmk-core/errno.h>
#include <bmk-core/sched.h>

#include <bmk-rumpuser/core_types.h>
#include <bmk-rumpuser/rumpuser.h>

#include "xbdback_virt.h"
#include "xbdback_virt_user.h"

#define BMK_ENOTTY		25
#define EOPNOTSUPP		-1

/* 
#define XENDEBUG_VBD
#ifdef XENDEBUG_VBD
#define XENPRINTF(x) printf x
#else
#define XENPRINTF(x) bmk_printf x
#endif
*/

//#define XBDBACK_DBG
#ifndef XBDBACK_DBG
#define XENPRINTF(x)
#else
#define XENPRINTF(x) bmk_printf x
#endif

#define BLKIF_RING_SIZE __RING_SIZE((blkif_sring_t *)0, PAGE_SIZE)

/*
 * Backend block device driver for Xen
 */

/* Max number of pages per request. The request may not be page aligned */
#define BLKIF_MAX_PAGES_PER_REQUEST (BLKIF_MAX_SEGMENTS_PER_REQUEST + 1)

/* If this value is increased to more than 512 then necessary 
 * chages may be required in the corresponding code.
 */
#define MAX_INDIRECT_SEGMENTS 256

/* Values are expressed in 512-byte sectors */
#define VBD_BSIZE 512
#define VBD_MAXSECT ((PAGE_SIZE / VBD_BSIZE) - 1)

#define DEV_BSIZE (1 << 9)

struct xbdback_request;
struct xbdback_io;
struct xbdback_fragment;
struct xbdback_instance;
struct vnode;

/*
 * status of a xbdback instance:
 * WAITING: xbdback instance is connected, waiting for requests
 * RUN: xbdi thread must be woken up, I/Os have to be processed
 * DISCONNECTING: the instance is closing, no more I/Os can be scheduled
 * DISCONNECTED: no I/Os, no ring, the thread should terminate.
 */
typedef enum {WAITING, RUN, DISCONNECTING, DISCONNECTED} xbdback_state_t;

/*
 * Each xbdback instance is managed by a single thread that handles all
 * the I/O processing. As there are a variety of conditions that can block,
 * everything will be done in a sort of continuation-passing style.
 *
 * When the execution has to block to delay processing, for example to
 * allow system to recover because of memory shortage (via shared memory
 * callback), the return value of a continuation can be set to NULL. In that
 * case, the thread will go back to sleeping and wait for the proper
 * condition before it starts processing requests again from where it left.
 * Continuation state is "stored" in the xbdback instance (xbdi_cont and
 * xbdi_cont_aux), and should only be manipulated by the instance thread.
 *
 * As xbdback(4) has to handle different sort of asynchronous events (Xen
 * event channels, biointr() soft interrupts, xenbus commands), the xbdi_lock
 * spin lock is used to protect specific elements of the xbdback instance from
 * concurrent access: thread status and ring access (when pushing responses).
 * 
 * Here's how the call graph is supposed to be for a single I/O:
 *
 * xbdback_co_main()
 *        |
 *        |               --> xbdback_co_cache_doflush() or NULL
 *        |               |
 *        |               - xbdback_co_cache_flush2() <- xbdback_co_do_io() <-
 *        |                                            |                     |
 *        |               |-> xbdback_co_cache_flush() -> xbdback_co_map_io()-
 * xbdback_co_main_loop()-|
 *        |               |-> xbdback_co_main_done() ---> xbdback_co_map_io()-
 *        |                                           |                      |
 *        |               -- xbdback_co_main_done2() <-- xbdback_co_do_io() <-
 *        |               |
 *        |               --> xbdback_co_main() or NULL
 *        |
 *     xbdback_co_io() -> xbdback_co_main_incr() -> xbdback_co_main_loop()
 *        |
 *     xbdback_co_io_gotreq()--+--> xbdback_co_map_io() ---
 *        |                    |                          |
 *  -> xbdback_co_io_loop()----|  <- xbdback_co_do_io() <--
 *  |     |     |     |
 *  |     |     |     |----------> xbdback_co_io_gotio()
 *  |     |     |                         |
 *  |     |   xbdback_co_main_incr()      |
 *  |     |     |                         |
 *  |     |   xbdback_co_main_loop()      |
 *  |     |                               |
 *  |  xbdback_co_io_gotio2() <-----------|
 *  |     |           |
 *  |     |           |----------> xbdback_co_io_gotfrag()
 *  |     |                               |
 *  -- xbdback_co_io_gotfrag2() <---------|
 *        |
 *     xbdback_co_main_incr() -> xbdback_co_main_loop()
 */
typedef void *(* xbdback_cont_t)(struct xbdback_instance *, void *);

enum xbdi_proto {
	XBDIP_NATIVE,
	XBDIP_32,
	XBDIP_64
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

union blkif_back_ring_proto {
	blkif_back_ring_t ring_n; /* native/common members */
};
typedef union blkif_back_ring_proto blkif_back_ring_proto_t;
typedef unsigned long vaddr_t;

struct xbdback_watch {
	SLIST_ENTRY(xbdback_watch) next;
	char *path;
	struct xbdback_instance *xbdi;
	void (*cbfun)(char *path, struct xbdback_instance *xbdi);
};
SLIST_HEAD(, xbdback_watch) xbdback_watches;

/* we keep the xbdback instances in a linked list */
struct xbdback_instance {
	SLIST_ENTRY(xbdback_instance) next;
	struct xenbus_device *xbdi_xbusd; /* our xenstore entry */
	//struct xenbus_event_pool xbdi_watch; /* to watch our store */
	struct xbdback_watch *xbdw_front;
	struct xbdback_watch *xbdw_back;
	uint32_t xbdi_domid;	/* attached to this domain */
	uint32_t xbdi_handle;	/* domain-specific handle */
	char xbdi_name[16];	/* name of this instance */
	/* spin lock that protects concurrent access to the xbdback instance */
	spinlock_t xbdi_lock;
	struct bmk_thread *xbdi_thread;
	struct threadblk xbdi_thread_blk;
	xbdback_state_t xbdi_status; /* thread's status */
	/* backing device parameters */
	dev_t xbdi_dev;
	const struct bdevsw *xbdi_bdevsw; /* pointer to the device's bdevsw */
	struct vnode *xbdi_vp;
	uint64_t xbdi_size;
	unsigned short xbdi_ro; /* is device read-only ? */
	/* parameters for the communication */
	evtchn_port_t xbdi_evtchn;
	uint8_t persistent;
	/* private parameters for communication */
	blkif_back_ring_proto_t xbdi_ring;
        struct gntmap xbdi_ring_map;
        struct gntmap xbdi_entry_map;
	enum xbdi_proto xbdi_proto;
	grant_handle_t xbdi_ring_handle; /* to unmap the ring */
	void* xbdi_ring_va; /* to unmap the ring */
	/* disconnection must be postponed until all I/O is done */
	int xbdi_refcnt;
	/* 
	 * State for I/O processing/coalescing follows; this has to
	 * live here instead of on the stack because of the
	 * continuation-ness (see above).
	 */
	RING_IDX xbdi_req_prod; /* limit on request indices */
	xbdback_cont_t xbdi_cont, xbdi_cont_aux;
	SIMPLEQ_ENTRY(xbdback_instance) xbdi_on_hold; /* waiting on resources */

	/* _request state: track requests fetched from ring */
	struct xbdback_request *xbdi_req; /* if NULL, ignore following */
	blkif_request_t xbdi_xen_req;
	/* indirect requests*/
	blkif_request_indirect_t xbdi_xen_indirect_req;
	struct blkif_request_segment xbdi_indirect_segments[
		MAX_INDIRECT_SEGMENTS];
	int xbdi_segno;
	/* _io state: I/O associated to this instance */
	struct xbdback_io *xbdi_io; /* if NULL, ignore next field */
	uint64_t xbdi_next_sector;
	uint8_t xbdi_last_fs, xbdi_this_fs; /* first sectors */
	uint8_t xbdi_last_ls, xbdi_this_ls; /* last sectors */
	grant_ref_t xbdi_thisgrt, xbdi_lastgrt; /* grants */
	/* other state */
	int xbdi_same_page; /* are we merging two segments on the same page? */
	uint xbdi_pendingreqs; /* number of I/O in fly */
};
/* Manipulation of the above reference count. */
#define xbdi_get(xbdip) __atomic_fetch_add(&(xbdip)->xbdi_refcnt, 1,  __ATOMIC_ACQ_REL)
#define xbdi_put(xbdip)                                      \
do {                                                         \
	if (__atomic_fetch_sub(&(xbdip)->xbdi_refcnt, 1,  __ATOMIC_ACQ_REL) == 0)  \
               xbdback_finish_disconnect(xbdip);             \
} while (/* CONSTCOND */ 0)

SLIST_HEAD(, xbdback_instance) xbdback_instances;

/*
 * For each request from a guest, a xbdback_request is allocated from
 * a pool.  This will describe the request until completion.  The
 * request may require multiple IO operations to perform, so the
 * per-IO information is not stored here.
 */
struct xbdback_request {
	struct xbdback_instance *rq_xbdi; /* our xbd instance */
	uint64_t rq_id;
	int rq_iocount; /* reference count; or, number of outstanding I/O's */
	int rq_ioerrs;
	uint8_t rq_operation;
};

struct buf; /* our I/O */
/*
 * For each I/O operation associated with one of those requests, an
 * xbdback_io is allocated from a pool.  It may correspond to multiple
 * Xen disk requests, or parts of them, if several arrive at once that
 * can be coalesced.
 */
struct xbdback_io {
	/* The instance pointer is duplicated for convenience. */
	struct xbdback_instance *xio_xbdi; /* our xbd instance */
	uint8_t xio_operation;
	union {
		struct {
			struct buf xio_buf[MAX_INDIRECT_SEGMENTS]; /* our I/O */
			/* xbd requests involved */
			SLIST_HEAD(, xbdback_fragment) xio_rq;
			/* the virtual address to map the request at */
			vaddr_t xio_vaddr[MAX_INDIRECT_SEGMENTS];
			/* grants to map */
			grant_ref_t xio_gref[MAX_INDIRECT_SEGMENTS];
			/* grants release */
			grant_handle_t xio_gh[BLKIF_MAX_PAGES_PER_REQUEST];
			uint16_t xio_nrma; /* number of guest pages */
			uint16_t xio_done; /* number of guest pages */
			uint16_t xio_mapped; /* == 1: grants are mapped */

		} xio_rw;
		uint64_t xio_flush_id;
	} u;
};
#define xio_buf		u.xio_rw.xio_buf
#define xio_rq		u.xio_rw.xio_rq
#define xio_vaddr	u.xio_rw.xio_vaddr
#define xio_gref	u.xio_rw.xio_gref
#define xio_gh		u.xio_rw.xio_gh
#define xio_nrma	u.xio_rw.xio_nrma
#define xio_done	u.xio_rw.xio_done
#define xio_mapped	u.xio_rw.xio_mapped

#define xio_flush_id	u.xio_flush_id

/*
 * Rather than having the xbdback_io keep an array of the
 * xbdback_requests involved, since the actual number will probably be
 * small but might be as large as BLKIF_RING_SIZE, use a list.  This
 * would be threaded through xbdback_request, but one of them might be
 * part of multiple I/O's, alas.
 */
struct xbdback_fragment {
	struct xbdback_request *car;
	SLIST_ENTRY(xbdback_fragment) cdr;
};

/*
 * xbdback_pools to manage the chain of block requests and I/Os fragments
 * submitted by frontend.
 */
struct xbdback_pool {
    uint64_t front, rear;
    uint64_t capacity, size;
    void **array;
} *xbdback_request_pool, *xbdback_io_pool, *xbdback_fragment_pool;

SIMPLEQ_HEAD(xbdback_iqueue, xbdback_instance);

struct gref_2_page {
	vaddr_t page;
	uint8_t op;
};

#define GREF_TABLE_SIZE 10000 // it should be 2^32 to hold page addr for any gref
struct gref_2_page gref_table[GREF_TABLE_SIZE];

static struct bmk_thread *xbdback_watch_thread;
static struct xenbus_event_queue watch_queue;

static void xbdbackattach(int);
static int  xbdback_init_watches(void); 
static void xbdw_thread_func(void *); 
static void xbdback_instance_search(char *, struct xbdback_instance *);
static int  probe_xbdback_device(const char *);
static int  xbdback_xenbus_create(char *);
static int  xbdback_xenbus_destroy(void *);
static void xbdback_frontend_changed(char *, struct xbdback_instance *);
static void xbdback_backend_changed(char *, struct xbdback_instance *);
static void xbdback_evthandler(evtchn_port_t, struct pt_regs *, void *);

static int  xbdback_connect(struct xbdback_instance *);
static void xbdback_disconnect(struct xbdback_instance *);
static void xbdback_finish_disconnect(struct xbdback_instance *);

static struct xbdback_instance *xbdif_lookup(domid_t, uint32_t);

static void *xbdback_co_main(struct xbdback_instance *, void *);
static void *xbdback_co_main_loop(struct xbdback_instance *, void *);
static void *xbdback_co_main_incr(struct xbdback_instance *, void *);
static void *xbdback_co_main_done(struct xbdback_instance *, void *);
static void *xbdback_co_main_done2(struct xbdback_instance *, void *);

static void *xbdback_co_cache_flush(struct xbdback_instance *, void *);
static void *xbdback_co_cache_flush2(struct xbdback_instance *, void *);
static void *xbdback_co_cache_doflush(struct xbdback_instance *, void *);

static void *xbdback_co_io(struct xbdback_instance *, void *);
static void *xbdback_co_io_gotreq(struct xbdback_instance *, void *);
static void *xbdback_co_io_loop(struct xbdback_instance *, void *);
static void *xbdback_co_io_gotio(struct xbdback_instance *, void *);
static void *xbdback_co_io_gotio2(struct xbdback_instance *, void *);
static void *xbdback_co_io_gotfrag(struct xbdback_instance *, void *);
static void *xbdback_co_io_gotfrag2(struct xbdback_instance *, void *);

static void *xbdback_co_map_io(struct xbdback_instance *, void *);
static void *xbdback_co_do_io(struct xbdback_instance *, void *);

//static void *xbdback_co_wait_shm_callback(struct xbdback_instance *, void *);

//static int  xbdback_shm_callback(void *);
static void xbdback_io_error(struct xbdback_io *, int);
static void xbdback_iodone(struct buf *);
static void xbdback_send_reply(struct xbdback_instance *, uint64_t , int , int);

static void *xbdback_map_shm(struct xbdback_io *);
static void xbdback_unmap_shm(struct xbdback_io *);

static struct xbdback_pool *create_pool(uint64_t capacity);
static void xbdback_pool_put(struct xbdback_pool *pool, void *item); 
static void* xbdback_pool_get(struct xbdback_pool *pool);
//static void destroy_pool(struct xbdback_pool *pool); 

static void xbdback_trampoline(struct xbdback_instance *, void *);
static void xbdback_thread(void *);
	
static void put_page_to_gref_table(grant_ref_t ref, vaddr_t page, uint8_t op);
static vaddr_t get_page_from_gref_table(grant_ref_t ref, uint8_t op);

static void xbdback_wakeup_thread(struct xbdback_instance *);
static void threadblk_callback(struct bmk_thread *, struct bmk_block_data *);

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

static void xbdback_sched_wake(struct threadblk *tblk, struct bmk_thread *thread)
{
	if (__atomic_exchange_n(&tblk->status, THREADBLK_STATUS_NOTIFY,
				__ATOMIC_ACQ_REL) == THREADBLK_STATUS_SLEEP) {
		bmk_sched_wake(thread);
	}
}

void VIFHYPER_ENTRY(void)
{
	int nlocks;

	rumpkern_unsched(&nlocks, NULL);

	if (gntmap_create_addr_list() == NULL)
		bmk_platform_halt("gntmap address list creation failed\n");

	xbdbackattach(0);
	xbdback_init_watches();

	rumpkern_sched(nlocks, NULL);
}

void
xbdbackattach(int n)
{
	unsigned long long i;
	struct xbdback_request *request;
	struct xbdback_io *io;
	struct xbdback_fragment *fragment;

	XENPRINTF(("%s\n", __func__));

	/*
	 * initialize the backend driver, register the control message handler
	 * and send driver up message.
	 */
	SLIST_INIT(&xbdback_instances);

	xbdback_request_pool = create_pool(BLKIF_RING_SIZE);
	xbdback_io_pool = create_pool(BLKIF_RING_SIZE);
	xbdback_fragment_pool = create_pool(MAX_INDIRECT_SEGMENTS * BLKIF_RING_SIZE);

	for(i = 0; i < BLKIF_RING_SIZE; i++) {
		request = bmk_memcalloc(1, sizeof(struct xbdback_request), BMK_MEMWHO_WIREDBMK);
		if(request == NULL)
			bmk_platform_halt("xbdbackattach: Couldn't allocate request pool\n");
		xbdback_pool_put(xbdback_request_pool, request);

		io = bmk_memcalloc(1, sizeof(struct xbdback_io), BMK_MEMWHO_WIREDBMK);
		if(io == NULL)
			bmk_platform_halt("xbdbackattach: Couldn't allocate io pool\n");

		xbdback_pool_put(xbdback_io_pool, io);
	}
	for(i = 0; i < MAX_INDIRECT_SEGMENTS * BLKIF_RING_SIZE; i++) {
		fragment = bmk_memcalloc(1, sizeof(struct xbdback_fragment), BMK_MEMWHO_WIREDBMK);
		if(fragment == NULL)
			bmk_platform_halt("xbdbackattach: Couldn't allocate fragment pool\n");

		xbdback_pool_put(xbdback_fragment_pool, fragment);
	}
}

struct xbdback_watch *xbdw_vbd;

static int xbdback_init_watches(void) {
	char path[64];
	int dom;

	SLIST_INIT(&xbdback_watches);

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
	xbdw_vbd->cbfun = xbdback_instance_search;
	xbdw_vbd->xbdi = NULL;
	SLIST_INSERT_HEAD(&xbdback_watches, xbdw_vbd, next);

    	xenbus_event_queue_init(&watch_queue);
    	bmk_snprintf(path, sizeof(path), "/local/domain");
	xenbus_watch_path_token(XBT_NIL, path, path, &watch_queue);
 	xbdback_watch_thread = bmk_sched_create("xbdback_watch", NULL, 0, -1,
		                                       xbdw_thread_func, NULL, NULL, 0);

	return 0;
}

static void xbdw_thread_func(void *ign) 
{
	char **ret;
	struct xbdback_watch *xbdw;

	XENPRINTF(("%s\n", __func__));
		
	/* give us a rump kernel context */
    	rumpuser__hyp.hyp_schedule();
    	rumpuser__hyp.hyp_lwproc_newlwp(0);
    	rumpuser__hyp.hyp_unschedule();

	for(;;) {
		ret = xenbus_wait_for_watch_return(&watch_queue);
		if(ret == NULL)
			continue;

		SLIST_FOREACH(xbdw, &xbdback_watches, next) {
			if (bmk_strcmp(xbdw->path, *ret) == 0) {
				XENPRINTF(("Event match for path %s\n", *ret));
				xbdw->cbfun(*ret, xbdw->xbdi);
				continue;
			}
		}
	}
}

static void
xbdback_instance_search(char *backend_root, struct xbdback_instance *xbdi){
	char *msg;
    	char **dirt, **dirid;
    	unsigned int type, id;
    	char path[30];
    	char vbd_found = 0;
    	int err;

        bmk_printf("Checking for backend changes\n");
        msg = xenbus_ls(XBT_NIL, "backend", &dirt);
        if (msg) {
            bmk_printf("No backend found: %s\n", msg);
            bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
            return;
        }

        for (type = 0; dirt[type]; type++) {
            if (bmk_strcmp(dirt[type], "vbd") == 0) {
                vbd_found = 1;
                break;
            }
        }

        if (vbd_found == 0)
            return;

        msg = xenbus_ls(XBT_NIL, "backend/vbd", &dirid);
        if (msg) {
            bmk_printf("Error in xenbus ls: %s\n", msg);
            bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);

            return;
        }

        for (id = 0; dirid[id]; id++) {
            bmk_snprintf(path, sizeof(path), "backend/vbd/%s", dirid[id]);
            err = probe_xbdback_device(path);
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

static int 
probe_xbdback_device(const char *vbdpath) {
    int err = 0, pos, msize, state;
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
        msize = bmk_strlen(vbdpath) + bmk_strlen(dir[pos]) + 2;
        devpath = bmk_memalloc(msize, 0, BMK_MEMWHO_WIREDBMK);
        if (devpath == NULL) {
            bmk_printf("can't malloc xbusd");
            return 1;
        }

        bmk_snprintf(devpath, msize, "%s/%s", vbdpath, dir[pos]);
        bmk_snprintf(path, sizeof(path), "%s/state", devpath);
        state = xenbus_read_integer(path);
        if (state != XenbusStateInitialising) {
            /* device is not new */
	    bmk_printf("%s state is %d\n", devpath, state);
            bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);

	    if (state == -1)
		    return 1;
            continue;
        }

	err = xbdback_xenbus_create(devpath);
        //bmk_memfree(devpath, BMK_MEMWHO_WIREDBMK);
	break;
    }

    return err;
}

static int
xbdback_xenbus_create(char *xbusd_path)
{
	struct xbdback_instance *xbdi;
	struct xenbus_device *xbusd;
	long domid, handle;
	int error, i;
	char *ep, *message, *otherend;
	char path[64], *absolute_path, *device_path;
	unsigned long dev;

	xbusd = (struct xenbus_device*)bmk_memcalloc(1, sizeof(xbusd),
			BMK_MEMWHO_WIREDBMK);
	xbusd->xbusd_path = xbusd_path;
	XENPRINTF(("%s = %s\n", __func__, xbusd->xbusd_path));

	bmk_snprintf(path, sizeof(path), "%s/frontend-id", xbusd->xbusd_path);
	domid = xenbus_read_integer(path);

	/*
	 * get handle: this is the last component of the path; which is
	 * a decimal number. $path/dev contains the device name, which is not
	 * appropriate.
	 */
	for (i = bmk_strlen(xbusd->xbusd_path); i > 0; i--) {
		if (xbusd->xbusd_path[i] == '/')
			break;
	}
	if (i == 0) {
		bmk_printf("xbdback: can't parse %s\n",
		    xbusd->xbusd_path);
		return BMK_EINVAL;
	}
	handle = bmk_strtoul(&xbusd->xbusd_path[i+1], &ep, 10);
	if (*ep != '\0') {
		bmk_printf("xbdback: can't parse %s\n",
		    xbusd->xbusd_path);
		return BMK_EINVAL;
	}
			
	if (xbdif_lookup(domid, handle) != NULL) {
		bmk_printf("xbdback: backend exists\n");
		return BMK_EINVAL;
	}
	xbdi = bmk_memcalloc(1, sizeof(*xbdi), BMK_MEMWHO_WIREDBMK);

	xbdi->xbdi_domid = domid;
	xbdi->xbdi_handle = handle;
	bmk_snprintf(xbdi->xbdi_name, sizeof(xbdi->xbdi_name), "xbdb%di%d",
	    xbdi->xbdi_domid, xbdi->xbdi_handle);

	XENPRINTF(("xbdi_name = %s\n", xbdi->xbdi_name));
	/* initialize status and reference counter */
	xbdi->xbdi_status = DISCONNECTED;
	xbdi_get(xbdi);

	spin_lock_init(&xbdi->xbdi_lock);
	xbdi->xbdi_thread_blk.header.callback = threadblk_callback;
	xbdi->xbdi_thread_blk.status = THREADBLK_STATUS_AWAKE;
	SLIST_INSERT_HEAD(&xbdback_instances, xbdi, next);

	xbusd->xbusd_u.b.b_cookie = xbdi;	
	xbusd->xbusd_u.b.b_detach = xbdback_xenbus_destroy;

	bmk_snprintf(path, sizeof(path), "%s/frontend", xbusd->xbusd_path);
	message = xenbus_read(XBT_NIL, path, &otherend);
	if(message)
		bmk_platform_halt("Cannot read frontend path\n");

	bmk_snprintf(xbusd->xbusd_otherend, sizeof(xbusd->xbusd_otherend), "%s", otherend);
	XENPRINTF(("%s: other end path %s\n", __func__, xbusd->xbusd_otherend));

	xbdi->xbdw_front = bmk_memcalloc(1, sizeof(xbdi->xbdw_front), BMK_MEMWHO_WIREDBMK);
	xbdi->xbdw_front->path = bmk_memcalloc(1, sizeof(path), BMK_MEMWHO_WIREDBMK);
	bmk_snprintf(xbdi->xbdw_front->path, sizeof(path), "%s/state", xbusd->xbusd_otherend);
	xbdi->xbdw_front->cbfun = xbdback_frontend_changed;
	xbdi->xbdw_front->xbdi = xbdi;
	SLIST_INSERT_HEAD(&xbdback_watches, xbdi->xbdw_front, next);

	xbdi->xbdi_xbusd = xbusd;

	/* Read absolute path of this backend from it's frontend's directory */
	bmk_snprintf(path, sizeof(path), "%s/backend", xbusd->xbusd_otherend);
	bmk_memcalloc(1, sizeof(path), BMK_MEMWHO_WIREDBMK);
	message = xenbus_read(XBT_NIL, path, &absolute_path);
	if(message)
		bmk_platform_halt("Cannot read frontend path\n");

	xbdi->xbdw_back = bmk_memcalloc(1, sizeof(xbdi->xbdw_back), BMK_MEMWHO_WIREDBMK);
	xbdi->xbdw_back->path = bmk_memcalloc(1, sizeof(path), BMK_MEMWHO_WIREDBMK);
	bmk_snprintf(xbdi->xbdw_back->path, sizeof(path), "%s/physical-device", absolute_path);
	bmk_memfree(absolute_path, BMK_MEMWHO_WIREDBMK);

	xbdi->xbdw_back->cbfun = xbdback_backend_changed;
	xbdi->xbdw_back->xbdi = xbdi;
	SLIST_INSERT_HEAD(&xbdback_watches, xbdi->xbdw_back, next);

	bmk_snprintf(path, sizeof(path), "%s/state", xbusd->xbusd_path);
	message = xenbus_switch_state(XBT_NIL, path, XenbusStateInitWait);
	if (message) {
		bmk_printf("Failed to switch state on %s: %d\n", path, error);
		goto fail2;
	}

	bmk_snprintf(path, sizeof(path), "%s/params", xbusd->xbusd_path);
	message = xenbus_read(XBT_NIL, path, &device_path);
	if(message)
		bmk_platform_halt("Cannot read device path\n");

    	rumpuser__hyp.hyp_schedule();
	dev = rump_xbdback_get_number(device_path);
    	rumpuser__hyp.hyp_unschedule();
	
	if(dev == 0)
		bmk_platform_halt("Cannot retrieve device number\n");

	message = xenbus_printf(XBT_NIL, xbusd->xbusd_path, "physical-device",
			"%lu", dev);
	if(message) {
		bmk_printf("Failed to write %s/physical-device\n", 
				xbusd->xbusd_path);
		goto fail2;
	}

	message = xenbus_printf(XBT_NIL, xbusd->xbusd_path, "hotplug-status",
			"%s", "connected");
	if (message) {
		bmk_printf("Failed to write %s/hotplug-status\n",
				xbusd->xbusd_path);
		goto fail2;
	}


	return 0;

fail2:
	xenbus_unwatch_path_token(XBT_NIL, path, path);
	bmk_memfree(xbdi, BMK_MEMWHO_WIREDBMK);
	return error;
}

static int
xbdback_xenbus_destroy(void *arg)
{
	struct xbdback_instance *xbdi = arg;
	
	/* give us a rump kernel context */
    	rumpuser__hyp.hyp_schedule();
    	rumpuser__hyp.hyp_lwproc_newlwp(0);
    	rumpuser__hyp.hyp_unschedule();

	XENPRINTF(("%s: status %d\n", __func__, xbdi->xbdi_status));

	xbdback_disconnect(xbdi);

	/* unregister watch */
	if (xbdi->xbdw_front) {
		SLIST_REMOVE(&xbdback_watches, xbdi->xbdw_front, xbdback_watch, next);
		bmk_memfree(xbdi->xbdw_front->path, BMK_MEMWHO_WIREDBMK);
		bmk_memfree(xbdi->xbdw_front, BMK_MEMWHO_WIREDBMK);
		xbdi->xbdw_front = NULL;
	}
	if (xbdi->xbdw_back) {
		SLIST_REMOVE(&xbdback_watches, xbdi->xbdw_back, xbdback_watch, next);
		bmk_memfree(xbdi->xbdw_back->path, BMK_MEMWHO_WIREDBMK);
		bmk_memfree(xbdi->xbdw_back, BMK_MEMWHO_WIREDBMK);
		xbdi->xbdw_back = NULL;
	}

	/* unmap ring */
	gntmap_fini(&xbdi->xbdi_ring_map);

	/* close device */
	if (xbdi->xbdi_size) {
    		rumpuser__hyp.hyp_schedule();
		rump_xbdback_virt_destroy(xbdi->xbdi_vp, xbdi->xbdi_domid);
    		rumpuser__hyp.hyp_unschedule();
	}
	SLIST_REMOVE(&xbdback_instances, xbdi, xbdback_instance, next);
	bmk_memfree(xbdi, BMK_MEMWHO_WIREDBMK);
	return 0;
}

static int
xbdback_connect(struct xbdback_instance *xbdi)
{
	u_long revtchn;
	uint32_t ring_ref;
	char *xsproto, *message;
	const char *proto;
	struct xenbus_device *xbusd = xbdi->xbdi_xbusd;
	char path[64];
	int rc;

	XENPRINTF(("%s: %s\n", __func__, xbusd->xbusd_path));

	/* read comunication informations */
	bmk_snprintf(path, sizeof(path), "%s/ring-ref", xbusd->xbusd_otherend);
	ring_ref = xenbus_read_integer(path);
	bmk_printf("xbdback %s: connect ring-ref %u\n", xbusd->xbusd_path, ring_ref);

	bmk_snprintf(path, sizeof(path), "%s/event-channel", xbusd->xbusd_otherend);
	revtchn = xenbus_read_integer(path);
	bmk_printf("xbdback %s: connect revtchn %lu\n", xbusd->xbusd_path, revtchn);

	bmk_snprintf(path, sizeof(path), "%s/feature-persistent", xbusd->xbusd_otherend);
	xbdi->persistent = xenbus_read_integer(path);
	bmk_printf("xbdback %s: connect persistent %u\n", xbusd->xbusd_path, xbdi->persistent);

	bmk_snprintf(path, sizeof(path), "%s/protocol", xbusd->xbusd_otherend);
	message = xenbus_read(XBT_NIL, path, &xsproto);
	if (message) {
		xbdi->xbdi_proto = XBDIP_NATIVE;
		proto = "unspecified";
		bmk_printf("xbdback %s: connect no xsproto\n", xbusd->xbusd_path);
	} else {
		bmk_printf("xbdback %s: connect xsproto %s\n", xbusd->xbusd_path, xsproto);
		if (bmk_strcmp(xsproto, XEN_IO_PROTO_ABI_NATIVE) == 0) {
			xbdi->xbdi_proto = XBDIP_NATIVE;
			proto = XEN_IO_PROTO_ABI_NATIVE;
		} else {
			bmk_printf("xbd domain %d: unknown proto %s\n",
			    xbdi->xbdi_domid, xsproto);
			bmk_memfree(xsproto, BMK_MEMWHO_WIREDBMK);
			return -1;
		}
		bmk_memfree(xsproto, BMK_MEMWHO_WIREDBMK);
	}

	gntmap_init(&xbdi->xbdi_entry_map);
	gntmap_init(&xbdi->xbdi_ring_map);

	xbdi->xbdi_ring_va = gntmap_map_grant_refs(&xbdi->xbdi_ring_map, 
				1, 
				&xbdi->xbdi_domid, 
				1, 
				&ring_ref, 
				1);
	if (xbdi->xbdi_ring_va == NULL) {
		goto err;
	}

	switch(xbdi->xbdi_proto) {
	case XBDIP_NATIVE:
	{
		blkif_sring_t *sring = (void *)xbdi->xbdi_ring_va;
		BACK_RING_INIT(&xbdi->xbdi_ring.ring_n, sring, PAGE_SIZE);
		break;
	}
	default:
	{
		bmk_platform_halt("We only support native sring\n");
	}
	}

 	xbdi->xbdi_thread = bmk_sched_create(xbdi->xbdi_name, NULL, 0, -1,
	                        xbdback_thread, xbdi, NULL, 0);
        if(xbdi->xbdi_thread == NULL)
		goto err2;
	
	rc = minios_evtchn_bind_interdomain(xbdi->xbdi_domid, revtchn,
				xbdback_evthandler, xbdi, &xbdi->xbdi_evtchn);
        if (rc)
		goto err2;

	xbdi->xbdi_status = WAITING;

	/* enable the xbdback event handler machinery */
	minios_unmask_evtchn(xbdi->xbdi_evtchn);
	minios_notify_remote_via_evtchn(xbdi->xbdi_evtchn);

	bmk_printf("xbd backend domain %d handle %#x (%d) "
		"using event channel %d, protocol %s\n", xbdi->xbdi_domid,
		  xbdi->xbdi_handle, xbdi->xbdi_handle, xbdi->xbdi_evtchn, proto);

	return 0;
err2:
	if(xbdi->xbdi_ring_va)
		gntmap_munmap(&xbdi->xbdi_ring_map, (unsigned long)xbdi->xbdi_ring_va, 1);
	gntmap_fini(&xbdi->xbdi_ring_map);

err:
	return -1;
}

/*
 * Signal a xbdback thread to disconnect. Done in 'xenwatch' thread context.
 */
static void
xbdback_disconnect(struct xbdback_instance *xbdi)
{
	XENPRINTF(("%s\n", __func__));

	spin_lock(&xbdi->xbdi_lock);
	XENPRINTF(("xbdback_disconnect: inside spinlock"));
	if (xbdi->xbdi_status == DISCONNECTED) {
		spin_unlock(&xbdi->xbdi_lock);
		return;
	}
	minios_unbind_evtchn(xbdi->xbdi_evtchn);

	/* signal thread that we want to disconnect, then wait for it */
	xbdi->xbdi_status = DISCONNECTING;
	xbdback_sched_wake(&xbdi->xbdi_thread_blk, xbdi->xbdi_thread);

	while (xbdi->xbdi_status != DISCONNECTED) {
		bmk_sched_blockprepare();
		bmk_sched_block(&xbdi->xbdi_thread_blk.header);
	}

	XENPRINTF(("xbdback_disconnect: outside spinlock"));
	spin_unlock(&xbdi->xbdi_lock);

	xenbus_switch_state(XBT_NIL, xbdi->xbdi_xbusd->xbusd_path, XenbusStateClosing);
}

static void
xbdback_frontend_changed(char *path, struct xbdback_instance* xbdi)
{
	struct xenbus_device *xbusd = xbdi->xbdi_xbusd;

	int new_state = xenbus_read_integer(path);
	XENPRINTF(("%s %s: new state %d\n", __func__, xbusd->xbusd_path, new_state));
	switch(new_state) {
	case XenbusStateInitialising:
		break;
	case XenbusStateInitialised:
	case XenbusStateConnected:
		if (xbdi->xbdi_status == WAITING || xbdi->xbdi_status == RUN)
			break;
		xbdback_connect(xbdi);
		//FIXME: We shouldn't remove the frontend watch
		//SLIST_REMOVE(&xbdback_watches, xbdi->xbdw_front, xbdback_watch, next);
		break;
	case XenbusStateClosing:
		xbdback_disconnect(xbdi);
		break;
	case XenbusStateClosed:
		/* otherend_changed() should handle it for us */
		bmk_platform_halt("xbdback_frontend_changed: closed\n");
	case XenbusStateUnknown:
	case XenbusStateInitWait:
	default:
		bmk_printf("xbdback %s: invalid frontend state %d\n",
		    xbusd->xbusd_path, new_state);
	}
	return;
}

static void
xbdback_backend_changed(char *phy_path, struct xbdback_instance *xbdi)
{
	unsigned long dev;
	int err, retry = 0;
	char *mode, *msg, path[128];
	xenbus_transaction_t xbt;
	struct xenbus_device *xbusd = xbdi->xbdi_xbusd;
	char xbusd_path[64];

	bmk_snprintf(xbusd_path, sizeof(xbusd_path), "%s", xbusd->xbusd_path);
	bmk_snprintf(path, sizeof(path), "%s/physical-device", xbusd_path);
	dev = xenbus_read_integer(path);

	/*
	 * we can also fire up after having opened the device, don't try
	 * to do it twice.
	 */
	if (xbdi->xbdi_vp != NULL) {
		if (xbdi->xbdi_status == WAITING || xbdi->xbdi_status == RUN) {
			if (xbdi->xbdi_dev != dev) {
				bmk_printf("xbdback %s: changing physical device "
				    "from %lx to %lx not supported\n",
				    xbusd->xbusd_path, xbdi->xbdi_dev, dev);
			}
		}
		return;
	}
	xbdi->xbdi_dev = dev;
	bmk_snprintf(path, sizeof(path), "%s/mode", xbusd_path);
	msg = xenbus_read(XBT_NIL, path, &mode);
	if (msg) {
		bmk_printf("xbdback: failed to read %s/mode: %d\n",
		    xbusd_path, err);
		return;
	}
	if (mode[0] == 'w')
		xbdi->xbdi_ro = false;
	else
		xbdi->xbdi_ro = true;
	bmk_memfree(mode, BMK_MEMWHO_WIREDBMK);

    	rumpuser__hyp.hyp_schedule();
	xbdi->xbdi_vp = rump_xbdback_virt_changed(xbdi->xbdi_dev, xbdi->xbdi_vp,
				&xbdi->xbdi_size, xbdi->xbdi_domid);
    	rumpuser__hyp.hyp_unschedule();

	if(xbdi->xbdi_vp == NULL) {
		bmk_printf("xbdback: Vnode retrival failed\n");
		xbdi->xbdi_dev = 0;
		xbdi->xbdi_vp = NULL;

		return;
	}

again:
	msg = xenbus_transaction_start(&xbt);
	if (msg) {
		bmk_printf("xbdback %s: can't start transaction\n",
		    xbusd_path);
		    return;
	}
	msg = xenbus_printf(xbt, xbusd_path, "sectors", "%lu",
	    xbdi->xbdi_size);
	if (msg) {
		bmk_printf("xbdback: failed to write %s/sectors\n",
		    xbusd_path);
		goto abort;
	}
	msg = xenbus_printf(xbt, xbusd_path, "info", "%u",
	    xbdi->xbdi_ro ? VDISK_READONLY : 0);
	if (msg) {
		bmk_printf("xbdback: failed to write %s/info\n",
		    xbusd_path);
		goto abort;
	}
	msg = xenbus_printf(xbt, xbusd_path, "sector-size", "%lu",
	    (u_long)DEV_BSIZE);
	if (msg) {
		bmk_printf("xbdback: failed to write %s/sector-size\n",
		    xbusd_path);
		goto abort;
	}
	msg = xenbus_printf(xbt, xbusd_path, "feature-flush-cache",
	    "%u", 1);
	if (msg) {
		bmk_printf("xbdback: failed to write %s/feature-flush-cached\n",
		    xbusd_path);
		goto abort;
	}
	msg = xenbus_printf(xbt, xbusd_path, "feature-persistent",
	    "%u", 1);
	if (msg) {
		bmk_printf("xbdback: failed to write %s/feature-persistent\n",
		    xbusd_path);
		goto abort;
	}

	msg = xenbus_printf(xbt, xbusd_path, "feature-max-indirect-segments",
	    "%u", MAX_INDIRECT_SEGMENTS);
	if (msg) {
		bmk_printf("xbdback: failed to write %s/feature-max-indirect-segments\n",
		    xbusd_path);
		goto abort;
	}

	msg = xenbus_transaction_end(xbt, 0, &retry);
	if (msg) {
		bmk_printf("xbdback %s: can't end transaction\n",
		    xbusd_path);
	}
	if (retry)
		goto again;
        bmk_snprintf(path, sizeof(path), "%s/state", xbusd_path);
	msg = xenbus_switch_state(XBT_NIL, path, XenbusStateConnected);
	if (msg) {
		bmk_printf("xbdback %s: can't switch state\n",
		    xbusd_path);
	}
	bmk_snprintf(xbusd->xbusd_path, sizeof(xbusd_path), "%s", xbusd_path);
	return;
abort:
	xenbus_transaction_end(xbt, 1, &retry);
}

/*
 * Used by a xbdi thread to signal that it is now disconnected.
 */
static void
xbdback_finish_disconnect(struct xbdback_instance *xbdi)
{
	XENPRINTF(("%s\n", __func__));
	bmk_assert(!spin_is_locked(&xbdi->xbdi_lock));
	bmk_assert(xbdi->xbdi_status == DISCONNECTING);

	xbdi->xbdi_status = DISCONNECTED;

	xbdback_sched_wake(&xbdi->xbdi_thread_blk, xbdi->xbdi_thread);
}

static struct xbdback_instance *
xbdif_lookup(domid_t dom , uint32_t handle)
{
	struct xbdback_instance *xbdi;

	SLIST_FOREACH(xbdi, &xbdback_instances, next) {
		if (xbdi->xbdi_domid == dom && xbdi->xbdi_handle == handle)
			return xbdi;
	}
	return NULL;
}

static void 
xbdback_evthandler(evtchn_port_t port, struct pt_regs *regs,
		                            void *data)
{
	struct xbdback_instance *xbdi = data;

	xbdback_wakeup_thread(xbdi);

	return;
}

/*
 * Main thread routine for one xbdback instance. Woken up by
 * xbdback_evthandler when a domain has I/O work scheduled in a I/O ring.
 */
static void
xbdback_thread(void *arg)
{
	struct xbdback_instance *xbdi = arg;

	/* give us a rump kernel context */
    	rumpuser__hyp.hyp_schedule();
    	rumpuser__hyp.hyp_lwproc_newlwp(0);
	rumpuser__hyp.hyp_unschedule();

	for (;;) {
		XENPRINTF(("%s\n", __func__));
		bmk_platform_splhigh();
		spin_lock(&xbdi->xbdi_lock);
		XENPRINTF(("xbdback_thread: inside spinlock\n"));
		switch (xbdi->xbdi_status) {
		case WAITING:
			spin_unlock(&xbdi->xbdi_lock);
			bmk_platform_splx(0);
			bmk_sched_blockprepare();
			bmk_sched_block(&xbdi->xbdi_thread_blk.header);
			XENPRINTF(("xbdback_thread: wait: unblocked\n"));

			__atomic_store_n(&xbdi->xbdi_thread_blk.status, THREADBLK_STATUS_AWAKE,
				__ATOMIC_RELEASE);
			break;
		case RUN:
			XENPRINTF(("xbdback_thread: run: outside spinlock\n"));
			xbdi->xbdi_status = WAITING; /* reset state */
			spin_unlock(&xbdi->xbdi_lock);
			bmk_platform_splx(0);

			do {
				if (xbdi->xbdi_cont == NULL) {
					xbdi->xbdi_cont = xbdback_co_main;
				}

				xbdback_trampoline(xbdi, xbdi);

	      		} while (__atomic_exchange_n(
				&xbdi->xbdi_thread_blk.status, THREADBLK_STATUS_AWAKE,
                     		__ATOMIC_ACQ_REL) == THREADBLK_STATUS_NOTIFY);
			break;
		case DISCONNECTING:
			XENPRINTF(("xbdback_thread: disconnecting\n"));
			if (__atomic_load_n(&(xbdi)->xbdi_pendingreqs, __ATOMIC_RELAXED) > 0) {
				/* there are pending I/Os. Wait for them. */
				bmk_sched_blockprepare();
				bmk_sched_block(&xbdi->xbdi_thread_blk.header);
				__atomic_store_n(&xbdi->xbdi_thread_blk.status, THREADBLK_STATUS_AWAKE,
						__ATOMIC_RELEASE);
	
				XENPRINTF(("xbdback_thread: disconnect outside spinlock\n"));
				spin_unlock(&xbdi->xbdi_lock);
				break;
			}
			
			/* All I/Os should have been processed by now,
			 * xbdi_refcnt should drop to 0 */
			xbdi_put(xbdi);
			bmk_assert(xbdi->xbdi_refcnt == 0);
			spin_unlock(&xbdi->xbdi_lock);
			bmk_sched_exit();
			return;
		default:
			bmk_printf("%s: invalid state %d",
			    xbdi->xbdi_name, xbdi->xbdi_status);
			bmk_platform_halt(NULL);
		}
	}
}

static void *
xbdback_co_main(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;

	XENPRINTF(("%s\n", __func__));

	xbdi->xbdi_req_prod = xbdi->xbdi_ring.ring_n.sring->req_prod;
	rmb(); /* ensure we see all requests up to req_prod */
	/*
	 * note that we'll eventually get a full ring of request.
	 * in this case, MASK_BLKIF_IDX(req_cons) == MASK_BLKIF_IDX(req_prod)
	 */
	xbdi->xbdi_cont = xbdback_co_main_loop;
	return xbdi;
}

/*
 * Fetch a blkif request from the ring, and pass control to the appropriate
 * continuation.
 * If someone asked for disconnection, do not fetch any more request from
 * the ring.
 */
static void *
xbdback_co_main_loop(struct xbdback_instance *xbdi, void *obj) 
{
	blkif_request_t *req, *temp;
	blkif_request_indirect_t *indirect_req;

	XENPRINTF(("%s\n", __func__));

	(void)obj;
	req = &xbdi->xbdi_xen_req;
	indirect_req = &xbdi->xbdi_xen_indirect_req;

	if (xbdi->xbdi_ring.ring_n.req_cons != xbdi->xbdi_req_prod) {
		switch(xbdi->xbdi_proto) {
		case XBDIP_NATIVE:
			temp = RING_GET_REQUEST(&xbdi->xbdi_ring.ring_n,
					xbdi->xbdi_ring.ring_n.req_cons);

			if(temp->operation == BLKIF_OP_INDIRECT) {
				int i, j, n;
				vaddr_t indirect_addr;
				struct blkif_request_segment *seg;

				bmk_memcpy(indirect_req, temp,
						sizeof(blkif_request_indirect_t));

				req->id = indirect_req->id;
				req->operation = indirect_req->operation;

				if (indirect_req->nr_segments > MAX_INDIRECT_SEGMENTS) {
					bmk_printf("Too many indirected segment received %s\n", __func__);
					xbdback_send_reply(xbdi, req->id, req->operation,
							BLKIF_RSP_ERROR);
					xbdi->xbdi_cont = xbdback_co_main_incr;
					return xbdi;
				}

				req->nr_segments = indirect_req->nr_segments;
				req->sector_number = indirect_req->sector_number;
				req->handle = indirect_req->handle;

				if (xbdi->xbdi_xen_indirect_req.nr_segments % 512)
					j = (xbdi->xbdi_xen_indirect_req.nr_segments / 512) + 1;
				else
					j = xbdi->xbdi_xen_indirect_req.nr_segments / 512;

				if (j > BLKIF_MAX_INDIRECT_PAGES_PER_REQUEST) {
					bmk_printf("%s: Too many indirect indirect grefs\n", __func__);
					bmk_platform_halt(NULL);
				}

				for (i = 0; i < j; i++) {
					indirect_addr = (vaddr_t)gntmap_map_grant_refs(
								&xbdi->xbdi_entry_map,
								1,
								&xbdi->xbdi_domid,
								1,
								&xbdi->xbdi_xen_indirect_req.indirect_grefs[i],
								1);
					if (indirect_addr <= 0) {
						bmk_printf("i=%d j=%d gref=%u\n", i, j, 
								xbdi->xbdi_xen_indirect_req.indirect_grefs[i]);
						bmk_platform_halt("xbdback_co_main_loop: indirect grant mapping failed\n");
					}

					for (n = 0; n < xbdi->xbdi_xen_indirect_req.nr_segments; n++) {
						seg = (struct blkif_request_segment*)indirect_addr + n;
						__atomic_load(seg, &xbdi->xbdi_indirect_segments[n],
								__ATOMIC_CONSUME);
					}

					if (gntmap_munmap(&xbdi->xbdi_entry_map, (vaddr_t)indirect_addr, 1) != 0) {
						bmk_printf("xbdback_co_main_loop: unmapping failed\n");
					}
				}
			} else {
				bmk_memcpy(req, temp, sizeof(blkif_request_t));
			}

			break;
		default:
			bmk_platform_halt("xbdback_co_main_loop: We only support native requests for now.\n");
			break;
		}
		__insn_barrier();
		XENPRINTF(("xbdback op %d req_cons 0x%x req_prod 0x%x "
		    "resp_prod 0x%x id %" PRIu64 "\n", req->operation,
			xbdi->xbdi_ring.ring_n.req_cons,
			xbdi->xbdi_req_prod,
			xbdi->xbdi_ring.ring_n.rsp_prod_pvt,
			req->id));
		switch(req->operation) {
		case BLKIF_OP_READ:
		case BLKIF_OP_WRITE:
		case BLKIF_OP_INDIRECT:
			xbdi->xbdi_cont = xbdback_co_io;
			break;
		case BLKIF_OP_FLUSH_DISKCACHE:
			xbdi_get(xbdi);
			xbdi->xbdi_cont = xbdback_co_cache_flush;
			break;
		default:
			xbdback_send_reply(xbdi, req->id, req->operation,
			    BLKIF_RSP_ERROR);
			xbdi->xbdi_cont = xbdback_co_main_incr;
			break;
		}
	} else {
		xbdi->xbdi_cont = xbdback_co_main_done;
	}
	return xbdi;
}

/*
 * Increment consumer index and move on to the next request. In case
 * we want to disconnect, leave continuation now.
 */
static void *
xbdback_co_main_incr(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;
	blkif_back_ring_t *ring = &xbdi->xbdi_ring.ring_n;

	XENPRINTF(("%s\n", __func__));

	ring->req_cons++;

	/*
	 * Do not bother with locking here when checking for xbdi_status: if
	 * we get a transient state, we will get the right value at
	 * the next increment.
	 */
	if (xbdi->xbdi_status == DISCONNECTING)
		xbdi->xbdi_cont = NULL;
	else
		xbdi->xbdi_cont = xbdback_co_main_loop;

	/*
	 * Each time the thread processes a full ring of requests, give
	 * a chance to other threads to process I/Os too
	 */
	if ((ring->req_cons % BLKIF_RING_SIZE) == 0)
		bmk_sched_yield();

	return xbdi;
}

/*
 * Ring processing is over. If there are any I/O still present for this
 * instance, handle them first.
 */
static void *
xbdback_co_main_done(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;

	XENPRINTF(("%s\n", __func__));

	if (xbdi->xbdi_io != NULL) {
		bmk_assert(xbdi->xbdi_io->xio_operation == BLKIF_OP_READ ||
		    xbdi->xbdi_io->xio_operation == BLKIF_OP_WRITE);
		xbdi->xbdi_cont = xbdback_co_map_io;
		xbdi->xbdi_cont_aux = xbdback_co_main_done2;
	} else {
		xbdi->xbdi_cont = xbdback_co_main_done2;
	}
	return xbdi;
}

/*
 * Check for requests in the instance's ring. In case there are, start again
 * from the beginning. If not, stall.
 */
static void *
xbdback_co_main_done2(struct xbdback_instance *xbdi, void *obj)
{
	int work_to_do;

	XENPRINTF(("%s\n", __func__));

	wmb();
	RING_FINAL_CHECK_FOR_REQUESTS(&xbdi->xbdi_ring.ring_n, work_to_do);
	if (work_to_do)
		xbdi->xbdi_cont = xbdback_co_main;
	else
		xbdi->xbdi_cont = NULL;

	return xbdi;
}

/*
 * Frontend requested a cache flush operation.
 */
static void *
xbdback_co_cache_flush(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;

	XENPRINTF(("%s %p %p\n", __func__, xbdi, obj));

	if (xbdi->xbdi_io != NULL) {
		/* Some I/Os are required for this instance. Process them. */
		bmk_assert(xbdi->xbdi_io->xio_operation == BLKIF_OP_READ ||
		    xbdi->xbdi_io->xio_operation == BLKIF_OP_WRITE);
		bmk_assert(__atomic_load_n(&(xbdi)->xbdi_pendingreqs, __ATOMIC_RELAXED) > 0); 
		xbdi->xbdi_cont = xbdback_co_map_io;
		xbdi->xbdi_cont_aux = xbdback_co_cache_flush2;
	} else {
		xbdi->xbdi_cont = xbdback_co_cache_flush2;
	}
	return xbdi;
}

static void *
xbdback_co_cache_flush2(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;
	
	XENPRINTF(("%s %p %p\n", __func__, xbdi, obj));

	if (__atomic_load_n(&(xbdi)->xbdi_pendingreqs, __ATOMIC_RELAXED) > 0) {
		/*
		 * There are pending requests.
		 * Event or iodone() will restart processing
		 */
		xbdi->xbdi_cont = NULL;
		xbdi_put(xbdi);
		return NULL;
	}
	xbdi->xbdi_cont = xbdback_co_cache_doflush;
	return xbdback_pool_get(xbdback_io_pool);
}

/* Start the flush work */
static void *
xbdback_co_cache_doflush(struct xbdback_instance *xbdi, void *obj)
{
	struct xbdback_io *xbd_io;

	XENPRINTF(("%s %p %p\n", __func__, xbdi, obj));

	xbd_io = xbdi->xbdi_io = obj;
	xbd_io->xio_xbdi = xbdi;
	xbd_io->xio_operation = xbdi->xbdi_xen_req.operation == BLKIF_OP_INDIRECT ?
		xbdi->xbdi_xen_indirect_req.indirect_op : xbdi->xbdi_xen_req.operation;
	xbd_io->xio_flush_id = xbdi->xbdi_xen_req.id;
	xbdi->xbdi_cont = xbdback_co_do_io;
	return xbdi;
}

/*
 * A read or write I/O request must be processed. Do some checks first,
 * then get the segment information directly from the ring request.
 */
static void *
xbdback_co_io(struct xbdback_instance *xbdi, void *obj)
{	
	int error;
	blkif_request_t *req;
	uint8_t operation;

	(void)obj;

	XENPRINTF(("%s\n", __func__));

	/* some sanity checks */
	req = &xbdi->xbdi_xen_req;

	if (req->nr_segments < 1 || (req->operation != BLKIF_OP_INDIRECT
		&& req->nr_segments > BLKIF_MAX_SEGMENTS_PER_REQUEST)) {
		bmk_printf("%s Too many sengments received\n", __func__);
		error = BMK_EINVAL;
		goto end;
	}

	bmk_assert(req->nr_segments <= MAX_INDIRECT_SEGMENTS);

	operation = req->operation == BLKIF_OP_INDIRECT ?
		xbdi->xbdi_xen_indirect_req.indirect_op : req->operation;

	bmk_assert(operation == BLKIF_OP_READ || operation == BLKIF_OP_WRITE);
	if (operation == BLKIF_OP_WRITE) {
		if (xbdi->xbdi_ro) {
			error = BMK_EROFS;
			bmk_printf("%s: Write not supported\n", __func__);
			goto end;
		}
	}

	xbdi->xbdi_segno = 0;

	/* copy request segments */
	switch(xbdi->xbdi_proto) {
	case XBDIP_NATIVE:
		/* already copied in xbdback_co_main_loop */

		break;
	case XBDIP_32:
	case XBDIP_64:
		bmk_platform_halt("xbdback_co_io: We only support native requests now\n");
		break;
	}

	xbdi->xbdi_cont = xbdback_co_io_gotreq;
	return xbdback_pool_get(xbdback_request_pool);

 end:
	xbdback_send_reply(xbdi, xbdi->xbdi_xen_req.id,
	    xbdi->xbdi_xen_req.operation, error);
	xbdi->xbdi_cont = xbdback_co_main_incr;
	return xbdi;
}

/*
 * We have fetched segment requests from the ring. In case there are already
 * I/Os prepared for this instance, we can try coalescing the requests
 * with these I/Os.
 */
static void *
xbdback_co_io_gotreq(struct xbdback_instance *xbdi, void *obj) 
{
	struct xbdback_request *xrq;

	XENPRINTF(("%s\n", __func__));

	xrq = xbdi->xbdi_req = obj;
	
	xrq->rq_xbdi = xbdi;
	xrq->rq_iocount = 0;
	xrq->rq_ioerrs = 0;
	xrq->rq_id = xbdi->xbdi_xen_req.id;

	xrq->rq_operation = xbdi->xbdi_xen_req.operation == BLKIF_OP_INDIRECT ?
		xbdi->xbdi_xen_indirect_req.indirect_op : xbdi->xbdi_xen_req.operation;

	bmk_assert(xrq->rq_operation == BLKIF_OP_READ ||
			xrq->rq_operation == BLKIF_OP_WRITE);

	/* 
	 * Request-level reasons not to coalesce: different device,
	 * different op, or noncontiguous disk sectors (vs. previous
	 * request handed to us).
	 */
	xbdi->xbdi_cont = xbdback_co_io_loop;
	if (xbdi->xbdi_io != NULL) {
		struct xbdback_request *last_req;
		last_req = SLIST_FIRST(&xbdi->xbdi_io->xio_rq)->car;
		XENPRINTF(("%s domain %d: hoping for sector %" PRIu64
		    "; got %" PRIu64 "\n", __func__, xbdi->xbdi_domid,
		    xbdi->xbdi_next_sector,
		    xbdi->xbdi_xen_req.sector_number));
		if ((xrq->rq_operation != last_req->rq_operation)
		    || (xbdi->xbdi_xen_req.sector_number !=
		    xbdi->xbdi_next_sector)) {
			XENPRINTF(("%s domain %d: segment break\n",
			    __func__, xbdi->xbdi_domid));
			xbdi->xbdi_next_sector =
			    xbdi->xbdi_xen_req.sector_number;
			bmk_assert(xbdi->xbdi_io->xio_operation == BLKIF_OP_READ ||
			    xbdi->xbdi_io->xio_operation == BLKIF_OP_WRITE);
			xbdi->xbdi_cont_aux = xbdback_co_io_loop;
			xbdi->xbdi_cont = xbdback_co_map_io;
		}
	} else {
		xbdi->xbdi_next_sector = xbdi->xbdi_xen_req.sector_number;
	}
	return xbdi;
}

/* Handle coalescing of multiple segment requests into one I/O work */
static void *
xbdback_co_io_loop(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;
	
	XENPRINTF(("%s\n", __func__));

	bmk_assert(xbdi->xbdi_req->rq_operation == BLKIF_OP_READ ||
	    xbdi->xbdi_req->rq_operation == BLKIF_OP_WRITE);

	if (xbdi->xbdi_segno < xbdi->xbdi_xen_req.nr_segments) {
		uint8_t this_fs, this_ls, last_ls;
		grant_ref_t thisgrt;
		/* 
		 * Segment-level reason to coalesce: handling full
		 * pages, or adjacent sector ranges from the same page
		 * (and yes, this latter does happen).  But not if the
		 * array of client pseudo-physical pages is full.
		 */

		if (xbdi->xbdi_xen_req.operation == BLKIF_OP_INDIRECT) {
			this_fs = xbdi->xbdi_indirect_segments[xbdi->xbdi_segno].first_sect;
			this_ls = xbdi->xbdi_indirect_segments[xbdi->xbdi_segno].last_sect;
			thisgrt = xbdi->xbdi_indirect_segments[xbdi->xbdi_segno].gref;
		} else {
			this_fs = xbdi->xbdi_xen_req.seg[xbdi->xbdi_segno].first_sect;
			this_ls = xbdi->xbdi_xen_req.seg[xbdi->xbdi_segno].last_sect;
			thisgrt = xbdi->xbdi_xen_req.seg[xbdi->xbdi_segno].gref;
		}

		XENPRINTF(("%s domain %d: "
			   "first,last_sect[%d]=0%o,0%o grant=%u\n",
			   __func__, xbdi->xbdi_domid, xbdi->xbdi_segno,
			   this_fs, this_ls,thisgrt));
		last_ls = xbdi->xbdi_last_ls = xbdi->xbdi_this_ls;
		xbdi->xbdi_this_fs = this_fs;
		xbdi->xbdi_this_ls = this_ls;
		xbdi->xbdi_thisgrt = thisgrt;

		if (xbdi->xbdi_io != NULL) {
			if (last_ls == VBD_MAXSECT
				&& this_fs == 0
				&& xbdi->xbdi_xen_req.operation == BLKIF_OP_INDIRECT
				&& xbdi->xbdi_io->xio_nrma
					< MAX_INDIRECT_SEGMENTS) {
				xbdi->xbdi_same_page = 0;
			} else if (last_ls == VBD_MAXSECT
				&& this_fs == 0
				&& xbdi->xbdi_xen_req.operation != BLKIF_OP_INDIRECT
				&& xbdi->xbdi_io->xio_nrma
					< BLKIF_MAX_SEGMENTS_PER_REQUEST) {
				xbdi->xbdi_same_page = 0;
			} else if (last_ls + 1
					== this_fs
#ifdef notyet
				  && (last_fas & ~PAGE_MASK)
					== (this_fas & ~PAGE_MASK)
#else 
				  && 0 /* can't know frame number yet */
#endif
			  ) {
				bmk_printf("xbdback_io: would maybe glue "
			   	    "same page sec %d (%d->%d)\n",
			    	   xbdi->xbdi_segno, this_fs, this_ls);
				bmk_printf("xbdback_io domain %d: glue same "
			    	   "page", xbdi->xbdi_domid);
				panic("notyet!");
				xbdi->xbdi_same_page = 1;
			} else {
				bmk_assert(xbdi->xbdi_io->xio_operation ==
				     BLKIF_OP_READ ||
				    xbdi->xbdi_io->xio_operation ==
				     BLKIF_OP_WRITE);
				xbdi->xbdi_cont_aux = xbdback_co_io_loop;
				xbdi->xbdi_cont = xbdback_co_map_io;
				return xbdi;
			}
		} else
			xbdi->xbdi_same_page = 0;

		if (xbdi->xbdi_io == NULL) {
			xbdi->xbdi_cont = xbdback_co_io_gotio;
			return xbdback_pool_get(xbdback_io_pool);
		} else {
			xbdi->xbdi_cont = xbdback_co_io_gotio2;
		}
	} else {
		/* done with the loop over segments; get next request */
		xbdi->xbdi_cont = xbdback_co_main_incr;
	}
	return xbdi;
}

/* Prepare an I/O buffer for a xbdback instance */
static void *
xbdback_co_io_gotio(struct xbdback_instance *xbdi, void *obj)
{
	struct xbdback_io *xbd_io;
	vaddr_t start_offset; /* start offset in vm area */
	int buf_flags, i;

	XENPRINTF(("%s\n", __func__));

	xbdi_get(xbdi);
 	__atomic_fetch_add(&(xbdi)->xbdi_pendingreqs, 1,  __ATOMIC_ACQ_REL);	
	xbd_io = xbdi->xbdi_io = obj;

	if(xbdi->xbdi_vp == NULL)
		bmk_platform_halt("xbdi->xbdi_vp == NULL\n");

    	rumpuser__hyp.hyp_schedule();
	rump_xbdback_buf_init(xbd_io->xio_buf, xbdi->xbdi_vp,
				MAX_INDIRECT_SEGMENTS);
    	rumpuser__hyp.hyp_unschedule();

	xbd_io->xio_xbdi = xbdi;
	SLIST_INIT(&xbd_io->xio_rq);
	xbd_io->xio_nrma = 0;
	xbd_io->xio_done = 0;
	xbd_io->xio_mapped = 0;
	xbd_io->xio_operation = xbdi->xbdi_xen_req.operation == BLKIF_OP_INDIRECT ?
		xbdi->xbdi_xen_indirect_req.indirect_op : xbdi->xbdi_xen_req.operation;

	start_offset = xbdi->xbdi_this_fs * VBD_BSIZE;
	
	if (xbd_io->xio_operation == BLKIF_OP_WRITE) {
		buf_flags = B_WRITE;
	} else {
		buf_flags = B_READ;
	}

	for( i = 0; i < MAX_INDIRECT_SEGMENTS; i++) {
		xbd_io->xio_buf[i].b_flags = buf_flags;
		xbd_io->xio_buf[i].b_cflags = 0;
		xbd_io->xio_buf[i].b_oflags = 0;
		xbd_io->xio_buf[i].b_iodone = xbdback_iodone;
		xbd_io->xio_buf[i].b_proc = NULL;
		xbd_io->xio_buf[i].b_vp = xbdi->xbdi_vp;
		xbd_io->xio_buf[i].b_dev = xbdi->xbdi_dev;
		xbd_io->xio_buf[i].b_blkno = xbdi->xbdi_next_sector;
		xbd_io->xio_buf[i].b_bcount = 0;
		xbd_io->xio_buf[i].b_data = (void *)start_offset;
		xbd_io->xio_buf[i].b_private = xbd_io;
	}

	xbdi->xbdi_cont = xbdback_co_io_gotio2;
	return xbdi;
}

/* Manage fragments */
static void *
xbdback_co_io_gotio2(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;
	
	XENPRINTF(("%s\n", __func__));

	if (xbdi->xbdi_segno == 0 || SLIST_EMPTY(&xbdi->xbdi_io->xio_rq)) {
		/* if this is the first segment of a new request */
		/* or if it's the first segment of the io */
		xbdi->xbdi_cont = xbdback_co_io_gotfrag;
		return xbdback_pool_get(xbdback_fragment_pool);
	}
	xbdi->xbdi_cont = xbdback_co_io_gotfrag2;
	return xbdi;
}

/* Prepare the instance for its first fragment */
static void *
xbdback_co_io_gotfrag(struct xbdback_instance *xbdi, void *obj)
{
	struct xbdback_fragment *xbd_fr;

	XENPRINTF(("%s\n", __func__));

	xbd_fr = obj;
	xbd_fr->car = xbdi->xbdi_req;
	SLIST_INSERT_HEAD(&xbdi->xbdi_io->xio_rq, xbd_fr, cdr);
	++xbdi->xbdi_req->rq_iocount;

	xbdi->xbdi_cont = xbdback_co_io_gotfrag2;
	return xbdi;
}

/* Last routine to manage segments fragments for one I/O */
static void *
xbdback_co_io_gotfrag2(struct xbdback_instance *xbdi, void *obj)
{
	struct xbdback_io *xbd_io;
	int seg_size;
	uint8_t this_fs, this_ls;

	XENPRINTF(("%s\n", __func__));

	this_fs = xbdi->xbdi_this_fs;
	this_ls = xbdi->xbdi_this_ls;
	xbd_io = xbdi->xbdi_io;
	seg_size = this_ls - this_fs + 1;

	if (seg_size < 0) {
		/*if (ratecheck(&xbdi->xbdi_lasterr_time, &xbdback_err_intvl)) {
			printf("xbdback_io domain %d: negative-size request "
			    "(%d %d)\n",
			    xbdi->xbdi_domid, this_ls, this_fs);
		}*/
		xbdback_io_error(xbdi->xbdi_io, BMK_EINVAL);
		xbdi->xbdi_io = NULL;
		xbdi->xbdi_cont = xbdback_co_main_incr;
		return xbdi;
	}
	
	if (!xbdi->xbdi_same_page) {
		XENPRINTF(("xbdback_io domain %d: appending grant %u\n",
			   xbdi->xbdi_domid, (u_int)xbdi->xbdi_thisgrt));
		xbd_io->xio_gref[xbd_io->xio_nrma++] = xbdi->xbdi_thisgrt;
	}

	xbd_io->xio_buf[xbd_io->xio_nrma - 1].b_bcount += (uint64_t)(seg_size * VBD_BSIZE);
	XENPRINTF(("xbdback_io domain %d: start sect %d size %d\n",
				-           xbdi->xbdi_domid, (int)xbdi->xbdi_next_sector, seg_size));
	
	/* Finally, the end of the segment loop! */
	xbdi->xbdi_next_sector += seg_size;
	++xbdi->xbdi_segno;
	xbdi->xbdi_cont = xbdback_co_io_loop;
	return xbdi;
}

/*
 * Map the different I/O requests in backend's VA space.
 */
static void *
xbdback_co_map_io(struct xbdback_instance *xbdi, void *obj)
{
	(void)obj;

	XENPRINTF(("xbdback_co_map_io domain %d: flush sect %ld size %d ptr 0x%lx\n",
	    xbdi->xbdi_domid, (long)xbdi->xbdi_io->xio_buf[0].b_blkno,
	    (int)xbdi->xbdi_io->xio_buf[0].b_bcount, (long)xbdi->xbdi_io));
	xbdi->xbdi_cont = xbdback_co_do_io;
	return xbdback_map_shm(xbdi->xbdi_io);
}

static void
xbdback_io_error(struct xbdback_io *xbd_io, int error)
{
	bmk_printf("%s\n", __func__);

	xbd_io->xio_buf[0].b_error = error;
	xbdback_iodone(&xbd_io->xio_buf[0]);
}

/*
 * Main xbdback I/O routine. It can either perform a flush operation or
 * schedule a read/write operation.
 */
static void *
xbdback_co_do_io(struct xbdback_instance *xbdi, void *obj)
{
	struct xbdback_io *xbd_io = xbdi->xbdi_io;
	int i, seg_size, next_sector = 0;

	XENPRINTF(("%s\n", __func__));

	switch (xbd_io->xio_operation) {
	case BLKIF_OP_FLUSH_DISKCACHE:
	{
		int error;

		XENPRINTF(("BLKIF_OP_FLUSH_DISKCACHE\n"));
		if(xbdi->xbdi_vp == NULL)
			bmk_platform_halt("xbdi->xbdi_vp == NULL\n");
		
    		rumpuser__hyp.hyp_schedule();
		error = rump_xbdback_vop_ioctl(xbdi->xbdi_vp);
    		rumpuser__hyp.hyp_unschedule();

		if (error) {
			bmk_printf("xbdback %s: DIOCCACHESYNC returned %d\n",
			    xbdi->xbdi_xbusd->xbusd_path, error);
			 if (error == EOPNOTSUPP || error == BMK_ENOTTY)
				error = BLKIF_RSP_EOPNOTSUPP;
			 else
				error = BLKIF_RSP_ERROR;
		} else
			error = BLKIF_RSP_OKAY;
		xbdback_send_reply(xbdi, xbd_io->xio_flush_id,
		    xbd_io->xio_operation, error);
		xbdback_pool_put(xbdback_io_pool, xbd_io);
		xbdi_put(xbdi);
		xbdi->xbdi_io = NULL;
		xbdi->xbdi_cont = xbdback_co_main_incr;
		return xbdi;
	}
	case BLKIF_OP_READ:
	case BLKIF_OP_WRITE:
		XENPRINTF(("BLKIF_OP_R/W\n"));

		next_sector = 0;
		for(i = 0; i < xbd_io->xio_nrma; i++) {
			xbd_io->xio_buf[i].b_data = (void *)
		    		((vaddr_t)xbd_io->xio_buf[i].b_data 
				 + xbd_io->xio_vaddr[i]);

			xbd_io->xio_buf[i].b_blkno += next_sector;
			seg_size =  xbd_io->xio_buf[i].b_bcount / VBD_BSIZE;
			next_sector += seg_size;
		}
#ifdef DIAGNOSTIC
		{
		vaddr_t bdata = (vaddr_t)xbd_io->xio_buf.b_data;
		int nsegs =
		    ((((bdata + xbd_io->xio_buf.b_bcount - 1) & ~PAGE_MASK) -
		    (bdata & ~PAGE_MASK)) >> PAGE_SHIFT) + 1;
		if ((bdata & ~PAGE_MASK) != (xbd_io->xio_vaddr & ~PAGE_MASK)) {
			printf("xbdback_co_do_io: vaddr %#" PRIxVADDR
			    " bdata %#" PRIxVADDR "\n",
			    xbd_io->xio_vaddr, bdata);
			panic("xbdback_co_do_io: bdata page change");
		}
		if (nsegs > xbd_io->xio_nrma) {
			printf("xbdback_co_do_io: vaddr %#" PRIxVADDR
			    " bcount %#x doesn't fit in %d pages\n",
			    bdata, xbd_io->xio_buf.b_bcount, xbd_io->xio_nrma);
			panic("xbdback_co_do_io: not enough pages");
		}
		}
#endif
    		rumpuser__hyp.hyp_schedule();
		rump_xbdback_bdev_strategy(xbd_io->xio_buf, xbd_io->xio_nrma);
    		rumpuser__hyp.hyp_unschedule();
	
		/* will call xbdback_iodone() asynchronously when done */
		xbdi->xbdi_io = NULL;
		xbdi->xbdi_cont = xbdi->xbdi_cont_aux;
		return xbdi;
	default:
		/* Should never happen */
		bmk_printf("xbdback_co_do_io: unsupported operation %d",
		    xbd_io->xio_operation);
		bmk_platform_halt(NULL);
		return NULL;
	}
}

void VIFHYPER_IODONE(struct buf *bp)
{
	int nlocks;

        rumpkern_unsched(&nlocks, NULL);
	xbdback_iodone(bp);
        rumpkern_sched(nlocks, NULL);
}

/*
 * Called from softint(9) context when an I/O is done: for each request, send
 * back the associated reply to the domain.
 *
 * This gets reused by xbdback_io_error to report errors from other sources.
 */
static void
xbdback_iodone(struct buf *bp)
{
	struct xbdback_io *xbd_io;
	struct xbdback_instance *xbdi;
	int errp;

	xbd_io = bp->b_private;
	xbdi = xbd_io->xio_xbdi;

	XENPRINTF(("%s %d: iodone ptr 0x%lx\n",
		   __func__, xbdi->xbdi_domid, (long)xbd_io));

	xbd_io->xio_done++;
	if(xbd_io->xio_done != xbd_io->xio_nrma)
		return;

	if (xbd_io->xio_mapped == 1)
		xbdback_unmap_shm(xbd_io);
	else
		bmk_printf("I/O not mapped\n");

	if (bp->b_error != 0) {
		bmk_printf("xbd IO domain %d: error %d\n",
		       xbdi->xbdi_domid, bp->b_error);
		errp = 1;
	} else
		errp = 0;

	/* for each constituent xbd request */
	while(!SLIST_EMPTY(&xbd_io->xio_rq)) {
		struct xbdback_fragment *xbd_fr;
		struct xbdback_request *xbd_req;
		struct xbdback_instance *rxbdi __diagused;
		int error;

		xbd_fr = SLIST_FIRST(&xbd_io->xio_rq);
		xbd_req = xbd_fr->car;
		SLIST_REMOVE_HEAD(&xbd_io->xio_rq, cdr);
		xbdback_pool_put(xbdback_fragment_pool, xbd_fr);
		
		if (errp)
			++xbd_req->rq_ioerrs;
		
		/* finalize it only if this was its last I/O */
		if (--xbd_req->rq_iocount > 0)
			continue;

		rxbdi = xbd_req->rq_xbdi;
		bmk_assert(xbdi == rxbdi);
		
		error = xbd_req->rq_ioerrs > 0
		    ? BLKIF_RSP_ERROR
		    : BLKIF_RSP_OKAY;

		XENPRINTF(("xbdback_io domain %d: end request %"PRIu64
		    " error=%d\n",
		    xbdi->xbdi_domid, xbd_req->rq_id, error));

		xbdback_send_reply(xbdi, xbd_req->rq_id,
		    xbd_req->rq_operation, error);

		xbdback_pool_put(xbdback_request_pool, xbd_req);
	}
	xbdi_put(xbdi);
	__atomic_fetch_sub(&(xbdi)->xbdi_pendingreqs, 1,  __ATOMIC_ACQ_REL);
 
	rumpuser__hyp.hyp_schedule();
	rump_xbdback_buf_destroy(xbd_io->xio_buf, MAX_INDIRECT_SEGMENTS);
    	rumpuser__hyp.hyp_unschedule();

	xbdback_pool_put(xbdback_io_pool, xbd_io);
	xbdback_wakeup_thread(xbdi);
}

/*
 * Wake up the per xbdback instance thread.
 */
static void
xbdback_wakeup_thread(struct xbdback_instance *xbdi)
{

	bmk_platform_splhigh();
	spin_lock(&xbdi->xbdi_lock);
	/* only set RUN state when we are WAITING for work */
	if (xbdi->xbdi_status == WAITING)
	       xbdi->xbdi_status = RUN;
	xbdback_sched_wake(&xbdi->xbdi_thread_blk, xbdi->xbdi_thread);
	spin_unlock(&xbdi->xbdi_lock);
	bmk_platform_splx(0);
}

/*
 * called once a request has completed. Place the reply in the ring and
 * notify the guest OS.
 */
static void
xbdback_send_reply(struct xbdback_instance *xbdi, uint64_t id,
    int op, int status)
{
	blkif_response_t *resp_n;
	int notify;

	XENPRINTF(("%s: status %d op %d\n", __func__, status, op));
	/*
	 * The ring can be accessed by the xbdback thread, xbdback_iodone()
	 * handler, or any handler that triggered the shm callback. So
	 * protect ring access via the xbdi_lock spin lock.
	 */
	bmk_platform_splhigh();
	spin_lock(&xbdi->xbdi_lock);
	XENPRINTF(("%s: inside spin lock\n", __func__));

	switch (xbdi->xbdi_proto) {
	case XBDIP_NATIVE:
		resp_n = RING_GET_RESPONSE(&xbdi->xbdi_ring.ring_n,
		    xbdi->xbdi_ring.ring_n.rsp_prod_pvt);
		resp_n->id        = id;
		resp_n->operation = op;
		resp_n->status    = status;
		break;
	case XBDIP_32:
	case XBDIP_64:
		bmk_platform_halt("xbdback_send_reply: \
				We only support native protocol for now\n");
		break;
	default:
		bmk_platform_halt("xbdback_send_reply: Unknown protocol\n");

	}
	xbdi->xbdi_ring.ring_n.rsp_prod_pvt++;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&xbdi->xbdi_ring.ring_n, notify);
	XENPRINTF(("%s: outside spin lock\n", __func__));
	spin_unlock(&xbdi->xbdi_lock);

	if (notify) {
		XENPRINTF(("%s notify %d\n", __func__, xbdi->xbdi_domid));
		minios_notify_remote_via_evtchn(xbdi->xbdi_evtchn);
	}
	bmk_platform_splx(0);
}

static vaddr_t get_page_from_gref_table(grant_ref_t ref, uint8_t op)
{
	if(ref >= GREF_TABLE_SIZE) {
		bmk_printf("%s: very big gref = %u", __func__, ref);
		bmk_platform_halt(NULL);
	}

	//if(gref_table[ref].op != op)
	//	bmk_printf("Warning: Operation didn't match\n");

	return gref_table[ref].page;
}

static void put_page_to_gref_table(grant_ref_t ref, vaddr_t page, uint8_t op)
{
	if(ref >= GREF_TABLE_SIZE) {
		bmk_printf("%s: very big gref = %u", __func__, ref);
		bmk_platform_halt(NULL);
	}

	gref_table[ref].page = page;
	gref_table[ref].op = op;
}

/*
 * Map multiple entries of an I/O request into backend's VA space.
 * The xbd_io->xio_gref array has to be filled out by the caller.
 */
static void *
xbdback_map_shm(struct xbdback_io *xbd_io)
{
	struct xbdback_instance *xbdi;
	struct xbdback_request *xbd_rq;
	int error, i;

	XENPRINTF(("%s\n", __func__));

#ifdef XENDEBUG_VBD
	printf("xbdback_map_shm map grant ");
	for (i = 0; i < xbd_io->xio_nrma; i++) {
		printf("%u ", (u_int)xbd_io->xio_gref[i]);
	}
#endif

	bmk_assert(xbd_io->xio_mapped == 0);

	xbdi = xbd_io->xio_xbdi;
	xbd_rq = SLIST_FIRST(&xbd_io->xio_rq)->car;

	for(i = 0; i < xbd_io->xio_nrma; i++) {
		if (xbdi->persistent == 1)
			xbd_io->xio_vaddr[i] = get_page_from_gref_table(
					xbd_io->xio_gref[i],
					(xbd_rq->rq_operation == BLKIF_OP_WRITE) ? 1 : 0);
	
		if(xbd_io->xio_vaddr[i] == 0)
			xbd_io->xio_vaddr[i] = (vaddr_t)gntmap_map_grant_refs(
				&xbdi->xbdi_entry_map,
				1,
				&xbdi->xbdi_domid,
				1,
				&xbd_io->xio_gref[i],
				1);

		if(xbd_io->xio_vaddr[i] > 0)
			error = 0;
		else {
			error = xbd_io->xio_vaddr[i];
			break;
		}
	}

	switch(error) {
	case 0:
#ifdef XENDEBUG_VBD
		printf("handle ");
		for (i = 0; i < xbd_io->xio_nrma; i++) {
			printf("%u ", (u_int)xbd_io->xio_gh[i]);
		}
		printf("\n");
#endif
		xbd_io->xio_mapped = 1;
		return xbdi;
	case -BMK_ENOMEM:
		bmk_platform_halt("gntmap_map_grant_refs failed\n");
		return NULL;

	default:
		bmk_printf("xbdback_map_shm: xen_shm error %d ", error);
		xbdback_io_error(xbdi->xbdi_io, error);
		xbdi->xbdi_io = NULL;
		xbdi->xbdi_cont = xbdi->xbdi_cont_aux;
		return xbdi;
	}
}

/* unmap a request from our virtual address space (request is done) */
static void
xbdback_unmap_shm(struct xbdback_io *xbd_io)
{
	int j;
	struct xbdback_instance *xbdi = xbd_io->xio_xbdi; /* our xbd instance */
	struct xbdback_request *xbd_rq = SLIST_FIRST(&xbd_io->xio_rq)->car;
	XENPRINTF(("%s\n", __func__));

#ifdef XENDEBUG_VBD
	int i;
	printf("xbdback_unmap_shm handle ");
	for (i = 0; i < xbd_io->xio_nrma; i++) {
		printf("%u ", (u_int)xbd_io->xio_gh[i]);
	}
	printf("\n");
#endif

	bmk_assert(xbd_io->xio_mapped == 1);
	xbd_io->xio_mapped = 0;

	for (j = 0; j < xbd_io->xio_nrma; j++) {
		if (xbdi->persistent == 1) {
			put_page_to_gref_table(xbd_io->xio_gref[j],
					xbd_io->xio_vaddr[j],
					(xbd_rq->rq_operation == BLKIF_OP_WRITE) ? 1 : 0);
		}
		else if (gntmap_munmap(&xbdi->xbdi_entry_map, *xbd_io->xio_vaddr,
			       1) != 0) {
			bmk_printf("xbdback_unmap_shm: unmapping failed\n");
		}
	}
}

static struct xbdback_pool *create_pool(uint64_t capacity) {
    struct xbdback_pool *pool = (struct xbdback_pool *)bmk_memcalloc(1, 
		    	sizeof(struct xbdback_pool), BMK_MEMWHO_WIREDBMK);
    pool->capacity = capacity;
    pool->front = pool->size = 0;
    pool->rear = capacity - 1;
    pool->array = (void *)bmk_memcalloc(1, sizeof(void*)*pool->capacity, 
		    	BMK_MEMWHO_WIREDBMK);

    return pool;
}

static inline int is_full(struct xbdback_pool *pool) {
    return (pool->size == pool->capacity);
}

static inline int is_empty(struct xbdback_pool *pool) { return (pool->size == 0); }

/* Restore memory to a pool */
static void xbdback_pool_put(struct xbdback_pool *pool, void *item) {
     int i;

try_put:
     if (is_full(pool)) {
	    bmk_printf("Pool Full\n");
	    for(i = 0; i < 10000; i++)
		    bmk_sched_yield();
	    goto try_put;
    }

    pool->rear = (pool->rear + 1) % pool->capacity;
    pool->array[pool->rear] = item;
    pool->size = pool->size + 1;
}

/* Obtain memory from a pool */
static void* xbdback_pool_get(struct xbdback_pool *pool) {
    int i;

try_get:
    if (is_empty(pool)) {
	    bmk_printf("Pool Empty\n");
	    for(i = 0; i < 10000; i++)
		    bmk_sched_yield();
	    goto try_get;
    }
	    
    void *item = pool->array[pool->front];
    pool->front = (pool->front + 1) % pool->capacity;
    pool->size = pool->size - 1;

    return item;
}

#if 0
static void destroy_pool(struct xbdback_pool *pool) {
    while(!is_empty(pool))
	    bmk_memfree(xbdback_pool_get(pool), BMK_MEMWHO_WIREDBMK);

    bmk_memfree(pool->array, BMK_MEMWHO_WIREDBMK);
}
#endif

/*
 * Trampoline routine. Calls continuations in a loop and only exits when
 * either the returned object or the next callback is NULL.
 */
static void
xbdback_trampoline(struct xbdback_instance *xbdi, void *obj)
{
	xbdback_cont_t cont;

	while(obj != NULL && xbdi->xbdi_cont != NULL) {
		cont = xbdi->xbdi_cont;
#ifdef DIAGNOSTIC
		xbdi->xbdi_cont = (xbdback_cont_t)0xDEADBEEF;
#endif
		obj = (*cont)(xbdi, obj);
#ifdef DIAGNOSTIC
		if (xbdi->xbdi_cont == (xbdback_cont_t)0xDEADBEEF) {
			printf("xbdback_trampoline: 0x%lx didn't set "
			       "xbdi->xbdi_cont!\n", (long)cont);
			panic("xbdback_trampoline: bad continuation");
		}
#endif
	}
}
