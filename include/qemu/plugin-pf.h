/**
 * @file include/qemu/plugin-pf.h
 *
 * This file contains the definitions for the ParaFlex's plugin system.
 */

#ifndef QEMU_PLUGIN_PF_H
#define QEMU_PLUGIN_PF_H

#include "qemu/qemu-plugin.h"

// This file keeps the following callbacks for the plugin system:
// - Callback for branch resolution.
// - Callback for savevm (after the VM state is saved).

// The callback for branch resolution.
extern qemu_plugin_vcpu_branch_resolved_cb_t pf_br_cb;

// The callback for savevm (after the VM state is saved).
extern qemu_plugin_snapshot_cb_t pf_savevm_cb;

// The callback for loadvm (after the VM state is loaded).
extern qemu_plugin_snapshot_cb_t pf_loadvm_cb;

// The event loop polling callback for the plugin system.
extern qemu_plugin_event_loop_poll_cb_t pf_el_pool_cb;

// The periodic check callback for the plugin system.
extern qemu_plugin_periodic_check_cb_t pf_periodic_check_cb;

// The callback for flushing the local TLB of a vCPU.
extern qemu_plugin_flushing_local_tlb_t pf_flushing_local_tlb_cb;


// The callback for saving statistics.
extern qemu_plugin_save_statistics_callback_t pf_save_statistics_cb;


#endif
