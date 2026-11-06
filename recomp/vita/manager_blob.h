#ifndef ISAAC_MANAGER_BLOB_H
#define ISAAC_MANAGER_BLOB_H

#include <stdint.h>

/*
 * Read-only random access to one large file, for the game-skin archive
 * reader (manager_arch.c): the ARCH000 index sits at the end of a 385 MB
 * repentance.a, so the reader needs pread(offset).  This is deliberately a
 * separate interface from struct manager_storage_io (manager_storage.h),
 * which has no seek and stays untouched (the game skin adds no field to
 * any existing struct).
 *
 * open  returns 0 with *handle and *size set, or a negative value.
 * pread returns the number of bytes read (a short count is allowed; 0 means
 *       end of file) or a negative value.  The caller loops.
 * close returns 0 or a negative value.
 */
struct manager_blob_io {
    void *context;
    int (*open)(void *context, const char *path, int *handle, uint64_t *size);
    int (*pread)(void *context, int handle, void *buffer, uint32_t size,
                 uint64_t offset);
    int (*close)(void *context, int handle);
};

/* sceIoOpen / sceIoGetstatByFd / sceIoPread / sceIoClose; defined in
 * manager_blob_vita.c (Vita builds with MANAGER_UI2_GAME_SKIN only).  The
 * host tests use host_tests/fake_blob.h instead. */
extern const struct manager_blob_io manager_blob_vita_io;

#endif
