include ../../global.mk
include ${BUILDRUMP_TOOLFLAGS}

default: all

# Check if we're building for a supported target.
supported= false
# assume we're doing "make clean"
MACHINE?= i386
ifeq (${MACHINE},i386)
HW_MACHINE_ARCH=i386
HW_MACHINE_ARCH_FAM=x86
supported:= true
endif
ifeq (${MACHINE},amd64)
HW_MACHINE_ARCH_FAM=x86
supported:= true
endif
ifeq (${MACHINE},evbarm)
HW_MACHINE_ARCH_FAM=arm
supported:= true
ARCHDIR= arm/integrator
endif
ifneq (${supported},true)
$(error only supported target is x86, you have ${MACHINE})
endif

ARCHDIR?= ${MACHINE}
HW_MACHINE_ARCH?= ${MACHINE_GNU_ARCH}

INSTALLTGTS=    librumpxen_netback_install librumpnet_xenif_install

CPPFLAGS+=	-isystem ../xen/xen/include
CFLAGS+=	-D__XEN_INTERFACE_VERSION__=0x00030205  -Wno-unused-function -Wno-unused-but-set-variable

LDSCRIPT:=	$(abspath arch/${ARCHDIR}/kern.ldscript)
SRCS+=		intr.c clock_subr.c kernel.c multiboot.c undefs.c

include ../Makefile.inc
include xen/Makefile.inc
include arch/${ARCHDIR}/Makefile.inc

# Disable PIE, but need to check if compiler supports it
LDFLAGS-$(call cc-option,-no-pie) += -no-pie
LDFLAGS += $(LDFLAGS-y)

OBJS:=	$(patsubst %.c,${RROBJ}/platform/%.o,${SRCS}) \
	$(patsubst %.S,${RROBJ}/platform/%.o,${ASMS})

.PHONY:	clean cleandir all links

all:  links archdirs xendir xenlibs ${MAINOBJ} ${TARGETS}

${RROBJ}/include/hw/machine:
	@mkdir -p ${RROBJ}/include/hw
	@ln -sfn $(shell pwd)/include/arch/${HW_MACHINE_ARCH} $@

# Xen PV creates bmk-pcpu and other links outside of RROBJ; do the same for HW
links: ${RROBJ}/include/hw/machine
	[ -e ../xen/xen/include/xen ] || ln -sf $(XEN_HEADERS) ../xen/xen/include/xen
	[ -e ../xen/xen/include/mini-os/machine ] || ln -sf $(HW_MACHINE_ARCH_FAM) ../xen/xen/include/mini-os/machine
	ln -sfn ../platform/hw/include/arch/${HW_MACHINE_ARCH} ../../include/bmk-pcpu

${RROBJ}/platform/%.o: %.c
	${CC} ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${RROBJ}/platform/%.o: %.S
	${CC} -D_LOCORE ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${MAINOBJ}: ${OBJS} platformlibs
$(eval $(call BUILDLIB_target,librumpxen_netback,.))
$(eval $(call BUILDLIB_target,librumpnet_xenif,.))

xenlibs: ${RROBJLIB}/librumpxen_netback/librumpxen_netback.a ${RROBJLIB}/librumpnet_xenif/librumpnet_xenif.a

${MAINOBJ}: ${OBJS} platformlibs xenlibs
	${CC} -nostdlib ${CFLAGS} ${LDFLAGS} -Wl,-r ${OBJS} -o $@ \
	    -L${RROBJLIB}/libbmk_core -L${RROBJLIB}/libbmk_rumpuser \
	    -Wl,--whole-archive -lbmk_rumpuser -lbmk_core -Wl,--no-whole-archive
	${OBJCOPY} -w -G bmk_* -G rumpuser_* -G jsmn_* -G minios_* -G xenbus_* \
	    -G rumprun_platform_rumpuser_init -G _start -G netback_* \
	    -G netfront_* -G gntmap_* -G gnttab_* -G _minios_* $@

clean: commonclean
	rm -f ${OBJS_BMK} include/hw/machine buildtest ${MAINOBJ}

cleandir: clean
