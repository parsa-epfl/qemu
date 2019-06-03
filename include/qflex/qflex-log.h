#ifndef QFLEX_LOG_H
#define QFLEX_LOG_H

#include "qemu/osdep.h"
#include "qemu/log.h"

extern int qflex_loglevel;

#define QFLEX_LOG_GENERAL       (1 << 0)
#define QFLEX_LOG_INTERRUPT     (1 << 1)
#define QFLEX_LOG_USER_EXEC     (1 << 2)
#define QFLEX_LOG_KERNEL_EXEC   (1 << 3)
#define QFLEX_LOG_MAGIC_INSN    (1 << 4)

/* Returns true if a bit is set in the current loglevel mask
 */
static inline bool qflex_loglevel_mask(int mask)
{
    return (qflex_loglevel & mask) != 0;
}

/* Logging functions: */

/* log only if a bit is set on the current loglevel mask:
 * @mask: bit to check in the mask
 * @fmt: printf-style format string * @args: optional arguments for format string
 */
#define qflex_log_mask(MASK, FMT, ...)                  \
    do {                                                \
        if (unlikely(qflex_loglevel_mask(MASK))) {      \
            qemu_log(FMT, ## __VA_ARGS__);              \
        }                                               \
    } while (0)

#define qflex_log_mask_enable(MASK)     \
    do { qflex_loglevel |= MASK; } while (0)

#define qflex_log_mask_disable(MASK)    \
    do { qflex_loglevel &= ~MASK; } while (0)

#endif /* QFLEX_LOG_H */