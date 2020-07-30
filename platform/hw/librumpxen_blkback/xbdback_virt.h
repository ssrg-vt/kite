#ifndef _XEN_XBDBACK_VIRT_H_
#define	_XEN_XBDBACK_VIRT_H_

#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/types.h>

#define VIF_STRING(x) #x
#define VIF_STRINGIFY(x) VIF_STRING(x)
#define VIF_CONCAT(x, y) x##y
#define VIF_CONCAT3(x, y, z) x##y##z
#define VIF_BASENAME(x, y) VIF_CONCAT(x, y)
#define VIF_BASENAME3(x, y, z) VIF_CONCAT3(x, y, z)

#define VIF_CLONER VIF_BASENAME(VIRTIF_BASE, _cloner)
#define VIF_NAME VIF_STRINGIFY(VIRTIF_BASE)

#define VIFHYPER_ENTRY VIF_BASENAME3(rump_xbdbackcomp_, VIRTIF_BASE, _entry)
#define VIFHYPER_IODONE VIF_BASENAME3(rump_xbdbackcomp_, VIRTIF_BASE, _getiodone)

typedef struct _physical_device {
	char path[520];
	unsigned long number;
	int status;
}physical_device;

extern physical_device storage_devices[10];

void xbdback_entry(void);
void xbdback_getiodone(struct buf *buf);
int rump_xbdback_vop_ioctl(struct vnode *xbdi_vp);
void rump_xbdback_bdev_strategy(struct buf *xio_buf);
void rump_xbdback_virt_destroy(struct vnode *xbdi_vp, uint16_t xbdi_domid);
struct vnode* rump_xbdback_virt_changed(unsigned short xbdi_dev, struct vnode *xbdi_vp, uint64_t *xbdi_size, uint16_t xbdi_domid);
void rump_xbdback_buf_init(struct buf *b, struct vnode * vp);
void rump_xbdback_buf_destroy(struct buf *xio_buf);
unsigned long rump_xbdback_get_number(const char *dev_path);

#endif
