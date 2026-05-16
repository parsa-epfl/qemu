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

#ifndef QIO_CHANNEL_ZSTD_FILE_H
#define QIO_CHANNEL_ZSTD_FILE_H

#include "io/channel.h"
#include "qom/object.h"

#define TYPE_QIO_CHANNEL_ZSTD_FILE "qio-channel-zstd-file"
OBJECT_DECLARE_SIMPLE_TYPE(QIOChannelZstdFile, QIO_CHANNEL_ZSTD_FILE)


/**
 * QIOChannelZstdFile:
 *
 * The QIOChannelZstdFile object provides a channel implementation
 * that performs zstd compression or decompression on data flowing
 * to/from an underlying file.  When opened for writing, data passed
 * to io_writev is compressed and written to the file.  When opened
 * for reading, compressed data read from the file is decompressed
 * and returned via io_readv.
 */

#include <zstd.h>

struct QIOChannelZstdFile {
    QIOChannel parent;
    int fd;
    bool writing;

    ZSTD_CCtx *cctx;
    uint8_t *comp_out_buf;
    size_t comp_out_size;

    ZSTD_DCtx *dctx;
    uint8_t *raw_in_buf;
    size_t raw_in_size;
    size_t raw_in_pos;
    size_t raw_in_filled;
    uint8_t *decomp_buf;
    size_t decomp_buf_size;
    size_t decomp_buf_pos;
    size_t decomp_buf_filled;
    bool input_eof;
};


/**
 * qio_channel_zstd_file_new_output:
 * @path: the output file path
 * @errp: pointer to initialized error object
 *
 * Create a new IO channel that compresses data written to it via
 * zstd and writes the compressed output to @path.
 *
 * Returns: the new channel object, or NULL on error.
 */
QIOChannelZstdFile *
qio_channel_zstd_file_new_output(const char *path, Error **errp);

/**
 * qio_channel_zstd_file_new_input:
 * @path: the input file path (zstd-compressed)
 * @errp: pointer to initialized error object
 *
 * Create a new IO channel that reads zstd-compressed data from
 * @path and returns decompressed data via io_readv.
 *
 * Returns: the new channel object, or NULL on error.
 */
QIOChannelZstdFile *
qio_channel_zstd_file_new_input(const char *path, Error **errp);

#endif /* QIO_CHANNEL_ZSTD_FILE_H */
