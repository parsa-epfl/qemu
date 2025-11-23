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
    QEMUTimer *poll_timer;
} PDESNetState;

static void pdes_net_cleanup(NetClientState *nc) {
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    if (s->poll_timer) {
        timer_free(s->poll_timer);
    }
    if (s->engine) {
        pdes_engine_destroy(s->engine);
    }
}

static ssize_t pdes_net_receive(NetClientState *nc, const uint8_t *buf, size_t size) {
    PDESNetState *s = DO_UPCAST(PDESNetState, nc, nc);
    printf("PDES Netdev sending packet of length %zu\n", size);
    return pdes_engine_send(s->engine, buf, size);
}

static void pdes_recv_callback(void *opaque, const uint8_t *data, size_t len) {
    NetClientState *nc = opaque;
    printf("PDES Netdev received packet of length %zu\n", len);
    qemu_send_packet(nc, data, len);
}

static void pdes_poll_timer_cb(void *opaque) {
    PDESNetState *s = opaque;
    pdes_engine_poll(s->engine);
    timer_mod(s->poll_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
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
    
    s->engine = pdes_engine_create(pdes_opts->shm_send, pdes_opts->shm_recv);
    pdes_engine_set_recv_callback(s->engine, pdes_recv_callback, nc);
    
    s->poll_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, pdes_poll_timer_cb, s);
    // TODO This needs to be changed into async 
    timer_mod(s->poll_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    
    return 0;
}