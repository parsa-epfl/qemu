#ifndef NET_PDES_COMMUNICATOR_H
#define NET_PDES_COMMUNICATOR_H

#include <stdint.h>
#include <stddef.h>

typedef struct PDESCommunicator PDESCommunicator;

#define MAX_MSG_SIZE 2048
#define RING_SIZE    1024

#define MSG_TYPE_NORMAL 0
#define MSG_TYPE_SYNC 1
#define NO_MESSAGE -1
#define END_OF_EMULATION -2

typedef struct {
    uint64_t ts_ns;       /* timestamp in nanoseconds */
    uint32_t len;
    uint8_t  data[MAX_MSG_SIZE];
    uint8_t  type;        /* 0 = normal, 1 = sync TODO: define these properly */
} Message;



PDESCommunicator *pdes_comm_create(const char *shm_send_name, const char *shm_recv_name);
void pdes_comm_destroy(PDESCommunicator *comm);
Message create_message(const uint8_t *data, size_t len, uint8_t type, uint64_t ts_ns);
int pdes_comm_send(PDESCommunicator *comm, Message *msg);
int pdes_comm_recv(PDESCommunicator *comm, Message *msg);

#endif