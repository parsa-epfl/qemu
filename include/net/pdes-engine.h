#ifndef NET_PDES_ENGINE_H
#define NET_PDES_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include "pdes-communicator.h"
#include "qemu/timer.h"
#include "migration/snapshot.h"

typedef struct PDESEngine PDESEngine;
typedef struct PDESWWT PDESWWT;
typedef struct MessageReceiveContext MessageReceiveContext;

typedef void (*PDESFinalRecvCallback)(void *opaque, const uint8_t *data, size_t len);
typedef void (*PDESRecvCallback)(void *opaque, Message *msg);
// TODO This might be too QEMU specific, think about it in terms of general soltions later
typedef void (*PauseStatusCallBack)(void *opaque);


struct PDESEngine {
    PDESCommunicator *comm;
    bool needs_sync;
    int64_t latencyns;
    PDESRecvCallback recv_cb;
    void *recv_opaque;
    QEMUTimer *msg_rec_poll_timer;
    QEMUTimer *sync_poll_timer;
    QEMUTimer *setup_poll_timer;
    bool has_first_sync;
    bool pair_has_finished;

    // Specific to QEMU implications of blocking event queu in case of pause on device and icount TODO generalize
    PauseStatusCallBack pause_status_cb;
    void *pause_status_opaque;

    bool paused;


    int64_t base_diff;
    int64_t first_sync_time;

    

    // This is only used since client and server might come from checkpoints that are not synced and there will be a delta: TODO see if this can be improved
    bool caclulated_time_diff;
    // Check if time diff has been calculated correctly and type conversion is ok
    int64_t first_sync_virtual_time;
    int64_t base_time_diff;

    // Checkpoint specific
    int neighbour_drained;
    QEMUTimer *drain_poll_timer;
    bool checkpoint_in_progress;
    QEMUTimer *checkpoint_initiate_timer;
    QEMUBH *checkpoint_bh;


    // WWT specific
    bool waiting_for_quanta;

    // node specific
    bool master;
    int init_flag;
    bool master_init;

    // V2 timer impl
    // TODO all these bools need a lock
    QEMUBH *pause_bh;
    bool needs_to_checkpoint;
    bool notified_neighbors;
    char checkpoint_name[10006];
    SnapshotFormat checkpoint_format;
    // TODO this is specific to wwt, needs to be fixed
    uint64_t checkpoint_quantum_round;
    QEMUBH *boundry_checkpoint_bh;
    // Special bool: if we checkpointed: since it can move time by qemu for all nodes: don't do boundry check : TODO clean this check up later
    bool skip_boundry_check_after_checkpoint;

    // exit changes
    bool notified_neighbors_for_exit;
    int ready_to_exit_neighbors;
    bool permitted_to_exit;
    bool ready_to_exit;
};

PDESEngine *pdes_engine_create(
    const char *shm_send, 
    const char *shm_recv, 
    bool sync, 
    int64_t latencyns,
    PDESRecvCallback cb, 
    void *opaque,
    PauseStatusCallBack pause_status_cb,
    void *pause_status_opaque,
    int64_t first_sync_virtual_time,
    bool master
);
void pdes_engine_destroy(PDESEngine *engine);
void notify_neighbours_of_end(PDESEngine *engine);
int pdes_engine_send(PDESEngine *engine, Message *msg);
void pdes_engine_poll(void *opaque);

// TODO this needs to move to a proper library and its own thread
void schedule_poll(void *opaque);


void pdes_pause(void *opaque);
void pdes_play(void *opaque);

// drain: Define the function to send everything to neighbours through singleton used for example when savingvm
// TODO check how generalizable this is for more neighbours and the other strategies
PDESEngine *get_singleton_engine();
int pdes_drain(PDESEngine *engine, char * snapshot_name, SnapshotFormat format);






// WWT specific: TODO move to its own headr file later
struct PDESWWT{
    PDESEngine *engine;
    int64_t latencyns;
    int64_t quantum_ns;
    int64_t number_of_neighbors;
    int64_t number_of_neighbors_finished;
    bool has_finished;
    PDESFinalRecvCallback recv_cb;
    void *recv_opaque;
    bool should_sync;

    QEMUTimer *setup_timer;
    QEMUTimer *quantum_timer;
    GHashTable *sync_counts;
    uint64_t current_quantum_round;


    
    // V2 timer impl
    // Timer to check sync status without blocking
    QEMUTimer *sync_check_timer;
    bool finished_quantum;

    
};

PDESWWT *pdes_engine_wwt_create(
    const char *shm_send,
    const char *shm_recv,
    bool sync,
    int64_t latencyns,
    PDESFinalRecvCallback cb, 
    void *opaque,
    bool master
);
void setup_wwt(PDESWWT *wwt_engine);
void send_sync(PDESWWT *wwt_engine);
void finish_quantum(PDESWWT *wwt_engine);
int wwt_send(PDESWWT *wwt_engine, const uint8_t *data, size_t len);
// TODO put its type to PDESRecvCallback
void wwt_recivied_callback(void *opaque, Message *msg);
bool is_waiting_for_quanta(PDESWWT *wwt_engine);
void quanta_sync(PDESWWT *wwt_engine);





// Utility functions

struct MessageReceiveContext {
    PDESFinalRecvCallback recv_cb;
    void *recv_opaque;
    Message msg;
    QEMUTimer *one_time_poll_timer;
    int64_t timestamp_ns;
};

void process_message_at_virtual_time(MessageReceiveContext *opaque);

int64_t get_universal_virtual_time(PDESEngine *engine);
PDESWWT *get_singleton_wwt_engine();
int send_initiate_checkpoint_message(PDESEngine *engine);
void sync_count_increment(GHashTable *table, uint64_t round);
int sync_count_get(GHashTable *table, uint64_t round);
void finish_initiate_checkpoint(PDESEngine *engine);

void destroy_strategy();
bool can_stop(PDESEngine *engine);

#endif