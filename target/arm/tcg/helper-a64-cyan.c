// This file is added by Cyan for QEMU plugin. 

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "gdbstub/helpers.h"
#include "exec/helper-proto.h"
#include "cpu.h"
#include "hw/core/cpu.h"
#include "qemu/plugin-cyan.h"



void HELPER(cyan_branch_resolved)(CPUARMState *env, uint64_t pc, uint64_t target, uint32_t hint_flags) {
    if (cyan_br_cb) {
        // branch_resolved: void (*branch_resolved)(unsigned int vcpu_index, uint64_t pc, uint64_t target, uint32_t hint_flags);
        cyan_br_cb(current_cpu->cpu_index, pc, target, hint_flags);
        assert((target - pc) % 4 == 0);
    }
}