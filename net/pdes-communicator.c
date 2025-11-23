#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "qemu/atomic.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "qemu/timer.h"

#define MAX_MSG_SIZE 2048

typedef struct {
    uint64_t ts_ns;       // timestamp in nanoseconds
    uint32_t len;
    uint8_t data[MAX_MSG_SIZE];
    uint8_t type;         // Add this: 0 = normal, 1 = sync TODO define these properly
} Message;

typedef struct {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    Message messages[1024];
} ShmRing;

struct PDESCommunicator {
    int fd;
    ShmRing *ring;
    size_t size;
    QEMUTimer *timer;  // TODO this needs to move to PDES Engine
    bool synced;
    bool has_connection; // TODO this is temp fix since we don't have any orchestration yet
};

static void pdes_timer_cb(void *opaque) {
    PDESCommunicator *comm = (PDESCommunicator *)opaque;

    // We have not yet established connection, just reschedule
    
    // Pause all vCPUs
    printf("============== PDES Comm: Pausing all vCPUs for synchronization.==============\n");
    pause_all_vcpus();
    printf("============== PDES Comm: vCPUs paused.==============\n");
    comm->synced = false;

    // We have sent all our messages so need to let others know
    printf("============== PDES Comm: Sending sync message and waiting for sync from others.==============\n");
    pdes_comm_send_sync(comm);
    printf("============== PDES Comm: Sync message sent, now waiting for others.==============\n");
    
    while (comm->synced == false) {
        printf("============== PDES Comm: Waiting for sync from others.==============\n");  
        usleep(100);  // Sleep for 100 microseconds
    }
    // Resume all vCPUs
    printf("============== PDES Comm: Resuming all vCPUs after synchronization.==============\n");
    resume_all_vcpus();
    printf("============== PDES Comm: vCPUs resumed.==============\n");
    
    
    // Reschedule for next interval (e.g., 1ms later)
    // TODO change this hardcoded value to a parameter for latency
    timer_mod(comm->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
}

PDESCommunicator *pdes_comm_create(const char *shm_name) {
    PDESCommunicator *comm = g_new0(PDESCommunicator, 1);
    size_t shm_size = sizeof(ShmRing);
    
    comm->fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);   
    bool is_creator = (comm->fd >= 0);
    
    if (!is_creator) {
        // Already exists, just open it
        comm->fd = shm_open(shm_name, O_RDWR, 0666);
        if (comm->fd < 0) {
            g_free(comm);
            return NULL;
        }
    } else {
        // We created it, so initialize
        if (ftruncate(comm->fd, shm_size) < 0) {
            shm_unlink(shm_name);
            close(comm->fd);
            g_free(comm);
            return NULL;
        }
    }
    
    comm->ring = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, comm->fd, 0);
    if (comm->ring == MAP_FAILED) {
        close(comm->fd);
        if (is_creator) shm_unlink(shm_name);
        g_free(comm);
        return NULL;
    }
    
    if (is_creator) {
        // Initialize the ring buffer
        memset(comm->ring, 0, shm_size);
    } else {
        // Wait for initialization by creator
        while (comm->ring->write_idx == 0 && comm->ring->read_idx == 0) {
            usleep(1000);
        }
    }
    
    comm->ring = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, comm->fd, 0);
    comm->size = shm_size;

    comm->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pdes_timer_cb, comm);
    printf("==============PDES Comm: Created communicator with shared memory '%s' and sending sync message.==============\n", shm_name);
    pdes_comm_send_sync(comm);
    comm->synced = true;
    comm->has_connection = false;
    // TODO change this hardcoded value to a parameter for latency
    timer_mod(comm->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);

    
    return comm;
}

void pdes_comm_destroy(PDESCommunicator *comm) {
    if (comm->ring) {
        munmap(comm->ring, comm->size);
    }
    if (comm->fd >= 0) {
        close(comm->fd);
    }
    timer_free(comm->timer);
    g_free(comm);
}

int pdes_comm_send(PDESCommunicator *comm, const uint8_t *data, size_t len) {
    if (len > MAX_MSG_SIZE) return -1;
    
    uint32_t next_write = (comm->ring->write_idx + 1) % 1024;
    if (next_write == comm->ring->read_idx) return -EAGAIN;
    
    Message *msg = &comm->ring->messages[comm->ring->write_idx];
    msg->len = len;

    // Dummy latency
    uint64_t latency = 1500;
    msg->ts_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + latency;
    msg->type = 0; // normal message

    memcpy(msg->data, data, len);
    qatomic_set_mb(&comm->ring->write_idx, next_write);
    
    return len;
}

int pdes_comm_send_sync(PDESCommunicator *comm) {
    uint32_t next_write = (comm->ring->write_idx + 1) % 1024;
    if (next_write == comm->ring->read_idx) return -EAGAIN;
    
    Message *msg = &comm->ring->messages[comm->ring->write_idx];
    msg->len = 0;
    msg->type = 1;  // Sync message
    msg->ts_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);  // No latency for sync
    qatomic_set_mb(&comm->ring->write_idx, next_write);
    
    printf("==============PDES Comm sent sync message 3.==============\n");
    return 0;
}

int pdes_comm_recv(PDESCommunicator *comm, uint8_t *buf, size_t buf_len) {
    if (comm->ring->read_idx == comm->ring->write_idx) return 0;
    
    Message *msg = &comm->ring->messages[comm->ring->read_idx];
    size_t len = msg->len < buf_len ? msg->len : buf_len;
    memcpy(buf, msg->data, len);
    
    if (msg->type == 1) {
        comm->synced = true;  // Set synced flag when sync received
        qatomic_set_mb(&comm->ring->read_idx, (comm->ring->read_idx + 1) % 1024);
        printf("PDES Comm received sync message.\n");
        return -1;  // Return special value to indicate sync message
    }

    if (comm->has_connection == false){
        printf("PDES Comm: Connection established.\n");
        comm->has_connection = true;
    }

    qatomic_set_mb(&comm->ring->read_idx, (comm->ring->read_idx + 1) % 1024);
    
    if (len > 0){
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        // Printf message, length, when it was suppoused to be received and current time
        printf("PDES Comm received message of length %zu, ts_ns: %lu, now: %lu\n", len, msg->ts_ns, now);
        if (now > msg->ts_ns) {
            printf("!!!!!!!!!!!!!!!!!!!Warning: causality violation detected!!!!!!!!!!!!!!!!!!!\n");
        }
    }
    return len;
}