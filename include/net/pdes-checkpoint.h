#ifndef NET_PDES_CHECKPOINT_H
#define NET_PDES_CHECKPOINT_H

#include <stdint.h>
#include <stddef.h>
#include "pdes-communicator.h"
#include "pdes-engine.h"

typedef struct InflightMessageArray {
    Message *messages;
    int64_t *scheduled_times;
    int count;
} InflightMessageArray;

void pdes_inflight_add(Message *msg, int64_t scheduled_time_ns);
void pdes_inflight_remove(Message *msg, int64_t scheduled_time_ns);
InflightMessageArray *pdes_inflight_get_all(void);
void pdes_inflight_array_free(InflightMessageArray *arr);
int pdes_inflight_count(void);
int pdes_inflight_save_json(const char *filename);
InflightMessageArray *pdes_inflight_load_json(const char *filename);
int pdes_inflight_restore_and_schedule(const char *filename, PDESFinalRecvCallback recv_cb, void *recv_opaque);

char *get_json_file_name(const char *base_name);
int get_number_of_inflight_messages(void);

bool validate_checkpoint(const char **check_point_name);


#endif