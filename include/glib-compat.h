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
 * GLIB Compatibility Functions
 *
 * Copyright IBM, Corp. 2013
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Michael Tokarev   <mjt@tls.msk.ru>
 *  Paolo Bonzini     <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_GLIB_COMPAT_H
#define QEMU_GLIB_COMPAT_H

/* Ask for warnings for anything that was marked deprecated in
 * the defined version, or before. It is a candidate for rewrite.
 */
#define GLIB_VERSION_MIN_REQUIRED GLIB_VERSION_2_48

/* GLIB version compatibility flags */

/* Ask for warnings if code tries to use function that did not
 * exist in the defined version. These risk breaking builds
 */
#define GLIB_VERSION_MAX_ALLOWED GLIB_VERSION_2_48

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <glib.h>
#include <glib/gprintf.h>

#if !GLIB_CHECK_VERSION(2, 26, 0)
#define G_TIME_SPAN_SECOND              (G_GINT64_CONSTANT(1000000))
#endif


#if !GLIB_CHECK_VERSION(2, 28, 0)
static inline gint64 qemu_g_get_monotonic_time(void)
{
    /* g_get_monotonic_time() is best-effort so we can use the wall clock as a
     * fallback.
     */

    GTimeVal time;
    g_get_current_time(&time);

    return time.tv_sec * G_TIME_SPAN_SECOND + time.tv_usec;
}
/* work around distro backports of this interface */
#define g_get_monotonic_time() qemu_g_get_monotonic_time()
#endif
/*
 * Note that because of the GLIB_VERSION_MAX_ALLOWED constant above, allowing
 * use of functions from newer GLib via this compat header needs a little
 * trickery to prevent warnings being emitted.
 *
 * Consider a function from newer glib-X.Y that we want to use
 *
 *    int g_foo(const char *wibble)
 *
 * We must define a static inline function with the same signature that does
 * what we need, but with a "_qemu" suffix e.g.
 *
 * static inline void g_foo_qemu(const char *wibble)
 * {
 *     #if GLIB_CHECK_VERSION(X, Y, 0)
 *        g_foo(wibble)
 *     #else
 *        g_something_equivalent_in_older_glib(wibble);
 *     #endif
 * }
 *
 * The #pragma at the top of this file turns off -Wdeprecated-declarations,
 * ensuring this wrapper function impl doesn't trigger the compiler warning
 * about using too new glib APIs. Finally we can do
 *
 *   #define g_foo(a) g_foo_qemu(a)
 *
 * So now the code elsewhere in QEMU, which *does* have the
 * -Wdeprecated-declarations warning active, can call g_foo(...) as normal,
 * without generating warnings.
 */

#if defined(_WIN32) && !GLIB_CHECK_VERSION(2, 50, 0)
/*
 * g_poll has a problem on Windows when using
 * timeouts < 10ms, so use wrapper.
 */
#define g_poll(fds, nfds, timeout) g_poll_fixed(fds, nfds, timeout)
gint g_poll_fixed(GPollFD *fds, guint nfds, gint timeout);
#endif

#if !GLIB_CHECK_VERSION(2, 30, 0)
/* Not a 100% compatible implementation, but good enough for most
 * cases. Placeholders are only supported at the end of the
 * template. */
static inline gchar *qemu_g_dir_make_tmp(gchar const *tmpl, GError **error)
{
    gchar *path = g_build_filename(g_get_tmp_dir(), tmpl ?: ".XXXXXX", NULL);

    if (mkdtemp(path) != NULL) {
        return path;
    }
    /* Error occurred, clean up. */
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "mkdtemp() failed");
    g_free(path);
    return NULL;
}
#define g_dir_make_tmp(tmpl, error) qemu_g_dir_make_tmp(tmpl, error)
#endif /* glib 2.30 */

#if !GLIB_CHECK_VERSION(2, 31, 0)
/* before glib-2.31, GMutex and GCond was dynamic-only (there was a separate
 * GStaticMutex, but it didn't work with condition variables).
 *
 * Our implementation uses GOnce to fake a static implementation that does
 * not require separate initialization.
 * We need to rename the types to avoid passing our CompatGMutex/CompatGCond
 * by mistake to a function that expects GMutex/GCond.  However, for ease
 * of use we keep the GLib function names.  GLib uses macros for the
 * implementation, we use inline functions instead and undefine the macros.
 */

typedef struct CompatGMutex {
    GOnce once;
} CompatGMutex;

typedef struct CompatGCond {
    GOnce once;
} CompatGCond;

static inline gpointer do_g_mutex_new(gpointer unused)
{
    return (gpointer) g_mutex_new();
}

static inline void g_mutex_init(CompatGMutex *mutex)
{
    mutex->once = (GOnce) G_ONCE_INIT;
}

static inline void g_mutex_clear(CompatGMutex *mutex)
{
    g_assert(mutex->once.status != G_ONCE_STATUS_PROGRESS);
    if (mutex->once.retval) {
        g_mutex_free((GMutex *) mutex->once.retval);
    }
    mutex->once = (GOnce) G_ONCE_INIT;
}

static inline void (g_mutex_lock)(CompatGMutex *mutex)
{
    g_once(&mutex->once, do_g_mutex_new, NULL);
    g_mutex_lock((GMutex *) mutex->once.retval);
}
#undef g_mutex_lock

static inline gboolean (g_mutex_trylock)(CompatGMutex *mutex)
{
    g_once(&mutex->once, do_g_mutex_new, NULL);
    return g_mutex_trylock((GMutex *) mutex->once.retval);
}
#undef g_mutex_trylock


static inline void (g_mutex_unlock)(CompatGMutex *mutex)
{
    g_mutex_unlock((GMutex *) mutex->once.retval);
}
#undef g_mutex_unlock

static inline gpointer do_g_cond_new(gpointer unused)
{
    return (gpointer) g_cond_new();
}

static inline void g_cond_init(CompatGCond *cond)
{
    cond->once = (GOnce) G_ONCE_INIT;
}

static inline void g_cond_clear(CompatGCond *cond)
{
    g_assert(cond->once.status != G_ONCE_STATUS_PROGRESS);
    if (cond->once.retval) {
        g_cond_free((GCond *) cond->once.retval);
    }
    cond->once = (GOnce) G_ONCE_INIT;
}

static inline void (g_cond_wait)(CompatGCond *cond, CompatGMutex *mutex)
{
    g_assert(mutex->once.status != G_ONCE_STATUS_PROGRESS);
    g_once(&cond->once, do_g_cond_new, NULL);
    g_cond_wait((GCond *) cond->once.retval, (GMutex *) mutex->once.retval);
}
#undef g_cond_wait

static inline void (g_cond_broadcast)(CompatGCond *cond)
{
    g_once(&cond->once, do_g_cond_new, NULL);
    g_cond_broadcast((GCond *) cond->once.retval);
}
#undef g_cond_broadcast

static inline void (g_cond_signal)(CompatGCond *cond)
{
    g_once(&cond->once, do_g_cond_new, NULL);
    g_cond_signal((GCond *) cond->once.retval);
}
#undef g_cond_signal

static inline gboolean (g_cond_timed_wait)(CompatGCond *cond,
                                           CompatGMutex *mutex,
                                           GTimeVal *time)
{
    g_assert(mutex->once.status != G_ONCE_STATUS_PROGRESS);
    g_once(&cond->once, do_g_cond_new, NULL);
    return g_cond_timed_wait((GCond *) cond->once.retval,
                             (GMutex *) mutex->once.retval, time);
}
#undef g_cond_timed_wait

/* This is not a macro, because it didn't exist until 2.32.  */
static inline gboolean g_cond_wait_until(CompatGCond *cond, CompatGMutex *mutex,
                                         gint64 end_time)
{
    GTimeVal time;

    /* Convert from monotonic to CLOCK_REALTIME.  */
    end_time -= g_get_monotonic_time();
    g_get_current_time(&time);
    end_time += time.tv_sec * G_TIME_SPAN_SECOND + time.tv_usec;

    time.tv_sec = end_time / G_TIME_SPAN_SECOND;
    time.tv_usec = end_time % G_TIME_SPAN_SECOND;
    return g_cond_timed_wait(cond, mutex, &time);
}

/* before 2.31 there was no g_thread_new() */
static inline GThread *g_thread_new(const char *name,
                                    GThreadFunc func, gpointer data)
{
    GThread *thread = g_thread_create(func, data, TRUE, NULL);
    if (!thread) {
        g_error("creating thread");
    }
    return thread;
}
#else
#define CompatGMutex GMutex
#define CompatGCond GCond
#endif /* glib 2.31 */

#if !GLIB_CHECK_VERSION(2, 32, 0)
/* Beware, function returns gboolean since 2.39.2, see GLib commit 9101915 */
static inline void g_hash_table_add(GHashTable *hash_table, gpointer key)
{
    g_hash_table_replace(hash_table, key, key);
}

static inline gboolean g_hash_table_contains(GHashTable *hash_table,
                                             gpointer key)
{
    return g_hash_table_lookup_extended(hash_table, key, NULL, NULL);
}
#define G_SOURCE_CONTINUE TRUE
#define G_SOURCE_REMOVE FALSE
#endif

#ifndef g_assert_true
#define g_assert_true(expr)                                                    \
    do {                                                                       \
        if (G_LIKELY(expr)) {                                                  \
        } else {                                                               \
            g_assertion_message(G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,   \
                                "'" #expr "' should be TRUE");                 \
        }                                                                      \
    } while (0)
#endif

#ifndef g_assert_false
#define g_assert_false(expr)                                                   \
    do {                                                                       \
        if (G_LIKELY(!(expr))) {                                               \
        } else {                                                               \
            g_assertion_message(G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,   \
                                "'" #expr "' should be FALSE");                \
        }                                                                      \
    } while (0)
#endif

#ifndef g_assert_null
#define g_assert_null(expr)                                                    \
    do {                                                                       \
        if (G_LIKELY((expr) == NULL)) {                                        \
        } else {                                                               \
            g_assertion_message(G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,   \
                                "'" #expr "' should be NULL");                 \
        }                                                                      \
    } while (0)
#endif

#ifndef g_assert_nonnull
#define g_assert_nonnull(expr)                                                 \
    do {                                                                       \
        if (G_LIKELY((expr) != NULL)) {                                        \
        } else {                                                               \
            g_assertion_message(G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,   \
                                "'" #expr "' should not be NULL");             \
        }                                                                      \
    } while (0)
#endif

#ifndef g_assert_cmpmem
#define g_assert_cmpmem(m1, l1, m2, l2)                                        \
    do {                                                                       \
        gconstpointer __m1 = m1, __m2 = m2;                                    \
        int __l1 = l1, __l2 = l2;                                              \
        if (__l1 != __l2) {                                                    \
            g_assertion_message_cmpnum(                                        \
                G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,                   \
                #l1 " (len(" #m1 ")) == " #l2 " (len(" #m2 "))", __l1, "==",   \
                __l2, 'i');                                                    \
        } else if (memcmp(__m1, __m2, __l1) != 0) {                            \
            g_assertion_message(G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC,   \
                                "assertion failed (" #m1 " == " #m2 ")");      \
        }                                                                      \
    } while (0)
#endif

#if !GLIB_CHECK_VERSION(2, 28, 0)
static inline void g_list_free_full(GList *list, GDestroyNotify free_func)
{
    GList *l;

    for (l = list; l; l = l->next) {
        free_func(l->data);
    }

    g_list_free(list);
}

static inline void g_slist_free_full(GSList *list, GDestroyNotify free_func)
{
    GSList *l;

    for (l = list; l; l = l->next) {
        free_func(l->data);
    }

    g_slist_free(list);
}
#endif

#if !GLIB_CHECK_VERSION(2, 26, 0)
static inline void g_source_set_name(GSource *source, const char *name)
{
    /* This is just a debugging aid, so leaving it a no-op */
}
static inline void g_source_set_name_by_id(guint tag, const char *name)
{
    /* This is just a debugging aid, so leaving it a no-op */
}
#endif

#if !GLIB_CHECK_VERSION(2, 36, 0)
/* Always fail.  This will not include error_report output in the test log,
 * sending it instead to stderr.
 */
#define g_test_initialized() (0)
#endif
#if !GLIB_CHECK_VERSION(2, 38, 0)
#ifdef CONFIG_HAS_GLIB_SUBPROCESS_TESTS
#error schizophrenic detection of glib subprocess testing
#endif
#define g_test_subprocess() (0)
#endif


#if !GLIB_CHECK_VERSION(2, 34, 0)
static inline void
g_test_add_data_func_full(const char *path,
                          gpointer data,
                          gpointer fn,
                          gpointer data_free_func)
{
#if GLIB_CHECK_VERSION(2, 26, 0)
    /* back-compat casts, remove this once we can require new-enough glib */
    g_test_add_vtable(path, 0, data, NULL,
                      (GTestFixtureFunc)fn, (GTestFixtureFunc) data_free_func);
#else
    /* back-compat casts, remove this once we can require new-enough glib */
    g_test_add_vtable(path, 0, data, NULL,
                      (void (*)(void)) fn, (void (*)(void)) data_free_func);
#endif
}
#endif

#pragma GCC diagnostic pop

#endif
