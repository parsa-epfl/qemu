/*
 * ParaFlex Plugin API
 *
 * This file defines ParaFlex's API for QEMU.
 * These plugins are only for ARM processor (aarch64).
 *
 * Copyright (C) ParaFlex Project
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-plugin.h"
#include "qemu/plugin-pf.h"

#ifndef CONFIG_USER_ONLY

#include "qemu/plugin.h"
#include "qemu/log.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"
#include "tcg/tcg.h"
#include "exec/exec-all.h"
#include "exec/ram_addr.h"
#include "disas/disas.h"
#include "plugin.h"
#include "hw/boards.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"

/* Callback function pointers - global for access from other files */
qemu_plugin_vcpu_branch_resolved_cb_t pf_br_cb = NULL;
qemu_plugin_snapshot_cb_t pf_savevm_cb = NULL;
qemu_plugin_snapshot_cb_t pf_loadvm_cb = NULL;
qemu_plugin_event_loop_poll_cb_t pf_el_pool_cb = NULL;
qemu_plugin_periodic_check_cb_t pf_periodic_check_cb = NULL;
qemu_plugin_flushing_local_tlb_t pf_flushing_local_tlb_cb = NULL;
qemu_plugin_save_statistics_callback_t pf_save_statistics_cb = NULL;

uint64_t qemu_plugin_read_ttbr_el1(int which_ttbr)
{
    g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

    CPUState *cpu = current_cpu;
    g_assert(cpu != NULL);

    g_assert(which_ttbr == 0 || which_ttbr == 1);

    if (which_ttbr == 0) {
        return (uint64_t)cpu->env_ptr->cp15.ttbr0_el[1];
    } else {
        return (uint64_t)cpu->env_ptr->cp15.ttbr1_el[1];
    }
}

uint64_t qemu_plugin_read_tcr_el1(void)
{
    g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

    CPUState *cpu = current_cpu;
    g_assert(cpu != NULL);

    return (uint64_t)cpu->env_ptr->cp15.tcr_el[1];
}

void qemu_plugin_read_physical_memory(uint64_t physical_address, uint64_t size,
                                      void *buf)
{
    cpu_physical_memory_rw(physical_address, buf, size, false);
}

bool qemu_plugin_register_vcpu_branch_resolved_cb(
    qemu_plugin_vcpu_branch_resolved_cb_t cb)
{
    if (pf_br_cb) {
        return false;
    }
    pf_br_cb = cb;
    return true;
}

uint64_t qemu_plugin_read_pc_vpn(void)
{
    CPUState *cpu = current_cpu;
    g_assert(cpu != NULL);

    return (uint64_t)cpu->env_ptr->pc >> 12;
}

bool qemu_plugin_register_savevm_cb(qemu_plugin_snapshot_cb_t cb)
{
    if (pf_savevm_cb) {
        return false;
    }
    pf_savevm_cb = cb;
    return true;
}

bool qemu_plugin_register_loadvm_cb(qemu_plugin_snapshot_cb_t cb)
{
    if (pf_loadvm_cb) {
        return false;
    }
    pf_loadvm_cb = cb;
    return true;
}

void qemu_plugin_savevm(const char *name,
                        enum qemu_plugin_snapshot_format_t format)
{
    /* In this mode, we should not use savevm directly */
    abort();
}

bool qemu_plugin_register_event_loop_poll_cb(qemu_plugin_event_loop_poll_cb_t cb)
{
    /* In this mode, we should not use event loop poll callback */
    abort();
    return false;
}

bool qemu_plugin_register_periodic_check_cb(qemu_plugin_periodic_check_cb_t cb)
{
    /* In this mode, we should not use periodic check callback */
    abort();
    return false;
}

uint64_t qemu_plugin_get_vcpu_vtime(uint32_t cpu_idx)
{
    /* For now, return 0 as specified */
    return 0;
}

bool qemu_plugin_register_flushing_local_tlb_cb(
    qemu_plugin_flushing_local_tlb_t cb)
{
    if (pf_flushing_local_tlb_cb) {
        return false;
    }

    pf_flushing_local_tlb_cb = cb;
    return true;
}

bool qemu_plugin_register_save_statistics_callback(
    qemu_plugin_save_statistics_callback_t cb)
{
    if (pf_save_statistics_cb) {
        return false;
    }

    pf_save_statistics_cb = cb;
    return true;
}

#endif /* CONFIG_USER_ONLY */
