#ifndef _MINIOS_NETBACK_H_
#define _MINIOS_NETBACK_H_

#include <mini-os/wait.h>
struct netback_dev;
struct netback_dev *netback_init(char *nodename, void (*netif_rx)(struct netback_dev *, unsigned char *data, int len, unsigned char csum), unsigned char rawmac[6], char **ip, void *priv, char* vifname);
int netback_prepare_xmit(struct netback_dev *dev, unsigned char* data, int len, int offset, unsigned int index);
void netback_xmit(struct netback_dev *dev, int* csum_blank, int count); 
int netback_rxring_full(struct netback_dev *dev); 
void netback_shutdown(struct netback_dev *dev);

void *netback_get_private(struct netback_dev *);

extern struct wait_queue_head netback_queue;

#endif /* _MINIOS_NETBACK_H_ */
