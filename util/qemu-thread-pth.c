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

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/notify.h"
#include "include/qemu/thread-pth.h"


static bool name_threads;

void qemu_thread_naming(bool enable)
{
    name_threads = enable;

#ifndef CONFIG_THREAD_SETNAME_BYTHREAD
    /* This is a debugging option, not fatal */
    if (enable) {
        fprintf(stderr, "qemu: thread naming not supported on this host\n");
    }
#endif
}

static void error_exit(int err, const char *msg)
{
    fprintf(stderr, "qemu: %s: %s\n", msg, strerror(err));
    abort();
}

void qemu_mutex_init(QemuMutex *mutex)
{
    int err;

    err = pthpthread_mutex_init(&mutex->lock, NULL);

    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    int err;

    err = pthpthread_mutex_destroy(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_lock(QemuMutex *mutex)
{
    int err;

    err = pthpthread_mutex_lock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
    PTH_YIELD;
}

int qemu_mutex_trylock(QemuMutex *mutex)
{
    return pthpthread_mutex_trylock(&mutex->lock);
}

void qemu_mutex_unlock(QemuMutex *mutex)
{
    int err;

    err = pthpthread_mutex_unlock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_rec_mutex_init(QemuRecMutex *mutex)
{
    int err;
    pthpthread_mutexattr_t attr;

    pthpthread_mutexattr_init(&attr);
    pthpthread_mutexattr_settype(&attr, PTHPTHREAD_MUTEX_RECURSIVE);
    err = pthpthread_mutex_init(&mutex->lock, &attr);
    pthpthread_mutexattr_destroy(&attr);
    if (err) {
        error_exit(err, __func__);
    }
}

void qemu_cond_init(QemuCond *cond)
{
    int err;

    err = pthpthread_cond_init(&cond->cond, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_destroy(QemuCond *cond)
{
    int err;

    err = pthpthread_cond_destroy(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_signal(QemuCond *cond)
{
    int err;

    err = pthpthread_cond_signal(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    int err;

    err = pthpthread_cond_broadcast(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
{
    int err;

    err = pthpthread_cond_wait(&cond->cond, &mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_sem_init(QemuSemaphore *sem, int init)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__) || defined(CONFIG_PTH)
    rc = pthpthread_mutex_init(&sem->lock, NULL);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
    rc = pthpthread_cond_init(&sem->cond, NULL);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
    if (init < 0) {
        error_exit(EINVAL, __func__);
    }
    sem->count = init;
#else
    rc = sem_init(&sem->sem, 0, init);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_sem_destroy(QemuSemaphore *sem)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__) || defined (CONFIG_PTH)
     rc = pthpthread_cond_destroy(&sem->cond);
    if (rc < 0) {
        error_exit(rc, __func__);
    }
    rc = pthpthread_mutex_destroy(&sem->lock);
    if (rc < 0) {
        error_exit(rc, __func__);
    }
#else
    rc = sem_destroy(&sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_sem_post(QemuSemaphore *sem)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__)|| defined (CONFIG_PTH)
    pthpthread_mutex_lock(&sem->lock);
    if (sem->count == UINT_MAX) {
        rc = EINVAL;
    } else {
        sem->count++;
        rc = pthpthread_cond_signal(&sem->cond);
    }
    pthpthread_mutex_unlock(&sem->lock);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
#else
    rc = sem_post(&sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

static void compute_abs_deadline(struct timespec *ts, int ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_nsec = tv.tv_usec * 1000 + (ms % 1000) * 1000000;
    ts->tv_sec = tv.tv_sec + ms / 1000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

int qemu_sem_timedwait(QemuSemaphore *sem, int ms)
{
    int rc;
    struct timespec ts;

#if defined(__APPLE__) || defined(__NetBSD__)|| defined (CONFIG_PTH)
    rc = 0;
    compute_abs_deadline(&ts, ms);
    pthpthread_mutex_lock(&sem->lock);
    while (sem->count == 0) {
        rc = pthpthread_cond_timedwait(&sem->cond, &sem->lock, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
        if (rc != 0) {
            error_exit(rc, __func__);
        }
    }
    if (rc != ETIMEDOUT) {
        --sem->count;
    }
    pthpthread_mutex_unlock(&sem->lock);
    return (rc == ETIMEDOUT ? -1 : 0);
#else
    if (ms <= 0) {
        /* This is cheaper than sem_timedwait.  */
        do {
            rc = sem_trywait(&sem->sem);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1 && errno == EAGAIN) {
            return -1;
        }
    } else {
        compute_abs_deadline(&ts, ms);
        do {
            rc = sem_timedwait(&sem->sem, &ts);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1 && errno == ETIMEDOUT) {
            return -1;
        }
    }
    if (rc < 0) {
        error_exit(errno, __func__);
    }
    return 0;
#endif
}

void qemu_sem_wait(QemuSemaphore *sem)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__)|| defined (CONFIG_PTH)
    pthpthread_mutex_lock(&sem->lock);
    while (sem->count == 0) {
        rc = pthpthread_cond_wait(&sem->cond, &sem->lock);
        if (rc != 0) {
            error_exit(rc, __func__);
        }
    }
    --sem->count;
    pthpthread_mutex_unlock(&sem->lock);
#else
    do {
        rc = sem_wait(&sem->sem);
    } while (rc == -1 && errno == EINTR);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

#ifdef __linux__ 
static inline void qemu_futex_wake(QemuEvent *ev, int n)
{
  pthpthread_mutex_lock(&ev->lock);
    if (n == 1) {
	pthpthread_cond_signal(&ev->cond);
    } else {
	pthpthread_cond_broadcast(&ev->cond);
    }
    pthpthread_mutex_unlock(&ev->lock);
}

static inline void qemu_futex_wait(QemuEvent *ev, unsigned val)
{
    pthpthread_mutex_lock(&ev->lock);
    if (ev->value == val) {
	pthpthread_cond_wait(&ev->cond, &ev->lock);
    }
    pthpthread_mutex_unlock(&ev->lock);
}
#endif

/* Valid transitions:
 * - free->set, when setting the event
 * - busy->set, when setting the event, followed by qemu_futex_wake
 * - set->free, when resetting the event
 * - free->busy, when waiting
 *
 * set->busy does not happen (it can be observed from the outside but
 * it really is set->free->busy).
 *
 * busy->free provably cannot happen; to enforce it, the set->free transition
 * is done with an OR, which becomes a no-op if the event has concurrently
 * transitioned to free or busy.
 */

#define EV_SET         0
#define EV_FREE        1
#define EV_BUSY       -1

void qemu_event_init(QemuEvent *ev, bool init)
{
#ifndef __linux__  
    pthpthread_mutex_init(&ev->lock, NULL);
    pthpthread_cond_init(&ev->cond, NULL);
#endif

    ev->value = (init ? EV_SET : EV_FREE);
}

void qemu_event_destroy(QemuEvent *ev)
{
#ifndef __linux__
    pthpthread_mutex_destroy(&ev->lock);
    pthpthread_cond_destroy(&ev->cond);
#endif
}

void qemu_event_set(QemuEvent *ev)
{
    /* qemu_event_set has release semantics, but because it *loads*
     * ev->value we need a full memory barrier here.
     */
    smp_mb();
    if (atomic_read(&ev->value) != EV_SET) {
        if (atomic_xchg(&ev->value, EV_SET) == EV_BUSY) {
            /* There were waiters, wake them up.  */
            qemu_futex_wake(ev, INT_MAX);
        }
    }
}

void qemu_event_reset(QemuEvent *ev)
{
    unsigned value;

    value = atomic_read(&ev->value);
    smp_mb_acquire();
    if (value == EV_SET) {
        /*
         * If there was a concurrent reset (or even reset+wait),
         * do nothing.  Otherwise change EV_SET->EV_FREE.
         */
        atomic_or(&ev->value, EV_FREE);
    
    PTH_YIELD
    }
}

void qemu_event_wait(QemuEvent *ev)
{
    unsigned value;

    value = atomic_read(&ev->value);
    smp_mb_acquire();
    if (value != EV_SET) {
        if (value == EV_FREE) {
            /*
             * Leave the event reset and tell qemu_event_set that there
             * are waiters.  No need to retry, because there cannot be
             * a concurrent busy->free transition.  After the CAS, the
             * event will be either set or busy.
             */
            if (atomic_cmpxchg(&ev->value, EV_FREE, EV_BUSY) == EV_SET) {
                return;
            }
        }
        qemu_futex_wait(ev, EV_BUSY);
    }
}

static pth_key_t exit_key;

union NotifierThreadData {
    void *ptr;
    NotifierList list;
};
QEMU_BUILD_BUG_ON(sizeof(union NotifierThreadData) != sizeof(void *));

void qemu_thread_atexit_add(Notifier *notifier)
{
    union NotifierThreadData ntd;
    ntd.ptr = pthpthread_getspecific(exit_key);
    notifier_list_add(&ntd.list, notifier);
    pthpthread_setspecific(exit_key, ntd.ptr);
}

void qemu_thread_atexit_remove(Notifier *notifier)
{
    union NotifierThreadData ntd;
    ntd.ptr = pthpthread_getspecific(exit_key);
    notifier_remove(notifier);
    pthpthread_setspecific(exit_key, ntd.ptr);
}

static void qemu_thread_atexit_run(void *arg)
{
    union NotifierThreadData ntd = { .ptr = arg };
    notifier_list_notify(&ntd.list, NULL);
}

static void __attribute__((constructor)) qemu_thread_atexit_init(void)
{
    pth_key_create(&exit_key, qemu_thread_atexit_run);
}


/* Attempt to set the threads name; note that this is for debug, so
 * we're not going to fail if we can't set it.
 */

// already done

//static void qemu_thread_set_name(QemuThread *thread, const char *name)
//{
//#ifdef CONFIG_PTHREAD_SETNAME_NP
//    //pthread_setname_np(thread->thread, name);
//#endif
//}

pth_t main_thread = NULL;

pth_t get_main_thread(void);

pth_t get_main_thread(void) {
  if( main_thread == NULL )
    main_thread = pthpthread_self();
  return main_thread;
}

static bool threadlist_initialized = false;
static QLIST_HEAD(, threadlist) pth_wrappers =
        QLIST_HEAD_INITIALIZER(pth_wrappers);
static int threadlist_size;


void initMainThread(void)
{
    if (!threadlist_initialized){
	      get_main_thread();
        threadlist* head = calloc(1, sizeof(threadlist));
        head->qemuthread = calloc(1, sizeof(QemuThread));
        head->qemuthread->wrapper.pth_thread = pth_self();
        threadlist_initialized = true;
        memset(&head->qemuthread->wrapper.rcu_reader, 0, sizeof(struct rcu_reader_data));
        head->qemuthread->wrapper.thread_name = strdup("main");
        QLIST_INSERT_HEAD(&pth_wrappers, head, next);
        threadlist_size++;

    }
}

void qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void*),
                       void *arg, int mode)
{
    sigset_t set, oldset;
    int err;

    pthpthread_attr_t attr;
    err = pthpthread_attr_init(&attr);

    if (err) {
        error_exit(err, __func__);
    }

    /* Use a large stack for the vcpu threads */
    const size_t qemu_vcpu_stack_size = 8*(1<<20); // 8MB
    err = pthpthread_attr_setstacksize(&attr,qemu_vcpu_stack_size);

    if (err) {
        error_exit(err, __func__);
    }

    /* Leave signal handling to the iothread.  */
    sigfillset(&set);
    pthpthread_sigmask(SIG_SETMASK, &set, &oldset);

    if (name_threads) {
        pth_attr_set((pth_attr_t)attr, PTH_ATTR_NAME, name);
    }

    if (mode == QEMU_THREAD_DETACHED) {
    err = pthpthread_attr_setdetachstate(&attr, PTHPTHREAD_CREATE_DETACHED);
        if (err) {
            error_exit(err, __func__);
        }
    }

    err = pthpthread_create(&thread->wrapper.pth_thread, &attr, start_routine, arg);
    if (err)
        error_exit(err, __func__);

    if (name) 
        thread->wrapper.thread_name = strdup(name);

    /* Make a heap-allocated copy of *thread that doesn't go out of scope when placed into a pth wrapper,
     * and then insert it into the PTH wrapper data structures
     */
    initMainThread(); // in case user has not explicitly called init yet

    QemuThread *thread_pth_copy = g_malloc0(sizeof(QemuThread));
    memcpy(thread_pth_copy,thread,sizeof(QemuThread));

    threadlist* head = calloc(1, sizeof(threadlist));
    head->qemuthread = thread_pth_copy;
    memset(&head->qemuthread->wrapper.rcu_reader, 0, sizeof(struct rcu_reader_data));
    threadlist_size++;
    QLIST_INSERT_HEAD(&pth_wrappers, head, next);

    //PTH_YIELD // TODO: is this mandatory??

    pthpthread_sigmask(SIG_SETMASK, &oldset, NULL);

    pthpthread_attr_destroy(&attr);
}

void qemu_thread_get_self(QemuThread *thread)
{
    thread->wrapper.pth_thread = pthpthread_self();
}

bool qemu_thread_is_self(QemuThread *thread)
{
   return pthpthread_equal(pthpthread_self(), thread->wrapper.pth_thread);
}

void qemu_thread_exit(void *retval)
{
    pthpthread_exit(retval);
}

void *qemu_thread_join(QemuThread *thread)
{
    int err;
    void *ret;

    err = pthpthread_join(thread->wrapper.pth_thread, &ret);
    if (err) {
        error_exit(err, __func__);
    }
    return ret;
}



pth_wrapper* pth_get_wrapper(void){
    threadlist *entry = NULL;
    QLIST_FOREACH(entry, &pth_wrappers, next){
        if (entry->qemuthread->wrapper.pth_thread == pth_self())
            return &entry->qemuthread->wrapper;
    }

    assert(false && "pth: thread was none added to pth thread list! did you use function attributes? if yes, they are not supported.");
}


#endif //  CONFIG_PTH
