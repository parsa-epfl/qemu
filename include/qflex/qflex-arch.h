#ifndef QFLEX_ARCH_H
#define QFLEX_ARCH_H

#include "qemu/osdep.h"

#include "qflex/qflex-trace.h"

#define QFLEX_GET_ARCH(name) glue(qflex_get_arch_, name)
#define QFLEX_SET_ARCH(name) glue(qflex_set_arch_, name)

uint32_t QFLEX_GET_ARCH(inst)(CPUState *cs, uint64_t addr);
char    *QFLEX_GET_ARCH(dis )(CPUState *cs, uint64_t addr);
uint64_t QFLEX_GET_ARCH(csr )(CPUState *cs, int idx);
uint64_t QFLEX_GET_ARCH(fpr )(CPUState *cs, int idx);
uint64_t QFLEX_GET_ARCH(gpr )(CPUState *cs, int idx);
uint32_t QFLEX_GET_ARCH(irq )(CPUState *cs);
uint64_t QFLEX_GET_ARCH(pc  )(CPUState *cs);
uint64_t QFLEX_GET_ARCH(pa  )(CPUState *cs, uint64_t addr, int access);
uint64_t QFLEX_GET_ARCH(pl  )(CPUState *cs);
char    *QFLEX_GET_ARCH(snap)(CPUState *cs);

void     QFLEX_SET_ARCH(pc  )(CPUState *cs, uint64_t pc);

void     QFLEX_GET_ARCH(dec )(qflex_insn_t *insn);

#endif /* QFLEX_ARCH_H */
