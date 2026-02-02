#include "qemu/osdep.h"
#include "net/pdes-engine.h"
#include "net/pdes-communicator.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"

struct PDESEngine {
    PDESCommunicator *comm;
    bool needs_sync;
    uint64_t latencyns;
    PDESRecvCallback recv_cb;
    void *recv_opaque;
    QEMUTimer *msg_rec_poll_timer;
    QEMUTimer *sync_poll_timer;
    QEMUTimer *setup_poll_timer;
    QEMUTimer *virtual_heartbeat_timer;
    bool has_first_sync;
    bool pair_has_finished;
    
    // WWT specific
    bool waiting_for_quanta;
};

struct message_receive_context {
    PDESEngine *engine;
    Message msg;
    QEMUTimer *one_time_poll_timer;
};
static void virtual_time_heartbeat(void *opaque) {
    PDESEngine *engine = opaque;
    printf(">>>>>> Virtual heartbeat fired at virtual time %lu\n", 
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    
    // Reschedule
    timer_mod(engine->virtual_heartbeat_timer, 
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10);
}
uint64_t get_current_virtual_for_normal_message(PDESEngine *engine) {
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + engine->latencyns;
}

uint64_t get_current_virtual_for_sync_message(PDESEngine *engine) {
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + engine->latencyns;
}
PDESEngine *pdes_engine_create(
    const char *shm_send, 
    const char *shm_recv, 
    bool sync, 
    uint64_t latencyns,
    PDESRecvCallback cb, 
    void *opaque
) {
    PDESEngine *engine = g_new0(PDESEngine, 1);
    engine->comm = pdes_comm_create(shm_send, shm_recv);
    engine->needs_sync = sync;
    engine->latencyns = latencyns;
    engine->recv_cb = cb;
    engine->recv_opaque = opaque;
    engine->has_first_sync = false;
    engine->waiting_for_quanta = false;
    engine->pair_has_finished = false;

    // // MUST have a virtual timer in icount mode
    // engine->virtual_heartbeat_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, virtual_time_heartbeat, engine);
    
    // // Schedule it IMMEDIATELY with offset of 1 nanosecond
    // uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    // printf("Scheduling first poll at virtual time %lu (now=%lu)\n", now + 1, now);
    // timer_mod(engine->virtual_heartbeat_timer, now+1);


    engine->msg_rec_poll_timer = timer_new_ns(QEMU_CLOCK_HOST, pdes_engine_poll, engine);
    // Schedule it IMMEDIATELY
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+50000000); // 50 ms

    uint64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    Message sync_msg = create_message(NULL, 0, MSG_TYPE_SYNC, get_current_virtual_for_sync_message(engine));
    pdes_comm_send(engine->comm, &sync_msg);
    
    printf(">>>>>>> NET_INIT_PDES CALLED <<<<<<<\n");
    qemu_notify_event();
    return engine;
}


void pdes_engine_destroy(PDESEngine *engine) {
    printf("==========================================Destroying PDES Engine...==========================================\n");
    Message mssg = create_message(NULL, 0, END_OF_EMULATION, get_current_virtual_for_sync_message(engine));
    pdes_comm_send(engine->comm, &mssg);
    if (engine->comm) {
        pdes_comm_destroy(engine->comm);
    }
    g_free(engine);
    printf("==========================================PDES Engine destroyed.==========================================\n");
}

int pdes_engine_send(PDESEngine *engine, const uint8_t *data, size_t len) {
    /* TODO: Add your PDES decision logic here */
    Message mssg = create_message(data, len, MSG_TYPE_NORMAL, get_current_virtual_for_normal_message(engine));
    // Get current virtual time and add latency
    mssg.ts_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (engine->latencyns);
    return pdes_comm_send(engine->comm, &mssg);
}


void process_message_at_virtual_time(void *opaque) {
    struct message_receive_context *ctx = opaque;
    PDESEngine *engine = ctx->engine;
    Message *msg = &ctx->msg;

    printf("****got to process message of length %u at scheduled time %lu ns****\n", msg->len, msg->ts_ns);

    if (msg->len > 0 && msg->type != MSG_TYPE_SYNC && engine->recv_cb) {
        // Assert the time has arrived
        int64_t current_virtual_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (msg->ts_ns != current_virtual_time) {
            // This should not happen
            // TODO add this back in
            // fprintf(stderr, "==========================Error: message scheduled time has not arrived yet (mssg time %lu != %lu current time)==========================\n", msg->ts_ns, current_virtual_time);
        }
        printf("****sending message of length %u to recv callback at time %lu ns****\n", msg->len, current_virtual_time);
        engine->recv_cb(engine->recv_opaque, msg->data, msg->len);
    }

    g_free(ctx->one_time_poll_timer);
    g_free(ctx);
}

void process_message(PDESEngine *engine, Message *msg) {

    printf("==========================================PDES Engine: Received message of length %u with type %u and timestamp %lu ns==========================================\n", msg->len, msg->type, msg->ts_ns);
    int64_t current_virtual_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    

    if (msg->type == MSG_TYPE_SYNC) {
        if (!engine->has_first_sync){
            printf("$$$$$$$$$$$ at time %lu ns we recieved first sync with timestamp %lu ns $$$$$$$$$\n", current_virtual_time, msg->ts_ns);
        }
        engine->has_first_sync = true;  /* Set synced flag when sync received */
        engine->waiting_for_quanta = false;  /* WWT specific: release waiting quanta */
    }else if (msg->type == END_OF_EMULATION) {
        engine->pair_has_finished = true;
        printf("==========================================PDES Engine: Received end of emulation message from peer.==========================================\n");
    }else if (msg->len > 0 && msg->type != MSG_TYPE_SYNC && engine->recv_cb) {
        // Check message timestamp
        // Make sure time has not passed, if it has, just pass it at current time + 1
        // use timer_new_ms virtual time

        // if (msg->ts_ns < current_virtual_time - 1) {
        //     // TODO turn this to optional error later
        //     // printf("!!!!!!!!!!!!!!!!!!!!!!!! Detected causality violation: message time %lu < current virtual time %lu !!!!!!!!!!!!!!!!!!!!!!!!\n", msg->ts_ns, current_virtual_time);
        //     msg->ts_ns = current_virtual_time + 1; // Schedule it a bit later : TODO temp solution
        // }
        engine->recv_cb(engine->recv_opaque, msg->data, msg->len);
        // u_int64_t time_diff_seconds = (msg->ts_ns - current_virtual_time) / 1000000000;
        // printf("time difference is %lu ns\n", time_diff_seconds);
        // // msg->ts_ns = current_virtual_time + 1;  // Schedule at original time
        // // Schedule the message processing at the correct virtual time
        // struct message_receive_context *ctx = g_new0(struct message_receive_context, 1);
        // ctx->engine = engine;
        // memcpy(&ctx->msg, msg, sizeof(Message));
        // ctx->one_time_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, process_message_at_virtual_time, ctx);
        // timer_mod(ctx->one_time_poll_timer, msg->ts_ns);
    }
}

void pdes_engine_poll(void *opaque) {
    // printf("+++++++++++++++ PDES Engine polling for messages +++++++++++++++\n");
    u_int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    PDESEngine *engine = opaque;
    Message msg;
    static int poll_count = 0;

    printf("+++++++++++++++ PDES Engine polling #%d at host time %lu, virtual time %lu +++++++++++++++\n", 
            poll_count++,
            qemu_clock_get_ns(QEMU_CLOCK_HOST),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
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

    schedule_poll(engine);
}

void schedule_poll(void *opaque){
    PDESEngine *engine = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+50000000); // 50 ms
}