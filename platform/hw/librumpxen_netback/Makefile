.include <bsd.own.mk>

LIB=	rumpxen_netback

SRCS=	if_virt.c
SRCS+=	netbsd_if_virt.c
SRCS+=	xenif_component.c
SRCS+=  xennet_checksum.c

RUMPTOP= ${TOPRUMP}

IFBASE=		-DVIRTIF_BASE=xen_netback

CPPFLAGS+=	-I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet
CPPFLAGS+=	-I${.CURDIR}
CPPFLAGS+=	${IFBASE}
CPPFLAGS+= 	-I${.CURDIR}/../../../include

RUMPCOMP_USER_SRCS=	 xenif_user.c
RUMPCOMP_USER_SRCS+=	 netbsd_xenif_user.c
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../include
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../../xen/xen/include
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../../../include
RUMPCOMP_USER_CPPFLAGS+= ${IFBASE}
RUMPCOMP_USER_CPPFLAGS+= -I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet

# XXX
.undef RUMPKERN_ONLY

.include "${RUMPTOP}/Makefile.rump"
.include <bsd.lib.mk>
.include <bsd.klinks.mk>
