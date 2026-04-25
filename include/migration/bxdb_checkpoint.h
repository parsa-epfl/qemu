/*
 * bxdb-backed incremental checkpoint wrapper
 *
 * Thin QEMU-facing interface over the bxdb C API (see bxdb/c-test/bxdb.h).
 * savevm.c talks to this header; it never includes bxdb.h directly.
 *
 * Each QEMU snapshot is paired with a small companion file
 * <name>.bxdb-meta containing:
 *     db_path=<path to the .bxdb directory>
 *     snap_id=<u32>
 * which tells the loader which bxdb DB and snapshot id to target.
 */

#ifndef QEMU_MIGRATION_BXDB_CHECKPOINT_H
#define QEMU_MIGRATION_BXDB_CHECKPOINT_H

#include "qapi/error.h"

struct DirtyBitmapSnapshot;

#ifdef CONFIG_BXDB

/*
 * Save paths. `memory` points at the first byte of the guest RAM region to
 * snapshot; `memory_size` is its size in bytes and must be a multiple of 4096.
 *
 * save_base writes bxdb snap_id 0 (no shadow, all-ones bitmap, 8 workers) and
 * closes the DB. save_delta requires that a prior load_bulk has opened the DB
 * with shadow enabled, and appends a new snapshot to it using `dirty->dirty`
 * as the per-page bitmap (same memory/size must be passed).
 */
int bxdb_ckpt_save_base(const char *name,
                        const void *memory, uint64_t memory_size,
                        Error **errp);
int bxdb_ckpt_save_delta(const char *name,
                         struct DirtyBitmapSnapshot *dirty,
                         const void *memory, uint64_t memory_size,
                         Error **errp);

/* True iff <name>.bxdb-meta exists. Used by savevm.c for format detection. */
bool bxdb_ckpt_snapshot_exists(const char *name);

/*
 * Bulk load (on_demand == 0). Reads the meta file to discover the target DB
 * and snap_id, opens a fw handle with shadow ON, and materialises the entire
 * guest RAM via bxdb_load_all_pages. Keeps the handle open so that subsequent
 * save_delta calls can append to the same DB.
 */
int bxdb_ckpt_load_bulk(const char *name,
                        void *memory, uint64_t memory_size,
                        Error **errp);

/*
 * On-demand load (on_demand == 1 or 2). Opens a timing DB; the uffd handler
 * then calls bxdb_ckpt_fetch_page for each page fault. Must be paired with
 * bxdb_ckpt_ondemand_close once the VM is shut down.
 */
int  bxdb_ckpt_ondemand_open(const char *name, Error **errp);
bool bxdb_ckpt_fetch_page(uint64_t offset, void *buffer);
void bxdb_ckpt_ondemand_close(void);

void bxdb_ckpt_shutdown(void);

#else  /* !CONFIG_BXDB */

static inline int bxdb_ckpt_save_base(const char *name,
                                      const void *memory, uint64_t memory_size,
                                      Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb; "
                     "incremental base snapshots are unavailable");
    return -ENOTSUP;
}

static inline int bxdb_ckpt_save_delta(const char *name,
                                       struct DirtyBitmapSnapshot *dirty,
                                       const void *memory, uint64_t memory_size,
                                       Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb; "
                     "incremental delta snapshots are unavailable");
    return -ENOTSUP;
}

static inline bool bxdb_ckpt_snapshot_exists(const char *name) { return false; }

static inline int bxdb_ckpt_load_bulk(const char *name,
                                      void *memory, uint64_t memory_size,
                                      Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb");
    return -ENOTSUP;
}

static inline int bxdb_ckpt_ondemand_open(const char *name, Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb");
    return -ENOTSUP;
}

static inline bool bxdb_ckpt_fetch_page(uint64_t offset, void *buffer) { return false; }
static inline void bxdb_ckpt_ondemand_close(void) { }
static inline void bxdb_ckpt_shutdown(void) { }

#endif /* CONFIG_BXDB */

#endif /* QEMU_MIGRATION_BXDB_CHECKPOINT_H */
