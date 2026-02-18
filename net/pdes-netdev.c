#include "qemu/osdep.h"
#include "net/net.h"
#include "net/pdes-netdev.h"
#include "net/pdes-engine.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qapi/qapi-types-net.h"

typedef struct PDESNetState {
    NetClientState nc;
    PDESWWT *engine;
} PDESNetState;

static void pdes_net_cleanup(NetClientState *nc) {
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    if (s->engine) {
        pdes_engine_destroy(s->engine);
    }
}

static ssize_t pdes_net_receive(NetClientState *nc, const uint8_t *buf, size_t size) {
    // printf("^^^^^^^^^^^^^^ PDES Netdev receive (request to send) called with packet of length %zu ^^^^^^^^^^^^^^ \n", size);
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    int ret = wwt_send(s->engine, buf, size);
    // printf("^^^^^^^^^^^^^^ PDES Netdev sending packet of length %zu with return value %d^^^^^^^^^^^^^^ \n", size, ret);   
    return (ret < 0) ? ret : size;
}

static void pdes_recv_callback(void *opaque, const uint8_t *data, size_t len) {
    NetClientState *nc = opaque;
    // printf("^^^^^^^^^^^^^^ PDES Netdev received packet of length %zu^^^^^^^^^^^^^^ \n", len);
    int res = qemu_send_packet(nc, data, len);
    if (res <= 0) {
        // Packet was dropped — need to handle this
        fprintf(stderr, "PDES: qemu_send_packet dropped packet of len %zu\n", len);
    }
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
    
    s->engine = pdes_engine_wwt_create(pdes_opts->shm_send, pdes_opts->shm_recv, pdes_opts->sync, pdes_opts->latencyns, pdes_recv_callback, nc, pdes_opts->master);



    
    printf("PDES Netdev initialized with shm_send=%s, shm_recv=%s, sync=%d, latencyns=%lu\n",
           pdes_opts->shm_send, pdes_opts->shm_recv, pdes_opts->sync, pdes_opts->latencyns);

    return 0;
}