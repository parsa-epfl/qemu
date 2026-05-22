/*
 * Raw-file incremental checkpoint wrapper implementation.
 *
 * See include/migration/raw_checkpoint.h for the public contract.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/bitops.h"
#include "io/channel-command.h"
#include "io/channel-file.h"
#include "qemu-file.h"
#include "exec/memory.h"
#include "migration/raw_checkpoint.h"
#include "migration/external_snapshot_util.h"
#include "qemu/plugin-pf.h"

#define RAW_CKPT_PAGE_SIZE  4096u

/*
 * When true, every save also writes "<chain_dir>/raw_complete_<snap_id>.zstd"
 * and every load decompresses that blob to verify the loaded data byte-for-byte.
 */
static bool g_test_mode = false;

/*
 * Raw file format (little-endian, host order):
 *
 *   Offset 0:    uint64_t num_records
 *   Offset 8:    uint64_t addr[0]
 *                uint64_t addr[1]
 *                ...
 *                uint64_t addr[N-1]
 *   Padding:     zero-fill to next 4KB boundary
 *   data_offset: uint8_t  page[0][4096]
 *                uint8_t  page[1][4096]
 *                ...
 *                uint8_t  page[N-1][4096]
 *
 *   data_offset = QEMU_ALIGN_UP(8 + N * 8, 4096)
 *
 * Directory layout:
 *   <chain_base>.rawmem/              # chain folder (created by save_base)
 *   <chain_base>.rawmem/0             # base data (snap_id 0)
 *   <chain_base>.rawmem/1             # delta 1
 *   <chain_base>.rawmem/2             # delta 2, etc.
 *   <chain_base>.rawmem/raw-meta      # "current_snap_id=<N>"
 *   <chain_base>.rawmem/raw_complete_<N>.zstd  # test-mode reference blob
 *
 * Pointer files (per-snapshot, side by side with the chain folder):
 *   <snapshot_name>.raw-meta   # "chain_dir=<chain_base>.rawmem\nsnap_id=<N>"
 */

/* ------------------------------------------------------------------ *
 * Persistent context (kept across load/save calls within one session)
 * ------------------------------------------------------------------ */

static struct {
    char     chain_dir[PATH_MAX];  /* e.g. "init_warmed.rawmem" — the folder */
    uint32_t current_snap_id;      /* latest snap_id in the chain */
} g_raw_ctx;

/* ------------------------------------------------------------------ *
 * On-demand context (open files, maintained until ondemand_close)
 * ------------------------------------------------------------------ */

typedef struct {
    int      fd;
    void    *mapping;
    size_t   file_size;
    uint64_t num_records;
    uint64_t data_offset;  /* QEMU_ALIGN_UP(8 + num_records * 8, 4096) */
} RawOndemandFile;

static struct {
    int              num_files;   /* base + all deltas = current_snap_id + 1 */
    RawOndemandFile *files;       /* index 0 = latest delta, last = base */
    uint8_t         *ref_host;    /* test-mode reference RAM for on-demand verify */
    uint64_t         ref_size;
} g_raw_ondemand;

/* ------------------------------------------------------------------ *
 * Path helpers
 * ------------------------------------------------------------------ */

/* Top-level pointer file: <name>.raw-meta */
static void pointer_path(const char *name, char *out, size_t outlen)
{
    size_t name_len = strlen(name);
    memcpy(out, name, name_len);
    memcpy(out + name_len, ".raw-meta", 10);
}

/* Chain meta: <chain_dir>/raw-meta */
static void chain_meta_path(const char *chain_dir, char *out, size_t outlen)
{
    (void)outlen;
    size_t dir_len = strlen(chain_dir);
    memcpy(out, chain_dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, "raw-meta", 9);
}

/* Data file: <chain_dir>/<snap_id> */
static void data_file_path(const char *chain_dir, uint32_t snap_id,
                           char *out, size_t outlen)
{
    size_t dir_len = strlen(chain_dir);
    memcpy(out, chain_dir, dir_len);
    out[dir_len] = '/';
    snprintf(out + dir_len + 1, outlen - dir_len - 1,
             "%" PRIu32, snap_id);
}

/* Test-mode zstd blob: <chain_dir>/raw_complete_<snap_id>.zstd */
static void complete_path(const char *chain_dir, uint32_t snap_id,
                          char *out, size_t outlen)
{
    size_t dir_len = strlen(chain_dir);
    memcpy(out, chain_dir, dir_len);
    out[dir_len] = '/';
    snprintf(out + dir_len + 1, outlen - dir_len - 1,
             "raw_complete_%" PRIu32 ".zstd", snap_id);
}

/* ------------------------------------------------------------------ *
 * Meta-file read/write
 * ------------------------------------------------------------------ */

/* Write chain-internal meta: "current_snap_id=<N>" */
static int write_chain_meta(const char *chain_dir, uint32_t snap_id,
                            Error **errp)
{
    char path[PATH_MAX];
    chain_meta_path(chain_dir, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        error_setg_errno(errp, errno, "Could not write %s", path);
        return -1;
    }
    fprintf(f, "current_snap_id=%" PRIu32 "\n", snap_id);
    if (fflush(f) != 0 || fclose(f) != 0) {
        error_setg_errno(errp, errno, "Failed to flush/close %s", path);
        return -1;
    }
    return 0;
}

/* Read chain-internal meta, return current_snap_id */
static int read_chain_meta(const char *chain_dir, uint32_t *out_snap_id,
                           Error **errp)
{
    char path[PATH_MAX];
    chain_meta_path(chain_dir, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        error_setg_errno(errp, errno, "Could not open %s", path);
        return -1;
    }

    char line[256];
    bool have_id = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "current_snap_id=", 16) == 0) {
            char *end = NULL;
            unsigned long v = strtoul(line + 16, &end, 10);
            if (end == line + 16 || v > UINT32_MAX) {
                fclose(f);
                error_setg(errp, "Malformed current_snap_id in %s", path);
                return -1;
            }
            *out_snap_id = (uint32_t)v;
            have_id = true;
        }
    }
    fclose(f);

    if (!have_id) {
        error_setg(errp, "Missing current_snap_id in %s", path);
        return -1;
    }
    return 0;
}

/* Write pointer file: "chain_dir=<...>\nsnap_id=<N>" */
static int write_pointer(const char *name, const char *chain_dir,
                         uint32_t snap_id, Error **errp)
{
    char path[PATH_MAX];
    pointer_path(name, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        error_setg_errno(errp, errno, "Could not write %s", path);
        return -1;
    }
    fprintf(f, "chain_dir=%s\nsnap_id=%" PRIu32 "\n", chain_dir, snap_id);
    if (fflush(f) != 0 || fclose(f) != 0) {
        error_setg_errno(errp, errno, "Failed to flush/close %s", path);
        return -1;
    }
    return 0;
}

/* Read pointer file, fill chain_dir and snap_id */
static int read_pointer(const char *name,
                        char *out_chain_dir, size_t chain_dir_sz,
                        uint32_t *out_snap_id, Error **errp)
{
    char path[PATH_MAX];
    pointer_path(name, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        error_setg_errno(errp, errno, "Could not open %s", path);
        return -1;
    }

    char line[PATH_MAX + 64];
    bool have_dir = false, have_id = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "chain_dir=", 10) == 0) {
            pstrcpy(out_chain_dir, chain_dir_sz, line + 10);
            have_dir = true;
        } else if (strncmp(line, "snap_id=", 8) == 0) {
            char *end = NULL;
            unsigned long v = strtoul(line + 8, &end, 10);
            if (end == line + 8 || v > UINT32_MAX) {
                fclose(f);
                error_setg(errp, "Malformed snap_id in %s", path);
                return -1;
            }
            *out_snap_id = (uint32_t)v;
            have_id = true;
        }
    }
    fclose(f);

    if (!have_dir || !have_id) {
        error_setg(errp,
                   "Missing chain_dir or snap_id in %s", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Discover chain directory from a snapshot name.
 * Priority: 1) running context  2) <name>.raw-meta pointer  3) <name>.rawmem/
 * Also resolves the snap_id that `name` corresponds to.
 * Returns 0 on success and fills *out_chain_dir and *out_snap_id.
 * ------------------------------------------------------------------ */

static int resolve_chain(const char *name,
                         char *out_chain_dir, size_t chain_dir_sz,
                         uint32_t *out_snap_id, Error **errp)
{
    /* 1) Running context (same session) */
    if (g_raw_ctx.chain_dir[0] != '\0') {
        pstrcpy(out_chain_dir, chain_dir_sz, g_raw_ctx.chain_dir);
        *out_snap_id = g_raw_ctx.current_snap_id;
        return 0;
    }

    /* 2) <name>.raw-meta pointer file */
    char ptr_path[PATH_MAX];
    pointer_path(name, ptr_path, sizeof(ptr_path));
    if (g_file_test(ptr_path, G_FILE_TEST_IS_REGULAR)) {
        return read_pointer(name, out_chain_dir, chain_dir_sz,
                            out_snap_id, errp);
    }

    /* 3) <name>.rawmem/raw-meta chain folder (name IS the base name) */
    char rawmem_dir[PATH_MAX];
    snprintf(rawmem_dir, sizeof(rawmem_dir), "%s.rawmem", name);
    char cm_path[PATH_MAX];
    chain_meta_path(rawmem_dir, cm_path, sizeof(cm_path));
    if (g_file_test(cm_path, G_FILE_TEST_IS_REGULAR)) {
        pstrcpy(out_chain_dir, chain_dir_sz, rawmem_dir);
        return read_chain_meta(rawmem_dir, out_snap_id, errp);
    }

    error_setg(errp, "Cannot resolve chain for '%s'", name);
    return -1;
}

/* ------------------------------------------------------------------ *
 * Test-mode zstd blob helpers
 * ------------------------------------------------------------------ */

static int save_complete_blob(const char *chain_dir, uint32_t snap_id,
                              const void *memory, uint64_t memory_size,
                              Error **errp)
{
    char path[PATH_MAX];
    complete_path(chain_dir, snap_id, path, sizeof(path));

    QEMUFile *f = qemu_file_open_zstd_output(path, errp);
    if (!f) {
        return -1;
    }
    qemu_put_buffer(f, (const uint8_t *)memory, memory_size);
    int ret = qemu_fclose(f);
    if (ret < 0) {
        error_setg(errp, "Failed to write zstd blob %s", path);
        return -1;
    }
    return 0;
}

static int load_complete_blob(const char *chain_dir, uint32_t snap_id,
                              void *memory, uint64_t memory_size,
                              Error **errp)
{
    char path[PATH_MAX];
    complete_path(chain_dir, snap_id, path, sizeof(path));

    QEMUFile *f = qemu_file_open_zstd_input(path, errp);
    if (!f) {
        return -1;
    }
    size_t got = qemu_get_buffer(f, (uint8_t *)memory, memory_size);
    int ret = qemu_fclose(f);
    if (ret < 0 || got != memory_size) {
        error_setg(errp, "Failed to read zstd blob %s (got %zu of %" PRIu64 ")",
                   path, got, memory_size);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Common preconditions
 * ------------------------------------------------------------------ */

static bool check_memory_args(const void *memory, uint64_t memory_size,
                              Error **errp)
{
    if (memory == NULL) {
        error_setg(errp, "raw_ckpt: memory is NULL");
        return false;
    }
    if (memory_size == 0 || (memory_size % RAW_CKPT_PAGE_SIZE) != 0) {
        error_setg(errp, "raw_ckpt: memory_size (%" PRIu64
                         ") must be a nonzero multiple of %u",
                   memory_size, RAW_CKPT_PAGE_SIZE);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * Core I/O: write a collection of pages to a raw file
 * ------------------------------------------------------------------ */

static int write_page_file(const char *path,
                           uint64_t num_records,
                           const uint64_t *addrs,
                           const void *memory, uint64_t memory_size,
                           Error **errp)
{
    uint64_t data_offset = QEMU_ALIGN_UP(8 + num_records * 8, 4096);

    FILE *f = fopen(path, "wb");
    if (!f) {
        error_setg_errno(errp, errno, "Could not create %s", path);
        return -1;
    }

    /* header: num_records */
    if (fwrite(&num_records, sizeof(num_records), 1, f) != 1) {
        error_setg_errno(errp, errno, "Failed to write header to %s", path);
        fclose(f);
        return -1;
    }

    /* address array */
    if (num_records > 0) {
        if (fwrite(addrs, sizeof(uint64_t), num_records, f) !=
            (size_t)num_records) {
            error_setg_errno(errp, errno,
                             "Failed to write address array to %s", path);
            fclose(f);
            return -1;
        }
    }

    /* pad to 4KB boundary */
    long pos = ftell(f);
    if (pos < 0) {
        error_setg_errno(errp, errno, "ftell failed on %s", path);
        fclose(f);
        return -1;
    }
    size_t pad = data_offset - (size_t)pos;
    if (pad > 0) {
        uint8_t *zeros = g_malloc0(pad);
        if (fwrite(zeros, 1, pad, f) != pad) {
            error_setg_errno(errp, errno, "Failed to write padding to %s", path);
            g_free(zeros);
            fclose(f);
            return -1;
        }
        g_free(zeros);
    }

    /* data pages */
    for (uint64_t i = 0; i < num_records; i++) {
        uint64_t addr = addrs[i];
        if (addr + RAW_CKPT_PAGE_SIZE > memory_size) {
            error_setg(errp, "Address 0x%" PRIx64 " out of range in %s",
                       addr, path);
            fclose(f);
            return -1;
        }
        const uint8_t *page = (const uint8_t *)memory + addr;
        if (fwrite(page, RAW_CKPT_PAGE_SIZE, 1, f) != 1) {
            error_setg_errno(errp, errno, "Failed to write page data to %s",
                             path);
            fclose(f);
            return -1;
        }
    }

    if (fclose(f) != 0) {
        error_setg_errno(errp, errno, "Failed to close %s", path);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 * Load records from a file into guest memory
 * ------------------------------------------------------------------ */

static int load_file_into_memory(const char *path,
                                 void *memory, uint64_t memory_size,
                                 Error **errp)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        error_setg_errno(errp, errno, "Could not open %s for reading", path);
        return -1;
    }

    uint64_t num_records;
    if (fread(&num_records, sizeof(num_records), 1, f) != 1) {
        error_setg_errno(errp, errno, "Failed to read header from %s", path);
        fclose(f);
        return -1;
    }

    if (num_records == 0) {
        fclose(f);
        return 0;
    }

    uint64_t data_offset = QEMU_ALIGN_UP(8 + num_records * 8, 4096);

    /* read address array */
    uint64_t *addrs = g_malloc(num_records * sizeof(uint64_t));
    if (fread(addrs, sizeof(uint64_t), num_records, f) !=
        (size_t)num_records) {
        error_setg_errno(errp, errno,
                         "Failed to read address array from %s", path);
        g_free(addrs);
        fclose(f);
        return -1;
    }

    /* seek to data */
    if (fseek(f, (long)data_offset, SEEK_SET) != 0) {
        error_setg_errno(errp, errno, "Failed to seek to data in %s", path);
        g_free(addrs);
        fclose(f);
        return -1;
    }

    uint8_t *page_buf = g_malloc(RAW_CKPT_PAGE_SIZE);
    for (uint64_t i = 0; i < num_records; i++) {
        uint64_t addr = addrs[i];
        if (addr + RAW_CKPT_PAGE_SIZE > memory_size) {
            error_setg(errp, "Address 0x%" PRIx64 " out of range in %s",
                       addr, path);
            g_free(page_buf);
            g_free(addrs);
            fclose(f);
            return -1;
        }
        if (fread(page_buf, RAW_CKPT_PAGE_SIZE, 1, f) != 1) {
            error_setg_errno(errp, errno,
                             "Failed to read page %" PRIu64 " from %s",
                             i, path);
            g_free(page_buf);
            g_free(addrs);
            fclose(f);
            return -1;
        }
        memcpy((uint8_t *)memory + addr, page_buf, RAW_CKPT_PAGE_SIZE);
    }

    g_free(page_buf);
    g_free(addrs);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

bool raw_ckpt_snapshot_exists(const char *name)
{
    char path[PATH_MAX];

    /* <name>.raw-meta pointer file */
    pointer_path(name, path, sizeof(path));
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        return true;
    }

    /* <name>.rawmem/raw-meta chain folder */
    char rawmem_dir[PATH_MAX];
    snprintf(rawmem_dir, sizeof(rawmem_dir), "%s.rawmem", name);
    chain_meta_path(rawmem_dir, path, sizeof(path));
    return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

int raw_ckpt_save_base(const char *name,
                       const void *memory, uint64_t memory_size,
                       Error **errp)
{
    if (!check_memory_args(memory, memory_size, errp)) {
        return -1;
    }

    char chain_dir[PATH_MAX];
    snprintf(chain_dir, sizeof(chain_dir), "%s.rawmem", name);

    /* Ensure the chain directory exists */
    if (g_mkdir_with_parents(chain_dir, 0755) < 0) {
        error_setg_errno(errp, errno, "Could not create directory %s", chain_dir);
        return -1;
    }

    uint64_t page_count = memory_size / RAW_CKPT_PAGE_SIZE;

    /* first pass: count and collect addresses of non-zero pages */
    uint64_t *addrs = g_malloc(page_count * sizeof(uint64_t));
    uint64_t num_records = 0;

    for (uint64_t i = 0; i < page_count; i++) {
        const uint8_t *page = (const uint8_t *)memory + i * RAW_CKPT_PAGE_SIZE;
        if (!buffer_is_zero(page, RAW_CKPT_PAGE_SIZE)) {
            addrs[num_records++] = i * RAW_CKPT_PAGE_SIZE;
        }
    }

    char path[PATH_MAX];
    data_file_path(chain_dir, 0, path, sizeof(path));

    int ret = write_page_file(path, num_records, addrs,
                               memory, memory_size, errp);
    g_free(addrs);
    if (ret < 0) {
        return -1;
    }

    if (write_chain_meta(chain_dir, 0, errp) < 0) {
        return -1;
    }
    if (g_test_mode &&
        save_complete_blob(chain_dir, 0, memory, memory_size, errp) < 0) {
        return -1;
    }

    pstrcpy(g_raw_ctx.chain_dir, sizeof(g_raw_ctx.chain_dir), chain_dir);
    g_raw_ctx.current_snap_id = 0;
    return 0;
}

int raw_ckpt_save_delta(const char *name,
                        struct DirtyBitmapSnapshot *dirty,
                        const void *memory, uint64_t memory_size,
                        Error **errp)
{
    if (!check_memory_args(memory, memory_size, errp)) {
        return -1;
    }
    if (dirty == NULL) {
        error_setg(errp, "raw_ckpt_save_delta: dirty bitmap is NULL");
        return -1;
    }
    if (dirty->start != 0) {
        error_setg(errp, "raw_ckpt_save_delta: dirty bitmap start (%" PRIu64
                         ") must be 0", (uint64_t)dirty->start);
        return -1;
    }
    uint64_t bitmap_size = (uint64_t)(dirty->end - dirty->start);
    if (bitmap_size != memory_size) {
        error_setg(errp, "raw_ckpt_save_delta: dirty bitmap covers %" PRIu64
                         " bytes but memory_size is %" PRIu64,
                   bitmap_size, memory_size);
        return -1;
    }

    char chain_dir[PATH_MAX];
    uint32_t prior_snap_id;

    if (resolve_chain(name, chain_dir, sizeof(chain_dir),
                      &prior_snap_id, errp) < 0) {
        return -1;
    }

    uint32_t snap_id = prior_snap_id + 1;
    uint64_t page_count = memory_size / RAW_CKPT_PAGE_SIZE;

    /* collect addresses of all dirty pages (no zero-filtering) */
    uint64_t *addrs = g_malloc(page_count * sizeof(uint64_t));
    uint64_t num_records = 0;

    for (uint64_t i = 0; i < page_count; i++) {
        if (test_bit(i, dirty->dirty)) {
            addrs[num_records++] = i * RAW_CKPT_PAGE_SIZE;
        }
    }

    char path[PATH_MAX];
    data_file_path(chain_dir, snap_id, path, sizeof(path));

    int ret = write_page_file(path, num_records, addrs,
                               memory, memory_size, errp);
    g_free(addrs);
    if (ret < 0) {
        return -1;
    }

    if (write_chain_meta(chain_dir, snap_id, errp) < 0) {
        return -1;
    }
    if (write_pointer(name, chain_dir, snap_id, errp) < 0) {
        return -1;
    }
    if (g_test_mode &&
        save_complete_blob(chain_dir, snap_id, memory, memory_size, errp) < 0) {
        return -1;
    }

    pstrcpy(g_raw_ctx.chain_dir, sizeof(g_raw_ctx.chain_dir), chain_dir);
    g_raw_ctx.current_snap_id = snap_id;
    return 0;
}

int raw_ckpt_load_bulk(const char *name,
                       void *memory, uint64_t memory_size,
                       Error **errp)
{
    if (!check_memory_args(memory, memory_size, errp)) {
        return -1;
    }

    char chain_dir[PATH_MAX];
    uint32_t current_snap_id;

    if (resolve_chain(name, chain_dir, sizeof(chain_dir),
                      &current_snap_id, errp) < 0) {
        return -1;
    }

    /* Drop all pages; OS will fault in zero pages on first access. */
    if (madvise(memory, memory_size, MADV_DONTNEED) != 0) {
        error_setg_errno(errp, errno, "madvise(MADV_DONTNEED) failed");
        return -1;
    }

    /* Load base and all deltas in order */
    for (uint32_t sid = 0; sid <= current_snap_id; sid++) {
        char path[PATH_MAX];
        data_file_path(chain_dir, sid, path, sizeof(path));
        if (load_file_into_memory(path, memory, memory_size, errp) < 0) {
            return -1;
        }
    }

    pstrcpy(g_raw_ctx.chain_dir, sizeof(g_raw_ctx.chain_dir), chain_dir);
    g_raw_ctx.current_snap_id = current_snap_id;

    if (g_test_mode) {
        uint64_t page_count = memory_size / RAW_CKPT_PAGE_SIZE;
        uint8_t *temp = g_malloc(memory_size);
        if (load_complete_blob(chain_dir, current_snap_id, temp, memory_size,
                               errp) < 0) {
            g_free(temp);
            return -1;
        }
        if (memcmp(memory, temp, memory_size) != 0) {
            for (uint64_t p = 0; p < page_count; p++) {
                uint64_t off = p * RAW_CKPT_PAGE_SIZE;
                if (memcmp((const uint8_t *)memory + off, temp + off,
                           RAW_CKPT_PAGE_SIZE) != 0) {
                    fprintf(stderr,
                            "raw_ckpt: bulk-load mismatch at offset %" PRIu64
                            " (snap_id=%" PRIu32 ")\n", off, current_snap_id);
                    break;
                }
            }
            g_free(temp);
            assert(false && "raw ckpt bulk load disagrees with zstd reference");
        }
        g_free(temp);
    }

    return 0;
}

int raw_ckpt_ondemand_open(const char *name, uint64_t memory_size,
                            Error **errp)
{
    (void)memory_size;

    char chain_dir[PATH_MAX];
    uint32_t current_snap_id;

    if (resolve_chain(name, chain_dir, sizeof(chain_dir),
                      &current_snap_id, errp) < 0) {
        return -1;
    }

    /* close any previously opened files */
    raw_ckpt_ondemand_close();

    int num_files = (int)current_snap_id + 1; /* base + all deltas */
    g_raw_ondemand.num_files = num_files;
    g_raw_ondemand.files = g_malloc0((size_t)num_files * sizeof(RawOndemandFile));

    /* Open from latest to oldest (index 0 = latest) */
    for (uint32_t sid = current_snap_id; ; sid--) {
        int idx = (int)(current_snap_id - sid);
        char path[PATH_MAX];
        data_file_path(chain_dir, sid, path, sizeof(path));

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            g_raw_ondemand.files[idx].fd = -1;
            if (sid == 0) {
                break;
            }
            continue;
        }

        struct stat st;
        if (fstat(fd, &st) < 0) {
            close(fd);
            error_setg_errno(errp, errno, "fstat failed for %s", path);
            raw_ckpt_ondemand_close();
            return -1;
        }

        void *mapping = mmap(NULL, (size_t)st.st_size, PROT_READ,
                             MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            close(fd);
            error_setg_errno(errp, errno, "mmap failed for %s", path);
            raw_ckpt_ondemand_close();
            return -1;
        }

        uint64_t n = *(const uint64_t *)mapping;

        g_raw_ondemand.files[idx].fd = fd;
        g_raw_ondemand.files[idx].mapping = mapping;
        g_raw_ondemand.files[idx].file_size = (size_t)st.st_size;
        g_raw_ondemand.files[idx].num_records = n;
        g_raw_ondemand.files[idx].data_offset =
            QEMU_ALIGN_UP(8 + n * 8, 4096);

        if (sid == 0) {
            break;
        }
    }

    pstrcpy(g_raw_ctx.chain_dir, sizeof(g_raw_ctx.chain_dir), chain_dir);
    g_raw_ctx.current_snap_id = current_snap_id;

    if (g_test_mode) {
        g_raw_ondemand.ref_host = g_malloc(memory_size);
        g_raw_ondemand.ref_size = memory_size;
        if (load_complete_blob(chain_dir, current_snap_id,
                               g_raw_ondemand.ref_host, memory_size,
                               errp) < 0) {
            g_free(g_raw_ondemand.ref_host);
            g_raw_ondemand.ref_host = NULL;
            g_raw_ondemand.ref_size = 0;
            raw_ckpt_ondemand_close();
            return -1;
        }
    }

    return 0;
}

bool raw_ckpt_fetch_page(uint64_t offset, void *buffer)
{
    struct timespec _t_total, _t_seg, _t_seg_end;
    uint64_t t_index_acc = 0, t_copy_acc = 0;

    clock_gettime(CLOCK_MONOTONIC_RAW, &_t_total);

    uint64_t target_addr = offset;
    uint64_t files_searched = 0;
    uint64_t bsearch_steps = 0;

    /* Search from latest to oldest */
    for (int i = 0; i < g_raw_ondemand.num_files; i++) {
        RawOndemandFile *f = &g_raw_ondemand.files[i];
        if (f->fd == -1 || f->num_records == 0) {
            continue;
        }

        files_searched++;

        const uint8_t *base = (const uint8_t *)f->mapping;
        const uint64_t *addrs = (const uint64_t *)(base + sizeof(uint64_t));

        /* binary search the sorted address array */
        clock_gettime(CLOCK_MONOTONIC_RAW, &_t_seg);
        uint64_t lo = 0, hi = f->num_records;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            bsearch_steps++;
            if (addrs[mid] < target_addr) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        clock_gettime(CLOCK_MONOTONIC_RAW, &_t_seg_end);
        t_index_acc += (uint64_t)(_t_seg_end.tv_sec  - _t_seg.tv_sec)  * 1000000000ULL
                     + (uint64_t)(_t_seg_end.tv_nsec - _t_seg.tv_nsec);

        if (lo < f->num_records && addrs[lo] == target_addr) {
            clock_gettime(CLOCK_MONOTONIC_RAW, &_t_seg);
            memcpy(buffer,
                   base + f->data_offset + lo * RAW_CKPT_PAGE_SIZE,
                   RAW_CKPT_PAGE_SIZE);
            clock_gettime(CLOCK_MONOTONIC_RAW, &_t_seg_end);
            t_copy_acc = (uint64_t)(_t_seg_end.tv_sec  - _t_seg.tv_sec)  * 1000000000ULL
                       + (uint64_t)(_t_seg_end.tv_nsec - _t_seg.tv_nsec);

            clock_gettime(CLOCK_MONOTONIC_RAW, &_t_seg);
            g_timing_info.raw_ckpt_total_ns +=
                (uint64_t)(_t_seg.tv_sec  - _t_total.tv_sec)  * 1000000000ULL
                + (uint64_t)(_t_seg.tv_nsec - _t_total.tv_nsec);
            g_timing_info.raw_ckpt_index_ns += t_index_acc;
            g_timing_info.raw_ckpt_copy_ns  += t_copy_acc;
            g_timing_info.raw_ckpt_pages_found += 1;
            g_timing_info.raw_ckpt_files_searched += files_searched;
            g_timing_info.raw_ckpt_bsearch_steps += bsearch_steps;

            return true;
        }
    }

    /* zero page: not found in any file */
    clock_gettime(CLOCK_MONOTONIC_RAW, &_t_seg);
    g_timing_info.raw_ckpt_total_ns +=
        (uint64_t)(_t_seg.tv_sec  - _t_total.tv_sec)  * 1000000000ULL
        + (uint64_t)(_t_seg.tv_nsec - _t_total.tv_nsec);
    g_timing_info.raw_ckpt_index_ns += t_index_acc;
    g_timing_info.raw_ckpt_pages_zero += 1;
    g_timing_info.raw_ckpt_files_searched += files_searched;
    g_timing_info.raw_ckpt_bsearch_steps += bsearch_steps;

    return false;
}

void raw_ckpt_verify_page(uint64_t offset, const void *buffer)
{
    if (!g_test_mode || g_raw_ondemand.ref_host == NULL) {
        return;
    }
    if (offset + RAW_CKPT_PAGE_SIZE > g_raw_ondemand.ref_size) {
        fprintf(stderr, "raw_ckpt_verify_page: offset %" PRIu64
                        " out of reference range %" PRIu64 "\n",
                offset, g_raw_ondemand.ref_size);
        assert(false && "raw ckpt verify_page offset out of range");
    }
    if (memcmp(buffer, g_raw_ondemand.ref_host + offset,
               RAW_CKPT_PAGE_SIZE) != 0) {
        fprintf(stderr, "raw_ckpt_verify_page: mismatch at offset %" PRIu64
                        " (snap_id=%" PRIu32 ")\n",
                offset, g_raw_ctx.current_snap_id);
        assert(false && "raw ckpt on-demand page disagrees with zstd reference");
    }
}

void raw_ckpt_ondemand_close(void)
{
    if (g_raw_ondemand.files != NULL) {
        for (int i = 0; i < g_raw_ondemand.num_files; i++) {
            RawOndemandFile *f = &g_raw_ondemand.files[i];
            if (f->fd >= 0) {
                if (f->mapping != NULL && f->mapping != MAP_FAILED) {
                    munmap(f->mapping, f->file_size);
                }
                close(f->fd);
            }
        }
        g_free(g_raw_ondemand.files);
        g_raw_ondemand.files = NULL;
        g_raw_ondemand.num_files = 0;
    }

    if (g_raw_ondemand.ref_host != NULL) {
        g_free(g_raw_ondemand.ref_host);
        g_raw_ondemand.ref_host = NULL;
        g_raw_ondemand.ref_size = 0;
    }
}

void raw_ckpt_shutdown(void)
{
    raw_ckpt_ondemand_close();
    memset(&g_raw_ctx, 0, sizeof(g_raw_ctx));
}
