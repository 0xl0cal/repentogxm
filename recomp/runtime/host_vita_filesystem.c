/* Evidence-bounded Vita adapters for the complete narrow UCRT filesystem DLL. */
#include <errno.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "host_vita_crt.h"
#include "host_vita_filesystem.h"
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
#include "host_vita_async_write.h"
#endif
#include "host_vita_import_id.h"
#include "host_vita_startup.h"
#if defined(ISAAC_VITA_LUA_RECEIPT)
#include "platform.h"
#endif

typedef void (*vita_filesystem_handler)(CPU *__restrict c);

typedef struct vita_filesystem_import_entry {
    const char *name;
    vita_filesystem_handler handler;
} vita_filesystem_import_entry;

static uint32_t vita_filesystem_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_filesystem_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

/* UCRT remove documents ENOENT for a missing path and EACCES for a directory,
 * read-only/open file, or another access denial.  Vita's native error space is
 * slightly wider; collapse only the equivalent proven cases and keep every
 * unexpected result loud. */
static int vita_filesystem_remove_errno(int native_error)
{
    switch (native_error) {
    case ENOENT:
    case ENOTDIR:
        return ENOENT;
    case EACCES:
    case EPERM:
    case EBUSY:
    case EISDIR:
    case EROFS:
        return EACCES;
    default:
        return 0;
    }
}

/* sceIo* returns the encoded SCE errno directly instead of assigning errno.
 * Keep this list as narrow as the UCRT outcomes already proven below. */
#define VITA_FILESYSTEM_SCE_EPERM   UINT32_C(0x80010001)
#define VITA_FILESYSTEM_SCE_ENOENT  UINT32_C(0x80010002)
#define VITA_FILESYSTEM_SCE_EACCES  UINT32_C(0x8001000d)
#define VITA_FILESYSTEM_SCE_ENOTDIR UINT32_C(0x80010014)
#define VITA_FILESYSTEM_SCE_EROFS   UINT32_C(0x8001001e)
/* Malformed or over-long paths.  Measured on device 2026-09-03: the EID mod
 * probes a path with backslashes when a bomb is placed and sceIoGetstat answers
 * SCE EINVAL (0x80010016); UCRT _access reports such paths as ENOENT. */
#define VITA_FILESYSTEM_SCE_EINVAL  UINT32_C(0x80010016)
#define VITA_FILESYSTEM_SCE_ENAMETOOLONG UINT32_C(0x80010024)

static int vita_filesystem_access_errno(int native_error)
{
    switch ((uint32_t)native_error) {
    case VITA_FILESYSTEM_SCE_ENOENT:
    case VITA_FILESYSTEM_SCE_ENOTDIR:
    case VITA_FILESYSTEM_SCE_EINVAL:
    case VITA_FILESYSTEM_SCE_ENAMETOOLONG:
        return ENOENT;
    case VITA_FILESYSTEM_SCE_EACCES:
    case VITA_FILESYSTEM_SCE_EPERM:
    case VITA_FILESYSTEM_SCE_EROFS:
        return EACCES;
    default:
        return 0;
    }
}

static void vita_filesystem_remove(CPU *__restrict c)
{
    uint32_t guest_path = vita_filesystem_arg(c, 0U);
    char native_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    int saved_errno;
    int native_error;
    int guest_error;
    int result;

    if (!isaac_vita_startup_map_path(
            c, guest_path, native_path, sizeof native_path))
        return;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* A queued save image for this path must land before it is removed. */
    isaac_vita_async_write_sync_path(native_path);
#endif

    saved_errno = errno;
    errno = 0;
    /* Do not call newlib remove(): its Vita implementation retries EISDIR with
     * rmdir(), while the UCRT operation must never delete a directory.  unlink
     * reaches sceIoRemove and keeps this a file-only boundary. */
    result = unlink(native_path);
    native_error = errno;
    errno = saved_errno;
    if (result != 0) {
        guest_error = vita_filesystem_remove_errno(native_error);
        if (!guest_error) {
            guest_fault(c, (uint32_t)native_error,
                        "remove produced an unproven native filesystem error");
            return;
        }
        g_isaac_vita_crt_errno = guest_error;
        result = -1;
    }
    c->eax = (uint32_t)result;
    vita_filesystem_cdecl_return(c);
}

static void vita_filesystem_access(CPU *__restrict c)
{
    uint32_t guest_path = vita_filesystem_arg(c, 0U);
    uint32_t mode = vita_filesystem_arg(c, 1U);
    char native_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    SceIoStat status;
    int native_error;
    int guest_error;
    int result;

    if ((mode & ~ISAAC_VITA_FILESYSTEM_ACCESS_MODE_MASK) != 0U) {
        g_isaac_vita_crt_errno = EINVAL;
        c->eax = UINT32_MAX;
        vita_filesystem_cdecl_return(c);
        return;
    }
    if (!isaac_vita_startup_map_path(
            c, guest_path, native_path, sizeof native_path))
        return;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* Existence checks must observe a save whose image is still queued. */
    isaac_vita_async_write_sync_path(native_path);
#endif

    /* Vita newlib access() calls stat(), whose _stat_r() first allocates a
     * PATH_MAX realpath buffer.  At the Repentance Lua/mod startup frontier
     * that hidden allocation can fail with ENOMEM even though the path is
     * valid.  Query the same native metadata directly: this is allocation-
     * free and reproduces newlib access()'s directory/write-bit semantics. */
    memset(&status, 0, sizeof status);
    result = sceIoGetstat(native_path, &status);
    native_error = result;
    if (result < 0) {
        guest_error = vita_filesystem_access_errno(native_error);
        if (!guest_error) {
            /* An unlisted native code must not kill a running game: a mod's
             * path probe is not worth the process.  Report it once (bounded)
             * and answer the UCRT default for an unresolvable path. */
            static unsigned s_unproven_access_reports;
            if (s_unproven_access_reports < 8U) {
                ++s_unproven_access_reports;
                sceClibPrintf("[kage-vita] _access unproven native error "
                              "0x%08x for %s (answered ENOENT)\n",
                              (unsigned)native_error, native_path);
            }
            guest_error = ENOENT;
        }
        g_isaac_vita_crt_errno = guest_error;
        result = -1;
    } else if ((mode & ISAAC_VITA_FILESYSTEM_ACCESS_WRITE) != 0U &&
               !SCE_S_ISDIR(status.st_mode) &&
               (status.st_mode & SCE_S_IWUSR) == 0U) {
        g_isaac_vita_crt_errno = EACCES;
        result = -1;
    } else {
        result = 0;
    }
#if defined(ISAAC_VITA_LUA_RECEIPT)
    /* The frozen mod loader decides "disable.it"/"update.it"/"main.lua"
     * presence through this exact import (Resource probe 0x00563120 ->
     * _access).  Bounded receipt for mod-tree probes only. */
    {
        static unsigned s_mod_probe_receipts;
        if (strstr(native_path, "/mods/") && s_mod_probe_receipts < 48U) {
            ++s_mod_probe_receipts;
            isaac_vita_log("[isaac-lua] access path=%.200s rc=%d native=0x%08x",
                           native_path, result, (unsigned)native_error);
        }
    }
#endif
    c->eax = (uint32_t)result;
    vita_filesystem_cdecl_return(c);
}

#define ISAAC_VITA_FILESYSTEM_TABLE_ENTRY(                         \
    id, name, iat_rva, call_count, call_hash)                      \
    { name, vita_filesystem_##id },
static const vita_filesystem_import_entry s_vita_filesystem_imports[] = {
    ISAAC_VITA_FILESYSTEM_IMPORTS(ISAAC_VITA_FILESYSTEM_TABLE_ENTRY)
};
#undef ISAAC_VITA_FILESYSTEM_TABLE_ENTRY

_Static_assert(sizeof s_vita_filesystem_imports /
                   sizeof s_vita_filesystem_imports[0] ==
                   ISAAC_VITA_FILESYSTEM_IMPORT_COUNT,
               "Vita filesystem import inventory drifted");
_Static_assert((ISAAC_VITA_FILESYSTEM_LAST_IAT_RVA -
                ISAAC_VITA_FILESYSTEM_FIRST_IAT_RVA) / 4U + 1U ==
                   ISAAC_VITA_FILESYSTEM_IMPORT_COUNT,
               "Vita filesystem IAT block is no longer contiguous");
_Static_assert(ISAAC_VITA_FILESYSTEM_REMOVE_CALL_COUNT +
                   ISAAC_VITA_FILESYSTEM_ACCESS_CALL_COUNT ==
                   ISAAC_VITA_FILESYSTEM_PHYSICAL_CALL_COUNT,
               "Vita filesystem physical-call census drifted");

const char *isaac_vita_filesystem_import_name(uint32_t index)
{
    return index < ISAAC_VITA_FILESYSTEM_IMPORT_COUNT
        ? s_vita_filesystem_imports[index].name : NULL;
}

int isaac_vita_filesystem_import_indexed(CPU *__restrict c, uint32_t index,
                                         unsigned *call_count)
{
    if (index >= ISAAC_VITA_FILESYSTEM_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_filesystem_imports[index].handler(c);
    return 1;
}

static int vita_filesystem_dispatch(CPU *__restrict c, const char *name,
                                    unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_FILESYSTEM_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_filesystem_imports[index].name) == 0)
            return isaac_vita_filesystem_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_filesystem_import(CPU *__restrict c, const char *name)
{
    return vita_filesystem_dispatch(c, name, NULL);
}

int isaac_vita_filesystem_import_counted(CPU *__restrict c, const char *name,
                                         unsigned *call_count)
{
    return vita_filesystem_dispatch(c, name, call_count);
}
