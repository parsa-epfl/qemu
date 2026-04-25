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

#endif /* EXTERNAL_SNAPSHOT_UTIL_H */