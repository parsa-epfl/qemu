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
// Copyright (c) 2021, Parallel Systems Architecture Lab, EPFL
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
#ifndef QFLEX_H
#define QFLEX_H

#include <stdbool.h>

#include "qemu/osdep.h"
#include "qemu/thread.h"

#include "qflex/qflex-log.h"

#define QFLEX_EXEC_IN  (0)
#define QFLEX_EXEC_OUT (1)

/** NOTE for cpu_exec (accel/tcg/cpu_exec.c)
  * Depending on the desired type of execution,
  * cpu_exec should break from the double while loop
  * in the correct manner.
  */
typedef enum {
    PROLOGUE,   // Breaks when the Arch State is back to the initial user program
    SINGLESTEP, // Breaks when a single TB (instruction) is executed
    EXECEXCP,   // Breaks when the exeception routine is done
    QEMU        // Normal qemu execution
} QFlexExecType_t;

extern bool qflex_inst_done;
extern bool qflex_prologue_done;
extern uint64_t qflex_prologue_pc;
extern bool qflex_broke_loop;
extern bool qflex_control_with_flexus;
extern bool qflex_trace_enabled;

/** qflex_api_values_init
 * Inits extern flags and vals
 */
void qflex_api_values_init(CPUState *cpu);

/** qflex_prologue
 * When starting from a saved vm state, QEMU first batch of instructions
 * are many nested interrupts.
 * This functions skips this part till QEMU is back into the USER program
 */
int qflex_prologue(CPUState *cpu);
int qflex_singlestep(CPUState *cpu);

/** qflex_cpu_step (cpus.c)
 */
int qflex_cpu_step(CPUState *cpu, QFlexExecType_t type);

/** qflex_cpu_exec (accel/tcg/cpu-exec.c)
 * mirror cpu_exec, with qflex execution flow control
 * for TCG execution. Type defines how the while loop break.
 */
int qflex_cpu_exec(CPUState *cpu, QFlexExecType_t type);


/* Get and Setters for flags and vars
 *
 */
static inline bool qflex_is_inst_done(void)     { return qflex_inst_done; }
static inline bool qflex_is_prologue_done(void) { return qflex_prologue_done; }
static inline bool qflex_update_prologue_done(uint64_t cur_pc) {
    qflex_prologue_done = ((cur_pc >> 48) == 0);
    return qflex_prologue_done;
}
static inline void qflex_update_inst_done(bool done) { qflex_inst_done = done; }

/* Wrapper function for porting "sleep" to work with either PTH or POSIX threads.
 * Parameters:
 *  - us: the time to sleep in microseconds
 */
void portable_usleep(unsigned long us);

#endif /* QFLEX_H */
