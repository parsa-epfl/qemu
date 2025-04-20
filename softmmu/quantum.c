#include "qemu/osdep.h"
#include "sysemu/quantum.h"
#include "qemu/option.h"

uint64_t quantum_size = 0;
uint64_t quantum_check_threshold = 0;
bool quantum_allow_interrupt_wakeup_inside = 0; // allow interrupts and other `run_on_cpu` to wake up a thread that spins on the quantum barrier.
static uint64_t quantum_enabled_lower_bound = 0;
static uint64_t quantum_enabled_upper_bound = 0;

void quantum_configure(QemuOpts *opts, Error **errp) {
    uint64_t quantum_size_tmp = qemu_opt_get_number(opts, "size", 0);
    // deplete_threshold = qemu_opt_get_number(opts, "deplete_threshold", 0xffffffffffffffff);
    quantum_check_threshold = qemu_opt_get_number(opts, "check_period", 0);

    if (quantum_check_threshold != 0) {
        assert((quantum_check_threshold >= quantum_size_tmp) && (quantum_check_threshold % quantum_size_tmp == 0));
    }

    const char *range = qemu_opt_get(opts, "range");
 
    if (!range) {
        quantum_enabled_lower_bound = 0;
        quantum_enabled_upper_bound = 0xFFFFFFFFFFFFFFFF; // all cores are enabled.
    } else {
        // need to split the range.
        char *range_tmp = g_strdup(range);
        char *range_start = strtok(range_tmp, "-");
        char *range_end = strtok(NULL, "-");
        quantum_enabled_lower_bound = strtoull(range_start, NULL, 0);
        quantum_enabled_upper_bound = strtoull(range_end, NULL, 0);
        g_free(range_tmp);
    }

    quantum_allow_interrupt_wakeup_inside = qemu_opt_get_bool(opts, "allow_interrupt_wakeup_inside", false);

    // make it as a global value.
    quantum_size = quantum_size_tmp;
    
    assert(quantum_size < 0x7fffffff);
}