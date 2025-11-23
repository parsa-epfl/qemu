#ifndef NET_PDES_COMMUNICATOR_H
#define NET_PDES_COMMUNICATOR_H

#include <stdint.h>
#include <stddef.h>

typedef struct PDESCommunicator PDESCommunicator;

PDESCommunicator *pdes_comm_create(const char *shm_name);
void pdes_comm_destroy(PDESCommunicator *comm);
int pdes_comm_send(PDESCommunicator *comm, const uint8_t *data, size_t len);
int pdes_comm_recv(PDESCommunicator *comm, uint8_t *buf, size_t buf_len);

#endif