#ifndef NET_PDES_ENGINE_H
#define NET_PDES_ENGINE_H

#include <stdint.h>
#include <stddef.h>

typedef struct PDESEngine PDESEngine;
typedef void (*PDESRecvCallback)(void *opaque, const uint8_t *data, size_t len);

PDESEngine *pdes_engine_create(
    const char *shm_send, 
    const char *shm_recv, 
    bool sync, 
    uint64_t latencyns,
    PDESRecvCallback cb, 
    void *opaque
);
void pdes_engine_destroy(PDESEngine *engine);
int pdes_engine_send(PDESEngine *engine, const uint8_t *data, size_t len);
void pdes_engine_poll(void *opaque);

// TODO this needs to move to a proper library and its own thread
void schedule_poll(void *opaque);

#endif