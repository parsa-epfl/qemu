#ifndef QFLEX_TRACE_H
#define QFLEX_TRACE_H

#include "qemu/osdep.h"

typedef struct {
    uint64_t va;
    uint32_t insn;
    int      size;
    int      br_type;
    bool     is_mem;
    bool     is_user;

    struct {
        int  size;
        bool is_load;
        bool is_store;
        bool is_signed;
        bool is_atomic;
    } mem;

} qflex_insn_t;

void qflex_init_trace(void);

#endif
