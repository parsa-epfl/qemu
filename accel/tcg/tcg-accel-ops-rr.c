/*
 * QEMU TCG Single Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "sysemu/tcg.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-rr.h"
#include "tcg-accel-ops-icount.h"
#include "qemu/plugin-cyan.h"

/* Kick all RR vCPUs */
void rr_kick_vcpu_thread(CPUState *unused)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_exit(cpu);
    };
}

/*
 * TCG vCPU kick timer
 *
 * The kick timer is responsible for moving single threaded vCPU
 * emulation on to the next vCPU. If more than one vCPU is running a
 * timer event we force a cpu->exit so the next vCPU can get
 * scheduled.
 *
 * The timer is removed if all vCPUs are idle and restarted again once
 * idleness is complete.
 */

static QEMUTimer *rr_kick_vcpu_timer;
static CPUState *rr_current_cpu;

static inline int64_t rr_next_kick_time(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TCG_KICK_PERIOD;
}

/* Kick the currently round-robin scheduled vCPU to next */
static void rr_kick_next_cpu(void)
{
    CPUState *cpu;
    do {
        cpu = qatomic_read(&rr_current_cpu);
        if (cpu) {
            cpu_exit(cpu);
        }
        /* Finish kicking this cpu before reading again.  */
        smp_mb();
    } while (cpu != qatomic_read(&rr_current_cpu));
}

static void rr_kick_thread(void *opaque)
{
    timer_mod(rr_kick_vcpu_timer, rr_next_kick_time());
    rr_kick_next_cpu();
}

static void rr_start_kick_timer(void)
{
    if (!rr_kick_vcpu_timer && CPU_NEXT(first_cpu)) {
        rr_kick_vcpu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           rr_kick_thread, NULL);
    }
    if (rr_kick_vcpu_timer && !timer_pending(rr_kick_vcpu_timer)) {
        timer_mod(rr_kick_vcpu_timer, rr_next_kick_time());
    }
}

static void rr_stop_kick_timer(void)
{
    if (rr_kick_vcpu_timer && timer_pending(rr_kick_vcpu_timer)) {
        timer_del(rr_kick_vcpu_timer);
    }
}

static void rr_wait_io_event(void)
{
    CPUState *cpu;

    while (all_cpu_threads_idle()) {
        rr_stop_kick_timer();
        qemu_cond_wait_iothread(first_cpu->halt_cond);
    }

    rr_start_kick_timer();

    CPU_FOREACH(cpu) {
        qemu_wait_io_event_common(cpu);
    }
}

/*
 * Destroy any remaining vCPUs which have been unplugged and have
 * finished running
 */
static void rr_deal_with_unplugged_cpus(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (cpu->unplug && !cpu_can_run(cpu)) {
            tcg_cpus_destroy(cpu);
            break;
        }
    }
}

static void rr_force_rcu(Notifier *notify, void *data)
{
    rr_kick_next_cpu();
}

/*
 * Calculate the number of CPUs that we will process in a single iteration of
 * the main CPU thread loop so that we can fairly distribute the instruction
 * count across CPUs.
 *
 * The CPU count is cached based on the CPU list generation ID to avoid
 * iterating the list every time.
 */
static int rr_cpu_count(void)
{
    static unsigned int last_gen_id = ~0;
    static int cpu_count;
    CPUState *cpu;

    QEMU_LOCK_GUARD(&qemu_cpu_list_lock);

    if (cpu_list_generation_id_get() != last_gen_id) {
        cpu_count = 0;
        CPU_FOREACH(cpu) {
            ++cpu_count;
        }
        last_gen_id = cpu_list_generation_id_get();
    }

    return cpu_count;
}

static void load_ipc_table(uint64_t *table, uint64_t core_count, const char *filename) {
    // Each line of the file contains the IPC of the core.
    // And it should be an integer.
    FILE *file = fopen(filename, "r");
    if (!file) {
        // If this file does not exist, we assume all cores have the same IPC 1.
        for (int i = 0; i < core_count; i++) {
            table[i] = 1;
        }

        // print a log.
        fprintf(stderr, "IPC file %s does not exist. Assume all cores have the same IPC 1.\n", filename);
    } else {
        char line[1024];
        int i = 0;

        while (fgets(line, 1024, file)) {
            table[i] = strtoull(line, NULL, 10);
            i++;
        }
        fclose(file);
    }
}

/*
 * In the single-threaded case each vCPU is simulated in turn. If
 * there is more than a single vCPU we create a simple timer to kick
 * the vCPU and ensure we don't get stuck in a tight loop in one vCPU.
 * This is done explicitly rather than relying on side-effects
 * elsewhere.
 */

static void *rr_cpu_thread_fn(void *arg)
{
    Notifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    rcu_register_thread();
    force_rcu.notify = rr_force_rcu;
    rcu_add_force_rcu_notifier(&force_rcu);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    uint64_t cpu_count = 0;

    /* wait for initial kick-off after machine start */
    while (first_cpu->stopped) {
        qemu_cond_wait_iothread(first_cpu->halt_cond);

        /* process any pending work */
        CPU_FOREACH(cpu) {
            cpu_count += 1;
            current_cpu = cpu;
            qemu_wait_io_event_common(cpu);
        }
    }

    // Load the IPC table.
    uint64_t *ipc_table = g_new0(uint64_t, cpu_count);
    load_ipc_table(ipc_table, cpu_count, "ipc.txt");

    rr_start_kick_timer();

    cpu = first_cpu;

    /* process any pending work */
    cpu->exit_request = 1;

    uint64_t next_check_threshold = icount_checking_period;
    
    uint64_t cycle = 0;
    uint64_t last_cycle = 0;

    while (1) {
        /* Only used for icount_enabled() */
        int64_t cpu_budget = 0;

        qemu_mutex_unlock_iothread();
        replay_mutex_lock();
        qemu_mutex_lock_iothread();

        if (icount_enabled()) {
//            int cpu_count = rr_cpu_count();

            /* Account partial waits to QEMU_CLOCK_VIRTUAL.  */
            icount_account_warp_timer();
            /*
             * Run the timers here.  This is much more efficient than
             * waking up the I/O thread and waiting for completion.
             */
            icount_handle_deadline();

            int64_t original_budget = icount_percpu_budget(1);

            if (icount_switch_period != 0) {
                cpu_budget = icount_switch_period > original_budget ? original_budget : icount_switch_period;
            }
        }

        replay_mutex_unlock();

        if (!cpu) {
            cycle += cpu_budget;
            if (icount_checking_period != 0 && cycle >= next_check_threshold) {
                if (cyan_icount_periodic_checking_cb) cyan_icount_periodic_checking_cb(cycle - last_cycle);
                next_check_threshold += icount_checking_period;
                last_cycle = cycle;
            }

            // The time is increased here to avoid problem.
            icount_increase(cpu_budget);

           cpu = first_cpu;
        }

        while (cpu && cpu_work_list_empty(cpu) && !cpu->exit_request) {
            /* Store rr_current_cpu before evaluating cpu_can_run().  */
            qatomic_set_mb(&rr_current_cpu, cpu);

            current_cpu = cpu;

            qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                              (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);

            if (cpu_can_run(cpu)) {
                int r;

                qemu_mutex_unlock_iothread();
                if (icount_enabled()) {
                    icount_prepare_for_run(cpu, cpu_budget * ipc_table[cpu->cpu_index]);
                }
                r = tcg_cpus_exec(cpu);
                if (icount_enabled()) {
                    icount_process_data(cpu);
                }
                qemu_mutex_lock_iothread();

                if (r == EXCP_DEBUG) {
                    cpu_handle_guest_debug(cpu);
                    break;
                } else if (r == EXCP_ATOMIC) {
                    qemu_mutex_unlock_iothread();
                    cpu_exec_step_atomic(cpu);
                    qemu_mutex_lock_iothread();
                    break;
                }
            } else if (cpu->stop) {
                if (cpu->unplug) {
                    cpu = CPU_NEXT(cpu);
                }
                break;
            }

            cpu = CPU_NEXT(cpu);
        } /* while (cpu && !cpu->exit_request).. */

        /* Does not need a memory barrier because a spurious wakeup is okay.  */
        qatomic_set(&rr_current_cpu, NULL);

        if (cpu && cpu->exit_request) {
            qatomic_set_mb(&cpu->exit_request, 0);
        }

        if (icount_enabled() && all_cpu_threads_idle()) {
            /*
             * When all cpus are sleeping (e.g in WFI), to avoid a deadlock
             * in the main_loop, wake it up in order to start the warp timer.
             */
            qemu_notify_event();
        }

        rr_wait_io_event();

        // Activate the plugn system.
        rr_deal_with_unplugged_cpus();
    }

    rcu_remove_force_rcu_notifier(&force_rcu);
    rcu_unregister_thread();

    g_free(ipc_table);

    return NULL;
}

void rr_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    static QemuCond *single_tcg_halt_cond;
    static QemuThread *single_tcg_cpu_thread;

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, false);

    if (!single_tcg_cpu_thread) {
        cpu->thread = g_new0(QemuThread, 1);
        cpu->halt_cond = g_new0(QemuCond, 1);
        qemu_cond_init(cpu->halt_cond);

        /* share a single thread for all cpus with TCG */
        snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "ALL CPUs/TCG");
        qemu_thread_create(cpu->thread, thread_name,
                           rr_cpu_thread_fn,
                           cpu, QEMU_THREAD_JOINABLE);

        single_tcg_halt_cond = cpu->halt_cond;
        single_tcg_cpu_thread = cpu->thread;
    } else {
        /* we share the thread */
        cpu->thread = single_tcg_cpu_thread;
        cpu->halt_cond = single_tcg_halt_cond;
        cpu->thread_id = first_cpu->thread_id;
        cpu->can_do_io = 1;
        cpu->created = true;
    }
}
