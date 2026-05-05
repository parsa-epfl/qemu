#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "net/pdes-checkpoint.h"
#include "net/pdes-engine.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "sysemu/cpu-timers.h"
#include "hw/core/cpu.h"

extern PDESWWT *singleton_wwt_engine = NULL;


PDESWWT *get_singleton_wwt_engine(){
    return singleton_wwt_engine;
}

int64_t get_current_virtual_for_sync_message(PDESEngine *engine) {
    return get_universal_virtual_time(engine);
}

PDESWWT *pdes_engine_wwt_create(
    const char *shm_send,
    const char *shm_recv,
    bool sync,
    int64_t latencyns,
    PDESFinalRecvCallback cb,
    void *opaque,
    bool master
){

    assert(singleton_wwt_engine == NULL && "Singleton wwt engine already created");
    // Create WWT specific engine
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    printf("WWT: Creating WWT engine at virtual time %lu ns.\n", current_time);
    int64_t time_to_setup = current_time;

    PDESWWT *wwt = g_new0(PDESWWT, 1);
    wwt->sync_counts = g_hash_table_new(g_direct_hash, g_direct_equal);
    wwt->current_quantum_round = 0;
    // TODO make sure this is always done first here and for the engine


    // Creating underlying PDESEngine
    wwt->engine = pdes_engine_create(
        shm_send,
        shm_recv,
        sync,
        latencyns,
        wwt_recivied_callback,
        wwt,
        is_waiting_for_quanta,
        wwt,
        time_to_setup,
        master
    );


    // Setup final callback and opaque for when receiving messages
    wwt->recv_opaque = opaque;
    wwt->recv_cb = cb;



    wwt->quantum_ns = latencyns;
    wwt->latencyns = latencyns;
    // TODO remove all hard codes to number of neighbors to 1
    wwt->number_of_neighbors = 1;
    wwt->number_of_neighbors_finished = 0;
    wwt->should_sync = sync;
    wwt->has_finished = false;

    // Setup timer to call setup_wwt
    wwt->setup_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)setup_wwt, wwt);
    // get current ns time and start in 1 ns
    timer_mod(wwt->setup_timer, time_to_setup);


    singleton_wwt_engine = wwt;

    if (wwt->should_sync){
        icount_set_sleep(false);
    }


    return wwt;
}



void setup_wwt(PDESWWT *wwt_engine){
    // Send initial sync message
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    // assert setup happened at the right time
    // assert(current_time == wwt_engine->first_sync_virtual_time && "WWT setup called at the wrong time");
    // TODO above assertion fails due to how time is managed in qemu , and since other messages are sent out
    // The blow hack is used, so fake first time then a correction to the time here
    // TODO see if this can be fixed later


    //TODO change this name to setup wwt
    wwt_engine->engine->first_sync_virtual_time = current_time;
    printf("WWT: Setup called, setting first sync virtual time to %lu ns.\n", current_time);

    PDESWWT *wwt = wwt_engine;

    // We are always doing barrier in case there are other ops there, but if sync is off we don't wait for neighbors to finish
    // TODO this condition should be before and should have a special setting that skips things when not synced
    wwt->quantum_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)quanta_sync, wwt);
    // Schedule for first quantum which is based on latencyns
    timer_mod(wwt->quantum_timer, 0);


    // TODO below is caused by the same problem, this marks the time diff as not calculated so it will be recalculated on first message
    wwt_engine->engine->caclulated_time_diff = false;


    printf("WWT: Setup called at virtual time %lu ns (first sync time was %lu ns).\n", current_time, wwt_engine->engine->first_sync_virtual_time);

    printf("WWT: Setup called, sent initial sync message.\n");


    // if (wwt_engine->should_sync){
    //     pdes_pause(wwt_engine->engine);
    // }

    printf("WWT: All neighbors finished setup.\n");
    // Reset for next quantum
    // TODO address the bug that may be caused without sync (as you can see multiple sync messages at once)
    wwt_engine->number_of_neighbors_finished = 0;
    printf("WWT: Setup starting at virtual time %lu ns and universal time off: %lu ns.\n", current_time, get_universal_virtual_time(wwt_engine->engine));
    // TODO increasing this for when resource contention can happen when running things in parallel, needs a better cleaner solution
    // 500ms one-shot defers the receive poll past first sync; without it a packet that arrives before our first sync goes out can be processed early and break time-bias setup
    timer_mod(wwt_engine->engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+500000000); // 500 milliseconds, just the first time
}

void send_sync(PDESWWT *wwt_engine){
    uint64_t round = wwt_engine->current_quantum_round;
    Message sync_msg = create_message((uint8_t *)&round, sizeof(round), MSG_TYPE_SYNC, get_current_virtual_for_sync_message(wwt_engine->engine));
    pdes_engine_send(wwt_engine->engine, &sync_msg);
    // printf("WWT: Sent sync message for round %lu at virtual time %lu ns.\n", round, get_universal_virtual_time(wwt_engine->engine));
}
void finish_quantum(PDESWWT *wwt_engine){
    send_sync(wwt_engine);
}
int wwt_send(PDESWWT *wwt_engine, const uint8_t *data, size_t len){
    int64_t current_virtual_time = get_universal_virtual_time(wwt_engine->engine);
    int64_t scheduled_time = current_virtual_time + wwt_engine->latencyns;
    // time difference in seconds
    float time_diff_sec = (scheduled_time - current_virtual_time) / 1e9;
    // printf("Message to be processed in %.3f seconds at virtual time %lu ns (current virtual time is %lu ns).\n", time_diff_sec, scheduled_time, current_virtual_time);
    Message msg = create_message(data, len, MSG_TYPE_NORMAL, scheduled_time);
    // printf("WWT_SEND: raw=%ld universal=%ld scheduled=%ld latency=%ld\n",
    // qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), current_virtual_time, scheduled_time, wwt_engine->latencyns);
    return pdes_engine_send(wwt_engine->engine, &msg);
}

// TODO put its type to PDESRecvCallback
void wwt_recivied_callback(void *opaque, Message *msg){
    PDESWWT *wwt_engine = (PDESWWT *)opaque;

    int64_t translated_time = msg->ts_ns;
    int64_t current_virtual_time_translated = get_universal_virtual_time(wwt_engine->engine);


    if(msg->type == MSG_TYPE_SYNC){
        // Received sync message from neighbor

        // assert that time difference between nodes can not be more than quanta
        // TODO removed due to the host time poll of underlying engine causing issues, needs to be fixed later, should be ok for later syncs still
        // if (abs(translated_time - current_virtual_time) > wwt_engine->quantum_ns) {
        //     printf("WWT Engine received sync message with timestamp %lu ns while current virtual time is %lu ns\n", translated_time, current_virtual_time);
        //     assert(false && "Received sync message with timestamp too far in the future or past");
        // }

        // printf("WWT Engine received sync message, marking one neighbor as finished for current quantum.\n");

        uint64_t msg_round = 0;
        if (msg->len >= sizeof(uint64_t)) {
            memcpy(&msg_round, msg->data, sizeof(uint64_t));
        }


        int max_round_distnce = 0;
        if (wwt_engine->finished_quantum){
            max_round_distnce = 1;
        }
        // TODO add assertions to the increment value
        bool valid_round = (msg_round == wwt_engine->current_quantum_round) || (wwt_engine->finished_quantum && msg_round == wwt_engine->current_quantum_round + 1);
        if(wwt_engine->should_sync){
            if (!valid_round) {
                printf("WWT Engine received sync message for round %lu but current round is %lu and finished_quantum is %d\n", msg_round, wwt_engine->current_quantum_round, wwt_engine->finished_quantum);
            }
            assert (valid_round && "Received sync message for wrong quantum round, this should not happen");
        }
        sync_count_increment(wwt_engine->sync_counts, msg_round);
        // printf("WWT: Sync received for round %lu (count now %d)\n", msg_round, sync_count_get(wwt_engine->sync_counts, msg_round));

    } else if (msg->type == MSG_TYPE_NORMAL){
        // Normal message, pass to final callback
        // Use message timestamp to process it later at correct virtual time
        MessageReceiveContext *ctx = g_new0(MessageReceiveContext, 1);
        ctx->recv_cb = wwt_engine->recv_cb;
        ctx->recv_opaque = wwt_engine->recv_opaque;
        ctx->msg = *msg;
        ctx->one_time_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)process_message_at_virtual_time, ctx);

        int64_t processing_time = translated_time;

        // Process at schedule or now + 1 which ever is later
        if (wwt_engine->should_sync) {
            // If should sync, process exactly at timestamp and throw an error if its in the past
            if (translated_time < current_virtual_time_translated) {
                // Should not happen
                printf("WWT Engine received message with timestamp %lu ns while current virtual time is %lu\n", translated_time, current_virtual_time_translated);
                // TODO IMPORTANT address this properly later
                assert(false && "Received message with timestamp in the past while should_sync is enabled");
            }
        }else{
            processing_time = (translated_time > current_virtual_time_translated + 1) ? translated_time : current_virtual_time_translated + 1;
        }

        int64_t raw_processing_time = processing_time + wwt_engine->engine->first_sync_virtual_time;
        ctx->timestamp_ns = raw_processing_time;
        // calculate time diffrence in seconds (not ns) and print in how many seconds the message will be processed
        // printf("Message will be processed in %.3f seconds at virtual time %lu ns (current virtual time is %lu ns, translated message time is %lu ns).\n", time_diff_sec, processing_time, current_virtual_time_translated, translated_time);
        timer_mod(ctx->one_time_poll_timer, raw_processing_time);
        // TODO remove inflight things
        // pdes_inflight_add(msg, raw_processing_time);

        // printf("WWT_RECV: msg_ts=%ld universal_now=%ld raw_processing=%ld FST=%ld\n",
        // translated_time, current_virtual_time_translated, raw_processing_time,
        // wwt_engine->engine->first_sync_virtual_time);

    }
}


bool is_waiting_for_quanta(PDESWWT *wwt_engine) {
    // TODO this needs to be addressed
    // as this thread can block polling, call message reading after each sleep
    pdes_engine_poll(wwt_engine->engine);
    int count = sync_count_get(wwt_engine->sync_counts, wwt_engine->current_quantum_round);
    bool waiting = count < 1;
    waiting = waiting && wwt_engine->should_sync;

    return waiting;
}

int64_t time_test=0;



int64_t get_quantum_time_universal(int64_t quantum_round){
    return quantum_round * get_singleton_wwt_engine()->quantum_ns;
}
int64_t get_quantum_time_local(int64_t quantum_round){
    return get_quantum_time_universal(quantum_round) + get_singleton_wwt_engine()->engine->first_sync_virtual_time;
}



int notify_neighbors_for_drain(PDESEngine *engine, char * snapshot_name, SnapshotFormat format){
    if(!engine->needs_to_checkpoint){
        assert(false && "Should not be notifying neighbors for drain if we don't need to checkpoint");
    }
    if (engine->notified_neighbors){
        return 0;
    }
    engine->notified_neighbors = true;
    printf("Notifying neighbors for drain with snapshot name: %s and format: %d\n", snapshot_name, format);
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
    // Validate this formatting of string, for both send and receive
    memcpy(snapshot_name_data + n, &format, sizeof(SnapshotFormat));
    size_t total_len = (size_t)n + sizeof(SnapshotFormat);

    // Add in quantum round too:
    // TODO this is specific to wwt, need to generalize
    uint64_t quantum_round = get_singleton_wwt_engine()->current_quantum_round;
    memcpy(snapshot_name_data + total_len, &quantum_round, sizeof(quantum_round));
    total_len += sizeof(quantum_round);

    Message drain_start_msg = create_message(snapshot_name_data, total_len, DRAIN_START, get_universal_virtual_time(engine));
    pdes_comm_send(engine->comm, &drain_start_msg);
    printf("notified neighbours with drain start message with snapshot name: %s with size %zu and sent it\n", snapshot_name_data, (size_t)n);
}

bool sync_checkpoint_check(){
    PDESWWT *wwt_engine = get_singleton_wwt_engine();
    if(wwt_engine->engine->needs_to_checkpoint){
        if(!wwt_engine->engine->notified_neighbors){
            notify_neighbors_for_drain(wwt_engine->engine, wwt_engine->engine->checkpoint_name, wwt_engine->engine->checkpoint_format);
        }
        // Create bh and reschedule this again
        if ((wwt_engine->current_quantum_round < wwt_engine->engine->checkpoint_quantum_round) && wwt_engine->should_sync){
            printf("Checkpoint quantum round %lu is less than current quantum round %lu\n", wwt_engine->engine->checkpoint_quantum_round, wwt_engine->current_quantum_round);
            // Just continue until we reach the quantum, so no return and no assert and no reschedule
        }else if((wwt_engine->current_quantum_round == wwt_engine->engine->checkpoint_quantum_round) || (!wwt_engine->should_sync)){
            if (wwt_engine->engine->boundry_checkpoint_bh == NULL){
                wwt_engine->engine->boundry_checkpoint_bh = qemu_bh_new(create_checkpoint_bh, false);
                qemu_bh_schedule(wwt_engine->engine->boundry_checkpoint_bh);
                printf("===========================scheduled checkpoint for quantum round %lu===========================\n", wwt_engine->current_quantum_round);
            }
            else{
                printf("===========================already scheduled checkpoint, skipping===========================\n");
            }
            // Reschedule the quantum check so we don't progress until we checkpoint
            // Making the wait longer as checkpoints are long by their nature and we can afford this overhead
            // printf("Rescheduling quantum check to wait for checkpoint to finish for quantum round %lu\n", wwt_engine->current_quantum_round);
            timer_mod(get_singleton_wwt_engine()->sync_check_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 10000000);
            return true;
        }else{
            printf("Current quantum round %lu has already passed checkpoint quantum round %lu, this should not happen\n", wwt_engine->current_quantum_round, wwt_engine->engine->checkpoint_quantum_round);
            assert(false && "Checkpoint quantum round should not be greater than current quantum round, if this assertion fails it means that there is an issue with how the checkpoint quantum round is being set or compared, needs to be fixed for correct checkpointing behavior");
            return true;
        }
    }
    return false;
}

void wwt_sync_check(){
    // Don't block but keep checking

    PDESWWT *wwt_engine = get_singleton_wwt_engine();
    bool waiting = is_waiting_for_quanta(get_singleton_wwt_engine());
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while(waiting){
        // Poll for messages while waiting to avoid blocking message processing
        pdes_engine_poll(wwt_engine->engine);
        waiting = is_waiting_for_quanta(wwt_engine);
    }
    // printf("WWT: Finished waiting for quanta for round %lu at virtual time %lu ns, proceeding with quantum sync.\n", wwt_engine->current_quantum_round, current_time);
    if (waiting){
        // TODO remove this if
        // Reschedule check
        // printf("WWT: Still waiting for quanta for round %lu at virtual time %lu ns, rescheduling sync check.\n", wwt_engine->current_quantum_round, current_time);
        timer_mod(get_singleton_wwt_engine()->sync_check_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 1);
        return;
    } else {
        // Finished waiting, can delete timer
        // get engine
        // TODO this part of checkpoint is to wwt specific, change later
        // TODO Add race condition lock so double checkpoint never happens (reason we double check needs_to_checkpoint)
        bool monitor_virtual_time_drift = wwt_engine->should_sync && (!wwt_engine->engine->skip_boundry_check_after_checkpoint);
        wwt_engine->engine->skip_boundry_check_after_checkpoint = false;
        monitor_virtual_time_drift = false; // disabled for now TODO add it back in and also fix the problem with checkpointing in non parallel mode
        if (monitor_virtual_time_drift){
            if (wwt_engine->current_quantum_round > 4){
                int64_t universal_time = get_universal_virtual_time(wwt_engine->engine);
                // TODO check why this went to -2
                int64_t expected_time = get_quantum_time_universal(wwt_engine->current_quantum_round);
                // IMPORTANT TODO fix this
                bool validity = universal_time == expected_time;
                if (!validity) {
                    printf("Current universal time %lu is not the same as expected quantum time %lu at round %lu, this should not happen\n", universal_time, expected_time, wwt_engine->current_quantum_round);
                    printf("current real virtual time is %lu\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
                }
                assert(validity && "Current universal time should be equal to the quantum time at the end of quanta_sync, if this assertion fails it means that the host time poll of the underlying engine is causing issues with the timing of the quanta sync, needs to be fixed for better sync performance");
            }
        }

        if(sync_checkpoint_check()){
            return;
        }

        timer_free(wwt_engine->sync_check_timer);
        wwt_engine->sync_check_timer = NULL;


        PDESEngine *engine = wwt_engine->engine;
        if(engine->master){
            // TODO again dependant to 2 nodes
            // TODO make this more general to not be reliant on conservative boundaries
            // TODO factor it out like the checkpoint portion
            if (engine->ready_to_exit_neighbors >= 1 && engine->ready_to_exit){
                engine->permitted_to_exit = true;
                // Send PERMISSION_TO_END_EMULATION message to neighbor
                Message permission_to_end_msg = create_message(NULL, 0, PERMISSION_TO_END_EMULATION, get_universal_virtual_time(engine));
                pdes_comm_send(engine->comm, &permission_to_end_msg);
                printf("Master received intent to end emulation message, permitting neighbor to exit and sending permission message back.\n");
            }
        }

        // TODO number_of_neighbors_finished should be deprecated
        wwt_engine->number_of_neighbors_finished -= wwt_engine->number_of_neighbors;
        wwt_engine->current_quantum_round++;
        wwt_engine->finished_quantum = false;




        // Schedule next quantum
        // if (time_test != 0){
        //     if (current_time != time_test) {
        //         printf("Current time %lu is less than time_test %lu, this should not happen\n", current_time, time_test);
        //     }
        //     assert (current_time == time_test && "Current time should be equal to time_test at the start of quanta_sync, if this assertion fails it means that the host time poll of the underlying engine is causing issues with the timing of the quanta sync, needs to be fixed for better sync performance");
        // }

        // TODO make these quantum rounds into macros

        // TODO add this for parallel mode:  this seems to be empty time passed with no instruction after quantum. the boundry is still kept, due to how timers are set but this needs to be addressed


        // Compute the next time for quantum
        int64_t next_quantum_time = wwt_engine->current_quantum_round * wwt_engine->quantum_ns;
        // Transform it to local time
        int64_t next_quantum_time_local = next_quantum_time + wwt_engine->engine->first_sync_virtual_time;
        timer_mod(wwt_engine->quantum_timer, next_quantum_time_local);
        // call play to resume
        // printf("===================WWT: Finished quantum %lu at virtual time %lu ns and universal time %lu ns.===================\n", wwt_engine->current_quantum_round - 1, current_time, get_universal_virtual_time(wwt_engine->engine));
        if(wwt_engine->should_sync){
            pdes_play(wwt_engine->engine);
        }
    }
}

void quanta_sync(PDESWWT *wwt_engine){
    // Sends sync, pauses and waits for others sync, then resumes
    // printf("WWT: Starting quantum sync at universal virtual time %lu ns.\n", get_universal_virtual_time(wwt_engine->engine));
    wwt_engine->finished_quantum = true;
    // printf("WWT: Sent sync for quantum %lu at virtual time %lu ns and universal time %lu ns.\n", wwt_engine->current_quantum_round, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), get_universal_virtual_time(wwt_engine->engine));

    if(wwt_engine->should_sync){
        pdes_pause(wwt_engine->engine);
    }
    // same using is_waiting_for_quanta as setup, as its the same logic
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    time_test = current_time;
    // if (wwt_engine->current_quantum_round > 1){
    //     int64_t expected_time = (wwt_engine->current_quantum_round ) * wwt_engine->quantum_ns;
    //     int64_t universal_time = get_universal_virtual_time(wwt_engine->engine);


    //     if (wwt_engine->current_quantum_round==1){
    //         // There might be a time difference due to the internal qemu clock skipping a constant amount
    //         int64_t time_diff = universal_time - expected_time;
    //         printf("WWT: Time difference between universal time and expected time for quantum %lu is %ld ns.\n", wwt_engine->current_quantum_round, time_diff);
    //         // add this to the variable we have for bias : first_sync_virtual_time
    //         // TODO revisit this logic and verify it
    //         wwt_engine->engine->first_sync_virtual_time -= time_diff;
    //     }else if(wwt_engine->should_sync){
    //         if (expected_time != universal_time) {
    //             printf("Current time %lu is not the same as expected time %lu at round %lu, this should not happen\n", universal_time, expected_time, wwt_engine->current_quantum_round);
    //         }
    //         assert (universal_time == expected_time && "Current time should be greater than or equal to expected time at the start of quanta_sync, if this assertion fails it means that the host time poll of the underlying engine is causing issues with the timing of the quanta sync, needs to be fixed for better sync performance");
    //     }
    // }
    // Pause any progress before we decide if we need to send in sync and other communications

    // TODO DOCUMENT THIS MORE: for any operation between nodes that can have potential race conditions, it should be done after pause (to prevent race in node) but before send synnc (to prevent race in the other node)
    // TODO add a lock to engine and everything that needs it. notify neighbor is a good example
    pdes_engine_poll(wwt_engine->engine);



    // printf("===================WWT: going to pause for quantum %lu at virtual time %lu ns and universal time %lu ns.===================\n", wwt_engine->current_quantum_round, current_time, get_universal_virtual_time(wwt_engine->engine));

    // Need to send notify neighbors before sync, or else before here and notify the neighbor might move to the next quantum
    if(wwt_engine->engine->needs_to_checkpoint && !wwt_engine->engine->notified_neighbors){
        notify_neighbors_for_drain(wwt_engine->engine, wwt_engine->engine->checkpoint_name, wwt_engine->engine->checkpoint_format);
    }
    if(wwt_engine->should_sync){
        // Else you'd fill up buffer
        send_sync(wwt_engine);
    }





    // Create a timer to check sync status without blocking
    assert(wwt_engine->sync_check_timer == NULL && "Sync check timer should be NULL before creating");
    wwt_engine->sync_check_timer = timer_new_ns(QEMU_CLOCK_REALTIME, (QEMUTimerCB *)wwt_sync_check, wwt_engine);
    // Schedule first check immidiately and then reschedule inside the callback until sync is done
    timer_mod(wwt_engine->sync_check_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 1);




    // Reset for next quantum
    // Do not set to 0, as if we have processed the next quantum's sync it will cause deadlock (i.e. the other qemu goes to end and waits while we are getting done processing this sync)

    // printf("WWT: Starting quantum %lu at virtual time %lu ns.\n", wwt_engine->current_quantum_round, get_universal_virtual_time(wwt_engine->engine));
}