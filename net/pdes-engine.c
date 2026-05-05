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

#ifdef CONFIG_LIBQFLEX
#include "middleware/libqflex/libqflex-legacy-api.h"
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
    engine->notified_neighbors_for_exit = false;
    engine->boundry_checkpoint_bh = NULL;
    engine->skip_boundry_check_after_checkpoint = false;
    engine->ready_to_exit_neighbors = 0;
    engine->permitted_to_exit = false;
    engine->ready_to_exit = false;



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


void notify_neighbours_of_end(PDESEngine *engine){
    if(engine->notified_neighbors_for_exit){
        return;
    }
    engine->notified_neighbors_for_exit = true;

    printf("==========================================Existing PDES Engine...==========================================\n");
    Message mssg = create_message(NULL, 0, END_OF_EMULATION, get_current_virtual_for_destroy_message(engine));
    pdes_comm_send(engine->comm, &mssg);
    printf("==========================================PDES Engine exited.==========================================\n");
}
// TODO clean this up, destroying needs clean up
void destroy_strategy(){
    PDESWWT *wwt_engine = get_singleton_wwt_engine();
    if (wwt_engine != NULL){
        // remove the sync timer
        if (wwt_engine->sync_check_timer != NULL){
            timer_free(wwt_engine->sync_check_timer);
            wwt_engine->sync_check_timer = NULL;
        }
    }
}
void pdes_engine_destroy(PDESEngine *engine) {
    // Notify neighbors that we are ending the simulation
    notify_neighbours_of_end(engine);
    if(engine->needs_to_checkpoint){
        // Create bh
        if (!engine->boundry_checkpoint_bh){
            engine->boundry_checkpoint_bh = qemu_bh_new(create_checkpoint_bh, true);
        }
        qemu_bh_schedule(engine->boundry_checkpoint_bh);
    }else{
        destroy_strategy();
#ifdef CONFIG_LIBQFLEX
        libqflex_stop("Simulation terminated by flexus.");
#endif
        exit(0);
    }
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
                true, NULL, false, NULL, SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_BASE, NULL);
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
    engine->checkpoint_format = SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_BASE;
    // TODO this is specific to wwt, need to generalize later, maybe include this in the message
    PDESWWT *wwt_engine = get_singleton_wwt_engine();
    engine->checkpoint_quantum_round = wwt_engine->current_quantum_round; // this is specific to wwt, need to generalize later
    char* snapshot_name = "init_warmed";
    snprintf(engine->checkpoint_name, sizeof(engine->checkpoint_name), "%s", snapshot_name);
    printf("Setting checkpoint values for master, snapshot name: %s, format: %d, quantum round: %lu\n", engine->checkpoint_name, engine->checkpoint_format, engine->checkpoint_quantum_round);
    // For now skipping
}
void process_message(PDESEngine *engine, Message *msg) {

    // Make sure the message goes up the chain before doing anything else
    engine->recv_cb(engine->recv_opaque, msg);


    // printf("PDES Engine received message of type %u with timestamp %lu ns and len %u bytes.\n", msg->type, msg->ts_ns, msg->len);

    // TODO both drain start and and end are based on just one neighbor for now, need to generalize later
    if(msg->type == INTENT_TO_END_EMULATION){
        if (engine->master){
            printf("Received intent to end emulation message from neighbor, permitting neighbor to exit and sending permission message back.\n");
            engine->ready_to_exit_neighbors++;
        }
    }
    if (msg->type == PERMISSION_TO_END_EMULATION){
        printf("Received permission to end emulation message from master, setting permitted_to_exit to true.\n");
        engine->permitted_to_exit = true;
    }
    if (msg->type == END_OF_EMULATION){
        printf("PDES Engine received end of emulation message, finishing simulation.\n");
        // TODO add any cleanup needed here
        pdes_engine_destroy(engine);
    }
    if (msg->type==DRAIN_START){
        printf("PDES Engine received drain end message, marking drained as true.\n");
        engine->checkpoint_in_progress = true;

        if (!engine->master){
            printf("PDES Engine initiating systemic snapshot save after drain.\n");
            // This is not master so we need to savesnapshot immidiately
            // TODO change this so the message includes snapshot name
            // TODO : Ugly solution for now to avoid deadlock:  create a host time timer, call this later, call it immidiately after this
            // TODO We will get stuck thanks to quanta, need to generalize later
            Message *msg_copy = g_new(Message, 1);
            *msg_copy = *msg;

            engine->needs_to_checkpoint = true;

            // TODO verify new snapshot name formatting and parsing, for both send and receive
            // TODO just turn this into a struct message
            size_t name_len = msg->len - sizeof(SnapshotFormat) - sizeof(uint64_t);
            memcpy(engine->checkpoint_name, msg->data, name_len);
            engine->checkpoint_name[name_len] = '\0'; // null-terminate if needed

            SnapshotFormat format;
            memcpy(&format, msg->data + name_len, sizeof(SnapshotFormat));

            // read quantum round too, for later use if needed
            uint64_t quantum_round = 0;
            if (msg->len >= sizeof(SnapshotFormat) + sizeof(uint64_t)) {
                memcpy(&quantum_round, msg->data + name_len + sizeof(SnapshotFormat),sizeof(uint64_t));
            }

            engine->checkpoint_format = format;
            engine->checkpoint_quantum_round = quantum_round;

            printf("Parsed checkpoint initiation message, snapshot name: %s, format: %d, quantum round: %lu\n", engine->checkpoint_name, format, quantum_round);





        }else{
            // This variable is only used for master, TODO maybe move this
            engine->neighbour_drained += 1;
        }
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

    vm_stop(RUN_STATE_PAUSED);
    // Remove the bottom half
    qemu_bh_delete(engine->pause_bh);
    engine->pause_bh = NULL;
}
#ifndef CONFIG_LIBQFLEX
// TODO this relies on being on main thread always, add some safeguards for this
CPUState *paused_cpu = NULL;
#endif
void pdes_pause(void *opaque){
    PDESEngine *engine = opaque;


    // Create bh
    // Make sure bh is empty
    // assert(engine->pause_bh == NULL && "Pause BH is not NULL when trying to pause, this should not happen");
    // engine->pause_bh = qemu_bh_new(pdes_pause_bh, engine);
    // qemu_bh_schedule(engine->pause_bh);

    // qemu_system_vmstop_request_prepare();
    // qemu_system_vmstop_request(RUN_STATE_PAUSED);


    engine->paused = true;
#ifdef CONFIG_LIBQFLEX
    if (flexus_api.pause != NULL){
        flexus_api.pause();
    }else if(flexus_api.stop != NULL){
        assert(false && "Flexus resume API is not implemented, but stop API is implemented, this should not happen as both should be implemented together");
    }
#endif

    assert(engine->pause_bh == NULL);
    engine->pause_bh = qemu_bh_new(pdes_pause_bh, engine);
    qemu_bh_schedule(engine->pause_bh);

#ifndef CONFIG_LIBQFLEX
    if (current_cpu != NULL){
        paused_cpu = current_cpu;
        current_cpu->stop = true;
        // cpu_exit(current_cpu);
    }
#endif


    return;
}


void pdes_play(void *opaque){
    // TODO add doc where this can be called from (not virt)
    PDESEngine *engine = opaque;
    // Create bh
    // Make sure bh is empty
    // assert(engine->pause_bh == NULL && "Pause BH is not NULL when trying to play, this should not happen");
    // engine->pause_bh = qemu_bh_new(play_bh, engine);
    // qemu_bh_schedule(engine->pause_bh);
#ifndef CONFIG_LIBQFLEX
    if (paused_cpu != NULL){
        paused_cpu->stop = false;
        paused_cpu = NULL;
    }
#endif
    vm_start();
#ifdef CONFIG_LIBQFLEX
    if (flexus_api.resume != NULL){
        flexus_api.resume();
    }else if(flexus_api.stop != NULL){
        assert(false && "Flexus resume API is not implemented, but stop API is implemented, this should not happen as both should be implemented together");
    }
#endif
    engine->paused = false;
    return;
}



int pdes_drain(PDESEngine *engine, char * snapshot_name, SnapshotFormat format) {
#ifdef CONFIG_LIBQFLEX
    assert(format == SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_BASE && "qemu fork: pdes_drain only supports SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_BASE");
#endif
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

bool can_stop(PDESEngine *engine){
    if (!engine->master){
        // Send INTENT_TO_END_EMULATION message to master
        // TODO make all these bool flags atomic:

        // TODO these message are specific to 2 nodes, need to generalize for more nodes and send only to master
        if(!engine->notified_neighbors_for_exit){
            Message intent_to_end_msg = create_message(NULL, 0, INTENT_TO_END_EMULATION, get_universal_virtual_time(engine));
            pdes_comm_send(engine->comm, &intent_to_end_msg);
            printf("Sent intent to end emulation message to master, waiting for permission to exit.\n");
            engine->notified_neighbors_for_exit = true;
        }
    }else{

        bool can_end = engine->ready_to_exit_neighbors >= 1;
        if(can_end){
            engine->permitted_to_exit = true;
            // Send PERMISSION_TO_END_EMULATION message to neighbor
            Message permission_to_end_msg = create_message(NULL, 0, PERMISSION_TO_END_EMULATION, get_universal_virtual_time(engine));
            pdes_comm_send(engine->comm, &permission_to_end_msg);
            printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!Allowing neighbor to exit as master and sending permission message back.!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        }
    }
    engine->ready_to_exit = true;
    return engine->permitted_to_exit;
}