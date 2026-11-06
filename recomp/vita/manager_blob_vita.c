/*
 * struct manager_blob_io over the Vita's file calls, for the game-skin
 * archive reader (manager_arch.c).  sceIoOpen, sceIoGetstatByFd and
 * sceIoPread come from SceLibKernel_stub, sceIoClose from
 * SceIofilemgr_stub; the manager links both.  The open/fstat/pread shape is
 * the one host_vita_archive_receipt.c uses on the same archives.
 */
#include "manager_blob.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <string.h>

static int manager_blob_vita_open(void *context, const char *path,
                                  int *handle, uint64_t *size)
{
    SceIoStat status;
    SceUID descriptor;
    int result;

    (void)context;
    descriptor = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (descriptor < 0)
        return descriptor;
    memset(&status, 0, sizeof(status));
    result = sceIoGetstatByFd(descriptor, &status);
    if (result < 0 || status.st_size < 0) {
        (void)sceIoClose(descriptor);
        return result < 0 ? result : -1;
    }
    *handle = descriptor;
    *size = (uint64_t)status.st_size;
    return 0;
}

static int manager_blob_vita_pread(void *context, int handle, void *buffer,
                                   uint32_t size, uint64_t offset)
{
    (void)context;
    return sceIoPread(handle, buffer, size, (SceOff)offset);
}

static int manager_blob_vita_close(void *context, int handle)
{
    int result;

    (void)context;
    result = sceIoClose(handle);
    return result < 0 ? result : 0;
}

const struct manager_blob_io manager_blob_vita_io = {
    NULL,
    manager_blob_vita_open,
    manager_blob_vita_pread,
    manager_blob_vita_close,
};
