/*
 * Raw-file incremental checkpoint wrapper
 *
 * Thin interface over flat binary files for incremental guest-RAM
 * checkpointing without bxdb.  Each checkpoint lives in a directory
 * <chain_base>.rawmem/ containing numbered data files (0 = base, 1/2/... = deltas)
 * and a <chain_base>.rawmem/raw-meta file with current_snap_id.
 *
 * Top-level pointer files <snapshot_name>.raw-meta map QEMU snapshot
 * names to a chain directory and snap_id.
 */

#ifndef QEMU_MIGRATION_RAW_CHECKPOINT_H
#define QEMU_MIGRATION_RAW_CHECKPOINT_H

#include "qapi/error.h"

struct DirtyBitmapSnapshot;

/*
 * Save paths. `memory` points at the first byte of the guest RAM region to
 * snapshot; `memory_size` is its size in bytes and must be a multiple of 4096.
 *
 * save_base creates <name>.rawmem/0 and <name>.rawmem/raw-meta.
 * save_delta saves a new data file into the chain directory discovered from
 * the running context or from <name>.raw-meta, then writes/updates
 * <name>.raw-meta as a pointer to the chain.
 */
int raw_ckpt_save_base(const char *name,
                       const void *memory, uint64_t memory_size,
                       Error **errp);
int raw_ckpt_save_delta(const char *name,
                        struct DirtyBitmapSnapshot *dirty,
                        const void *memory, uint64_t memory_size,
                        Error **errp);

/* True iff <name>.raw-meta exists (pointer file) or <name>.rawmem/raw-meta exists (chain dir). */
bool raw_ckpt_snapshot_exists(const char *name);

/*
 * Bulk load (on_demand == 0).  Reads the pointer/chain meta to discover
 * target files, madvises guest RAM with MADV_DONTNEED to zero it, then
 * materialises all saved pages from the base and every delta file.
 */
int raw_ckpt_load_bulk(const char *name,
                       void *memory, uint64_t memory_size,
                       Error **errp);

/*
 * On-demand load (on_demand != 0).  Opens and mmaps all data files from
 * latest to oldest.  The uffd handler then calls raw_ckpt_fetch_page for
 * each page fault.  Must be paired with raw_ckpt_ondemand_close.
 */
int  raw_ckpt_ondemand_open(const char *name, uint64_t memory_size,
                            Error **errp);

/*
 * Like raw_ckpt_ondemand_open, but takes a raw checkpoint chain directory
 * directly (e.g. "<snapshot>.rawmem-test") instead of resolving via a
 * QEMU snapshot name.  Used by the BXDB-vs-RAW dual-test harness.
 */
int  raw_ckpt_ondemand_open_at(const char *chain_dir, uint64_t memory_size,
                               Error **errp);

bool raw_ckpt_fetch_page(uint64_t offset, void *buffer);
void raw_ckpt_verify_page(uint64_t offset, const void *buffer);

void raw_ckpt_ondemand_close(void);

void raw_ckpt_shutdown(void);

#endif /* QEMU_MIGRATION_RAW_CHECKPOINT_H */
