SRCS+=	xen/gnttab.c
SRCS+=	xen/gntmap.c
SRCS+=	xen/events.c
SRCS+=	xen/hypervisor.c
SRCS+=	xen/xenbus.c
SRCS+=	xen/netback.c

.PHONY: xendir
xendir:
	mkdir -p ${RROBJ}/platform/xen
