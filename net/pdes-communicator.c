#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "qemu/atomic.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "qemu/timer.h"


PDESCommunicator * singleton_comm = NULL;

typedef struct {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    Message           messages[RING_SIZE];
} ShmRing;


struct PDESCommunicator {
    int       fd_send;
    int       fd_recv;
    ShmRing  *ring_send;
    ShmRing  *ring_recv;
    size_t    size;
};



/* Helper to create/open and mmap a ring */
static int pdes_comm_init_ring(const char *shm_name,
                               int *fd_out,
                               ShmRing **ring_out,
                               size_t shm_size)
{
    int fd;
    bool is_creator;
    ShmRing *ring;

    fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    is_creator = (fd >= 0);

    if (!is_creator) {
        /* Already exists, just open it */
        fd = shm_open(shm_name, O_RDWR, 0666);
        if (fd < 0) {
            return -1;
        }
    } else {
        /* We created it, so size it */
        if (ftruncate(fd, shm_size) < 0) {
            shm_unlink(shm_name);
            close(fd);
            return -1;
        }
    }

    ring = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        if (is_creator) {
            shm_unlink(shm_name);
        }
        close(fd);
        return -1;
    }

    if (is_creator) {
        /* Initialize the ring buffer */
        memset(ring, 0, shm_size);
    } 
    // else {
    //     /* Wait for initialization by creator */
    //     while (ring->write_idx == 0 && ring->read_idx == 0) {
    //         usleep(1000);
    //     }
    // }
    // TODO see if you need to add the above back in

    *fd_out   = fd;
    *ring_out = ring;
    return 0;
}


PDESCommunicator *pdes_comm_create(const char *shm_send_name,
                                   const char *shm_recv_name)
{
    if (singleton_comm != NULL) {
        // Print error and exit
        fprintf(stderr, "Error: Attempted to create multiple PDESCommunicator instances. Only one instance is allowed.\n");
        exit(EXIT_FAILURE);
    }
    PDESCommunicator *comm;
    size_t shm_size = sizeof(ShmRing);

    comm = g_new0(PDESCommunicator, 1);
    if (!comm) {
        return NULL;
    }

    comm->size = shm_size;

    if (pdes_comm_init_ring(shm_send_name, &comm->fd_send,
                            &comm->ring_send, shm_size) < 0) {
        g_free(comm);
        return NULL;
    }

    if (pdes_comm_init_ring(shm_recv_name, &comm->fd_recv,
                            &comm->ring_recv, shm_size) < 0) {
        munmap(comm->ring_send, shm_size);
        close(comm->fd_send);
        g_free(comm);
        return NULL;
    }

    singleton_comm = comm;
    return comm;
}


Message create_message(const uint8_t *data, size_t len, uint8_t type, uint64_t ts_ns)
{
    Message msg = {0};
    msg.ts_ns = ts_ns;
    msg.len   = (uint32_t)len;
    msg.type  = type;
    memcpy(msg.data, data, len);
    return msg;
}


void pdes_comm_destroy(PDESCommunicator *comm)
{
    if (!comm) {
        return;
    }

    if (comm->ring_send) {
        munmap(comm->ring_send, comm->size);
    }
    if (comm->ring_recv) {
        munmap(comm->ring_recv, comm->size);
    }
    if (comm->fd_send >= 0) {
        close(comm->fd_send);
    }
    if (comm->fd_recv >= 0) {
        close(comm->fd_recv);
    }
    g_free(comm);
}


int pdes_comm_send(PDESCommunicator *comm, Message *msg)
{

    ShmRing *ring;
    uint32_t next_write;

    if (!comm || !comm->ring_send || !msg) {
        printf("PDES Comm invalid parameters in send\n");
        return -EINVAL;
    }

    ring = comm->ring_send;
    next_write = (ring->write_idx + 1) % RING_SIZE;
    bool was_full = next_write == ring->read_idx;
    if(was_full){
        printf("PDES Comm ring buffer full, cannot send message now, blocking until space is available.\n");
    }
    while (next_write == ring->read_idx) {
        // TODO check if we can make this better
        // printf("PDES Comm ring buffer full, cannot send message now.\n");
        usleep(1);  /* Wait for space to become available */
        // pdes_engine_poll(get_singleton_engine());
        // return -EAGAIN;
    }
    if(was_full){
        printf("PDES Comm space available in ring buffer, resuming message send.\n");
    }

    // TODO check for race conditions
    /* Copy the message into the ring */
    ring->messages[ring->write_idx] = *msg;

    qatomic_set_mb(&ring->write_idx, next_write);

    // if(MSG_TYPE_NORMAL == msg->type){
    //     printf("=========================== PDES Engine: Sent message of length %u and of type %u =========================== \n", msg->len, msg->type);
    // }
    return 0;
}


int pdes_comm_recv(PDESCommunicator *comm, Message *msg){
    ShmRing *ring;
    if (!comm || !comm->ring_recv || !msg) {
        return -EINVAL;
    }

    ring = comm->ring_recv;
    if (ring->read_idx == ring->write_idx) {
        return NO_MESSAGE;  /* No message available */
    }

    /* Copy the message from the ring */
    *msg = ring->messages[ring->read_idx];

    qatomic_set_mb(&ring->read_idx, (ring->read_idx + 1) % RING_SIZE);

    // if(MSG_TYPE_NORMAL == msg->type){
    //     printf("=========================== PDES Engine: Received message of length %u and of type %u =========================== \n", msg->len, msg->type);
    // }
    return msg->len;
}

