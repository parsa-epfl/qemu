#include "qemu/osdep.h"
#include "net/pdes-engine.h"
#include "net/pdes-communicator.h"

struct PDESEngine {
    PDESCommunicator *send_comm;
    PDESCommunicator *recv_comm;
    PDESRecvCallback recv_cb;
    void *recv_opaque;
};

PDESEngine *pdes_engine_create(const char *shm_send, const char *shm_recv) {
    PDESEngine *engine = g_new0(PDESEngine, 1);
    engine->send_comm = pdes_comm_create(shm_send);
    engine->recv_comm = pdes_comm_create(shm_recv);
    return engine;
}

void pdes_engine_destroy(PDESEngine *engine) {
    if (engine->send_comm) {
        pdes_comm_destroy(engine->send_comm);
    }
    if (engine->recv_comm) {
        pdes_comm_destroy(engine->recv_comm);
    }
    g_free(engine);
}

void pdes_engine_set_recv_callback(PDESEngine *engine, PDESRecvCallback cb, void *opaque) {
    engine->recv_cb = cb;
    engine->recv_opaque = opaque;
}

int pdes_engine_send(PDESEngine *engine, const uint8_t *data, size_t len) {
    /* TODO: Add your PDES decision logic here */
    return pdes_comm_send(engine->send_comm, data, len);
}

void pdes_engine_poll(PDESEngine *engine) {
    uint8_t buf[2048];
    int len = pdes_comm_recv(engine->recv_comm, buf, sizeof(buf));
    
    if (len > 0 && engine->recv_cb) {
        /* TODO: Add your PDES decision logic here */
        engine->recv_cb(engine->recv_opaque, buf, len);
    }
}