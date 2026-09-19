/*
 * QEMU I/O channels zstd file driver
 *
 * Copyright (c) 2024
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "io/channel-zstd-file.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "trace.h"

static const bool g_generate_raw_file = false;

static void qio_channel_zstd_file_init(Object *obj)
{
    QIOChannelZstdFile *ioc = QIO_CHANNEL_ZSTD_FILE(obj);
    ioc->fd = -1;
    ioc->tee_fd = -1;
    ioc->writing = false;
    ioc->cctx = NULL;
    ioc->comp_out_buf = NULL;
    ioc->comp_out_size = 0;
    ioc->dctx = NULL;
    ioc->raw_in_buf = NULL;
    ioc->raw_in_size = 0;
    ioc->raw_in_pos = 0;
    ioc->raw_in_filled = 0;
    ioc->decomp_buf = NULL;
    ioc->decomp_buf_size = 0;
    ioc->decomp_buf_pos = 0;
    ioc->decomp_buf_filled = 0;
    ioc->input_eof = false;
}

static void qio_channel_zstd_file_finalize(Object *obj)
{
    QIOChannelZstdFile *ioc = QIO_CHANNEL_ZSTD_FILE(obj);

    if (ioc->cctx) {
        ZSTD_freeCCtx(ioc->cctx);
        ioc->cctx = NULL;
    }
    g_free(ioc->comp_out_buf);
    ioc->comp_out_buf = NULL;
    ioc->comp_out_size = 0;

    if (ioc->dctx) {
        ZSTD_freeDCtx(ioc->dctx);
        ioc->dctx = NULL;
    }
    g_free(ioc->raw_in_buf);
    ioc->raw_in_buf = NULL;
    ioc->raw_in_size = 0;
    g_free(ioc->decomp_buf);
    ioc->decomp_buf = NULL;
    ioc->decomp_buf_size = 0;

    if (ioc->tee_fd != -1) {
        qemu_close(ioc->tee_fd);
        ioc->tee_fd = -1;
    }

    if (ioc->fd != -1) {
        qemu_close(ioc->fd);
        ioc->fd = -1;
    }
}

static ssize_t qio_channel_zstd_file_writev(QIOChannel *ioc,
                                             const struct iovec *iov,
                                             size_t niov,
                                             int *fds,
                                             size_t nfds,
                                             int flags,
                                             Error **errp)
{
    QIOChannelZstdFile *zioc = QIO_CHANNEL_ZSTD_FILE(ioc);

    for (size_t i = 0; i < niov; i++) {
        if (g_generate_raw_file && zioc->tee_fd != -1) {
            size_t offset = 0;
            do {
                ssize_t written = write(zioc->tee_fd,
                                        (uint8_t *)iov[i].iov_base + offset,
                                        iov[i].iov_len - offset);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    error_setg_errno(errp, errno, "Failed to write tee data");
                    return -1;
                }
                offset += (size_t)written;
            } while (offset < iov[i].iov_len);
        }

        ZSTD_inBuffer z_in = {
            .src = iov[i].iov_base,
            .size = iov[i].iov_len,
            .pos = 0,
        };

        while (z_in.pos < z_in.size) {
            ZSTD_outBuffer z_out = {
                .dst = zioc->comp_out_buf,
                .size = zioc->comp_out_size,
                .pos = 0,
            };

            size_t ret = ZSTD_compressStream2(zioc->cctx, &z_out, &z_in,
                                               ZSTD_e_continue);
            if (ZSTD_isError(ret)) {
                error_setg(errp, "zstd compressStream2 error: %s",
                           ZSTD_getErrorName(ret));
                return -1;
            }

            if (z_out.pos > 0) {
                size_t offset = 0;
                do {
                    ssize_t written = write(zioc->fd,
                                            zioc->comp_out_buf + offset,
                                            z_out.pos - offset);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        error_setg_errno(errp, errno, "Failed to write compressed data");
                        return -1;
                    }
                    offset += (size_t)written;
                } while (offset < z_out.pos);
            }
        }
    }

    return iov_size(iov, niov);
}

static ssize_t qio_channel_zstd_file_readv(QIOChannel *ioc,
                                            const struct iovec *iov,
                                            size_t niov,
                                            int **fds,
                                            size_t *nfds,
                                            int flags,
                                            Error **errp)
{
    QIOChannelZstdFile *zioc = QIO_CHANNEL_ZSTD_FILE(ioc);
    ssize_t total_copied = 0;

    for (size_t i = 0; i < niov; i++) {
        uint8_t *dst = (uint8_t *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;

        while (remaining > 0) {
            if (zioc->decomp_buf_pos < zioc->decomp_buf_filled) {
                size_t avail = zioc->decomp_buf_filled - zioc->decomp_buf_pos;
                size_t take = MIN(remaining, avail);
                memcpy(dst, zioc->decomp_buf + zioc->decomp_buf_pos, take);
                dst += take;
                remaining -= take;
                zioc->decomp_buf_pos += take;
                total_copied += take;
                continue;
            }

            if (zioc->input_eof) {
                return total_copied > 0 ? total_copied : 0;
            }

            ZSTD_inBuffer z_in = {
                .src = zioc->raw_in_buf + zioc->raw_in_pos,
                .size = zioc->raw_in_filled - zioc->raw_in_pos,
                .pos = 0,
            };
            ZSTD_outBuffer z_out = {
                .dst = zioc->decomp_buf,
                .size = zioc->decomp_buf_size,
                .pos = 0,
            };

            size_t ret = ZSTD_decompressStream(zioc->dctx, &z_out, &z_in);
            if (ZSTD_isError(ret)) {
                error_setg(errp, "zstd decompressStream error: %s",
                           ZSTD_getErrorName(ret));
                return -1;
            }
            zioc->raw_in_pos += z_in.pos;

            if (z_out.pos > 0) {
                zioc->decomp_buf_pos = 0;
                zioc->decomp_buf_filled = z_out.pos;
                continue;
            }

            if (zioc->raw_in_pos >= zioc->raw_in_filled) {
                ssize_t n = read(zioc->fd, zioc->raw_in_buf,
                                 zioc->raw_in_size);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return total_copied > 0 ? total_copied
                                                : QIO_CHANNEL_ERR_BLOCK;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    error_setg_errno(errp, errno,
                                     "Failed to read compressed data");
                    return -1;
                }
                if (n == 0) {
                    zioc->input_eof = true;
                    continue;
                }
                zioc->raw_in_pos = 0;
                zioc->raw_in_filled = (size_t)n;
                continue;
            }
        }
    }

    return total_copied;
}

static int qio_channel_zstd_file_set_blocking(QIOChannel *ioc,
                                               bool enabled,
                                               Error **errp)
{
    QIOChannelZstdFile *zioc = QIO_CHANNEL_ZSTD_FILE(ioc);

    if (!g_unix_set_fd_nonblocking(zioc->fd, !enabled, NULL)) {
        error_setg_errno(errp, errno, "Failed to set FD nonblocking");
        return -1;
    }
    return 0;
}

static int qio_channel_zstd_file_close(QIOChannel *ioc,
                                        Error **errp)
{
    QIOChannelZstdFile *zioc = QIO_CHANNEL_ZSTD_FILE(ioc);

    if (zioc->writing && zioc->cctx && zioc->fd != -1) {
        ZSTD_inBuffer z_in = { .src = NULL, .size = 0, .pos = 0 };
        for (;;) {
            ZSTD_outBuffer z_out = {
                .dst = zioc->comp_out_buf,
                .size = zioc->comp_out_size,
                .pos = 0,
            };
            size_t remaining = ZSTD_compressStream2(zioc->cctx, &z_out, &z_in,
                                                     ZSTD_e_end);
            if (z_out.pos > 0) {
                size_t offset = 0;
                do {
                    ssize_t written = write(zioc->fd,
                                            zioc->comp_out_buf + offset,
                                            z_out.pos - offset);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        error_setg_errno(errp, errno,
                                         "Failed to write final compressed data");
                        return -1;
                    }
                    offset += (size_t)written;
                } while (offset < z_out.pos);
            }
            if (remaining == 0) {
                break;
            }
            if (ZSTD_isError(remaining)) {
                error_setg(errp, "zstd endStream error: %s",
                           ZSTD_getErrorName(remaining));
                return -1;
            }
        }
    }

    if (zioc->tee_fd != -1) {
        qemu_close(zioc->tee_fd);
        zioc->tee_fd = -1;
    }

    if (zioc->fd != -1) {
        if (qemu_close(zioc->fd) < 0) {
            error_setg_errno(errp, errno, "Unable to close file");
            zioc->fd = -1;
            return -1;
        }
        zioc->fd = -1;
    }
    return 0;
}

static void qio_channel_zstd_file_class_init(ObjectClass *klass,
                                              void *class_data G_GNUC_UNUSED)
{
    QIOChannelClass *ioc_klass = QIO_CHANNEL_CLASS(klass);

    ioc_klass->io_writev = qio_channel_zstd_file_writev;
    ioc_klass->io_readv = qio_channel_zstd_file_readv;
    ioc_klass->io_set_blocking = qio_channel_zstd_file_set_blocking;
    ioc_klass->io_close = qio_channel_zstd_file_close;
}

static const TypeInfo qio_channel_zstd_file_info = {
    .parent = TYPE_QIO_CHANNEL,
    .name = TYPE_QIO_CHANNEL_ZSTD_FILE,
    .instance_size = sizeof(QIOChannelZstdFile),
    .instance_init = qio_channel_zstd_file_init,
    .instance_finalize = qio_channel_zstd_file_finalize,
    .class_init = qio_channel_zstd_file_class_init,
};

static void qio_channel_zstd_file_register_types(void)
{
    type_register_static(&qio_channel_zstd_file_info);
}

type_init(qio_channel_zstd_file_register_types);

QIOChannelZstdFile *
qio_channel_zstd_file_new_output(const char *path, Error **errp)
{
    QIOChannelZstdFile *ioc;

    ioc = QIO_CHANNEL_ZSTD_FILE(object_new(TYPE_QIO_CHANNEL_ZSTD_FILE));
    ioc->fd = qemu_open_old(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ioc->fd < 0) {
        object_unref(OBJECT(ioc));
        error_setg_errno(errp, errno, "Unable to open %s for writing", path);
        return NULL;
    }
    ioc->writing = true;

    if (g_generate_raw_file) {
        char *raw_path = g_strdup(path);
        char *dot = strstr(raw_path, ".zstd");
        if (dot) {
            memcpy(dot, ".raw", 4);
        }
        ioc->tee_fd = qemu_open_old(raw_path,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ioc->tee_fd < 0) {
            error_report("failed to open raw debug tee file %s: %s",
                         raw_path, strerror(errno));
        }
        g_free(raw_path);
    }

    ioc->cctx = ZSTD_createCCtx();
    if (!ioc->cctx) {
        object_unref(OBJECT(ioc));
        error_setg(errp, "Failed to create zstd compression context");
        return NULL;
    }

    ioc->comp_out_size = ZSTD_CStreamOutSize();
    ioc->comp_out_buf = g_malloc(ioc->comp_out_size);

    return ioc;
}

QIOChannelZstdFile *
qio_channel_zstd_file_new_input(const char *path, Error **errp)
{
    QIOChannelZstdFile *ioc;

    ioc = QIO_CHANNEL_ZSTD_FILE(object_new(TYPE_QIO_CHANNEL_ZSTD_FILE));
    ioc->fd = qemu_open_old(path, O_RDONLY | O_BINARY, 0);
    if (ioc->fd < 0) {
        object_unref(OBJECT(ioc));
        error_setg_errno(errp, errno, "Unable to open %s for reading", path);
        return NULL;
    }
    ioc->writing = false;

    ioc->dctx = ZSTD_createDCtx();
    if (!ioc->dctx) {
        object_unref(OBJECT(ioc));
        error_setg(errp, "Failed to create zstd decompression context");
        return NULL;
    }

    ioc->raw_in_size = ZSTD_DStreamInSize();
    ioc->raw_in_buf = g_malloc(ioc->raw_in_size);
    ioc->raw_in_pos = 0;
    ioc->raw_in_filled = 0;

    ioc->decomp_buf_size = ZSTD_DStreamOutSize();
    ioc->decomp_buf = g_malloc(ioc->decomp_buf_size);
    ioc->decomp_buf_pos = 0;
    ioc->decomp_buf_filled = 0;

    return ioc;
}
