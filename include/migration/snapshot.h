/*
 * QEMU snapshots
 *
 * Copyright (c) 2004-2008 Fabrice Bellard
 * Copyright (c) 2009-2015 Red Hat Inc
 *
 * Authors:
 *  Juan Quintela <quintela@redhat.com>
 *
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_SNAPSHOT_H
#define QEMU_MIGRATION_SNAPSHOT_H

#include "qapi/qapi-builtin-types.h"

typedef enum SnapshotFormat {
    SNAPSHOT_FORMAT_INTERNAL_RAW = 0,
    SNAPSHOT_FORMAT_EXTERNAL_ZSTD = 2,
    SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_BASE = 4, // A complete snapshot, with zstd compression. <name>.state.zstd and <name>.basemem.zstd will be created.
    SNAPSHOT_FORMAT_EXTERNAL_INCREMENTAL_DELTA = 5, // an incremental snapshot based on the prior snapshot. <name>.state.zstd, <basename>-$i.list,<basename>-$i.delta, and <name>.loc will be created.
} SnapshotFormat;


/**
 * save_snapshot: Save an internal snapshot.
 * @name: name of internal snapshot
 * @overwrite: replace existing snapshot with @name
 * @vmstate: blockdev node name to store VM state in
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @format: snapshot format
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool save_snapshot(const char *name, bool overwrite,
                   const char *vmstate,
                   bool has_devices, strList *devices,
                   SnapshotFormat format,
                   Error **errp);

/**
 * load_snapshot: Load an internal snapshot.
 * @name: name of internal snapshot
 * @vmstate: blockdev node name to load VM state from
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 & @on_demand: whether to load the memory from the snapshot on demand (0: no, 1: yes, 2: yes and check)
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool load_snapshot(const char *name,
                   const char *vmstate,
                   bool has_devices, strList *devices,
                   int on_demand,
                   Error **errp);

/**
 * delete_snapshot: Delete a snapshot.
 * @name: path to snapshot
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool delete_snapshot(const char *name,
                    bool has_devices, strList *devices,
                    Error **errp);

#endif
