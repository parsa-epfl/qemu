#include "qemu/osdep.h"
#include "net/pdes-engine.h"
#include "net/pdes-communicator.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "include/migration/snapshot.h"
#include "include/sysemu/runstate.h"
#include "net/pdes-checkpoint.h"
#include "migration/snapshot.h"
#include "sysemu/cpu-timers.h"
#include "hw/core/cpu.h"
#include <assert.h>

#ifdef CONFIG_LIBQFLEX
#include "middleware/libqflex/libqflex-module.h"
#include "middleware/libqflex/libqflex.h"
#endif

// TODO this should be generlized to multiple neighbours later
// For now singleton pdes engine
extern PDESEngine *singleton_engine = NULL;

PDESEngine *get_singleton_engine(){
    return singleton_engine;
}


int64_t get_current_virtual_for_normal_message(PDESEngine *engine) {
    return get_universal_virtual_time(engine) + engine->latencyns;
}

int64_t get_current_virtual_for_destroy_message(PDESEngine *engine) {
    return get_universal_virtual_time(engine);
}

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
) {
    // Show error if singleton was created before
    assert(singleton_engine == NULL && "Singleton engine already created");
    icount_set_sleep(false);
    PDESEngine *engine = g_new0(PDESEngine, 1);
    engine->comm = pdes_comm_create(shm_send, shm_recv);
    engine->needs_sync = sync;
    engine->latencyns = latencyns;
    engine->recv_cb = cb;
    engine->recv_opaque = opaque;
    engine->has_first_sync = false;
    engine->waiting_for_quanta = false;
    engine->pair_has_finished = false;
    engine->base_diff = 0;
    engine->paused = false;
    engine->pause_status_cb = pause_status_cb;
    engine->pause_status_opaque = pause_status_opaque;

    engine->first_sync_virtual_time = first_sync_virtual_time;
    engine->caclulated_time_diff = false;
    engine->base_time_diff = 0;
    engine->neighbour_drained = 0;
    engine->checkpoint_in_progress = false;

    engine->master = master;
    engine->init_flag = 0;
    engine->master_init = false;
    engine->pause_bh = NULL;
    engine->needs_to_checkpoint = false;
    engine->notified_neighbors = false;



    engine->msg_rec_poll_timer = timer_new_ns(QEMU_CLOCK_HOST, pdes_engine_poll, engine);
    // Schedule it IMMEDIATELY

    // TODO look into optimizing this
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+50000); // 5 microseconds

    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    // TODO remove this field
    engine->first_sync_time = current_time;
    
    singleton_engine = engine;
    printf(">>>>>>> NET_INIT_PDES CALLED <<<<<<<\n");
    return engine;
}


void pdes_engine_destroy(PDESEngine *engine) {
    printf("==========================================Destroying PDES Engine...==========================================\n");
    Message mssg = create_message(NULL, 0, END_OF_EMULATION, get_current_virtual_for_destroy_message(engine));
    pdes_comm_send(engine->comm, &mssg);
    if (engine->comm) {
        pdes_comm_destroy(engine->comm);
    }
    g_free(engine);
    printf("==========================================PDES Engine destroyed.==========================================\n");
}

int pdes_engine_send(PDESEngine *engine, Message *msg) {
    /* TODO: Add your PDES decision logic here */
    if (engine->first_sync_time == -1){
        if (msg->type == MSG_TYPE_SYNC){
            // TODO remove this field
            engine->first_sync_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        } else {
            // Should not happen, as first message should be sync
            assert(false && "First message sent is not a sync message");
        }
    }
    return pdes_comm_send(engine->comm, msg);
}



void initiate_checkpoint_master(void *context){
    printf("Master initiating checkpoint after receiving initiation message from neighbor...\n");
    PDESEngine *engine = get_singleton_engine();
    if (!engine->master){
        assert(false && "Only master should receive checkpoint initiation callback");
    }
    printf("Master initiating systemic snapshot save for checkpoint initiation...\n");
    save_snapshot("init_warmed",
                true, NULL, false, NULL, NULL);
    qemu_bh_delete(engine->checkpoint_bh);
    printf("Master completed systemic snapshot save for checkpoint initiation.\n");

    return;
}
void set_checkpoint_values_for_master(){
    // if master is ready to initiate checkpoint start it
    PDESEngine *engine = get_singleton_engine();
    printf("Master is already initialized, initiating checkpoint immediately.\n");
    // TODO Make this repeated part into a function
    engine->notified_neighbors = false;
    engine->needs_to_checkpoint = true;
    // TODO this is specific to wwt, need to generalize later, maybe include this in the message
    PDESWWT *wwt_engine = get_singleton_wwt_engine();
    engine->checkpoint_quantum_round = wwt_engine->current_quantum_round; // this is specific to wwt, need to generalize later
    char* snapshot_name = "init_warmed"; 
    snprintf(engine->checkpoint_name, sizeof(engine->checkpoint_name), "%s", snapshot_name);
    printf("Setting checkpoint values for master, snapshot name: %s, quantum round: %lu\n", engine->checkpoint_name, engine->checkpoint_quantum_round);
    // For now skipping 
}
void process_message(PDESEngine *engine, Message *msg) {

    // Make sure the message goes up the chain before doing anything else
    engine->recv_cb(engine->recv_opaque, msg);


    // printf("PDES Engine received message of type %u with timestamp %lu ns and len %u bytes.\n", msg->type, msg->ts_ns, msg->len);

    // TODO both drain start and and end are based on just one neighbor for now, need to generalize later
    if (msg->type==DRAIN_START){
        printf("PDES Engine received drain end message, marking drained as true.\n");
        assert (false && "DO NOT SUPPORT CHECKPOINTING FOR KNOTTYKRAKEN YET.\n");

    }else if (msg->type==DRAIN_END){
        printf("PDES Engine received drain end message, marking checkpoint as completed.\n");
        if (!engine->master){
            // This is not master, we are letting known we can move on with simulation
            engine->checkpoint_in_progress = false;
        }
    }else if(msg->type==CHECKPOINT_INIT_STEP){
        printf("PDES Engine received checkpoint initiation message, initiating checkpoint.\n");
        engine->init_flag++;
        // TODO expand this into multiple nodes
        if (engine->master){
            printf("This is a master, checking if we can initiate checkpoint immediately or need to wait for next initiation message.\n");
            if (engine->init_flag == 1 && engine->master_init){
                set_checkpoint_values_for_master();
            }else{
                printf("Master received checkpoint initiation message, but master init flag is not set, marking master as ready and waiting for next checkpoint initiation message.\n");
            }
        }else{
            printf("Not a master, just returning after receiving checkpoint initiation message.\n");
        }
    }
}
void pdes_engine_poll(void *opaque) {
    PDESEngine *engine = opaque;
    
    while(true){
        Message msg;
        int res = pdes_comm_recv(engine->comm, &msg);
        if (res == NO_MESSAGE) {
            // No message to process
            break;
        } else if (res < 0 ) {
            // Error occurred while polling
            fprintf(stderr, "Error polling for messages: %d\n", res);
            break;
        } else {
            // Message received, process it
            process_message(engine, &msg);
        }
    }
    // TODO add a flag so that when calling this manually we don't reschedule again and again
    // schedule_poll(engine);
}

void schedule_poll(void *opaque){
    // getting rid of poll here
    printf("!!!!!!!!!!! should not be called TODO be removed !!!!!!!!!!!\n");
    PDESEngine *engine = opaque;
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+5000000);
}

void pdes_pause_bh(void *opaque){
    PDESEngine *engine = opaque;

    vm_stop(RUN_STATE_SAVE_VM);
    // Remove the bottom half
    qemu_bh_delete(engine->pause_bh);
    engine->pause_bh = NULL;
}

void pdes_pause(void *opaque){
    PDESEngine *engine = opaque;
    

    engine->paused = true;
    // Create bh
    // Make sure bh is empty
    // assert(engine->pause_bh == NULL && "Pause BH is not NULL when trying to pause, this should not happen");
    // engine->pause_bh = qemu_bh_new(pdes_pause_bh, engine);
    // qemu_bh_schedule(engine->pause_bh);
    
    // qemu_system_vmstop_request_prepare();
    // qemu_system_vmstop_request(RUN_STATE_PAUSED);

    assert(engine->pause_bh == NULL);
    engine->pause_bh = qemu_bh_new(pdes_pause_bh, engine);
    qemu_bh_schedule(engine->pause_bh);


    

    if (current_cpu == NULL){
        // This can happen if we call pause before the CPU is created, in that case we just return and do nothing as there is nothing to pause yet
        // printf("pdes_pause called but current_cpu is NULL, this can happen if pause is called before CPU is created, just returning without pausing.\n");
        return;
    }
    

    current_cpu->stop = true;
    cpu_exit(current_cpu);



    return;
}


void pdes_play(void *opaque){
    // TODO add doc where this can be called from (not virt)
    PDESEngine *engine = opaque;
    engine->paused = false;
    // Create bh
    // Make sure bh is empty
    // assert(engine->pause_bh == NULL && "Pause BH is not NULL when trying to play, this should not happen");
    // engine->pause_bh = qemu_bh_new(play_bh, engine);
    // qemu_bh_schedule(engine->pause_bh);
    vm_start();
    return;
}


int pdes_drain(PDESEngine *engine, char * snapshot_name) {
    if (engine->master){
        engine->checkpoint_in_progress = true;
   
    


        
        Message drain_end_msg = create_message(NULL, 0, DRAIN_END, get_universal_virtual_time(engine));
        pdes_comm_send(engine->comm, &drain_end_msg);
        engine->neighbour_drained = 0;

        printf("Everyone has drained and finished checkpointing.\n");
    }
    pdes_inflight_save_json(snapshot_name);
    return 0;
}



int send_initiate_checkpoint_message(PDESEngine *engine){
    // Create a message for checkpoint initiation, with the time being current virtual time

    // TODO add multi-neighbour support to send for everyone
    Message checkpoint_init_msg = create_message(NULL, 0, CHECKPOINT_INIT_STEP, get_universal_virtual_time(engine));
    pdes_comm_send(engine->comm, &checkpoint_init_msg);
    printf("Sent checkpoint initiation message to neighbor.\n");
    return 0;
}

void finish_initiate_checkpoint(PDESEngine *engine){
    printf("========================GOT signal for initiate_checkpoint========================\n");
    // printf("+++++++++++++ Skipping checkpoint initiation because this is not implemented yet, just returning. +++++++++++++\n");
    // return;
    if (engine->master){
        // This is master, we can start checkpoint immediately
        // TODO expand this to multiple nodes
        engine->master_init = true;

        if (engine->init_flag >= 1){
            printf("Master received checkpoint initiation message, initiating checkpoint immediately.\n");
            // pdes_drain(engine, "init_warmed", SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_BASE);

            // Make sure neighbors know they can checkpoint at end of quantum
            // and set flags for our selves as well
            set_checkpoint_values_for_master();
            printf("Master sent drain start message for checkpoint initiation to neighbors, waiting for neighbors to drain and checkpoint.\n");
        }else{
            printf("Master received checkpoint initiation message, but init flag is not set, marking master as ready and waiting for next checkpoint initiation message.\n");
            return;
        }
    }else{
        int res = send_initiate_checkpoint_message(engine);
        assert (res == 0 && "Failed to send checkpoint initiation message to master");
        printf("Sent checkpoint initiation message to master, returning.\n");
    }
}