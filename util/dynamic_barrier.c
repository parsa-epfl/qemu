#include "qemu/dynamic_barrier.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/quantum.h"
#include "qemu/plugin-cyan.h"



static uint64_t get_current_timestamp_ns(void) {
    struct timespec ts;
    // Get the current time
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to nanoseconds
    // tv_sec is seconds, tv_nsec is nanoseconds
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return timestamp_ns;
}

static void *report_time_peridically(void *arg) {
    dynamic_barrier_polling_t *barrier = arg;
    while (1) {
        sleep(10);
        uint64_t total_diff = barrier->total_diff;
        uint64_t generation = barrier->return_value.two_32.generation;
        printf("Total time spent in the barrier: %lu ns, generation: %lu, normalized_diff: %lf\n", total_diff, generation, (double)total_diff / generation);
    }
    return NULL;
}

// create a timestamp of each thread. 
// static __thread uint64_t thread_start_quantum_timestamp = 0;

// // Initialize the dynamic barrier
// int dynamic_barrier_init(dynamic_barrier_t *barrier, int initial_threshold) {
//     int status;

//     status = pthread_mutex_init(&barrier->mutex, NULL);
//     if (status != 0) return status;

//     status = pthread_cond_init(&barrier->cond, NULL);
//     if (status != 0) {
//         pthread_mutex_destroy(&barrier->mutex);
//         return status;
//     }

//     barrier->threshold = initial_threshold;
//     barrier->count = 0;
//     barrier->generation = 0;

//     // start another thread to call report_time_peridically.

//     return 0;
// }

// // Destroy the dynamic barrier
// int dynamic_barrier_destroy(dynamic_barrier_t *barrier) {
//     pthread_mutex_destroy(&barrier->mutex);
//     pthread_cond_destroy(&barrier->cond);
//     return 0;
// }

// // Wait on the barrier
// int dynamic_barrier_wait(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);

//     int gen = barrier->generation;
//     barrier->count++;

//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     } else {
//         while (gen == barrier->generation) {
//             pthread_cond_wait(&barrier->cond, &barrier->mutex);
//         }
//     }

//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }



// int dynamic_barrier_increase_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     barrier->threshold = barrier->threshold + 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }

// int dynamic_barrier_decrease_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     if (barrier->threshold <= 0) {
//         pthread_mutex_unlock(&barrier->mutex);
//         return -1;
//     }
//     barrier->threshold = barrier->threshold - 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }


int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold) {
    barrier->lock.next_ticket = 0;
    barrier->lock.now_serving = 0;

    barrier->threshold = initial_threshold;
    barrier->count = 0;
    barrier->return_value.two_32.generation = 0;
    barrier->return_value.two_32.stop_request = 0;
    barrier->next_virtual_time_deadline_in_ns = 0;

    if (quantum_enabled()) {
        // pthread_t tid;
        // pthread_create(&tid, NULL, report_time_peridically, barrier);
    }

    for (int i = 0; i < 128; i++) {
        barrier->histogram[i] = create_histogram(100, 1e5, 101e5);
    }

    barrier->timer_update_request = false;

    barrier->current_cycle = 0;
    barrier->next_check_threshold = quantum_check_threshold;
    
    return 0;
}

int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier) {
    for (int i = 0; i < 128; i++) {
        free_histogram(barrier->histogram[i]);
    }
    
    return 0;
}

static void dynamic_barrier_polling_acquire_lock(dynamic_barrier_polling_t *barrier) {
    uint64_t my_ticket = atomic_fetch_add(&barrier->lock.next_ticket, 1);
    while (atomic_load(&barrier->lock.now_serving) != my_ticket) {
        // do nothing
    }
}

static void dynamic_barrier_polling_release_lock(dynamic_barrier_polling_t *barrier) {
    atomic_fetch_add(&barrier->lock.now_serving, 1);
}

uint32_t dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier, uint32_t private_generation, bool *stop_request, bool check_time) {
    assert(current_cpu != NULL);

    dynamic_barrier_polling_acquire_lock(barrier);

    uint32_t current_gen = atomic_load(&barrier->return_value.two_32.generation);

    assert(private_generation == current_gen);

    if (check_time) {
        barrier->timer_update_request = true;
    }

    uint64_t waiting_count = barrier->count;
    
    if (waiting_count == barrier->threshold - 1) {
        barrier->current_cycle += quantum_size;
        // barrier->stop_request = 0;
        bool broadcast_stop_request = 0;

        barrier->count = 0;

        // Advance the virtual clock by the quantum size. 

        int64_t current_virtual_time = increase_quantum_time();
        barrier->next_virtual_time_deadline_in_ns -= quantum_size;
        
        if (barrier->timer_update_request || barrier->next_virtual_time_deadline_in_ns <= 0) {
            qemu_mutex_lock_iothread();
            qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
            qemu_mutex_unlock_iothread();

            int64_t deadline = qemu_clock_deadline_ns_virtual_clock_for_quantum(current_virtual_time);
            assert(deadline >= 0);

            barrier->next_virtual_time_deadline_in_ns = deadline;
        }


        barrier->timer_update_request = false;

        // Then, run the periodic check.
        if (barrier->next_check_threshold != 0 && barrier->current_cycle >= barrier->next_check_threshold) {
            if (cyan_periodic_check_cb != NULL) {
                if(cyan_periodic_check_cb(quantum_check_threshold)) {
                    broadcast_stop_request = 1;
                    // Notify the main loop for the incoming snapshot event.
                    qemu_notify_event();

                    // wait for the machine state to become suspended for VM.
                    while (current_cpu->stop != true) {
                        sched_yield();
                    }
                }
            } 
            barrier->next_check_threshold += quantum_check_threshold;
        }

        barrier_result_t return_value;

        return_value.stop_request = broadcast_stop_request;
        return_value.generation = current_gen + 1;

        // cancel the sgi waking up request, because the thread is going to wake up.
        current_cpu->sgi_sender_time_ns_valid = false;

        // increase the generation and notify others.
        atomic_store(&barrier->return_value.one_64, *((uint64_t *)&return_value));

        dynamic_barrier_polling_release_lock(barrier); // we can release the generation here.

        *stop_request = broadcast_stop_request;
    } else {
        barrier->count += 1;
        dynamic_barrier_polling_release_lock(barrier);

        barrier_result_t barrier_return_value;

        // You just need to wait.
        while (true) {
            *((uint64_t *)&barrier_return_value) = atomic_load(&barrier->return_value.one_64);

            if (barrier_return_value.generation != current_gen) {
                current_cpu->sgi_sender_time_ns_valid = false;
                break;
            }

            if (quantum_allow_interrupt_wakeup_inside) {
                // Now, we need to check whether the CPU has work to do
                
                // How many credits do I have?
                if (current_cpu->quantum_budget <= 0) {
                    // No need to continue.
                    continue;
                }

                if (cpu_thread_is_idle(current_cpu)) {
                    continue;
                }

                // Well, this means the CPU has work to do. 
                // Grab the lock.
                dynamic_barrier_polling_acquire_lock(barrier);

                if (barrier->return_value.two_32.generation != current_gen) {
                    // The generation has changed, which mean the last quantum has been finished. 
                    // This thread also needs to move to the next quantum.
                    dynamic_barrier_polling_release_lock(barrier);
                    break;
                }

                // Now, we need to detach from the barrier.
                assert(barrier->count > 0);
                barrier->count -= 1;

                if (current_cpu->sgi_sender_time_ns_valid) {
                    // this means the CPU thread is waken up by a SGI. The source CPU has the time.
                    uint64_t sender_time = current_cpu->sgi_sender_remaining_time_ns;
                    uint64_t sender_generation = current_cpu->sgi_sender_quantum_generation;
                    assert(sender_generation == current_gen);
                    int64_t new_budget_on_acceptance = (sender_time * current_cpu->ip10ps) / 100;
                    
                    // update the budget if the new budget is smaller than the current budget, meaning that the sleeping has happened.
                    if (new_budget_on_acceptance < current_cpu->quantum_budget) {
                        current_cpu->quantum_budget = new_budget_on_acceptance;
                    }

                    // cleared, meaning that the time is updated and the thread is waken up.
                    current_cpu->sgi_sender_time_ns_valid = false;
                }

                // release the lock.
                dynamic_barrier_polling_release_lock(barrier);

                return current_gen; 
            }

        }

        // read the stop request set by the last thread.
        bool require_stop = barrier_return_value.stop_request;

        *stop_request = require_stop;
    }

    return current_gen + 1;
}

uint32_t dynamic_barrier_polling_increase_by_1(dynamic_barrier_polling_t *barrier) {
    uint32_t current_generation;
    dynamic_barrier_polling_acquire_lock(barrier);
    current_generation = atomic_load(&barrier->return_value.two_32.generation);
    barrier->threshold += 1;
    dynamic_barrier_polling_release_lock(barrier);

    return current_generation;
}

int dynamic_barrier_polling_decrease_by_1(dynamic_barrier_polling_t *barrier) {
    dynamic_barrier_polling_acquire_lock(barrier);
    if (barrier->threshold <= 0) {
        dynamic_barrier_polling_release_lock(barrier);
        assert(false);
    }

    barrier->threshold -= 1;
    uint64_t waiting_count = barrier->count;

    if (waiting_count == barrier->threshold && waiting_count != 0) {
        barrier->current_cycle += quantum_size;

        if (barrier->next_check_threshold != 0 && barrier->current_cycle >= barrier->next_check_threshold) {
            if (cyan_periodic_check_cb != NULL) cyan_periodic_check_cb(quantum_check_threshold);
            barrier->next_check_threshold += quantum_check_threshold;
        }


        barrier->count = 0;

        // increase the generation and notify others.
        atomic_fetch_add(&barrier->return_value.two_32.generation, 1);
    }

    dynamic_barrier_polling_release_lock(barrier);
    return 0;
}

void dynamic_barrier_polling_reset(dynamic_barrier_polling_t *barrier) {
    dynamic_barrier_polling_acquire_lock(barrier);
    atomic_store(&barrier->return_value.two_32.generation, 0); // this should make everyone to not wait. 
    barrier->count = 0;
    dynamic_barrier_polling_release_lock(barrier);
}