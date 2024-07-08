#ifndef QFLEX_H
#define QFLEX_H

#include "qemu/osdep.h"

typedef struct __jmp_buf_tag jump_t;

typedef struct qflex_state_t {
    bool        enabled;

    bool        timing;
    int64_t     update;
    int64_t     cycles;
    int64_t     cycles_mask;

    const char *lib_path;
    const char *cfg_path;
    const char *dbg_mode;

    FILE       *dump;
    jump_t     *jump;
} qflex_state_t;

extern qflex_state_t qflex_state;


void qflex_init(Error **errp, const char* snap);
void qflex_tick(void);
int  qflex_step(CPUState *cpu);

void qflex_dump(uint64_t arg, size_t len, void *ptr);
void qflex_done(void);

#endif /* QFLEX_H */
