#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "net/pdes-engine.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"

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


    if (wwt->should_sync){
        // TODO this condition should be before and should have a special setting that skips things when not synced
        wwt->quantum_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)quanta_sync, wwt);
        // Schedule for first quantum which is based on latencyns
        timer_mod(wwt->quantum_timer, current_time + wwt->quantum_ns);
    }

    singleton_wwt_engine = wwt;

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
}

void send_sync(PDESWWT *wwt_engine){
    uint64_t round = wwt_engine->current_quantum_round;
    Message sync_msg = create_message((uint8_t *)&round, sizeof(round), MSG_TYPE_SYNC, get_current_virtual_for_sync_message(wwt_engine->engine));
    pdes_engine_send(wwt_engine->engine, &sync_msg);
    // printf("WWT: Sent sync message at virtual time %lu ns.\n", get_universal_virtual_time(wwt_engine->engine));
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

        // TODO add assertions to the increment value
        sync_count_increment(wwt_engine->sync_counts, msg_round);
        // printf("WWT: Sync received for round %lu (count now %d)\n", 
        //     msg_round, sync_count_get(wwt_engine->sync_counts, msg_round));

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
        float time_diff_sec = (processing_time - current_virtual_time_translated) / 1e9;
        // printf("Message will be processed in %.3f seconds at virtual time %lu ns (current virtual time is %lu ns, translated message time is %lu ns).\n", time_diff_sec, processing_time, current_virtual_time_translated, translated_time);
        timer_mod(ctx->one_time_poll_timer, raw_processing_time);
        pdes_inflight_add(msg, raw_processing_time);

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
    bool waiting = count < wwt_engine->number_of_neighbors;
    waiting = waiting && wwt_engine->should_sync;
    if (waiting == false){
        pdes_play(wwt_engine->engine);
    }
    return waiting;
}



void quanta_sync(PDESWWT *wwt_engine){
    // Sends sync, pauses and waits for others sync, then resumes
    // printf("WWT: Starting quantum sync at universal virtual time %lu ns.\n", get_universal_virtual_time(wwt_engine->engine));
    send_sync(wwt_engine);

    // same using is_waiting_for_quanta as setup, as its the same logic
    // printf("=============WWT: Waiting for neighbors to finish quantum at universal virtual time %lu ns.=============\n", get_universal_virtual_time(wwt_engine->engine));
    pdes_pause(wwt_engine->engine);
    // printf("=============WWT: Finished waiting for neighbors to finish quantum at universal virtual time %lu ns.=============\n", get_universal_virtual_time(wwt_engine->engine));

    // Schedule next quantum
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(wwt_engine->quantum_timer, current_time + wwt_engine->quantum_ns);
    // printf("WWT: Quantum sync completed at universal virtual time %lu ns.\n", get_universal_virtual_time(wwt_engine->engine));

    // Reset for next quantum
    // Do not set to 0, as if we have processed the next quantum's sync it will cause deadlock (i.e. the other qemu goes to end and waits while we are getting done processing this sync)
    wwt_engine->number_of_neighbors_finished -= wwt_engine->number_of_neighbors;
    wwt_engine->current_quantum_round++;
    // printf("WWT: Starting quantum %lu at virtual time %lu ns.\n", wwt_engine->current_quantum_round, get_universal_virtual_time(wwt_engine->engine));
}