#ifndef HW_RISCV_LITE_H
#define HW_RISCV_LITE_H

#include "qemu/osdep.h"
#include "hw/boards.h"


typedef struct RISCVHartArrayState RISCVHartArrayState;

typedef struct LiteDevice {
    MachineState         parent;
    RISCVHartArrayState *soc;
} LiteDevice;

#define TYPE_LITE_MACHINE MACHINE_TYPE_NAME("lite")

DECLARE_INSTANCE_CHECKER(LiteDevice,
                         LITE_MACHINE,
                         TYPE_LITE_MACHINE)


#endif