#include "qemu/osdep.h"
#include "net/pdes-engine.h"
#include "net/pdes-communicator.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "include/migration/snapshot.h"
#include "include/sysemu/runstate.h"
#include "net/pdes-checkpoint.h"

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


void initiate_checkpoint(void * opaque){

    PDESEngine *engine = get_singleton_engine();
    if (engine->master){
        assert(false && "Master should not receive checkpoint initiation callback");
    }
    
    // Just a temp function to test savevm during drain
    Error *err = NULL;
    printf("PDES Engine performing systemic snapshot save during drain...\n");

    // Get snapshot name from msg data
    Message * msg = (Message *)opaque;
    char snapshot_name[1000];
    printf("initiate_checkpoint called with snapshot name: %s and message len: %u\n", msg->data, msg->len);
    memcpy(snapshot_name, msg->data, msg->len);

    printf("current virtual time during checkpoint initiation: %lu ns\n", get_universal_virtual_time(get_singleton_engine()));

    // As part of savesnap shot, qemu will pause things, and it will call drain, so by the time we send drain start message , everything is paused and there is nothing on the fly (spagetified due to qemu clock design)
    save_snapshot(snapshot_name,
                true, NULL, false, NULL, &err);
    
    printf("After 3 virtual time during checkpoint initiation: %lu ns\n", get_universal_virtual_time(get_singleton_engine()));
    printf("PDES Engine completed systemic snapshot save during drain.\n");
    g_free(msg);
    qemu_bh_delete(engine->checkpoint_bh);
    engine->checkpoint_bh = NULL;
    vm_start();

    if (err) {
        error_reportf_err(err, "Error during temp snapshot save: ");
        exit(1);
    }
}

void process_message(PDESEngine *engine, Message *msg) {

    // Make sure the message goes up the chain before doing anything else
    engine->recv_cb(engine->recv_opaque, msg);


    // printf("PDES Engine received message of type %u with timestamp %lu ns and len %u bytes.\n", msg->type, msg->ts_ns, msg->len);

    // TODO both drain start and and end are based on just one neighbor for now, need to generalize later
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

            // This is caused due not being able to call savevm from a dev. This solution causes problems for pause, hence using qemu_clock_run_all_timers/or specific run. TODO this needs to be fixed later
            // engine->checkpoint_initiate_timer = timer_new_ns(QEMU_CLOCK_REALTIME, initiate_checkpoint, msg_copy);
            // timer_mod(engine->checkpoint_initiate_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));   
            engine->checkpoint_bh = qemu_bh_new(initiate_checkpoint, msg_copy);
            qemu_bh_schedule(engine->checkpoint_bh);
            
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
        if (engine->init_flag == 1 && engine->master){
            if (engine->master_init){
                // if master is ready to initiate checkpoint start it
                printf("Master is already initialized, initiating checkpoint immediately.\n");
                pdes_drain(engine, "init_warmed");
            }
        }
    }
}

void pdes_engine_poll(void *opaque) {
    PDESEngine *engine = opaque;
    Message msg;

    int len = pdes_comm_recv(engine->comm, &msg);
    
    if (len == NO_MESSAGE) {
        // No message available
        // TODO see if anything needs to happen here
    }
    else if (len < 0) {
        // Error handling
        fprintf(stderr, "Error receiving message: %d\n", len);
    }else{
        process_message(engine, &msg);
    }
    // TODO add a flag so that when calling this manually we don't reschedule again and again
    schedule_poll(engine);
}

void schedule_poll(void *opaque){
    PDESEngine *engine = opaque;
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+50000); // 5 microseconds
}

void pdes_pause(void *opaque){
    PDESEngine *engine = opaque;
    engine->paused = true;
    // printf("=========Going into PDES pause=========\n");
    while (engine->paused){
        // Wait until not in the middle of processing
        // TODO all usleeps need to be addressed for speedup
        usleep(1000); // Sleep for 1 ms

        // TODO again this is specific to QEMU and how sleeping is affected in ICOUNT mode, make it more generalized later
        engine->pause_status_cb(engine->pause_status_opaque);
        // TODO we need this as if we pull and see drain message, it will be scheduled for future but never called. This is due to how qemu manages clocks. Fix this
        // qemu_clock_run_timers(QEMU_CLOCK_REALTIME);
        aio_bh_poll(qemu_get_aio_context());
        // qemu_clock_run_all_timers();
    }
    // Since qemu_clock_run_timers can pause vm execution
    vm_start();
    // printf("WWT: quanta_sync resumed. VM running state: %d, current virtual time: %lu ns\n", 
    //    runstate_is_running(), get_universal_virtual_time(engine));
    // printf("=========Exiting PDES pause=========\n");
}

void pdes_play(void *opaque){
    PDESEngine *engine = opaque;
    engine->paused = false;
}

int pdes_drain(PDESEngine *engine, char * snapshot_name) {
    // TODO list of things that should be turned off when no sync is needed
    // TODO turn this based on connected neighbours later
    // if (!engine->needs_sync){
    //     // If sync is not needed, no drain is needed
    //     return 0;
    // }
    if (engine->master){
        engine->checkpoint_in_progress = true;
   
    

        // Create PDES start message for everyone lese
        uint8_t snapshot_name_data[1006];
        int n = snprintf((char *)snapshot_name_data, sizeof(snapshot_name_data),
                        "QPDES%s", snapshot_name ? snapshot_name : "");
        if (n < 0) {
            // encoding/format error
            return -1;
        }

        if (n >= sizeof(snapshot_name_data)) {
            // Output was truncated, handle the error
            fprintf(stderr, "Snapshot name is too long and was truncated\n");
            return -1;
        }
        snprintf((char *)snapshot_name_data, sizeof(snapshot_name_data), "QPDES%s", snapshot_name);
        Message drain_start_msg = create_message(snapshot_name_data, (size_t)n, DRAIN_START, get_universal_virtual_time(engine));
        pdes_comm_send(engine->comm, &drain_start_msg);
        printf("created drain start message with snapshot name: %s with size %zu and sent it\n", snapshot_name_data, (size_t)n);



        // TODO make this more generalized
        while (engine->neighbour_drained < 1){
            // Send drain start message repeatedly until drained is true
            usleep(1000); // Sleep for 1 ms
            pdes_engine_poll(engine);
        }

        Message drain_end_msg = create_message(NULL, 0, DRAIN_END, get_universal_virtual_time(engine));
        pdes_comm_send(engine->comm, &drain_end_msg);
        engine->neighbour_drained = 0;

        printf("Everyone has drained and finished checkpointing.\n");
    }else{
        // Just create an empty PDES start message
        Message drain_start_msg = create_message(NULL, 0, DRAIN_START, get_universal_virtual_time(engine));
        pdes_comm_send(engine->comm, &drain_start_msg);
        printf("Sent drain start message to master, waiting for checkpoint to complete.\n");
        // Wait until checkpoint is complete, which will be marked by checkpoint_in_progress to be false again
        while (engine->checkpoint_in_progress){
            printf("Checkpoint in progress, waiting... current virtual time: %lu ns\n", get_universal_virtual_time(engine));
            usleep(100000); // Sleep for 100 ms
            pdes_engine_poll(engine);
        }
        printf("Checkpoint completed, resuming execution.\n");
    }

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

