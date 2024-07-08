#include "qemu/osdep.h"
#include "qemu/config-file.h"
#include "qemu/option.h"

#include "qflex/qflex.h"
#include "qflex/qflex-opts.h"


QemuOptsList qemu_qflex_opts = {
    .name = "qflex",
    .merge_lists = true,
    .head = QTAILQ_HEAD_INITIALIZER(qemu_qflex_opts.head),
    .desc = {
        {
            .name = "timing",
            .type = QEMU_OPT_BOOL,
        },
        {
            .name = "update",
            .type = QEMU_OPT_BOOL,
        },
        {
            .name = "cycles",
            .type = QEMU_OPT_NUMBER,
        },
        {
            .name = "cycles-mask",
            .type = QEMU_OPT_NUMBER,
        },
        {
            .name = "lib-path",
            .type = QEMU_OPT_STRING,
        },
        {
            .name = "cfg-path",
            .type = QEMU_OPT_STRING,
        },
        {
            .name = "debug",
            .type = QEMU_OPT_STRING,
        },
        { /* end of list */ }
    },
};

// in vl.c
extern bool singlestep;

int qflex_parse_opts(int index, const char *optarg, Error **errp) {
    QemuOpts *opts = qemu_opts_parse_noisily(qemu_find_opts("qflex"), optarg, false);

    if (!opts)
        exit(EXIT_FAILURE);

    singlestep = true;

    qflex_state.timing      = qemu_opt_get_bool  (opts, "timing",      false);
    qflex_state.update      = qemu_opt_get_bool  (opts, "update",      false);
    qflex_state.cycles      = qemu_opt_get_number(opts, "cycles",      1);
    qflex_state.cycles_mask = qemu_opt_get_number(opts, "cycles-mask", 1);

    if (qflex_state.timing)
        qflex_state.update = false;

    const char *lib_path = qemu_opt_get(opts, "lib-path");
    const char *cfg_path = qemu_opt_get(opts, "cfg-path");
    const char *dbg_mode = qemu_opt_get(opts, "debug");

    if (lib_path)
        qflex_state.lib_path = strdup(lib_path);
    if (cfg_path)
        qflex_state.cfg_path = strdup(cfg_path);
    if (dbg_mode)
        qflex_state.dbg_mode = strdup(dbg_mode);

    qemu_opts_del(opts);

    return 0;
}
