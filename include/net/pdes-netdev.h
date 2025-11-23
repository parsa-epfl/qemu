#ifndef NET_PDES_NETDEV_H
#define NET_PDES_NETDEV_H

int net_init_pdes(const Netdev *netdev, const char *name, NetClientState *peer, Error **errp);

#endif