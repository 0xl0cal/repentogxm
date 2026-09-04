#include "manager_storage_vita.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <string.h>

static enum manager_storage_node_kind manager_storage_vita_kind(SceMode mode)
{
    if (SCE_S_ISREG(mode))
        return MANAGER_STORAGE_NODE_FILE;
    if (SCE_S_ISDIR(mode))
        return MANAGER_STORAGE_NODE_DIRECTORY;
    return MANAGER_STORAGE_NODE_OTHER;
}

static int manager_storage_vita_stat(void *context, const char *path,
                                     enum manager_storage_node_kind *kind,
                                     uint64_t *size)
{
    SceIoStat status;
    int result;
    (void)context;
    memset(&status, 0, sizeof(status));
    result = sceIoGetstat(path, &status);
    /* sceIo* returns the SCE errno value directly.  Only ENOENT means that
     * callers may safely create a managed file; permission/media/I/O errors
     * must not be flattened into "absent". */
    if ((uint32_t)result == UINT32_C(0x80010002))
        return 1;
    if (result < 0)
        return -1;
    *kind = manager_storage_vita_kind(status.st_mode);
    *size = status.st_size < 0 ? 0u : (uint64_t)status.st_size;
    return 0;
}

static int manager_storage_vita_open_read(void *context, const char *path)
{
    (void)context;
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int manager_storage_vita_open_write_exclusive(void *context,
                                                     const char *path)
{
    (void)context;
    /* Vita safe-user processes still require the system permission bits. */
    return sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0777);
}

static int manager_storage_vita_read(void *context, int handle, void *data,
                                     size_t size)
{
    (void)context;
    return (int)sceIoRead(handle, data, (SceSize)size);
}

static int manager_storage_vita_write(void *context, int handle,
                                      const void *data, size_t size)
{
    (void)context;
    return (int)sceIoWrite(handle, data, (SceSize)size);
}

static int manager_storage_vita_sync_file(void *context, int handle)
{
    (void)context;
    return sceIoSyncByFd(handle, 0);
}

static int manager_storage_vita_close_file(void *context, int handle)
{
    (void)context;
    return sceIoClose(handle);
}

static int manager_storage_vita_remove_file(void *context, const char *path)
{
    (void)context;
    return sceIoRemove(path);
}

static int manager_storage_vita_rename(void *context, const char *source,
                                       const char *destination)
{
    (void)context;
    return sceIoRename(source, destination);
}

static int manager_storage_vita_make_directory(void *context, const char *path)
{
    (void)context;
    return sceIoMkdir(path, 0777);
}

static int manager_storage_vita_sync_device(void *context)
{
    (void)context;
    return sceIoSync("ux0:", 0u);
}

static int manager_storage_vita_open_directory(void *context,
                                               const char *path)
{
    (void)context;
    return sceIoDopen(path);
}

static int manager_storage_vita_read_directory(
    void *context, int handle, struct manager_storage_dir_entry *entry)
{
    SceIoDirent native;
    size_t length;
    int result;
    (void)context;
    memset(&native, 0, sizeof(native));
    result = sceIoDread(handle, &native);
    if (result <= 0)
        return result;
    length = strnlen(native.d_name, sizeof(native.d_name));
    memset(entry, 0, sizeof(*entry));
    if (length == sizeof(native.d_name) ||
        length >= sizeof(entry->name)) {
        entry->kind = MANAGER_STORAGE_NODE_OTHER;
        return 1;
    }
    memcpy(entry->name, native.d_name, length + 1u);
    entry->kind = manager_storage_vita_kind(native.d_stat.st_mode);
    entry->size = native.d_stat.st_size < 0
                      ? 0u
                      : (uint64_t)native.d_stat.st_size;
    return 1;
}

static int manager_storage_vita_close_directory(void *context, int handle)
{
    (void)context;
    return sceIoDclose(handle);
}

const struct manager_storage_io *manager_storage_vita_io(void)
{
    static const struct manager_storage_io io = {
        NULL,
        manager_storage_vita_stat,
        manager_storage_vita_open_read,
        manager_storage_vita_open_write_exclusive,
        manager_storage_vita_read,
        manager_storage_vita_write,
        manager_storage_vita_sync_file,
        manager_storage_vita_close_file,
        manager_storage_vita_remove_file,
        manager_storage_vita_rename,
        manager_storage_vita_make_directory,
        manager_storage_vita_sync_device,
        manager_storage_vita_open_directory,
        manager_storage_vita_read_directory,
        manager_storage_vita_close_directory,
    };
    return &io;
}
