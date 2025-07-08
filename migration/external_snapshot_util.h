#ifndef EXTERNAL_SNAPSHOT_UTIL_H
#define EXTERNAL_SNAPSHOT_UTIL_H

static QEMUFile *qemu_file_open_output(const char *filename, Error **errp)
{
    QIOChannelFile *ioc = qio_channel_file_new_path(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666, errp);
    if (!ioc) {
        error_setg(errp, "Could not create file %s for writing", filename);
        return NULL;
    }
    qio_channel_set_name(QIO_CHANNEL(ioc), "save_snapshot");
    return qemu_file_new_output(QIO_CHANNEL(ioc));
}

static QEMUFile *qemu_file_open_input(const char *filename, Error **errp)
{
    QIOChannelFile *ioc = qio_channel_file_new_path(filename, O_RDONLY | O_BINARY, 0, errp);
    if (!ioc) {
        error_setg(errp, "Could not open file %s for reading", filename);
        return NULL;
    }
    qio_channel_set_name(QIO_CHANNEL(ioc), "load_snapshot");
    return qemu_file_new_input(QIO_CHANNEL(ioc));
}

static char *get_zstd(Error **errp)
{
    char *zstd = g_find_program_in_path("zstd");
    if (!zstd)
        error_setg(errp, "zstd not found in PATH");

    return zstd;
}

static QEMUFile *qemu_file_open_zstd_output(const char *filename, Error **errp) 
{
    char *zstd = get_zstd(errp);
    if (!zstd) {
        error_setg(errp, "zstd not found in PATH");
        return NULL;
    }

    const char *args[] = {zstd, "-f", "-q", "-T0", "-o", filename, NULL};

    QIOChannelCommand *ioc = qio_channel_command_new_spawn(args, O_WRONLY, errp);

    g_free(zstd);

    if (!ioc) {
        error_setg(errp, "Could not create pipe for zstd");
        return NULL;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "snapshot-zstd");
    return qemu_file_new_output(QIO_CHANNEL(ioc));
}

static QEMUFile *qemu_file_open_zstd_input(const char *filename, Error **errp) 
{
    char *zstd = get_zstd(errp);
    if (!zstd) {
        error_setg(errp, "zstd not found in PATH");
        return NULL;
    }

    const char *args[] = {zstd, "-f", "-q", "-T0", "-d", "-c", filename, NULL};

    QIOChannelCommand *ioc = qio_channel_command_new_spawn(args, O_RDONLY, errp);
    g_free(zstd);
    if (!ioc) {
        error_setg(errp, "Could not create pipe for zstd");
        return NULL;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "load_snapshot");

    return qemu_file_new_input(QIO_CHANNEL(ioc));
}

struct page_location_pair_t {
    uint64_t which_file;
    uint64_t file_offset;
};

static void serialize_incremental_loc_file(FILE *page_location_file, const char *base_name, uint64_t current_index, GHashTable *t)
{
    uint64_t base_name_size = strlen(base_name);
    assert(base_name_size < 256);
    fwrite(&base_name_size, 1, 1, page_location_file);
    fwrite(base_name, base_name_size, 1, page_location_file);
    // Write the current index.
    fwrite(&current_index, sizeof(current_index), 1, page_location_file);
    // Then, the page locations. Iterate over the hash table.
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, t);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        uint64_t page_location = (uint64_t)key;
        struct page_location_pair_t *info = value;
        fwrite(&page_location, sizeof(page_location), 1, page_location_file);
        fwrite(&info->which_file, sizeof(info->which_file), 1, page_location_file);
        fwrite(&info->file_offset, sizeof(info->file_offset), 1, page_location_file);
    }
    // close the file.
    fflush(page_location_file);
    fclose(page_location_file);            
}

static void deserialize_incremental_loc_file(FILE *f, char *base_name, uint64_t base_name_size, uint64_t *current_index, GHashTable *t)
{
    // the hash table has to be empty.
    assert(g_hash_table_size(t) == 0);

    uint8_t base_name_size_read;
    assert(fread(&base_name_size_read, sizeof(base_name_size_read), 1, f) == 1);
    assert(base_name_size_read <= base_name_size);

    assert(fread(base_name, base_name_size_read, 1, f) == 1); // read the base name.

    base_name[base_name_size_read] = '\0'; // null terminate the string.

    // Read the current index.
    if (fread(current_index, sizeof(*current_index), 1, f) != 1) {
        assert(false && "Error reading current index");
    }

    // Then, the page locations. Iterate over the hash table.
    while (1) {
        // Read the page location.
        uint64_t page_location;
        if (fread(&page_location, sizeof(page_location), 1, f) != 1) {
            if (feof(f)) {
                break;
            }
            assert(false && "unexpected.");
        }

        // Read the page location pair.
        struct page_location_pair_t *page_location_pair = g_new0(struct page_location_pair_t, 1);
        // Read which file.
        if (fread(&page_location_pair->which_file, sizeof(page_location_pair->which_file), 1, f) != 1) {
            g_free(page_location_pair);
            assert(false && "Error reading which file");
        }
        // Read the offset.
        if (fread(&page_location_pair->file_offset, sizeof(page_location_pair->file_offset), 1, f) != 1) {
            perror("Error reading offset");
            g_free(page_location_pair);
            assert(false && "Error reading offset");
        }
        // Insert the page location pair into the hash table.
        g_hash_table_insert(t, GINT_TO_POINTER(page_location), page_location_pair);
    }
}


#endif /* EXTERNAL_SNAPSHOT_UTIL_H */