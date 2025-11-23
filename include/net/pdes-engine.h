#ifndef NET_PDES_ENGINE_H
#define NET_PDES_ENGINE_H

#include <stdint.h>
#include <stddef.h>

typedef struct PDESEngine PDESEngine;
typedef void (*PDESRecvCallback)(void *opaque, const uint8_t *data, size_t len);

PDESEngine *pdes_engine_create(const char *shm_send, const char *shm_recv);
void pdes_engine_destroy(PDESEngine *engine);
void pdes_engine_set_recv_callback(PDESEngine *engine, PDESRecvCallback cb, void *opaque);
int pdes_engine_send(PDESEngine *engine, const uint8_t *data, size_t len);
void pdes_engine_poll(PDESEngine *engine);

#endif