#ifndef QFLEX_CONFIG_H
#define QFLEX_CONFIG_H

#include "qemu/osdep.h"

extern QemuOptsList qemu_qflex_opts;

int qflex_parse_opts(int index, const char *optarg, Error **errp);

#endif
