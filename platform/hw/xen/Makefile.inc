SRCS+=	xen/gnttab.c
SRCS+=	xen/gntmap.c
SRCS+=	xen/events.c
SRCS+=	xen/hypervisor.c

.PHONY: xendir
xendir:
	mkdir -p ${RROBJ}/platform/xen
