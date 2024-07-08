#include <dlfcn.h>

#include "qemu/osdep.h"
#include "qemu/option_int.h"

#include "hw/boards.h"
#include "sysemu/tcg.h"

#include "qflex/qflex.h"
#include "qflex/qflex-api.h"
#include "qflex/qflex-arch.h"


qflex_state_t qflex_state = {
    .enabled     = false,

    .timing      = false,
    .update      = 0,
    .cycles      = 0,
    .cycles_mask = 1,

    .lib_path    = NULL,
    .cfg_path    = NULL,
    .dbg_mode    = NULL,

    .dump        = NULL,
    .jump        = NULL
};

FLEXUS_API_t flexus_api;


typedef void (*FLEXUS_INIT_t)(QEMU_API_t *, FLEXUS_API_t *,
        int,
        const char *,
        const char *,
        int64_t,
        const char *);

static bool qflex_init_flexus(const char *cwd) {
    void         *handle = NULL;
    FLEXUS_INIT_t flexus = NULL;

    if (!qflex_state.lib_path) {
        error_report("ERROR: missing lib-path");
        return false;
    }

    if (!qflex_state.cfg_path) {
        error_report("ERROR: missing cfg-path");
        return false;
    }

    if ((handle = dlopen(qflex_state.lib_path, RTLD_LAZY)) == NULL) {
        error_report("ERROR: cannot load %s: %s", qflex_state.lib_path, dlerror());
        return false;
    }


    if ((flexus = (FLEXUS_INIT_t)(dlsym(handle, "flexus_init"))) == NULL) {
        error_report("ERROR: cannot load %s: %s", qflex_state.lib_path, dlerror());
        return false;
    }

    QEMU_API_t qemu_api = {
        .cpu_busy        = QEMU_cpu_busy,
        .cpu_exec        = QEMU_cpu_exec,
        .disass          = QEMU_disass,
        .get_all_cpus    = QEMU_get_all_cpus,
        .get_cpu_by_idx  = QEMU_get_cpu_by_idx,
        .get_cpu_idx     = QEMU_get_cpu_idx,
        .get_csr         = QEMU_get_csr,
        .get_en          = QEMU_get_en,
        .get_fpr         = QEMU_get_fpr,
        .get_gpr         = QEMU_get_gpr,
        .get_irq         = QEMU_get_irq,
        .get_mem         = QEMU_get_mem,
        .get_num_cores   = QEMU_get_num_cores,
        .get_obj_by_name = QEMU_get_obj_by_name,
        .get_pa          = QEMU_get_pa,
        .get_pc          = QEMU_get_pc,
        .get_pl          = QEMU_get_pl,
        .get_snap        = QEMU_get_snap,
        .mem_op_is_data  = QEMU_mem_op_is_data,
        .mem_op_is_write = QEMU_mem_op_is_write,
        .set_pc          = QEMU_set_pc,
        .stop            = QEMU_stop,
        .tick            = QEMU_tick
    };

    flexus(&qemu_api, &flexus_api,
           current_machine->smp.cores,
           qflex_state.cfg_path,
           qflex_state.dbg_mode,
           qflex_state.cycles,
           cwd);

    return true;
}

static char qflex_dump_buf[BUFSIZ];

void qflex_init(Error **errp, const char* snap) {
    if (!tcg_enabled()) {
        error_report("ERROR: TCG must be enabled");
        exit(EXIT_FAILURE);
    }

    qflex_init_api();

    if (!qflex_init_flexus(snap))
        exit(EXIT_FAILURE);

    if (!qflex_state.timing)
        qflex_init_trace();

    // always try to load the snapshot
    // cwd is changed when initializing flexus
    flexus_api.qmp(QMP_FLEXUS_DOLOAD, ".");

    // mmu resync
    for (int i = 0; i < current_machine->smp.cores; i++)
        flexus_api.parse_mmu(i, 0xf);

    const char *bin = qflex_state.timing ? "timing.bin" : "trace.bin";

    qflex_state.dump = fopen(bin, "wb");

    if (!qflex_state.dump) {
        error_report("ERROR: cannot open dump file");
        exit(EXIT_FAILURE);
    }

    setvbuf(qflex_state.dump, qflex_dump_buf, _IOFBF, BUFSIZ);

    qflex_state.enabled = true;
}

void qflex_dump(uint64_t arg, size_t len, void *ptr) {
    uint64_t buf[2] = {arg, len};

    fwrite(buf, sizeof(buf), 1, qflex_state.dump);
    fwrite(ptr,        len,  1, qflex_state.dump);
}

void qflex_done(void) {
    fflush(qflex_state.dump);
    fclose(qflex_state.dump);
}