/* 
 ****************************************************************************
 * (C) 2006 - Cambridge University
 ****************************************************************************
 *
 *        File: xenbus.c
 *      Author: Steven Smith (sos22@cam.ac.uk) 
 *     Changes: Grzegorz Milos (gm281@cam.ac.uk)
 *     Changes: John D. Ramsdell
 *              
 *        Date: Jun 2006, chages Aug 2005
 * 
 * Environment: Xen Minimal OS
 * Description: Minimal implementation of xenbus
 *
 ****************************************************************************
 **/
#include <mini-os/os.h>
#include <mini-os/mm.h>
#include <mini-os/lib.h>
#include <mini-os/xenbus.h>
#include <mini-os/events.h>
#include <mini-os/wait.h>
#include <xen/io/xs_wire.h>
#include <mini-os/spinlock.h>

#include <xen/hvm/params.h>

#define _BMK_PRINTF_VA
#include <bmk-core/memalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/string.h>
#include <bmk-core/platform.h>

#define min(x,y) ({                       \
        typeof(x) tmpx = (x);                 \
        typeof(y) tmpy = (y);                 \
        tmpx < tmpy ? tmpx : tmpy;            \
        })

#ifdef XENBUS_DEBUG
#define DEBUG(_f, _a...) \
    bmk_printf("MINI_OS(file=xenbus.c, line=%d) " _f , __LINE__, ## _a)
#else
#define DEBUG(_f, _a...)    ((void)0)
#endif

#define XENBUS_BUG_ON(cond) bmk_assert(!(cond))

static uint32_t store_evtchn;

static struct xenstore_domain_interface *xenstore_buf;
static DECLARE_WAIT_QUEUE_HEAD(xb_waitq);
static spinlock_t xb_lock = SPIN_LOCK_UNLOCKED; /* protects xenbus req ring */

struct xenbus_event_queue xenbus_default_watch_queue;
static MINIOS_LIST_HEAD(, xenbus_watch) watches;
struct xenbus_req_info 
{
    struct xenbus_event_queue *reply_queue; /* non-0 iff in use */
    struct xenbus_event *for_queue;
};


spinlock_t xenbus_req_lock = SPIN_LOCK_UNLOCKED;
/*
 * This lock protects:
 *    the xenbus request ring
 *    req_info[]
 *    all live struct xenbus_event_queue (including xenbus_default_watch_queue)
 *    nr_live_reqs
 *    req_wq
 *    watches
 */

static inline void wake_up(struct wait_queue_head *head)
{
    struct wait_queue *elem, *tmp;
    STAILQ_FOREACH_SAFE(elem, head, thread_list, tmp)
        bmk_sched_wake(elem->thread);
}

static void queue_wakeup(struct xenbus_event_queue *queue)
{
    wake_up(&queue->waitq);
}

void xenbus_event_queue_init(struct xenbus_event_queue *queue)
{
    MINIOS_STAILQ_INIT(&queue->events);
    queue->wakeup = queue_wakeup;
    minios_init_waitqueue_head(&queue->waitq);
}

static struct xenbus_event *remove_event(struct xenbus_event_queue *queue)
{
    /* Called with lock held */
    struct xenbus_event *event;

    event = MINIOS_STAILQ_FIRST(&queue->events);
    if (!event)
        goto out;
    MINIOS_STAILQ_REMOVE_HEAD(&queue->events, entry);

 out:
    return event;
}

static void queue_event(struct xenbus_event_queue *queue,
                        struct xenbus_event *event)
{
    /* Called with lock held */
    MINIOS_STAILQ_INSERT_TAIL(&queue->events, event, entry);
    queue->wakeup(queue);
}

static void
unlock_req_callback(struct bmk_thread *prev, struct bmk_block_data *data)
{
    spin_unlock(&xenbus_req_lock);
}

static struct bmk_block_data block_req = { .callback = unlock_req_callback };

static void
unlock_xb_callback(struct bmk_thread *prev, struct bmk_block_data *data)
{
    spin_unlock(&xb_lock);
    bmk_platform_splx(0);
}

static struct bmk_block_data block_xb = { .callback = unlock_xb_callback };


static struct xenbus_event *await_event(struct xenbus_event_queue *queue)
{
    struct xenbus_event *event;
    DEFINE_WAIT(w);
    spin_lock(&xenbus_req_lock);
    while (!(event = remove_event(queue))) {
        minios_add_wait_queue(&queue->waitq, &w);
        bmk_sched_blockprepare();
        bmk_sched_block(&block_req);
        spin_lock(&xenbus_req_lock);
    }
    minios_remove_wait_queue(&queue->waitq, &w);
    spin_unlock(&xenbus_req_lock);
    return event;
}

#define wait_event_req(wq, condition) do {                      \
    DEFINE_WAIT(__wait);                                        \
    spin_lock(&xenbus_req_lock);                                \
    while (!(condition)) {                                      \
        minios_add_wait_queue(&wq, &__wait);                    \
        bmk_sched_blockprepare();                               \
        bmk_sched_block(&block_req);                            \
        spin_lock(&xenbus_req_lock);                            \
    }                                                           \
    minios_remove_wait_queue(&wq, &__wait);                     \
    spin_unlock(&xenbus_req_lock);                              \
} while(0)

#define wait_event_xb(wq, condition) do {                       \
    DEFINE_WAIT(__wait);                                        \
    bmk_platform_splhigh();                                     \
    spin_lock(&xb_lock);                                        \
    while (!(condition)) {                                      \
        minios_add_wait_queue(&wq, &__wait);                    \
        bmk_sched_blockprepare();                               \
        bmk_sched_block(&block_xb);                             \
        bmk_platform_splhigh();                                 \
        spin_lock(&xb_lock);                                    \
    }                                                           \
    minios_remove_wait_queue(&wq, &__wait);                     \
    spin_unlock(&xb_lock);                                      \
    bmk_platform_splx(0);                                       \
} while(0)


#define NR_REQS 32
static struct xenbus_req_info req_info[NR_REQS];

static void memcpy_from_ring(const void *Ring,
        void *Dest,
        int off,
        int len)
{
    int c1, c2;
    const char *ring = Ring;
    char *dest = Dest;
    c1 = min(len, XENSTORE_RING_SIZE - off);
    c2 = len - c1;
    bmk_memcpy(dest, ring + off, c1);
    bmk_memcpy(dest + c1, ring, c2);
}

char **xenbus_wait_for_watch_return(struct xenbus_event_queue *queue)
{
    struct xenbus_event *event;
    if (!queue)
        queue = &xenbus_default_watch_queue;
    event = await_event(queue);
    return &event->path;
}

void xenbus_wait_for_watch(struct xenbus_event_queue *queue)
{
    char **ret;
    if (!queue)
        queue = &xenbus_default_watch_queue;
    ret = xenbus_wait_for_watch_return(queue);
    if (ret)
        bmk_memfree(ret, BMK_MEMWHO_WIREDBMK);
    else
        bmk_printf("unexpected path returned by watch\n");
}

char* xenbus_wait_for_value(const char* path, const char* value, struct xenbus_event_queue *queue)
{
    if (!queue)
        queue = &xenbus_default_watch_queue;
    for(;;)
    {
        char *res, *msg;
        int r;

        msg = xenbus_read(XBT_NIL, path, &res);
        if(msg) return msg;

        r = bmk_strcmp(value,res);
        bmk_memfree(res, BMK_MEMWHO_WIREDBMK);

        if(r==0) break;
        else xenbus_wait_for_watch(queue);
    }
    return NULL;
}

char *xenbus_switch_state(xenbus_transaction_t xbt, const char* path, XenbusState state)
{
    char *current_state;
    char *msg = NULL;
    char *msg2 = NULL;
    char value[2];
    XenbusState rs;
    int xbt_flag = 0;
    int retry = 0;

    do {
        if (xbt == XBT_NIL) {
            msg = xenbus_transaction_start(&xbt);
            if (msg) goto exit;
            xbt_flag = 1;
        }

        msg = xenbus_read(xbt, path, &current_state);
        if (msg) goto exit;

        rs = (XenbusState) (current_state[0] - '0');
        bmk_memfree(current_state, BMK_MEMWHO_WIREDBMK);
        if (rs == state) {
            msg = NULL;
            goto exit;
        }

        bmk_snprintf(value, 2, "%d", state);
        msg = xenbus_write(xbt, path, value);

exit:
        if (xbt_flag) {
            msg2 = xenbus_transaction_end(xbt, 0, &retry);
            xbt = XBT_NIL;
        }
        if (msg == NULL && msg2 != NULL)
            msg = msg2;
    } while (retry);

    return msg;
}

char *xenbus_wait_for_state_change(const char* path, XenbusState *state, struct xenbus_event_queue *queue)
{
    if (!queue)
        queue = &xenbus_default_watch_queue;
    for(;;)
    {
        char *res, *msg;
        XenbusState rs;

        msg = xenbus_read(XBT_NIL, path, &res);
        if(msg) return msg;

        rs = (XenbusState) (res[0] - 48);
        bmk_memfree(res, BMK_MEMWHO_WIREDBMK);

        if (rs == *state)
            xenbus_wait_for_watch(queue);
        else {
            *state = rs;
            break;
        }
    }
    return NULL;
}


static void xenbus_thread_func(void *ign)
{
    struct xsd_sockmsg msg;
    unsigned prod = xenstore_buf->rsp_prod;

    for (;;) 
    {
        wait_event_xb(xb_waitq, prod != xenstore_buf->rsp_prod);
        while (1) 
        {
            prod = xenstore_buf->rsp_prod;
            DEBUG("Rsp_cons %d, rsp_prod %d.\n", xenstore_buf->rsp_cons,
                    xenstore_buf->rsp_prod);
            if (xenstore_buf->rsp_prod - xenstore_buf->rsp_cons < sizeof(msg)) {
                minios_notify_remote_via_evtchn(store_evtchn);
                break;
            }
            rmb();
            memcpy_from_ring(xenstore_buf->rsp,
                    &msg,
                    MASK_XENSTORE_IDX(xenstore_buf->rsp_cons),
                    sizeof(msg));
            DEBUG("Msg len %d, %d avail, id %d.\n",
                    msg.len + sizeof(msg),
                    xenstore_buf->rsp_prod - xenstore_buf->rsp_cons,
                    msg.req_id);
            if (xenstore_buf->rsp_prod - xenstore_buf->rsp_cons <
                    sizeof(msg) + msg.len) {
                minios_notify_remote_via_evtchn(store_evtchn);
                break;
            }

            DEBUG("Message is good.\n");

            if(msg.type == XS_WATCH_EVENT)
            {
		struct xenbus_event *event
		    = bmk_xmalloc_bmk(sizeof(*event) + msg.len);
                struct xenbus_event_queue *events = NULL;
		char *data = (char*)event + sizeof(*event);
                struct xenbus_watch *watch;

                memcpy_from_ring(xenstore_buf->rsp,
		    data,
                    MASK_XENSTORE_IDX(xenstore_buf->rsp_cons + sizeof(msg)),
                    msg.len);

		event->path = data;
		event->token = event->path + bmk_strlen(event->path) + 1;

                mb();
                xenstore_buf->rsp_cons += msg.len + sizeof(msg);

                spin_lock(&xenbus_req_lock);

                MINIOS_LIST_FOREACH(watch, &watches, entry)
                    if (!bmk_strcmp(watch->token, event->token)) {
                        event->watch = watch;
                        events = watch->events;
                        break;
                    }

                if (events) {
                    queue_event(events, event);
                } else {
                    bmk_printf("unexpected watch token %s\n", event->token);
                    bmk_memfree(event, BMK_MEMWHO_WIREDBMK);
                }

                spin_unlock(&xenbus_req_lock);
            }

            else
            {
                req_info[msg.req_id].for_queue->reply =
                    bmk_xmalloc_bmk(sizeof(msg) + msg.len);
                memcpy_from_ring(xenstore_buf->rsp,
                    req_info[msg.req_id].for_queue->reply,
                    MASK_XENSTORE_IDX(xenstore_buf->rsp_cons),
                    msg.len + sizeof(msg));
                mb();
                xenstore_buf->rsp_cons += msg.len + sizeof(msg);
                spin_lock(&xenbus_req_lock);
                queue_event(req_info[msg.req_id].reply_queue,
                            req_info[msg.req_id].for_queue);
                spin_unlock(&xenbus_req_lock);
            }

            wmb();
            minios_notify_remote_via_evtchn(store_evtchn);
        }
    }
}

static void xenbus_evtchn_handler(evtchn_port_t port, struct pt_regs *regs,
				  void *ign)
{
    spin_lock(&xb_lock);
    wake_up(&xb_waitq);
    spin_unlock(&xb_lock);
}

static int nr_live_reqs;
static DECLARE_WAIT_QUEUE_HEAD(req_wq);

/* Release a xenbus identifier */
void xenbus_id_release(int id)
{
    XENBUS_BUG_ON(!req_info[id].reply_queue);
    spin_lock(&xenbus_req_lock);
    req_info[id].reply_queue = 0;
    nr_live_reqs--;
    if (nr_live_reqs == NR_REQS - 1)
        wake_up(&req_wq);
    spin_unlock(&xenbus_req_lock);
}

int xenbus_id_allocate(struct xenbus_event_queue *reply_queue,
                       struct xenbus_event *for_queue)
{
    static int probe;
    int o_probe;

    while (1) 
    {
        spin_lock(&xenbus_req_lock);
        if (nr_live_reqs < NR_REQS)
            break;
        spin_unlock(&xenbus_req_lock);
        wait_event_req(req_wq, (nr_live_reqs < NR_REQS));
    }

    o_probe = probe;
    for (;;) 
    {
        if (!req_info[o_probe].reply_queue)
            break;
        o_probe = (o_probe + 1) % NR_REQS;
        XENBUS_BUG_ON(o_probe == probe);
    }
    nr_live_reqs++;
    req_info[o_probe].reply_queue = reply_queue;
    req_info[o_probe].for_queue = for_queue;
    probe = (o_probe + 1) % NR_REQS;
    spin_unlock(&xenbus_req_lock);

    return o_probe;
}

void xenbus_watch_init(struct xenbus_watch *watch)
{
    watch->token = 0;
}

void xenbus_watch_prepare(struct xenbus_watch *watch)
{
    XENBUS_BUG_ON(!watch->events);
    size_t size = sizeof(void*)*2 + 5;
    watch->token = bmk_xmalloc_bmk(size);
    int r = bmk_snprintf(watch->token,size,"*%p",(void*)watch);
    XENBUS_BUG_ON(!(r > 0 && r < size));
    spin_lock(&xenbus_req_lock);
    MINIOS_LIST_INSERT_HEAD(&watches, watch, entry);
    spin_unlock(&xenbus_req_lock);
}

void xenbus_watch_release(struct xenbus_watch *watch)
{
    if (!watch->token)
        return;
    spin_lock(&xenbus_req_lock);
    MINIOS_LIST_REMOVE(watch, entry);
    spin_unlock(&xenbus_req_lock);
    bmk_memfree(watch->token, BMK_MEMWHO_WIREDBMK);
    watch->token = 0;
}

/* Initialise xenbus. */
void init_xenbus(void)
{
    struct xen_hvm_param hvpar;
    xen_pfn_t store_mfn;
    int err;

    DEBUG("init_xenbus called.\n");
    hvpar.domid = DOMID_SELF;
    hvpar.index = HVM_PARAM_STORE_EVTCHN;
    err = HYPERVISOR_hvm_op(HVMOP_get_param, &hvpar);
    XENBUS_BUG_ON(err < 0);
    store_evtchn = (uint32_t) hvpar.value;
    hvpar.domid = DOMID_SELF;
    hvpar.index = HVM_PARAM_STORE_PFN;
    err = HYPERVISOR_hvm_op(HVMOP_get_param, &hvpar);
    XENBUS_BUG_ON(err < 0);
    store_mfn = (xen_pfn_t) hvpar.value;
    xenstore_buf = (struct xenstore_domain_interface *) (store_mfn << PAGE_SHIFT);
    xenbus_event_queue_init(&xenbus_default_watch_queue);
    bmk_sched_create("xenstore", NULL, 0, -1, xenbus_thread_func, NULL,
      NULL, 0);
    DEBUG("buf at %p.\n", xenstore_buf);
    err = minios_bind_evtchn(store_evtchn,
		      xenbus_evtchn_handler,
              NULL);
    minios_unmask_evtchn(store_evtchn);
    bmk_printf("xenbus initialised on irq %d mfn %#lx\n",
	   err, store_mfn);
}

void fini_xenbus(void)
{
}

void xenbus_xb_write(int type, int req_id, xenbus_transaction_t trans_id,
		     const struct write_req *req, int nr_reqs)
{
    XENSTORE_RING_IDX prod;
    int r;
    int len = 0;
    const struct write_req *cur_req;
    int req_off;
    int total_off;
    int this_chunk;
    struct xsd_sockmsg m = {.type = type, .req_id = req_id,
        .tx_id = trans_id };
    struct write_req header_req = { &m, sizeof(m) };

    for (r = 0; r < nr_reqs; r++)
        len += req[r].len;
    m.len = len;
    len += sizeof(m);

    cur_req = &header_req;

    XENBUS_BUG_ON(len > XENSTORE_RING_SIZE);

    bmk_platform_splhigh();
    spin_lock(&xb_lock);
    /* Wait for the ring to drain to the point where we can send the
       message. */
    prod = xenstore_buf->req_prod;
    if (prod + len - xenstore_buf->req_cons > XENSTORE_RING_SIZE) 
    {
        /* Wait for there to be space on the ring */
        DEBUG("prod %d, len %d, cons %d, size %d; waiting.\n",
                prod, len, xenstore_buf->req_cons, XENSTORE_RING_SIZE);
        spin_unlock(&xb_lock);
        bmk_platform_splx(0);
        wait_event_xb(xb_waitq,
                xenstore_buf->req_prod + len - xenstore_buf->req_cons <=
                XENSTORE_RING_SIZE);
        bmk_platform_splhigh();
        spin_lock(&xb_lock);
        DEBUG("Back from wait.\n");
        prod = xenstore_buf->req_prod;
    }

    /* We're now guaranteed to be able to send the message without
       overflowing the ring.  Do so. */
    total_off = 0;
    req_off = 0;
    while (total_off < len) 
    {
        this_chunk = min(cur_req->len - req_off,
                XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(prod));
        bmk_memcpy((char *)xenstore_buf->req + MASK_XENSTORE_IDX(prod),
                (char *)cur_req->data + req_off, this_chunk);
        prod += this_chunk;
        req_off += this_chunk;
        total_off += this_chunk;
        if (req_off == cur_req->len) 
        {
            req_off = 0;
            if (cur_req == &header_req)
                cur_req = req;
            else
                cur_req++;
        }
    }

    DEBUG("Complete main loop of xb_write.\n");
    XENBUS_BUG_ON(req_off != 0);
    XENBUS_BUG_ON(total_off != len);
    XENBUS_BUG_ON(prod > xenstore_buf->req_cons + XENSTORE_RING_SIZE);

    /* Remote must see entire message before updating indexes */
    wmb();

    xenstore_buf->req_prod += len;
    spin_unlock(&xb_lock);
    bmk_platform_splx(0);

    /* Send evtchn to notify remote */
    minios_notify_remote_via_evtchn(store_evtchn);
}

/* Send a mesasge to xenbus, in the same fashion as xb_write, and
   block waiting for a reply.  The reply is malloced and should be
   freed by the caller. */
struct xsd_sockmsg *
xenbus_msg_reply(int type,
		 xenbus_transaction_t trans,
		 struct write_req *io,
		 int nr_reqs)
{
    int id;
    struct xsd_sockmsg *rep;
    struct xenbus_event_queue queue;
    struct xenbus_event event_buf;

    xenbus_event_queue_init(&queue);

    id = xenbus_id_allocate(&queue,&event_buf);

    xenbus_xb_write(type, id, trans, io, nr_reqs);

    struct xenbus_event *event = await_event(&queue);
    XENBUS_BUG_ON(event != &event_buf);

    rep = req_info[id].for_queue->reply;
    XENBUS_BUG_ON(rep->req_id != id);
    xenbus_id_release(id);
    return rep;
}

void xenbus_free(void *p) { bmk_memfree(p, BMK_MEMWHO_WIREDBMK); }

static char *errmsg(struct xsd_sockmsg *rep)
{
    char *res;
    if (!rep) {
	char msg[] = "No reply";
	size_t len = bmk_strlen(msg) + 1;
	return bmk_memcpy(bmk_xmalloc_bmk(len), msg, len);
    }
    if (rep->type != XS_ERROR)
	return NULL;
    res = bmk_xmalloc_bmk(rep->len + 1);
    bmk_memcpy(res, rep + 1, rep->len);
    res[rep->len] = 0;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    return res;
}	

#if 0
/* Send a debug message to xenbus.  Can block. */
static void xenbus_debug_msg(const char *msg)
{
    int len = bmk_strlen(msg);
    struct write_req req[] = {
        { "print", sizeof("print") },
        { msg, len },
        { "", 1 }};
    struct xsd_sockmsg *reply;

    reply = xenbus_msg_reply(XS_DEBUG, 0, req, ARRAY_SIZE(req));
    bmk_printf("Got a reply, type %d, id %d, len %d.\n",
            reply->type, reply->req_id, reply->len);
}
#endif

/* List the contents of a directory.  Returns a malloc()ed array of
   pointers to malloc()ed strings.  The array is NULL terminated.  May
   block. */
char *xenbus_ls(xenbus_transaction_t xbt, const char *pre, char ***contents)
{
    struct xsd_sockmsg *reply, *repmsg;
    struct write_req req[] = { { pre, bmk_strlen(pre)+1 } };
    int nr_elems, x, i;
    char **res, *msg;

    repmsg = xenbus_msg_reply(XS_DIRECTORY, xbt, req, ARRAY_SIZE(req));
    msg = errmsg(repmsg);
    if (msg) {
	*contents = NULL;
	return msg;
    }
    reply = repmsg + 1;
    for (x = nr_elems = 0; x < repmsg->len; x++)
        nr_elems += (((char *)reply)[x] == 0);
    res = bmk_memcalloc(nr_elems+1, sizeof(res[0]), BMK_MEMWHO_WIREDBMK);
    for (x = i = 0; i < nr_elems; i++) {
        int l = bmk_strlen((char *)reply + x);
        res[i] = bmk_xmalloc_bmk(l + 1);
        bmk_memcpy(res[i], (char *)reply + x, l + 1);
        x += l + 1;
    }
    res[i] = NULL;
    bmk_memfree(repmsg, BMK_MEMWHO_WIREDBMK);
    *contents = res;
    return NULL;
}

char *xenbus_read(xenbus_transaction_t xbt, const char *path, char **value)
{
    struct write_req req[] = { {path, bmk_strlen(path) + 1} };
    struct xsd_sockmsg *rep;
    char *res, *msg;
    rep = xenbus_msg_reply(XS_READ, xbt, req, ARRAY_SIZE(req));
    msg = errmsg(rep);
    if (msg) {
	*value = NULL;
	return msg;
    }
    res = bmk_xmalloc_bmk(rep->len + 1);
    bmk_memcpy(res, rep + 1, rep->len);
    res[rep->len] = 0;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    *value = res;
    return NULL;
}

char *xenbus_write(xenbus_transaction_t xbt, const char *path, const char *value)
{
    struct write_req req[] = { 
	{path, bmk_strlen(path) + 1},
	{value, bmk_strlen(value)},
    };
    struct xsd_sockmsg *rep;
    char *msg;
    rep = xenbus_msg_reply(XS_WRITE, xbt, req, ARRAY_SIZE(req));
    msg = errmsg(rep);
    if (msg) return msg;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    return NULL;
}

char* xenbus_watch_path_token( xenbus_transaction_t xbt, const char *path, const char *token, struct xenbus_event_queue *events)
{
    struct xsd_sockmsg *rep;

    struct write_req req[] = { 
        {path, bmk_strlen(path) + 1},
	{token, bmk_strlen(token) + 1},
    };

    struct xenbus_watch *watch = bmk_xmalloc_bmk(sizeof(*watch));

    char *msg;

    if (!events)
        events = &xenbus_default_watch_queue;

    watch->token = bmk_xmalloc_bmk(bmk_strlen(token)+1);
    bmk_strcpy(watch->token, token);
    watch->events = events;

    spin_lock(&xenbus_req_lock);
    MINIOS_LIST_INSERT_HEAD(&watches, watch, entry);
    spin_unlock(&xenbus_req_lock);

    rep = xenbus_msg_reply(XS_WATCH, xbt, req, ARRAY_SIZE(req));

    msg = errmsg(rep);
    if (msg) return msg;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);

    return NULL;
}

char* xenbus_unwatch_path_token( xenbus_transaction_t xbt, const char *path, const char *token)
{
    struct xsd_sockmsg *rep;

    struct write_req req[] = { 
        {path, bmk_strlen(path) + 1},
	{token, bmk_strlen(token) + 1},
    };

    struct xenbus_watch *watch;

    char *msg;

    rep = xenbus_msg_reply(XS_UNWATCH, xbt, req, ARRAY_SIZE(req));

    msg = errmsg(rep);
    if (msg) return msg;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);

    spin_lock(&xenbus_req_lock);
    MINIOS_LIST_FOREACH(watch, &watches, entry)
        if (!bmk_strcmp(watch->token, token)) {
            bmk_memfree(watch->token, BMK_MEMWHO_WIREDBMK);
            MINIOS_LIST_REMOVE(watch, entry);
            bmk_memfree(watch, BMK_MEMWHO_WIREDBMK);
            break;
        }
    spin_unlock(&xenbus_req_lock);

    return NULL;
}

char *xenbus_rm(xenbus_transaction_t xbt, const char *path)
{
    struct write_req req[] = { {path, bmk_strlen(path) + 1} };
    struct xsd_sockmsg *rep;
    char *msg;
    rep = xenbus_msg_reply(XS_RM, xbt, req, ARRAY_SIZE(req));
    msg = errmsg(rep);
    if (msg)
	return msg;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    return NULL;
}

char *xenbus_get_perms(xenbus_transaction_t xbt, const char *path, char **value)
{
    struct write_req req[] = { {path, bmk_strlen(path) + 1} };
    struct xsd_sockmsg *rep;
    char *res, *msg;
    rep = xenbus_msg_reply(XS_GET_PERMS, xbt, req, ARRAY_SIZE(req));
    msg = errmsg(rep);
    if (msg) {
	*value = NULL;
	return msg;
    }
    res = bmk_xmalloc_bmk(rep->len + 1);
    bmk_memcpy(res, rep + 1, rep->len);
    res[rep->len] = 0;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    *value = res;
    return NULL;
}

#define PERM_MAX_SIZE 32
char *xenbus_set_perms(xenbus_transaction_t xbt, const char *path, domid_t dom, char perm)
{
    char value[PERM_MAX_SIZE];
    struct write_req req[] = { 
	{path, bmk_strlen(path) + 1},
	{value, 0},
    };
    struct xsd_sockmsg *rep;
    char *msg;
    bmk_snprintf(value, PERM_MAX_SIZE, "%c%hu", perm, dom);
    req[1].len = bmk_strlen(value) + 1;
    rep = xenbus_msg_reply(XS_SET_PERMS, xbt, req, ARRAY_SIZE(req));
    msg = errmsg(rep);
    if (msg)
	return msg;
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    return NULL;
}

char *xenbus_transaction_start(xenbus_transaction_t *xbt)
{
    /* xenstored becomes angry if you send a length 0 message, so just
       shove a nul terminator on the end */
    struct write_req req = { "", 1};
    struct xsd_sockmsg *rep;
    char *err;

    rep = xenbus_msg_reply(XS_TRANSACTION_START, 0, &req, 1);
    err = errmsg(rep);
    if (err)
	return err;

    /* hint: typeof(*xbt) == unsigned long */
    *xbt = bmk_strtoul((char *)(rep+1), NULL, 10);

    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    return NULL;
}

char *
xenbus_transaction_end(xenbus_transaction_t t, int abort, int *retry)
{
    struct xsd_sockmsg *rep;
    struct write_req req;
    char *err;

    *retry = 0;

    req.data = abort ? "F" : "T";
    req.len = 2;
    rep = xenbus_msg_reply(XS_TRANSACTION_END, t, &req, 1);
    err = errmsg(rep);
    if (err) {
	if (!bmk_strcmp(err, "EAGAIN")) {
	    *retry = 1;
	    bmk_memfree(err, BMK_MEMWHO_WIREDBMK);
	    return NULL;
	} else {
	    return err;
	}
    }
    bmk_memfree(rep, BMK_MEMWHO_WIREDBMK);
    return NULL;
}

int xenbus_read_integer(const char *path)
{
    char *res, *buf;
    int t;

    res = xenbus_read(XBT_NIL, path, &buf);
    if (res) {
	bmk_printf("Failed to read %s.\n", path);
	bmk_memfree(res, BMK_MEMWHO_WIREDBMK);
	return -1;
    }
    t = bmk_strtoul(buf, NULL, 10);
    bmk_memfree(buf, BMK_MEMWHO_WIREDBMK);
    return t;
}

char* xenbus_printf(xenbus_transaction_t xbt,
                                  const char* node, const char* path,
                                  const char* fmt, ...)
{
#define BUFFER_SIZE 256
    char fullpath[BUFFER_SIZE];
    char val[BUFFER_SIZE];
    va_list args;
    int rv;

    rv = bmk_snprintf(fullpath,sizeof(fullpath),"%s/%s", node, path);
    XENBUS_BUG_ON(rv >= BUFFER_SIZE);

    va_start(args, fmt);
    rv = bmk_vsnprintf(val, sizeof(val), fmt, args);
    XENBUS_BUG_ON(rv >= BUFFER_SIZE);
    va_end(args);
    return xenbus_write(xbt,fullpath,val);
}

domid_t xenbus_get_self_id(void)
{
    char *dom_id;
    domid_t ret;

    XENBUS_BUG_ON(xenbus_read(XBT_NIL, "domid", &dom_id));
    ret = bmk_strtoul(dom_id, NULL, 10);

    return ret;
}

#if 0
static void do_ls_test(const char *pre)
{
    char **dirs, *msg;
    int x;

    bmk_printf("ls %s...\n", pre);
    msg = xenbus_ls(XBT_NIL, pre, &dirs);
    if (msg) {
	bmk_printf("Error in xenbus ls: %s\n", msg);
	bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
	return;
    }
    for (x = 0; dirs[x]; x++) 
    {
        bmk_printf("ls %s[%d] -> %s\n", pre, x, dirs[x]);
        bmk_memfree(dirs[x], BMK_MEMWHO_WIREDBMK);
    }
    bmk_memfree(dirs, BMK_MEMWHO_WIREDBMK);
}

static void do_read_test(const char *path)
{
    char *res, *msg;
    bmk_printf("Read %s...\n", path);
    msg = xenbus_read(XBT_NIL, path, &res);
    if (msg) {
	bmk_printf("Error in xenbus read: %s\n", msg);
	bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
	return;
    }
    bmk_printf("Read %s -> %s.\n", path, res);
    bmk_memfree(res, BMK_MEMWHO_WIREDBMK);
}

static void do_write_test(const char *path, const char *val)
{
    char *msg;
    bmk_printf("Write %s to %s...\n", val, path);
    msg = xenbus_write(XBT_NIL, path, val);
    if (msg) {
	bmk_printf("Result %s\n", msg);
	bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
    } else {
	bmk_printf("Success.\n");
    }
}

static void do_rm_test(const char *path)
{
    char *msg;
    bmk_printf("rm %s...\n", path);
    msg = xenbus_rm(XBT_NIL, path);
    if (msg) {
	bmk_printf("Result %s\n", msg);
	bmk_memfree(msg, BMK_MEMWHO_WIREDBMK);
    } else {
	bmk_printf("Success.\n");
    }
}

/* Simple testing thing */
void test_xenbus(void)
{
    bmk_printf("Doing xenbus test.\n");
    xenbus_debug_msg("Testing xenbus...\n");

    bmk_printf("Doing ls test.\n");
    do_ls_test("device");
    do_ls_test("device/vif");
    do_ls_test("device/vif/0");

    bmk_printf("Doing read test.\n");
    do_read_test("device/vif/0/mac");
    do_read_test("device/vif/0/backend");

    bmk_printf("Doing write test.\n");
    do_write_test("device/vif/0/flibble", "flobble");
    do_read_test("device/vif/0/flibble");
    do_write_test("device/vif/0/flibble", "widget");
    do_read_test("device/vif/0/flibble");

    bmk_printf("Doing rm test.\n");
    do_rm_test("device/vif/0/flibble");
    do_read_test("device/vif/0/flibble");
    bmk_printf("(Should have said ENOENT)\n");
}
#endif

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * End:
 */
