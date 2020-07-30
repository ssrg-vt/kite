.include <bsd.own.mk>

LIB=	rumpxen_blkback

SRCS=	xbdback_virt.c
SRCS+=	xbdback_component.c

RUMPTOP= ${TOPRUMP}

CPPFLAGS+=	-I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet
CPPFLAGS+=	-I${.CURDIR}
CPPFLAGS+=	${IFBASE}
CPPFLAGS+= 	-I${.CURDIR}/../../../include

RUMPCOMP_USER_SRCS=	 xbdback_xenbus.c
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../include
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../../xen/xen/include
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../../../include
RUMPCOMP_USER_CPPFLAGS+= -I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet

# XXX
.undef RUMPKERN_ONLY

.include "${RUMPTOP}/Makefile.rump"
.include <bsd.lib.mk>
.include <bsd.klinks.mk>
