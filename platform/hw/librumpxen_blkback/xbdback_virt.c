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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: xbdback_xenbus.c,v 1.63 2016/12/26 08:16:28 skrll Exp $");

#include <sys/atomic.h>
#include <sys/buf.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/device.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/syscallargs.h>

#include "xbdback_virt.h"
#include "xbdback_virt_user.h"

void 
xbdback_entry(void)
{
	VIFHYPER_ENTRY();
}

void
rump_xbdback_virt_destroy(struct vnode *xbdi_vp, uint16_t xbdi_domid)
{
	const char *name;
	struct dkwedge_info wi;
	if (getdiskinfo(xbdi_vp, &wi) == 0)
		name = wi.dkw_devname;
	else
		name = "*unknown*";
	aprint_normal("rump_xbdback_virt_destroy: detach device %s for domain %d\n",
		    name, xbdi_domid);
	vn_close(xbdi_vp, FREAD, NOCRED);
}

struct vnode*
rump_xbdback_virt_changed(unsigned short xbdi_dev, struct vnode *xbdi_vp, uint64_t *xbdi_size, uint16_t xbdi_domid)
{
	int err;
	const char *devname;
	int major;
	const struct bdevsw *xbdi_bdevsw; /* pointer to the device's bdevsw */

	major = major(xbdi_dev);
	devname = devsw_blk2name(major);
	if (devname == NULL) {
		aprint_normal("rump_xbdback_virt_changed: unknown device 0x%x\n", xbdi_dev);
		return NULL;
	}

	xbdi_bdevsw = bdevsw_lookup(xbdi_dev);
	if (xbdi_bdevsw == NULL) {
		aprint_normal("rump_xbdback_virt_changed: no bdevsw for device 0x%x\n", xbdi_dev);
		return NULL;
	}

	err = bdevvp(xbdi_dev, &xbdi_vp);
	if (err) {
		aprint_normal("rump_xbdback_virt_changed: can't open device 0x%x: %d\n", xbdi_dev, err);
		return NULL;
	}

	err = vn_lock(xbdi_vp, LK_EXCLUSIVE | LK_RETRY);
	if (err) {
		aprint_normal("rump_xbdback_virt_changed: can't vn_lock device 0x%x: %d\n", xbdi_dev, err);
		vrele(xbdi_vp);
		return NULL;
	}
	err  = VOP_OPEN(xbdi_vp, FREAD, NOCRED);
	if (err) {
		aprint_normal("rump_xbdback_virt_changed: can't VOP_OPEN device 0x%x: %d\n", xbdi_dev, err);
		vput(xbdi_vp);
		return NULL;
	}
	VOP_UNLOCK(xbdi_vp);

	/* dk device; get wedge data */
	struct dkwedge_info wi;
	if ((err = getdiskinfo(xbdi_vp, &wi)) == 0) {
		*xbdi_size = wi.dkw_size;
		aprint_normal("rump_xbdback_virt_changed: attach device %s (size %lu) "
		    "for domain %d\n", wi.dkw_devname, *xbdi_size,
		    xbdi_domid);
	} else {
		/* If both Ioctls failed set device size to 0 and return */
		aprint_normal("rump_xbdback_virt_changed: can't DIOCGWEDGEINFO device "
		    "0x%x: %d\n", xbdi_dev, err);		
		*xbdi_size = xbdi_dev = 0;
		vn_close(xbdi_vp, FREAD, NOCRED);
		xbdi_vp = NULL;
		return NULL;
	}

	return xbdi_vp;
}


int
rump_xbdback_vop_ioctl(struct vnode *xbdi_vp)
{
	int error;
	int force = 1;

	error = VOP_IOCTL(xbdi_vp, DIOCCACHESYNC, &force, FWRITE,
		    kauth_cred_get());

	return error;
}

void 
xbdback_getiodone(struct buf *buf)
{
	VIFHYPER_IODONE(buf);
}

void
rump_xbdback_bdev_strategy(struct buf *xio_buf)
{
	if ((xio_buf->b_flags & B_READ) == 0) {
		mutex_enter(xio_buf->b_vp->v_interlock);
		xio_buf->b_vp->v_numoutput++;
		mutex_exit(xio_buf->b_vp->v_interlock);
	}

	xio_buf->b_iodone = xbdback_getiodone;
	bdev_strategy(xio_buf);
}

void
rump_xbdback_buf_init(struct buf *xio_buf, struct vnode *vp)
{
	buf_init(xio_buf);
	xio_buf->b_objlock = vp->v_interlock;

}
	
void
rump_xbdback_buf_destroy(struct buf *xio_buf)
{
	buf_destroy(xio_buf);
}

physical_device storage_devices[10];

unsigned long
rump_xbdback_get_number(const char *dev_path)
{
	int i;

	for(i=0; i < 10; i++) {
		if (strcmp(dev_path, storage_devices[i].path) == 0) {
		       	if(storage_devices[i].status == 0) {
				aprint_normal("Device is already occupied\n");
				return 0;
			}

			storage_devices[i].status = 0;

			return storage_devices[i].number;
		}
	}

	aprint_normal("rump_xbdback_get_number: couldn't fetch the device number\n");
	return 0;
}
