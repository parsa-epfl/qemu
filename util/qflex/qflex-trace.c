/*
 * Copyright (C) 2021, Mahmoud Mandour <ma.mandourr@gmail.com>
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/qemu-plugin.h"

#include "hw/core/cpu.h"

#include "qflex/qflex-api.h"
#include "qflex/qflex-arch.h"


static GHashTable *qflex_trace_ht;
static GMutex      qflex_trace_ht_lock;

static void qflex_trace_ls(unsigned int idx,
                           qemu_plugin_meminfo_t info,
                           uint64_t vaddr,
                           void *user) {
    qflex_insn_t *insn = (qflex_insn_t *)(user);

    memory_transaction_t tr;

    memset(&tr, 0, sizeof(tr));

    tr.s.pc               = insn->va;
    tr.s.logical_address  = vaddr;
    tr.s.physical_address = QFLEX_GET_ARCH(pa)(qemu_get_cpu(idx), vaddr, insn->mem.is_load ? QEMU_Curr_Load : QEMU_Curr_Store);
    tr.s.type             = insn->mem.is_load ? QEMU_Trans_Load : QEMU_Trans_Store;
    tr.s.size             = insn->mem.size;
    tr.s.atomic           = insn->mem.is_atomic;
    tr.io                 = tr.s.physical_address < 0x80000000lu;

    flexus_api.trace_mem(idx, &tr);
}

static void qflex_trace_if(unsigned int idx, void *user) {
    qflex_insn_t *insn = (qflex_insn_t *)(user);

    memory_transaction_t tr;

    memset(&tr, 0, sizeof(tr));

    tr.s.pc               = insn->va;
    tr.s.logical_address  = insn->va;
    tr.s.physical_address = QFLEX_GET_ARCH(pa)(qemu_get_cpu(idx), insn->va, QEMU_Curr_Fetch);
    tr.s.type             = QEMU_Trans_Instr_Fetch;
    tr.s.size             = insn->size;
    tr.s.branch_type      = insn->br_type;
    tr.s.opcode           = insn->insn;
    tr.io                 = false;

    if (insn->va == 0)
        return;

    flexus_api.trace_mem(idx, &tr);
}

static qemu_plugin_id_t qflex_trace_id = 0;

static void qflex_trace_tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        uint16_t *haddr = qemu_plugin_insn_haddr(insn);
        uint64_t  vaddr = qemu_plugin_insn_vaddr(insn);

        g_mutex_lock(&qflex_trace_ht_lock);

        qflex_insn_t *data = g_hash_table_lookup(qflex_trace_ht, GUINT_TO_POINTER(haddr));

        if (data == NULL) {
            data = g_new0(qflex_insn_t, 1);

            data->va      = vaddr;
            data->insn    = haddr[0] | ((uint32_t)(haddr[1]) << 16);
            data->size    = qemu_plugin_insn_size(insn);
            data->is_user = QFLEX_GET_ARCH(pl)(current_cpu) == 0;

            QFLEX_GET_ARCH(dec)(data);

            g_hash_table_insert(qflex_trace_ht, GUINT_TO_POINTER(haddr),
                               (gpointer)(data));
        }

        g_mutex_unlock(&qflex_trace_ht_lock);

        qemu_plugin_register_vcpu_mem_cb      (insn, qflex_trace_ls, QEMU_PLUGIN_CB_R_REGS, QEMU_PLUGIN_MEM_RW, data);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, qflex_trace_if, QEMU_PLUGIN_CB_R_REGS, data);
    }
}

static void qflex_trace_insn_free(gpointer data) {
    g_free(data);
}

void qflex_init_trace(void) {
    qflex_trace_id = qemu_plugin_register_builtin();
    qflex_trace_ht = g_hash_table_new_full(NULL, g_direct_equal, NULL, qflex_trace_insn_free);

    qemu_plugin_register_vcpu_tb_trans_cb(qflex_trace_id, qflex_trace_tb_trans_cb);
}