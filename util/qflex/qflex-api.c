//  DO-NOT-REMOVE begin-copyright-block
// QFlex consists of several software components that are governed by various
// licensing terms, in addition to software that was developed internally.
// Anyone interested in using QFlex needs to fully understand and abide by the
// licenses governing all the software components.
//
// ### Software developed externally (not by the QFlex group)
//
//     * [NS-3] (https://www.gnu.org/copyleft/gpl.html)
//     * [QEMU] (http://wiki.qemu.org/License)
//     * [SimFlex] (http://parsa.epfl.ch/simflex/)
//     * [GNU PTH] (https://www.gnu.org/software/pth/)
//
// ### Software developed internally (by the QFlex group)
// **QFlex License**
//
// QFlex
// Copyright (c) 2020, Parallel Systems Architecture Lab, EPFL
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice,
//       this list of conditions and the following disclaimer in the documentation
//       and/or other materials provided with the distribution.
//     * Neither the name of the Parallel Systems Architecture Laboratory, EPFL,
//       nor the names of its contributors may be used to endorse or promote
//       products derived from this software without specific prior written
//       permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE PARALLEL SYSTEMS ARCHITECTURE LABORATORY,
// EPFL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//  DO-NOT-REMOVE end-copyright-block
#ifdef __cplusplus
extern "C" {
#endif

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/qapi-commands-misc.h"
#include "hw/boards.h"

#include "qflex/qflex.h"
#include "qflex/qflex-api.h"
#include "qflex/qflex-arch.h"


static conf_object_t *qflex_cpus = NULL;


bool QEMU_cpu_busy(conf_object_t *cpu){
    CPUState *cs = (CPUState *)(cpu->object);
    return !cs->halted;
}

int QEMU_cpu_exec(conf_object_t *cpu, bool count) {
    CPUState *cs = (CPUState *)(cpu->object);
    if (count)
        qflex_state.cycles--;
    return qflex_step(cs);
}

char *QEMU_disass(conf_object_t *cpu, uint64_t addr){
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(dis)(cs, addr);
}

conf_object_t * QEMU_get_all_cpus(void) {
    return qflex_cpus;
}

conf_object_t *QEMU_get_cpu_by_idx(int idx) {
    return &qflex_cpus[idx];
}

int QEMU_get_cpu_idx(conf_object_t *cpu) {
    CPUState *cs = (CPUState *)(cpu->object);
    return cs->cpu_index;
}

uint64_t QEMU_get_csr(conf_object_t *cpu, int idx) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(csr)(cs, idx);
}

bool QEMU_get_en(void) {
    return qflex_state.enabled;
}

uint64_t QEMU_get_fpr(conf_object_t *cpu, int idx) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(fpr)(cs, idx);
}

uint64_t QEMU_get_gpr(conf_object_t *cpu, int idx) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(gpr)(cs, idx);
}

uint32_t QEMU_get_irq(conf_object_t *cpu) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(irq)(cs);
}

bool QEMU_get_mem(uint8_t *buf, physical_address_t pa, int bytes) {
    if ((int64_t)(pa) < 0) {
        memset(buf, -1, bytes);
        return true;
    }

    return !!cpu_physical_memory_read(pa, buf, bytes);
}

int QEMU_get_num_cores(void) {
    return current_machine->smp.cores;
}

conf_object_t *QEMU_get_obj_by_name(const char *name) {
    int i;

    for(i = 0; i < current_machine->smp.cores; i++)
        if(strcmp(qflex_cpus[i].name, name) == 0)
            return &(qflex_cpus[i]);

    return NULL;
}

physical_address_t QEMU_get_pa(conf_object_t *cpu, trans_type_t trans, logical_address_t va) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(pa)(cs, va, trans);
}

uint64_t QEMU_get_pc(conf_object_t *cpu) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(pc)(cs);
}

int QEMU_get_pl(conf_object_t *cpu) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(pl)(cs);
}

char *QEMU_get_snap(conf_object_t* cpu) {
    CPUState *cs = (CPUState *)(cpu->object);
    return QFLEX_GET_ARCH(snap)(cs);
}

int QEMU_mem_op_is_data(generic_transaction_t *mop) {
    return mop->type == QEMU_Trans_Load ||
           mop->type == QEMU_Trans_Store;
}

int QEMU_mem_op_is_write(generic_transaction_t *mop) {
    return mop->type == QEMU_Trans_Store;
}

void QEMU_set_pc(conf_object_t *cpu, uint64_t pc) {
    CPUState *cs = (CPUState *)(cpu->object);
    QFLEX_SET_ARCH(pc)(cs, pc);
}

void QEMU_stop(const char *msg) {
    qemu_log("%s\n", msg);

    qmp_stop(NULL);
}

void QEMU_tick(void) {
    qflex_tick();
}

void qflex_init_api(void) {
    int n = current_machine->smp.cores;
    int i;

    qflex_cpus = malloc(sizeof(conf_object_t) * n);

    for (i = 0 ; i < n; i++) {
        GString *str = g_string_new("");
        g_string_printf(str, "cpu%d", i);

        CPUState *cs = qemu_get_cpu(i);

        qflex_cpus[i].name   = str->str;
        qflex_cpus[i].object = cs;

        // just loaded from snapshot
        cs->halted = 0;

        g_string_free(str, false);
    }
}

#ifdef __cplusplus
}
#endif
