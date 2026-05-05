#include "qemu/osdep.h"
#include "net/pdes-checkpoint.h"
#include "qemu/queue.h"
#include "qemu/timer.h"

typedef struct ScheduledMessage {
    Message msg;
    int64_t scheduled_time_ns;
    QLIST_ENTRY(ScheduledMessage) next;
} ScheduledMessage;

// TODO change this to hashmap to be faster with lower overhead
static QLIST_HEAD(, ScheduledMessage) pending_messages = QLIST_HEAD_INITIALIZER(pending_messages);
static int pending_count = 0;

int get_number_of_inflight_messages() {
    return pending_count;
}

void pdes_inflight_add(Message *msg, int64_t scheduled_time_ns) {
    ScheduledMessage *entry = g_new0(ScheduledMessage, 1);
    entry->msg = *msg;
    entry->scheduled_time_ns = scheduled_time_ns;
    QLIST_INSERT_HEAD(&pending_messages, entry, next);
    pending_count++;
    // printf("PEDS pending message added, total pending count: %d\n", pending_count);
}

void pdes_inflight_remove(Message *msg, int64_t scheduled_time_ns) {
    ScheduledMessage *entry, *tmp;
    // TODO change to hashmap, because this for can become an overhead on each message popping
    QLIST_FOREACH_SAFE(entry, &pending_messages, next, tmp) {
        if (entry->scheduled_time_ns == scheduled_time_ns &&
            entry->msg.len == msg->len &&
            memcmp(entry->msg.data, msg->data, msg->len) == 0) {
            QLIST_REMOVE(entry, next);
            g_free(entry);
            pending_count--;
            // printf("PEDS pending message removed, total pending count: %d\n", pending_count);
            return;
        }
    }
    // Print message not found, print queried message, its time stamp and then all existing messages
    printf("PEDS pending message to remove not found. Queried message len: %u, scheduled_time_ns: %" PRId64 "\n", msg->len, scheduled_time_ns);
    QLIST_FOREACH(entry, &pending_messages, next) {
        printf("Existing message len: %u, scheduled_time_ns: %" PRId64 "\n", entry->msg.len, entry->scheduled_time_ns);
    }
    assert(false && "Message to remove not found in pending messages");
}

InflightMessageArray *pdes_inflight_get_all(void) {
    InflightMessageArray *result = g_new0(InflightMessageArray, 1);
    result->count = pending_count;
    
    if (pending_count == 0) {
        result->messages = NULL;
        result->scheduled_times = NULL;
        return result;
    }

    result->messages = g_new0(Message, pending_count);
    result->scheduled_times = g_new0(int64_t, pending_count);

    ScheduledMessage *entry;
    int i = 0;
    QLIST_FOREACH(entry, &pending_messages, next) {
        result->messages[i] = entry->msg;
        result->scheduled_times[i] = entry->scheduled_time_ns;
        i++;
    }

    return result;
}

void pdes_inflight_array_free(InflightMessageArray *arr) {
    if (arr) {
        g_free(arr->messages);
        g_free(arr->scheduled_times);
        g_free(arr);
    }
}

int pdes_inflight_count(void) {
    return pending_count;
}

int pdes_inflight_save_json(const char *checkpoint_name) {
    char *filename = get_json_file_name(checkpoint_name);
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        return -1;
    }

    fprintf(fp, "{\n  \"inflight_messages\": [\n");

    ScheduledMessage *entry;
    int i = 0;
    QLIST_FOREACH(entry, &pending_messages, next) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"scheduled_time_ns\": %" PRId64 ",\n", entry->scheduled_time_ns);
        fprintf(fp, "      \"timestamp_ns\": %" PRIu64 ",\n", entry->msg.ts_ns);
        fprintf(fp, "      \"type\": %u,\n", entry->msg.type);
        fprintf(fp, "      \"len\": %u,\n", entry->msg.len);
        fprintf(fp, "      \"data\": [");
        for (uint32_t j = 0; j < entry->msg.len; j++) {
            fprintf(fp, "%u", entry->msg.data[j]);
            if (j < entry->msg.len - 1) {
                fprintf(fp, ", ");
            }
        }
        fprintf(fp, "]\n");
        fprintf(fp, "    }%s\n", (i < pending_count - 1) ? "," : "");
        i++;
    }

    printf("Saved %d in-flight messages to %s\n", pending_count, filename);

    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    g_free(filename);
    return 0;
}

InflightMessageArray *pdes_inflight_load_json(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *content = g_malloc(fsize + 1);
    fread(content, 1, fsize, fp);
    content[fsize] = '\0';
    fclose(fp);

    int capacity = 16;
    int count = 0;
    Message *messages = g_new0(Message, capacity);
    int64_t *scheduled_times = g_new0(int64_t, capacity);

    char *ptr = content;
    while ((ptr = strstr(ptr, "\"scheduled_time_ns\":")) != NULL) {
        if (count >= capacity) {
            capacity *= 2;
            messages = g_renew(Message, messages, capacity);
            scheduled_times = g_renew(int64_t, scheduled_times, capacity);
        }

        int64_t scheduled_time;
        uint64_t timestamp;
        uint32_t type, len;

        sscanf(ptr, "\"scheduled_time_ns\": %" SCNd64, &scheduled_time);
        scheduled_times[count] = scheduled_time;

        ptr = strstr(ptr, "\"timestamp_ns\":");
        sscanf(ptr, "\"timestamp_ns\": %" SCNu64, &timestamp);
        messages[count].ts_ns = timestamp;

        ptr = strstr(ptr, "\"type\":");
        sscanf(ptr, "\"type\": %u", &type);
        messages[count].type = (uint8_t)type;

        ptr = strstr(ptr, "\"len\":");
        sscanf(ptr, "\"len\": %u", &len);
        messages[count].len = len;

        ptr = strstr(ptr, "\"data\": [");
        ptr += strlen("\"data\": [");

        for (uint32_t j = 0; j < len; j++) {
            unsigned int byte;
            sscanf(ptr, "%u", &byte);
            messages[count].data[j] = (uint8_t)byte;
            ptr = strchr(ptr, ',');
            if (ptr) ptr++;
        }

        count++;
    }

    g_free(content);

    InflightMessageArray *result = g_new0(InflightMessageArray, 1);
    result->messages = messages;
    result->scheduled_times = scheduled_times;
    result->count = count;

    return result;
}

int pdes_inflight_restore_and_schedule(const char *checkpoint_name, PDESFinalRecvCallback recv_cb, void *recv_opaque) {
    char *filename = get_json_file_name(checkpoint_name);
    InflightMessageArray *arr = pdes_inflight_load_json(filename);
    if (!arr) {
        return -1;
    }

    for (int i = 0; i < arr->count; i++) {
        MessageReceiveContext *ctx = g_new0(MessageReceiveContext, 1);
        ctx->msg = arr->messages[i];
        ctx->recv_cb = recv_cb;
        ctx->recv_opaque = recv_opaque;
        ctx->timestamp_ns = arr->scheduled_times[i];
        ctx->one_time_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, 
            (QEMUTimerCB *)process_message_at_virtual_time, ctx);

        timer_mod(ctx->one_time_poll_timer, arr->scheduled_times[i]);

        pdes_inflight_add(&arr->messages[i], arr->scheduled_times[i]);
    }

    int count = arr->count;
    pdes_inflight_array_free(arr);
    g_free(filename);
    return count;
}

char *get_json_file_name(const char *base_name){
    // Return  base_name + _in_flight.json
    return g_strdup_printf("%s_in_flight.json", base_name);
}


bool validate_checkpoint(const char **check_point_name){
    
    char *name = *check_point_name;

    // TODO this is a very basic implementation, need to add error handling and make it more robust later
    PDESEngine *engine = get_singleton_engine();
    if (engine != NULL) {
        if (!engine->master){
            // TODO this solution needs to be improved instead of flag through string
            // check name size is at least 5 chars and the first 5 chars are "QPDES"
            if (name != NULL && strlen(name) >= 5 && strncmp(name, "QPDES", 5) == 0) {
                // This is a pdes snapshot initiated by master, we need to drain the pdes before we can save the snapshot
                // QPDESinit_warmed goes through here, but one initiated by WormCache goes through the next else if statement
                // remove "QPDES" from name to keep consistent
                *check_point_name = name + 5;
                printf("modified checkpoint name for PDESEngine: %s and continuing with checkpointing.\n", *check_point_name);
                return true;
            }else if (name != NULL && strcmp(name, "init_warmed") == 0) {
                printf("!!!!!!!!!!!!!!!! THIS SHOULD NOT BE HAPPENINGING !!!!!!!!!!!!!!!!\n");
                return false;
                // TODO expand this to multiple nodes
                // This is not the master so it can't initiate it, just let master know we are ready
                printf("Received drain signal but not master, marking engine as done for init.\n");
                int res = send_initiate_checkpoint_message(engine);
                assert (res == 0 && "Failed to send checkpoint initiation message to master");
                printf("Sent checkpoint initiation message to master successfully, waiting for checkpoint initiation signal.\n");
                return false;
            }else{
                // We will skip any checkpointing as master decides when to checkpoint, need to return (caused by pdes)
                printf("Skipping snapshot as not master PDESEngine\n");
                return false;
            } 
        }else{
            // Master
            if (name != NULL && strcmp(name, "init_warmed") == 0){
                // Master can initiate, but only once everyone is ready
                if (engine->init_flag < 1){
                    // if neighbours not ready yet (handled by engine when we get the message)
                    printf("Master received checkpoint initiation signal but neighbors not ready yet, waiting for drain signals from neighbors.\n");
                    engine->master_init = true;
                    // Once others are ready will puush through
                    return false;
                }else{
                    // Everyone is already ready just go for it
                    printf("Master received checkpoint initiation signal and neighbors are ready, initiating checkpoint immediately.\n");
                    return true;
                }
            }else{
                // Any other type of checkpoint can just go through
                printf("Master received checkpoint initiation signal for non-init checkpoint, initiating checkpoint immediately.\n");
                return true;
            }
        }
    }else{
        // not multi-node, just go through with checkpointing as normal
        printf("No PDESEngine found, assuming single node and allowing checkpointing to proceed as normal.\n");
        return true;
    }

}



// TODO make this a field
void create_checkpoint_bh(bool exit_after){
    PDESWWT *wwt_engine = get_singleton_wwt_engine();
    if (!wwt_engine->engine->needs_to_checkpoint){
        return;
    }

    printf("WWT: Finished waiting for quanta for round %lu, starting checkpoint for this quantum.\n", wwt_engine->current_quantum_round - 1);
    wwt_engine->engine->checkpoint_in_progress = false;
    printf("WWT: Starting checkpoint for quantum %lu with snapshot name %s and format %d.\n", wwt_engine->current_quantum_round - 1, wwt_engine->engine->checkpoint_name, wwt_engine->engine->checkpoint_format);
    save_snapshot(wwt_engine->engine->checkpoint_name, true, NULL, false, NULL, wwt_engine->engine->checkpoint_format, NULL);
    printf("WWT: Finished checkpoint for quantum %lu, starting next quantum.\n", wwt_engine->current_quantum_round - 1);
    wwt_engine->engine->needs_to_checkpoint = false;
    wwt_engine->engine->notified_neighbors = false;
    wwt_engine->engine->checkpoint_quantum_round = 0;
    // delete and remove bh
    qemu_bh_delete(wwt_engine->engine->boundry_checkpoint_bh);
    wwt_engine->engine->boundry_checkpoint_bh = NULL;
    wwt_engine->engine->skip_boundry_check_after_checkpoint = true;

    // TODO make this check more modular (also maybe move verify function to here?)
    // If checkpoint name is init_warmed, destroy engine and stop simulation
    if (strcmp(wwt_engine->engine->checkpoint_name, "init_warmed") == 0 && wwt_engine->engine->master){
        printf("Checkpoint name is init_warmed, destroying engine and stopping simulation.\n");
        pdes_engine_destroy(wwt_engine->engine);
    }
    if (exit_after){
        printf("Exiting after checkpointing because exit flag is set.\n");
        exit(0);
    }
}