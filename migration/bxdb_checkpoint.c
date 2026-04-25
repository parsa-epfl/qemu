/*
 * bxdb-backed incremental checkpoint wrapper implementation.
 *
 * See include/migration/bxdb_checkpoint.h for the public contract.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "exec/memory.h"
#include "migration/bxdb_checkpoint.h"

#include "bxdb.h"

#define BXDB_PAGE_SIZE       4096u
#define BXDB_WORKER_COUNT    8
#define BXDB_DELTA_THRESHOLD 0  /* 0 == library default (DEFAULT_DELTA_THRESHOLD) */

static struct {
    struct BxdbHandle *fw_db;     /* open for writes + bulk reads (shadow-on) */
    struct BxdbHandle *timing_db; /* open for on-demand page reads */
    char db_path[PATH_MAX];       /* e.g. "foo.bxdb" */
    uint32_t next_snap_id;        /* next snap_id to assign to a save_delta */
    uint32_t ondemand_snap_id;    /* snap_id to query in fetch_page */
} g_ctx;

/* ------------------------------------------------------------------ *
 * Meta-file helpers
 * ------------------------------------------------------------------ */

static void meta_path(const char *name, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s.bxdb-meta", name);
}

static int write_meta(const char *name, const char *db_path, uint32_t snap_id,
                      Error **errp)
{
    char path[PATH_MAX];
    meta_path(name, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        error_setg_errno(errp, errno, "Could not write %s", path);
        return -1;
    }
    fprintf(f, "db_path=%s\nsnap_id=%" PRIu32 "\n", db_path, snap_id);
    if (fflush(f) != 0 || fclose(f) != 0) {
        error_setg_errno(errp, errno, "Failed to flush/close %s", path);
        return -1;
    }
    return 0;
}

static int read_meta(const char *name, char *out_db_path, size_t db_path_sz,
                     uint32_t *out_snap_id, Error **errp)
{
    char path[PATH_MAX];
    meta_path(name, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        error_setg_errno(errp, errno, "Could not open %s", path);
        return -1;
    }

    char line[PATH_MAX + 64];
    bool have_path = false, have_id = false;
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "db_path=", 8) == 0) {
            pstrcpy(out_db_path, db_path_sz, line + 8);
            have_path = true;
        } else if (strncmp(line, "snap_id=", 8) == 0) {
            char *end = NULL;
            unsigned long v = strtoul(line + 8, &end, 10);
            if (end == line + 8 || v > MAX_SNAPSHOT_ID) {
                fclose(f);
                error_setg(errp, "Malformed snap_id in %s", path);
                return -1;
            }
            *out_snap_id = (uint32_t)v;
            have_id = true;
        }
    }
    fclose(f);

    if (!have_path || !have_id) {
        error_setg(errp, "Missing db_path or snap_id in %s", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Common preconditions
 * ------------------------------------------------------------------ */

static bool check_memory_args(const void *memory, uint64_t memory_size, Error **errp)
{
    if (memory == NULL) {
        error_setg(errp, "bxdb_ckpt: memory is NULL");
        return false;
    }
    if (memory_size == 0 || (memory_size % BXDB_PAGE_SIZE) != 0) {
        error_setg(errp, "bxdb_ckpt: memory_size (%" PRIu64
                         ") must be a nonzero multiple of %u",
                   memory_size, BXDB_PAGE_SIZE);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

bool bxdb_ckpt_snapshot_exists(const char *name)
{
    char path[PATH_MAX];
    meta_path(name, path, sizeof(path));
    return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

int bxdb_ckpt_save_base(const char *name,
                        const void *memory, uint64_t memory_size,
                        Error **errp)
{
    if (!check_memory_args(memory, memory_size, errp)) {
        return -1;
    }

    char db_path[PATH_MAX];
    snprintf(db_path, sizeof(db_path), "%s.bxdb", name);

    uint64_t page_count = memory_size / BXDB_PAGE_SIZE;

    struct BxdbHandle *db = bxdb_open_for_fw(db_path, BXDB_WORKER_COUNT,
                                             BXDB_DELTA_THRESHOLD, false);
    if (!db) {
        error_setg(errp, "bxdb_open_for_fw(%s) failed", db_path);
        return -1;
    }

    /* All-ones bitmap: every page is dirty for the base snapshot. */
    uint64_t nwords = (page_count + 63) / 64;
    uint64_t *bitmap = g_malloc(nwords * sizeof(uint64_t));
    memset(bitmap, 0xff, nwords * sizeof(uint64_t));

    bxdb_save_pages(db, (const char *)memory, bitmap, page_count, 0);

    g_free(bitmap);
    bxdb_close(db);

    if (write_meta(name, db_path, 0, errp) < 0) {
        return -1;
    }
    return 0;
}

int bxdb_ckpt_save_delta(const char *name,
                         struct DirtyBitmapSnapshot *dirty,
                         const void *memory, uint64_t memory_size,
                         Error **errp)
{
    if (!check_memory_args(memory, memory_size, errp)) {
        return -1;
    }
    if (g_ctx.fw_db == NULL) {
        error_setg(errp, "bxdb_ckpt_save_delta: no base loaded — load a "
                         "base snapshot before creating incremental deltas");
        return -1;
    }
    if (dirty == NULL) {
        error_setg(errp, "bxdb_ckpt_save_delta: dirty bitmap is NULL");
        return -1;
    }
    if (dirty->start != 0) {
        error_setg(errp, "bxdb_ckpt_save_delta: dirty bitmap start (%" PRIu64
                         ") must be 0", (uint64_t)dirty->start);
        return -1;
    }
    uint64_t bitmap_size = (uint64_t)(dirty->end - dirty->start);
    if (bitmap_size != memory_size) {
        error_setg(errp, "bxdb_ckpt_save_delta: dirty bitmap covers %" PRIu64
                         " bytes but memory_size is %" PRIu64,
                   bitmap_size, memory_size);
        return -1;
    }
    if (g_ctx.next_snap_id > MAX_SNAPSHOT_ID) {
        error_setg(errp, "bxdb_ckpt_save_delta: snap_id overflow "
                         "(max %u)", MAX_SNAPSHOT_ID);
        return -1;
    }

    uint64_t page_count = memory_size / BXDB_PAGE_SIZE;

    /*
     * DirtyBitmapSnapshot::dirty is `unsigned long dirty[]`. On 64-bit
     * platforms (all Linux hosts QEMU targets here) that's compatible with
     * uint64_t[]. bxdb expects the bitmap to have >= ceil(page_count/64) words.
     */
    bxdb_save_pages(g_ctx.fw_db,
                    (const char *)memory,
                    (const uint64_t *)dirty->dirty,
                    page_count,
                    g_ctx.next_snap_id);

    if (write_meta(name, g_ctx.db_path, g_ctx.next_snap_id, errp) < 0) {
        return -1;
    }
    g_ctx.next_snap_id++;
    return 0;
}

int bxdb_ckpt_load_bulk(const char *name,
                        void *memory, uint64_t memory_size,
                        Error **errp)
{
    if (!check_memory_args(memory, memory_size, errp)) {
        return -1;
    }

    char db_path[PATH_MAX];
    uint32_t snap_id = 0;
    if (read_meta(name, db_path, sizeof(db_path), &snap_id, errp) < 0) {
        return -1;
    }

    /* Shadow ON: later save_delta calls share the same handle. */
    struct BxdbHandle *db = bxdb_open_for_fw(db_path, BXDB_WORKER_COUNT,
                                             BXDB_DELTA_THRESHOLD, true);
    if (!db) {
        error_setg(errp, "bxdb_open_for_fw(%s) failed", db_path);
        return -1;
    }

    uint64_t page_count = memory_size / BXDB_PAGE_SIZE;
    if (!bxdb_load_all_pages(db, (char *)memory, 0, page_count, snap_id,
                             BXDB_WORKER_COUNT)) {
        bxdb_close(db);
        error_setg(errp, "bxdb_load_all_pages(%s, snap_id=%" PRIu32
                         ") reported missing pages", db_path, snap_id);
        return -1;
    }

    /* Close any previously held handle before replacing it. */
    if (g_ctx.fw_db != NULL) {
        bxdb_close(g_ctx.fw_db);
    }
    g_ctx.fw_db = db;
    pstrcpy(g_ctx.db_path, sizeof(g_ctx.db_path), db_path);
    g_ctx.next_snap_id = snap_id + 1;
    return 0;
}

int bxdb_ckpt_ondemand_open(const char *name, Error **errp)
{
    char db_path[PATH_MAX];
    uint32_t snap_id = 0;
    if (read_meta(name, db_path, sizeof(db_path), &snap_id, errp) < 0) {
        return -1;
    }

    struct BxdbHandle *db = bxdb_open_for_timing(db_path);
    if (!db) {
        error_setg(errp, "bxdb_open_for_timing(%s) failed", db_path);
        return -1;
    }

    if (g_ctx.timing_db != NULL) {
        bxdb_close(g_ctx.timing_db);
    }
    g_ctx.timing_db = db;
    pstrcpy(g_ctx.db_path, sizeof(g_ctx.db_path), db_path);
    g_ctx.ondemand_snap_id = snap_id;
    return 0;
}

bool bxdb_ckpt_fetch_page(uint64_t offset, void *buffer)
{
    if (g_ctx.timing_db == NULL) {
        return false;
    }
    uint64_t pa = offset / BXDB_PAGE_SIZE;
    return bxdb_load_page(g_ctx.timing_db, (char *)buffer, pa,
                          g_ctx.ondemand_snap_id);
}

void bxdb_ckpt_ondemand_close(void)
{
    if (g_ctx.timing_db != NULL) {
        bxdb_close(g_ctx.timing_db);
        g_ctx.timing_db = NULL;
    }
}

void bxdb_ckpt_shutdown(void)
{
    if (g_ctx.fw_db != NULL) {
        bxdb_close(g_ctx.fw_db);
        g_ctx.fw_db = NULL;
    }
    if (g_ctx.timing_db != NULL) {
        bxdb_close(g_ctx.timing_db);
        g_ctx.timing_db = NULL;
    }
}
