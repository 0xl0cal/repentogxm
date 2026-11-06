/*
 * In-memory struct manager_blob_io for the game-skin host tests
 * (manager_arch_host_test).  Blobs are registered by path over caller-owned
 * bytes; fault injection: refuse opening one path, fail the Nth pread,
 * return 0 (early EOF) on the Nth pread, cap every pread at N bytes (short
 * reads the reader must loop over), fail close.  Counts opens, preads,
 * bytes and open handles so a test can assert that nothing leaks.
 */
#ifndef ISAAC_MANAGER_FAKE_BLOB_H
#define ISAAC_MANAGER_FAKE_BLOB_H

#include "manager_blob.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_BLOB_MAX_FILES 64
#define FAKE_BLOB_MAX_HANDLES 16
#define FAKE_BLOB_PATH_CAPACITY 256

struct fake_blob_file {
    char path[FAKE_BLOB_PATH_CAPACITY];
    const uint8_t *data;
    uint64_t size;
};

struct fake_blob {
    struct fake_blob_file files[FAKE_BLOB_MAX_FILES];
    unsigned file_count;
    int handle_file[FAKE_BLOB_MAX_HANDLES]; /* -1 = free */
    /* statistics */
    unsigned opens;
    unsigned preads;
    uint64_t bytes_read;
    int open_handles;
    /* fault injection (0 / NULL = off) */
    const char *refuse_open_path;
    unsigned fail_pread_at;  /* 1-based pread number returning -1 */
    unsigned eof_pread_at;   /* 1-based pread number returning 0 */
    uint32_t short_read_cap; /* every pread returns at most this */
    int fail_close;
};

static inline void fake_blob_init(struct fake_blob *fake)
{
    int i;

    memset(fake, 0, sizeof(*fake));
    for (i = 0; i < FAKE_BLOB_MAX_HANDLES; ++i)
        fake->handle_file[i] = -1;
}

static inline int fake_blob_add(struct fake_blob *fake, const char *path,
                         const uint8_t *data, uint64_t size)
{
    struct fake_blob_file *file;

    if (fake->file_count >= FAKE_BLOB_MAX_FILES ||
        strlen(path) >= FAKE_BLOB_PATH_CAPACITY)
        return -1;
    file = &fake->files[fake->file_count++];
    snprintf(file->path, sizeof(file->path), "%s", path);
    file->data = data;
    file->size = size;
    return 0;
}

static inline int fake_blob_open(void *context, const char *path, int *handle,
                          uint64_t *size)
{
    struct fake_blob *fake = (struct fake_blob *)context;
    unsigned file;
    int slot;

    ++fake->opens;
    if (fake->refuse_open_path != NULL &&
        strcmp(fake->refuse_open_path, path) == 0)
        return -2;
    for (file = 0; file < fake->file_count; ++file) {
        if (strcmp(fake->files[file].path, path) == 0)
            break;
    }
    if (file == fake->file_count)
        return -3; /* absent */
    for (slot = 0; slot < FAKE_BLOB_MAX_HANDLES; ++slot) {
        if (fake->handle_file[slot] < 0)
            break;
    }
    if (slot == FAKE_BLOB_MAX_HANDLES)
        return -4;
    fake->handle_file[slot] = (int)file;
    ++fake->open_handles;
    *handle = slot;
    *size = fake->files[file].size;
    return 0;
}

static inline int fake_blob_pread(void *context, int handle, void *buffer,
                           uint32_t size, uint64_t offset)
{
    struct fake_blob *fake = (struct fake_blob *)context;
    const struct fake_blob_file *file;
    uint64_t available;
    uint32_t count;

    ++fake->preads;
    if (handle < 0 || handle >= FAKE_BLOB_MAX_HANDLES ||
        fake->handle_file[handle] < 0)
        return -5;
    if (fake->fail_pread_at != 0u && fake->preads == fake->fail_pread_at)
        return -6;
    if (fake->eof_pread_at != 0u && fake->preads == fake->eof_pread_at)
        return 0;
    file = &fake->files[fake->handle_file[handle]];
    if (offset >= file->size)
        return 0;
    available = file->size - offset;
    count = size;
    if ((uint64_t)count > available)
        count = (uint32_t)available;
    if (fake->short_read_cap != 0u && count > fake->short_read_cap)
        count = fake->short_read_cap;
    memcpy(buffer, file->data + offset, count);
    fake->bytes_read += count;
    return (int)count;
}

static inline int fake_blob_close(void *context, int handle)
{
    struct fake_blob *fake = (struct fake_blob *)context;

    if (handle < 0 || handle >= FAKE_BLOB_MAX_HANDLES ||
        fake->handle_file[handle] < 0)
        return -5;
    fake->handle_file[handle] = -1;
    --fake->open_handles;
    return fake->fail_close ? -7 : 0;
}

static inline struct manager_blob_io fake_blob_io(struct fake_blob *fake)
{
    struct manager_blob_io io;

    io.context = fake;
    io.open = fake_blob_open;
    io.pread = fake_blob_pread;
    io.close = fake_blob_close;
    return io;
}

/* Whole host file into a malloc'd buffer (NULL on failure). */
static inline uint8_t *fake_blob_load_file(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    uint8_t *data;
    long length;

    if (stream == NULL)
        return NULL;
    if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
        fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)length + 1u);
    if (data == NULL ||
        fread(data, 1, (size_t)length, stream) != (size_t)length) {
        free(data);
        fclose(stream);
        return NULL;
    }
    fclose(stream);
    *size = (size_t)length;
    return data;
}

#endif
