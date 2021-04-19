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
#ifdef CONFIG_PTH
#ifndef QEMU_THREAD_PTH_H
#define QEMU_THREAD_PTH_H

#include "util/coroutine-ucontext-pth.h"
#include "qemu/thread.h"
#include <pth.h>
#include <semaphore.h>
#include "qemu/thread-pth-internal.h"

typedef QemuMutex QemuRecMutex;
#define qemu_rec_mutex_destroy qemu_mutex_destroy
#define qemu_rec_mutex_lock qemu_mutex_lock
#define qemu_rec_mutex_try_lock qemu_mutex_try_lock
#define qemu_rec_mutex_unlock qemu_mutex_unlock

typedef struct AioHandler AioHandler;

struct rcu_reader_data {
    /* Data used by both reader and synchronize_rcu() */
    unsigned long ctr;
    bool waiting;

    /* Data used by reader only */
    unsigned depth;

    /* Data used for registry, protected by rcu_registry_lock */
    QLIST_ENTRY(rcu_reader_data) node;
};

typedef struct IOThread IOThread;
typedef struct pth_wrapper
{
    pth_t pth_thread;

// POSIX ALTERNATIVE TLS

    bool iothread_locked;

    // aio
    GPollFD *pollfds;
    AioHandler **nodes;
    unsigned npfd;
    unsigned  nalloc;
    Notifier  pollfds_cleanup_notifier;
    bool aio_init;

    // current cpu
    CPUState *current_cpu;

    // coroutine
    QSLIST_HEAD(, Coroutine) alloc_pool;
    unsigned int alloc_pool_size;
    Notifier coroutine_pool_cleanup_notifier;
    CoroutineUContext leader;
    Coroutine *current;

    //iothread
    IOThread *my_iothread;

    // rcu
    struct rcu_reader_data rcu_reader;

    // translation block context
    int have_tb_lock;

    char* thread_name;

    // thread ID utilized for unit tests
    int id;
}pth_wrapper;

typedef struct  pthpthread_st              *pthpthread_t;
typedef struct  pthpthread_attr_st         *pthpthread_attr_t;
typedef int                              pthpthread_key_t;
typedef int                              pthpthread_once_t;
typedef int                              pthpthread_mutexattr_t;
typedef struct  pthpthread_mutex_st        *pthpthread_mutex_t;
typedef int                              pthpthread_condattr_t;
typedef struct  pthpthread_cond_st         *pthpthread_cond_t;
typedef int                              pthpthread_rwlockattr_t;
typedef struct  pthpthread_rwlock_st       *pthpthread_rwlock_t;

struct QemuMutex {
    pthpthread_mutex_t lock;
};

struct QemuCond {
    pthpthread_cond_t cond;
};

struct QemuSemaphore {
    pthpthread_mutex_t lock;
    pthpthread_cond_t cond;
    unsigned int count;
};

struct QemuEvent {
    pthpthread_mutex_t lock;
    pthpthread_cond_t cond;
    unsigned value;
};

struct QemuThread {
    pth_wrapper wrapper;
};

typedef struct threadlist
{
    QemuThread * qemuthread;
    QLIST_ENTRY(threadlist) next;
}threadlist;

pth_wrapper* pth_get_wrapper(void);
void initMainThread(void);


#endif // QEMU_THREAD_PTH_H
#endif // CONFIG_PTH
