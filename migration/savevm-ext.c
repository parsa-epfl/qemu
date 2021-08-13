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
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009-2015 Red Hat Inc
 *
 * Authors:
 *  Klim Kireev <klim.s.kireev@gmail.com>
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
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qerror.h"
#include "block/block_int.h"
#include "io/channel-command.h"
#include "qemu-common.h"

#include "hw/boards.h"
#include "hw/hw.h"
#include "hw/qdev.h"
#include "hw/xen/xen.h"
#include "net/net.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qemu/timer.h"
#include "audio/audio.h"
#include "migration/migration.h"
#include "migration/postcopy-ram.h"
#include "migration/global_state.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qemu/queue.h"
#include "sysemu/cpus.h"
#include "exec/memory.h"
#include "qmp-commands.h"
#include "trace.h"
#include "savevm.h"
#include "qemu/bitops.h"
#include "qemu/iov.h"
#include "block/snapshot.h"
#include "block/qapi.h"
#include "qemu/cutils.h"
#include "io/channel-buffer.h"
#include "io/channel-file.h"
#include "qemu-file-channel.h"
#include "qemu-file.h"

#include "benchmark.h"

const char *input_command = "pbzip2 -d -c";
const char *output_command = "pbzip2 -c";

FILE *savedump = NULL;
FILE *loaddump = NULL;

static int qemu_savevm_state(QEMUFile *f, Error **errp)
{
    int ret;
    MigrationState *ms = migrate_init();
    MigrationStatus status;
    ms->to_dst_file = f;

    if (migration_is_blocked(errp)) {
        ret = -EINVAL;
        goto done;
    }

    if (migrate_use_block()) {
        error_setg(errp, "Saving vm and internal snapshots are incompatible");
        ret = -EINVAL;
        goto done;
    }

    qemu_mutex_unlock_iothread();
    qemu_savevm_state_header(f);
    qemu_savevm_state_setup(f);
    qemu_mutex_lock_iothread();

    while (qemu_file_get_error(f) == 0) {
        if (qemu_savevm_state_iterate(f, false) > 0) {
            break;
        }
    }

    ret = qemu_file_get_error(f);
    if (ret == 0) {
        qemu_savevm_state_complete_precopy(f, false, false);
        ret = qemu_file_get_error(f);
    }
    qemu_savevm_state_cleanup();
    if (ret != 0) {
        error_setg_errno(errp, -ret, "Error while writing VM state");
    }

done:
    if (ret != 0) {
        status = MIGRATION_STATUS_FAILED;
    } else {
        status = MIGRATION_STATUS_COMPLETED;
    }
    migrate_set_state(&ms->state, MIGRATION_STATUS_SETUP, status);

    /* f is outer parameter, it should not stay in global migration state after
     * this function finished */
    ms->to_dst_file = NULL;

    return ret;
}

static void qlist_push(QList *qlist, QObject *value)
{
    QListEntry *entry;

    entry = g_malloc(sizeof(*entry));
    entry->value = value;

    QTAILQ_INSERT_HEAD(&qlist->head, entry, next);
}

static bool isNumber(const char* name)
{
    while (*name) {
        if (!isdigit(*name)) {
                return false;
        }
        name++;
    }
    return true;
}

static BlockDriverState* find_active(void)
{
    BdrvNextIterator bi;
    BlockDriverState *bs = bdrv_first(&bi);
    while (bs && bs->read_only) {
        bs = bdrv_next(&bi);
    }
    return bs;
}

static BlockDriverState* find_base(void)
{
    BdrvNextIterator bi;
    BlockDriverState *bs = bdrv_first(&bi);
    while (bs && bs->backing) {
        bs = bs->backing->bs;
    }
    return bs;
}

static QString *get_dir_path (void) {
    BlockDriverState *bs = find_base();
    if (bs == NULL) {
        return NULL;
    }

    char* end = strrchr(bs->filename, '/');
    if (end == NULL) {
        return qstring_from_str("");
    }
    size_t size = end - bs->filename;
    return qstring_from_substr(bs->filename, 0, size - 1);
}

static QString *get_snap_path (const char *path) {
    char* end = strrchr(path, '/');
    if (end == NULL) {
        return NULL;
    }
    size_t size = end - path;
    return qstring_from_substr(path, 0, size - 1);
}

//static const char* get_snap_name (const char *path) {
//    char* end = strrchr(path, '/');
//    return end + 1;
//}

static QList *get_snap_chain (BlockDriverState *bs) {
    QList* lst = qlist_new();

    bs = bs->backing->bs;

    while (bs->backing != NULL) {
        char path[PATH_MAX];
        if (realpath(bs->filename, path) == NULL) {
            return NULL;
        }
        QString *snap = get_snap_path(path);
        qlist_push(lst, QOBJECT(snap));
        bs = bs->backing->bs;
    }

    return lst;
}

static const char *get_base_name(void) {
    BlockDriverState *bs = find_base();
    const char *ret = strrchr(bs->filename, '/');
    if (ret == NULL) {
        return bs->filename;
    }
    return ret;
}

static void generate_name(char *name, int inc) {
    const char* vmname = qemu_get_vm_name();
    BlockDriverState *bs = find_base();
    srand(time(NULL));
    long unsigned int rand = random() + inc;
    if (vmname != NULL) {
        sprintf(name, "%s-%08lX-%s-tmp", bs->filename, rand, vmname);
    } else {
        sprintf(name, "%s-%08lX-tmp", bs->filename, rand);
    }
}

int delete_tmp_overlay(void) {
    BlockDriverState *bs = find_active();
    if (bs == NULL) {
        return -EINVAL;
    }
    return unlink(bs->filename);
}

int create_tmp_overlay(void) {
    BlockDriverState *bs = find_active();
    char* tmp_name = g_malloc0(8192*sizeof(char));
    const char *dev_name = bdrv_get_device_name(bs);
    int i = 0;

    do {
        generate_name(tmp_name, i);
        i++;
    } while (!access(tmp_name, F_OK));

    Error *local_err = NULL;

    qmp_blockdev_snapshot_sync(true, dev_name, false, NULL,
                                     tmp_name,
                                     false,
                                     NULL,
                                     true, "qcow2",
                                     true, 1, &local_err);
    if (local_err != NULL) {
        error_report_err(local_err);
        g_free(tmp_name);
        return -EINVAL;
    }
    g_free(tmp_name);
    return 0;
}

static int mkdir_p(const char *dir, const mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    struct stat sb;
    size_t len;

    /* copy path */
    strncpy(tmp, dir, sizeof(tmp));
    len = strlen(dir);
    if (len >= sizeof(tmp)) {
        return -1;
    }

    /* remove trailing slash */
    if(tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    /* recursive mkdir */
    for(p = tmp + 1; *p; p++) {
        if(*p == '/') {
            *p = 0;
            /* test path */
            if (stat(tmp, &sb) != 0) {
                /* path does not exist - create directory */
                if (mkdir(tmp, mode) < 0) {
                    return -1;
                }
            } else if (!S_ISDIR(sb.st_mode)) {
                /* not a directory */
                return -1;
            }
            *p = '/';
        }
    }
    /* test path */
    if (stat(tmp, &sb) != 0) {
        /* path does not exist - create directory */
        if (mkdir(tmp, mode) < 0) {
            return -1;
        }
    } else if (!S_ISDIR(sb.st_mode)) {
        /* not a directory */
        return -1;
    }
    return 0;
}

static int create_new_snap_dir(const char *name) {
    int ret = -EINVAL;
    char snap_dir_path[PATH_MAX] = {};
    QString *dir_path = get_dir_path();

    sprintf(snap_dir_path, "%s/%s", qstring_get_str(dir_path),
                                 name);
    ret = mkdir_p(snap_dir_path, 0777);
    QDECREF(dir_path);
    return ret;
}

static int gen_snap_path(const char *name, char *path) {
    QString *dir_path = get_dir_path();
    if (dir_path == NULL) {
        return -EINVAL;
    }
    const char *base_name = get_base_name();
    if (base_name == NULL) {
        return -EINVAL;
    }
    sprintf(path, "%s/%s/%s-sn", dir_path->string, name, base_name);
    QDECREF(dir_path);
    return 0;
}

int save_vmstate_ext(Monitor *mon, const char *name)
{
    BlockDriverState *bs;
    int ret = -EINVAL;
    QEMUFile *f = NULL;
    QString *snap_dir = NULL;
    int saved_vm_running = 0;
    Error *local_err = NULL;
    char snapshot_file[PATH_MAX] = {};
    char command[PATH_MAX] = {};

    if(isNumber(name)){
	monitor_printf(mon, "Error: Please don't save snapshot with numeric name\n"); // Why?
        ret = -EINVAL;
        return ret;
    }

    bs = find_active();
    if (bs == NULL) {
        monitor_printf(mon, "There are not block devices on current VM\n");
        ret = -ENODEV;
        goto end;
    }


    ret = gen_snap_path(name, snapshot_file);
    if (ret < 0) {
        monitor_printf(mon, "Cannot save snapshot %s\n", name);
        goto end;
    }

    ret = create_new_snap_dir(name);
    if (ret < 0) {
        monitor_printf(mon, "Cannot create directory for snapshot %s\n", name);
        goto end;
    }

    if( access( snapshot_file, F_OK ) != -1 ) {
        monitor_printf(mon, "overwriting snapshot directory %s\n", name);
        remove(snapshot_file);
    }
    ret = link(bs->filename, snapshot_file);
    if (ret < 0) {
        monitor_printf(mon, "Cannot save snapshot %s\n", name);
        goto end;
    }
    ret = unlink(bs->filename);
    if (ret < 0) {
        monitor_printf(mon, "Cannot save snapshot %s\n", name);
        goto end;
    }

    memset(bs->filename, 0, PATH_MAX);
    memset(bs->exact_filename, 0, PATH_MAX);
    strcpy(bs->filename, snapshot_file);
    strcpy(bs->exact_filename, snapshot_file);

    ret = create_tmp_overlay();
    if (ret < 0) {
        monitor_printf(mon, "Cannot create temporary overlay %s\n", name);
        goto end;
    }

    saved_vm_running = runstate_is_running();

    ret = global_state_store();
    if (ret) {
        monitor_printf(mon, "Error saving global state\n");
        goto end;
    }
    vm_stop(RUN_STATE_SAVE_VM);

    snap_dir = get_snap_path(snapshot_file);
    if (snap_dir == NULL) {
        monitor_printf(mon, "Cannot get snap path %s\n", name);
        goto end;
    }

    sprintf(command, "%s > %s/mem", output_command, snap_dir->string);
    const char *argv[] = { "/bin/sh", "-c", command, NULL };

#ifdef CONFIG_FLEXUS
    flexus_doSave(snap_dir->string, &local_err);
    if (local_err)
        error_report_err(local_err);
#endif

    QIOChannel *ioc;
    ioc = QIO_CHANNEL(qio_channel_command_new_spawn(argv,
                                                    O_WRONLY,
                                                    &local_err));
    qio_channel_set_name(ioc, "migration-exec-outgoing");

    if (!ioc) {
        monitor_printf(mon, "Could not open VM state file's channel\n");
        goto end;
    }

    f = qemu_fopen_channel_output(ioc);
    if (!f) {
        monitor_printf(mon, "Could not open VM state file\n");
        goto end;
    }

    ret = qemu_savevm_state(f, &local_err);
    if (ret < 0) {
        error_report_err(local_err);
        goto end;
    }

    qemu_fclose(f);

end:
    if (saved_vm_running) {
        vm_start();
    }
    if (snap_dir != NULL) {
        QDECREF(snap_dir);
    }
    return ret;
}

static int goto_snap (const char* snap) {
    char image_path[PATH_MAX] = {};

    BlockDriverState *bs = find_active();
    if (bs == NULL) {
        return -EINVAL;
    }

    int ret = bs->drv->bdrv_make_empty(bs);
    QString *dir_path = get_dir_path();

    sprintf(image_path, "%s/%s/%s-sn", qstring_get_str(dir_path),
                                       snap,
                                       get_base_name());
    ret = bdrv_change_backing_file(bs, image_path, "qcow2");
    if (ret < 0) {
        goto end;
    }

    BlockDriverState *new_back = NULL;
    Error *local_err = NULL;
    new_back = bdrv_open(bs->backing_file, NULL, NULL, 0, &local_err);
    if (new_back == NULL) {
        error_report_err(local_err);
        ret = -EINVAL;
        goto end;
    }

    bdrv_set_backing_hd(bs, NULL, &local_err);
    if (local_err != NULL) {
        error_report_err(local_err);
        ret = -EINVAL;
        goto end;
    }
    bdrv_set_backing_hd(bs, new_back, &local_err);
    if (local_err != NULL) {
        error_report_err(local_err);
        ret = -EINVAL;
        goto end;
    }
    bdrv_unref(new_back);

end:
    QDECREF(dir_path);
    return ret;
}

static int load_state_ext(QString *dir_path)
{
    QEMUFile *f;
    int ret = -EINVAL;
    char command[NAME_MAX] = {};
    Error *local_err = NULL;
    MigrationIncomingState *mis = migration_incoming_get_current();

    sprintf(command, "%s %s/mem", input_command, dir_path->string);
    const char *argv[] = { "/bin/sh", "-c", command, NULL };

    QIOChannel *ioc;
    ioc = QIO_CHANNEL(qio_channel_command_new_spawn(argv,
                                                    O_RDONLY,
                                                    &local_err));
    if (!ioc) {
        error_report("Could not open VM state file's channel\n");
        goto end;
    }
    qio_channel_set_name(ioc, "loadvm-exec-incoming");

    f = qemu_fopen_channel_input(ioc);
    if (!f) {
        error_report("Cannot run a command %s\n", command);
        goto end;
    }
    input_channel = ioc;

    mis->from_src_file = f;

    ret = qemu_loadvm_state(f);

    migration_incoming_state_destroy();
    if (ret < 0) {
        error_report("Error %d while loading vm state", ret);
        goto end;
    }

    return 0;
end:
    return ret;
}

int incremental_load_vmstate_ext (const char *name, Monitor *mon) {
    int saved_vm_running  = runstate_is_running();
    int ret = -EINVAL;

    QString *dir_path = get_dir_path();
    if (dir_path == NULL) {
        monitor_printf(mon, "There are not block devices on current VM\n");
        ret = -ENODEV;
        goto end_snap_uncreated;
    }
    QDECREF(dir_path);

    ret = goto_snap(name);

    if (ret < 0) {
        monitor_printf(mon, "Cannot load snapshot %s\n", name);
        goto end_snap_uncreated;
    }

    BlockDriverState *bs = find_active();
    if (bs == NULL) {
        monitor_printf(mon, "There are not block devices on current VM\n");
        ret = -ENODEV;
        goto end_snap_uncreated;
    }

    // Incremental snapshots
    // Make QList of QStrings with backtracking of snapshot directories
    QList *snap_chain = get_snap_chain(bs);
    if (bs == NULL) {
        monitor_printf(mon, "Cannot build snapshot chain on current VM\n");
        ret = -EINVAL;
        goto end;
    }
    bdrv_drain_all();
    if (qlist_empty(snap_chain)) {
        monitor_printf(mon, "The snapshot chain is empty\n");
        ret = -EINVAL;
        goto end;
    }

    // Load incrementally snapshots
    QString *path = NULL;
    while ((path = qobject_to_qstring(qlist_pop(snap_chain))) != NULL) {
        vm_start();
        vm_stop(RUN_STATE_RESTORE_VM);

        ret = load_state_ext(path);
        if (ret < 0) {
            monitor_printf(mon, "Cannot load memory for snapshot located in %s\n", path->string);
            goto end;
        }

#ifdef CONFIG_FLEXUS
        set_flexus_load_dir(path->string);
#endif

        QDECREF(path);
    }

end:
    if (saved_vm_running) {
        vm_start();
    }
    if (snap_chain != NULL) {
        qlist_destroy_obj(QOBJECT(snap_chain));
    }
end_snap_uncreated:
    if (saved_vm_running) {
        vm_start();
    }
    return ret;
}

