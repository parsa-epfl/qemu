/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright 2021 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TCG_ACCEL_OPS_MTTCG_H
#define TCG_ACCEL_OPS_MTTCG_H

/* kick MTTCG vCPU thread */
void mttcg_kick_vcpu_thread(CPUState *cpu);

/* start an mttcg vCPU thread */
void mttcg_start_vcpu_thread(CPUState *cpu);

/* initialize the barrier for quantum */
void mttcg_initialize_barrier(void);

/* initialize the IPC table for each thread */
void mttcg_initialize_core_info_table(const char *file_name);

#endif /* TCG_ACCEL_OPS_MTTCG_H */
