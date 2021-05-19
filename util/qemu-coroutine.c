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
/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "qemu-common.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/coroutine.h"
#include "qemu/coroutine_int.h"
#include "block/aio.h"

enum {
    POOL_BATCH_SIZE = 64,
};

/** Free list to speed up creation */
static QSLIST_HEAD(, Coroutine) release_pool = QSLIST_HEAD_INITIALIZER(pool);
static unsigned int release_pool_size;
#ifndef CONFIG_PTH
static __thread QSLIST_HEAD(, Coroutine) alloc_pool = QSLIST_HEAD_INITIALIZER(pool);
static __thread unsigned int alloc_pool_size;
static __thread Notifier coroutine_pool_cleanup_notifier;
#endif

static void coroutine_pool_cleanup(Notifier *n, void *value)
{
    Coroutine *co;
    Coroutine *tmp;

    PTH_UPDATE_CONTEXT;

    QSLIST_FOREACH_SAFE(co, &PTH(alloc_pool), pool_next, tmp) {
        QSLIST_REMOVE_HEAD(&PTH(alloc_pool), pool_next);
        qemu_coroutine_delete(co);
    }
}

Coroutine *qemu_coroutine_create(CoroutineEntry *entry, void *opaque)
{
    PTH_UPDATE_CONTEXT;
    Coroutine *co = NULL;

    if (CONFIG_COROUTINE_POOL) {
        co = QSLIST_FIRST(&PTH(alloc_pool));
        if (!co) {
            if (release_pool_size > POOL_BATCH_SIZE) {
                /* Slow path; a good place to register the destructor, too.  */
                if (!PTH(coroutine_pool_cleanup_notifier).notify) {
                    PTH(coroutine_pool_cleanup_notifier).notify = coroutine_pool_cleanup;
                    qemu_thread_atexit_add(&PTH(coroutine_pool_cleanup_notifier));
                }

                /* This is not exact; there could be a little skew between
                 * release_pool_size and the actual size of release_pool.  But
                 * it is just a heuristic, it does not need to be perfect.
                 */
                PTH(alloc_pool_size) = atomic_xchg(&release_pool_size, 0);
                QSLIST_MOVE_ATOMIC(&PTH(alloc_pool), &release_pool);
                co = QSLIST_FIRST(&PTH(alloc_pool));
            }
        }
        if (co) {
            QSLIST_REMOVE_HEAD(&PTH(alloc_pool), pool_next);
            PTH(alloc_pool_size)--;
        }
    }

    if (!co) {
        co = qemu_coroutine_new();
    }

    co->entry = entry;
    co->entry_arg = opaque;
    QSIMPLEQ_INIT(&co->co_queue_wakeup);
    return co;
}

static void coroutine_delete(Coroutine *co)
{
    PTH_UPDATE_CONTEXT
    co->caller = NULL;

    if (CONFIG_COROUTINE_POOL) {
        if (release_pool_size < POOL_BATCH_SIZE * 2) {
            QSLIST_INSERT_HEAD_ATOMIC(&release_pool, co, pool_next);
            atomic_inc(&release_pool_size);
            return;
        }
        if (PTH(alloc_pool_size) < POOL_BATCH_SIZE) {
            QSLIST_INSERT_HEAD(&PTH(alloc_pool), co, pool_next);
            PTH(alloc_pool_size)++;
            return;
        }
    }

    qemu_coroutine_delete(co);
}

void qemu_aio_coroutine_enter(AioContext *ctx, Coroutine *co)
{
    Coroutine *self = qemu_coroutine_self();
    CoroutineAction ret;

    trace_qemu_aio_coroutine_enter(ctx, self, co, co->entry_arg);

    if (co->caller) {
        fprintf(stderr, "Co-routine re-entered recursively\n");
        abort();
    }

    co->caller = self;
    co->ctx = ctx;

    /* Store co->ctx before anything that stores co.  Matches
     * barrier in aio_co_wake and qemu_co_mutex_wake.
     */
    smp_wmb();

    ret = qemu_coroutine_switch(self, co, COROUTINE_ENTER);

    qemu_co_queue_run_restart(co);

    /* Beware, if ret == COROUTINE_YIELD and qemu_co_queue_run_restart()
     * has started any other coroutine, "co" might have been reentered
     * and even freed by now!  So be careful and do not touch it.
     */

    switch (ret) {
    case COROUTINE_YIELD:
        return;
    case COROUTINE_TERMINATE:
        assert(!co->locks_held);
        trace_qemu_coroutine_terminate(co);
        coroutine_delete(co);
        return;
    default:
        abort();
    }
}

void qemu_coroutine_enter(Coroutine *co)
{
    qemu_aio_coroutine_enter(qemu_get_current_aio_context(), co);
}

void qemu_coroutine_enter_if_inactive(Coroutine *co)
{
    if (!qemu_coroutine_entered(co)) {
        qemu_coroutine_enter(co);
    }
}

void coroutine_fn qemu_coroutine_yield(void)
{
    Coroutine *self = qemu_coroutine_self();
    Coroutine *to = self->caller;

    trace_qemu_coroutine_yield(self, to);

    if (!to) {
        fprintf(stderr, "Co-routine is yielding to no one\n");
        abort();
    }

    self->caller = NULL;
    qemu_coroutine_switch(self, to, COROUTINE_YIELD);
    PTH_YIELD;
}

bool qemu_coroutine_entered(Coroutine *co)
{
    return co->caller;
}
