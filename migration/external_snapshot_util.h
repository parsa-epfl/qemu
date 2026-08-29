#ifndef EXTERNAL_SNAPSHOT_UTIL_H
#define EXTERNAL_SNAPSHOT_UTIL_H

#include "io/channel-zstd-file.h"

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

static QEMUFile *qemu_file_open_zstd_output(const char *filename, Error **errp)
{
    QIOChannelZstdFile *ioc = qio_channel_zstd_file_new_output(filename, errp);
    if (!ioc) {
        return NULL;
    }
    qio_channel_set_name(QIO_CHANNEL(ioc), "snapshot-zstd");
    return qemu_file_new_output(QIO_CHANNEL(ioc));
}

static QEMUFile *qemu_file_open_zstd_input(const char *filename, Error **errp)
{
    QIOChannelZstdFile *ioc = qio_channel_zstd_file_new_input(filename, errp);
    if (!ioc) {
        return NULL;
    }
    qio_channel_set_name(QIO_CHANNEL(ioc), "load-snapshot-zstd");
    return qemu_file_new_input(QIO_CHANNEL(ioc));
}

#endif /* EXTERNAL_SNAPSHOT_UTIL_H */
