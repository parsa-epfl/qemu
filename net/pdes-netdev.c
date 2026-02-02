#include "qemu/osdep.h"
#include "net/net.h"
#include "net/pdes-netdev.h"
#include "net/pdes-engine.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qapi/qapi-types-net.h"

typedef struct PDESNetState {
    NetClientState nc;
    PDESEngine *engine;
} PDESNetState;

static void pdes_net_cleanup(NetClientState *nc) {
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    if (s->engine) {
        pdes_engine_destroy(s->engine);
    }
}

static ssize_t pdes_net_receive(NetClientState *nc, const uint8_t *buf, size_t size) {
    return;
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    printf("^^^^^^^^^^^^^^ PDES Netdev sending packet of length %zu^^^^^^^^^^^^^^ \n", size);
    int ret = pdes_engine_send(s->engine, buf, size);
    return (ret < 0) ? ret : size;
}

static void pdes_recv_callback(void *opaque, const uint8_t *data, size_t len) {
    return len;
    NetClientState *nc = opaque;
    printf("^^^^^^^^^^^^^^ PDES Netdev received packet of length %zu^^^^^^^^^^^^^^ \n", len);
    qemu_send_packet(nc, data, len);
    qemu_notify_event();
}

static NetClientInfo net_pdes_info = {
    .type = NET_CLIENT_DRIVER_PDES,
    .size = sizeof(PDESNetState),
    .receive = pdes_net_receive,
    .cleanup = pdes_net_cleanup,
};

int net_init_pdes(const Netdev *netdev, const char *name, NetClientState *peer, Error **errp) {
    const NetdevPdesOptions *pdes_opts = &netdev->u.pdes;
    
    NetClientState *nc = qemu_new_net_client(&net_pdes_info, peer, "pdes", name);
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    
    // s->engine = pdes_engine_create(pdes_opts->shm_send, pdes_opts->shm_recv, pdes_opts->sync, pdes_opts->latencyns, pdes_recv_callback, nc);
    // nc->link_down = false;
    
    // // Notify peer (the emulated NIC) about link status
    // if (nc->peer && nc->peer->info->link_status_changed) {
    //     nc->peer->info->link_status_changed(nc->peer);
    // }
    printf("PDES Netdev initialized with shm_send=%s, shm_recv=%s, sync=%d, latencyns=%lu\n",
           pdes_opts->shm_send, pdes_opts->shm_recv, pdes_opts->sync, pdes_opts->latencyns);

    return 0;
}