/* Dense Vita import dispatch for the frozen Repentance PE IAT.
 *
 * The generated guest owns the import IDs.  Before an ID is trusted, the
 * complete generated slot/name table and every active family's local index
 * are validated without calling a handler or allocating memory. */
#include <stddef.h>
#include <string.h>

#include "host_vita_import_id.h"
#include "host_vita_gl.h"

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif
#ifndef ISAAC_VITA_LUA
#define ISAAC_VITA_LUA 0
#endif
#ifndef ISAAC_VITA_XINPUT
#define ISAAC_VITA_XINPUT 0
#endif

#if ISAAC_VITA_XINPUT
#include "host_vita_xinput.h"
#endif
#if ISAAC_VITA_LUA && defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
/* Typed Lua endpoints (host_vita_lua.h): bound per local index after the
 * registration below passed; both dispatch routes consult the one table. */
#include "host_vita_lua.h"
#define ISAAC_VITA_LUA_FAST_BINDING(kind, local, fn)                       \
    do {                                                                   \
        if ((kind) == ISAAC_VITA_IMPORT_LUA && (fn)) {                     \
            guest_import_family_fn fast_ =                                 \
                isaac_vita_lua_import_fast_binding(local);                 \
            if (fast_)                                                     \
                (fn) = fast_;                                              \
        }                                                                  \
    } while (0)
#define ISAAC_VITA_LUA_FAST_PREPARE(enable) \
    ((void)isaac_vita_lua_import_fast_prepare(enable))
#else
#define ISAAC_VITA_LUA_FAST_BINDING(kind, local, fn) ((void)0)
#define ISAAC_VITA_LUA_FAST_PREPARE(enable) ((void)0)
#endif

typedef struct isaac_vita_import_id_entry {
    uint32_t slot_rva;
    const char *name;
    uint8_t kind;
    uint8_t local_index;
} isaac_vita_import_id_entry;

#define ISAAC_VITA_IMPORT_ID_ROW(id, slot, import_name, binding_kind, local) \
    { (slot), (import_name), (uint8_t)(binding_kind), (uint8_t)(local) },
static const isaac_vita_import_id_entry s_import_ids[] = {
#include "host_vita_import_id_map.inc"
};
#undef ISAAC_VITA_IMPORT_ID_ROW

_Static_assert(sizeof s_import_ids / sizeof s_import_ids[0] ==
                   ISAAC_VITA_IMPORT_ID_COUNT,
               "Vita dense import-ID contract drifted");

int g_isaac_vita_import_ids_ready;
static const char *s_import_ids_error =
    "Vita import-ID table was not registered";

_Static_assert(ISAAC_VITA_IMPORT_SHARED_LOADER + 1U ==
                   ISAAC_VITA_IMPORT_KIND_COUNT,
               "Vita import binding-kind count drifted");

#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* ---- import-kinds view of the census (profile builds; see the header) ----
 * IDs are the strict slot-RVA sort order of the frozen PE, so they are as
 * frozen as the table; the snapshot still re-checks every name each window
 * and reports hot_ok=0 instead of a wrong split if a row ever moves. */
static const struct {
    uint16_t id;
    const char *name;
} s_import_hot_pins[ISAAC_VITA_IMPORT_HOT_COUNT] = {
    { 61U, "KERNEL32.dll!EnterCriticalSection" },
    { 60U, "KERNEL32.dll!LeaveCriticalSection" },
    { 62U, "KERNEL32.dll!TryEnterCriticalSection" },
    { 76U, "KERNEL32.dll!Sleep" },
    { 307U, "api-ms-win-crt-heap-l1-1-0.dll!malloc" },
    { 305U, "api-ms-win-crt-heap-l1-1-0.dll!free" },
    { 306U, "api-ms-win-crt-heap-l1-1-0.dll!calloc" },
    { 309U, "api-ms-win-crt-heap-l1-1-0.dll!realloc" },
    { 281U, "VCRUNTIME140.dll!memcpy" },
    { 280U, "VCRUNTIME140.dll!memset" },
    { 284U, "VCRUNTIME140.dll!memmove" },
    { 294U, "WINMM.dll!timeGetTime" },
    { 47U, "KERNEL32.dll!QueryPerformanceCounter" },
    { 313U, "api-ms-win-crt-math-l1-1-0.dll!floor" },
};

void isaac_vita_import_id_calls_snapshot(const uint32_t *calls, uint32_t count,
                                         IsaacVitaImportKindSnapshot *snapshot)
{
    uint32_t i;

    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof *snapshot);
    if (!calls)
        return;
    if (count > ISAAC_VITA_IMPORT_ID_COUNT)
        count = ISAAC_VITA_IMPORT_ID_COUNT;
    for (i = 0U; i < count; ++i) {
        uint32_t kind = s_import_ids[i].kind;

        snapshot->total += calls[i];
        if (kind < ISAAC_VITA_IMPORT_KIND_COUNT)
            snapshot->kind[kind] += calls[i];
    }
    snapshot->hot_ok = 1U;
    for (i = 0U; i < ISAAC_VITA_IMPORT_HOT_COUNT; ++i) {
        uint32_t id = s_import_hot_pins[i].id;

        if (id >= ISAAC_VITA_IMPORT_ID_COUNT ||
                strcmp(s_import_ids[id].name, s_import_hot_pins[i].name) != 0) {
            snapshot->hot_ok = 0U;
            continue;
        }
        snapshot->hot[i] = id < count ? calls[id] : 0U;
    }
}
#endif

static const char *shared_loader_name(uint32_t index)
{
    static const char *const names[] = {
        ISAAC_VITA_GL_LOAD_LIBRARY_NAME,
        ISAAC_VITA_GL_GET_PROC_NAME,
        ISAAC_VITA_GL_FREE_LIBRARY_NAME
    };

    return index < sizeof names / sizeof names[0] ? names[index] : NULL;
}

typedef const char *(*isaac_vita_import_name_fn)(uint32_t index);

typedef struct isaac_vita_import_family_inventory {
    isaac_vita_import_name_fn import_name;
    uint16_t count;
} isaac_vita_import_family_inventory;

static const isaac_vita_import_family_inventory s_family_inventories[] = {
    { isaac_vita_baseline_import_name, 5U },
    { isaac_vita_crt_import_name, 60U },
    { isaac_vita_math_import_name, 21U },
#if ISAAC_VITA_LUA
    { isaac_vita_lua_import_name, 67U },
#endif
    { isaac_vita_rtti_import_name, 1U },
    { isaac_vita_exception_import_name, 2U },
    { isaac_vita_file_lock_import_name, 2U },
    { isaac_vita_filesystem_import_name, 2U },
    { isaac_vita_find_import_name, 3U },
    { isaac_vita_com_import_name, 3U },
    { isaac_vita_post_com_import_name, 6U },
    { isaac_vita_heap_import_name, 6U },
    { isaac_vita_steam_import_name, 7U },
#if ISAAC_VITA_AUDIO
    { isaac_vita_audio_import_name, 24U },
#endif
    { isaac_vita_sync_import_name, 14U },
    { isaac_vita_memory_import_name, 6U },
    { isaac_vita_console_import_name, 2U },
    { isaac_vita_user32_import_name, 6U },
    { isaac_vita_startup_import_name, 11U },
    { isaac_vita_fls_import_name, 3U }
};

static int contract_contains_name(const char *name)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_IMPORT_ID_COUNT; ++i) {
        if (strcmp(name, s_import_ids[i].name) == 0)
            return 1;
    }
    return 0;
}

static int family_inventory_is_valid(
    const isaac_vita_import_family_inventory *inventory)
{
    uint32_t index;

    for (index = 0U; index < inventory->count; ++index) {
        const char *name = inventory->import_name(index);
        uint32_t previous;

        if (!name)
            return 0;
        if (!contract_contains_name(name))
            return 0;
        for (previous = 0U; previous < index; ++previous) {
            const char *earlier = inventory->import_name(previous);
            if (!earlier || strcmp(earlier, name) == 0)
                return 0;
        }
    }
    return inventory->import_name(inventory->count) == NULL;
}

static uint32_t family_memberships(const char *name)
{
    static const char *const gl_names[] = {
        ISAAC_VITA_GL_LOAD_LIBRARY_NAME,
        ISAAC_VITA_GL_GET_PROC_NAME,
        ISAAC_VITA_GL_FREE_LIBRARY_NAME,
        ISAAC_VITA_GL_WGL_GET_PROC_NAME
    };
    uint32_t memberships = 0U;
    size_t family;
    size_t index;

    for (family = 0U;
         family < sizeof s_family_inventories /
                      sizeof s_family_inventories[0]; ++family) {
        for (index = 0U; index < s_family_inventories[family].count;
             ++index) {
            const char *candidate =
                s_family_inventories[family].import_name((uint32_t)index);
            if (strcmp(candidate, name) == 0) {
                ++memberships;
                break;
            }
        }
    }
    for (index = 0U; index < sizeof gl_names / sizeof gl_names[0]; ++index) {
        if (strcmp(gl_names[index], name) == 0) {
            ++memberships;
            break;
        }
    }
#if ISAAC_VITA_XINPUT
    {
        static const char *const xinput_names[] = {
            ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME,
            ISAAC_VITA_XINPUT_GET_PROC_NAME,
            ISAAC_VITA_XINPUT_FREE_LIBRARY_NAME
        };
        for (index = 0U;
             index < sizeof xinput_names / sizeof xinput_names[0]; ++index) {
            if (strcmp(xinput_names[index], name) == 0) {
                ++memberships;
                break;
            }
        }
    }
#endif
    return memberships;
}

static int family_inventories_and_overlaps_are_valid(void)
{
    uint32_t overlap_count = 0U;
    uint32_t i;

    for (i = 0U;
         i < sizeof s_family_inventories /
                 sizeof s_family_inventories[0]; ++i) {
        if (!family_inventory_is_valid(&s_family_inventories[i]))
            return 0;
    }
    for (i = 0U; i < ISAAC_VITA_IMPORT_ID_COUNT; ++i) {
        const isaac_vita_import_id_entry *entry = &s_import_ids[i];
        uint32_t memberships = family_memberships(entry->name);

#if !ISAAC_VITA_AUDIO
        if (entry->kind == ISAAC_VITA_IMPORT_AUDIO) {
            if (memberships != 0U)
                return 0;
            continue;
        }
#endif
#if !ISAAC_VITA_LUA
        if (entry->kind == ISAAC_VITA_IMPORT_LUA) {
            if (memberships != 0U)
                return 0;
            continue;
        }
#endif
        if (memberships == 0U) {
            if (entry->kind != ISAAC_VITA_IMPORT_UNRESOLVED)
                return 0;
        } else if (memberships == 1U) {
            if (entry->kind == ISAAC_VITA_IMPORT_UNRESOLVED ||
                entry->kind == ISAAC_VITA_IMPORT_SHARED_LOADER)
                return 0;
        } else {
            if (entry->kind != ISAAC_VITA_IMPORT_SHARED_LOADER)
                return 0;
            ++overlap_count;
        }
    }
    return overlap_count == 3U;
}

static const char *binding_name(uint32_t kind, uint32_t index,
                                const char *contract_name)
{
    (void)contract_name;
    switch (kind) {
    case ISAAC_VITA_IMPORT_BASELINE:
        return isaac_vita_baseline_import_name(index);
    case ISAAC_VITA_IMPORT_CRT:
        return isaac_vita_crt_import_name(index);
    case ISAAC_VITA_IMPORT_MATH:
        return isaac_vita_math_import_name(index);
    case ISAAC_VITA_IMPORT_LUA:
#if ISAAC_VITA_LUA
        return isaac_vita_lua_import_name(index);
#else
        return contract_name;
#endif
    case ISAAC_VITA_IMPORT_RTTI:
        return isaac_vita_rtti_import_name(index);
    case ISAAC_VITA_IMPORT_EXCEPTION:
        return isaac_vita_exception_import_name(index);
    case ISAAC_VITA_IMPORT_FILE_LOCK:
        return isaac_vita_file_lock_import_name(index);
    case ISAAC_VITA_IMPORT_FILESYSTEM:
        return isaac_vita_filesystem_import_name(index);
    case ISAAC_VITA_IMPORT_FIND:
        return isaac_vita_find_import_name(index);
    case ISAAC_VITA_IMPORT_COM:
        return isaac_vita_com_import_name(index);
    case ISAAC_VITA_IMPORT_POST_COM:
        return isaac_vita_post_com_import_name(index);
    case ISAAC_VITA_IMPORT_HEAP:
        return isaac_vita_heap_import_name(index);
    case ISAAC_VITA_IMPORT_STEAM:
        return isaac_vita_steam_import_name(index);
    case ISAAC_VITA_IMPORT_AUDIO:
#if ISAAC_VITA_AUDIO
        return isaac_vita_audio_import_name(index);
#else
        /* Audio is deliberately unresolved in an audio-off build.  The full
         * PE row was still checked above; there is no linked local table whose
         * order could be trusted or validated in this configuration. */
        return contract_name;
#endif
    case ISAAC_VITA_IMPORT_GL_WGL:
        return isaac_vita_gl_wgl_import_name(index);
    case ISAAC_VITA_IMPORT_SYNC:
        return isaac_vita_sync_import_name(index);
    case ISAAC_VITA_IMPORT_MEMORY:
        return isaac_vita_memory_import_name(index);
    case ISAAC_VITA_IMPORT_CONSOLE:
        return isaac_vita_console_import_name(index);
    case ISAAC_VITA_IMPORT_USER32:
        return isaac_vita_user32_import_name(index);
    case ISAAC_VITA_IMPORT_STARTUP:
        return isaac_vita_startup_import_name(index);
    case ISAAC_VITA_IMPORT_FLS:
        return isaac_vita_fls_import_name(index);
    case ISAAC_VITA_IMPORT_FREAD:
        return isaac_vita_crt_import_name(index);
    case ISAAC_VITA_IMPORT_SHARED_LOADER:
        return shared_loader_name(index);
    default:
        return NULL;
    }
}

int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    uint32_t i;

    g_isaac_vita_import_ids_ready = 0;
    s_import_ids_error = "Vita import-ID table registration failed";
    ISAAC_VITA_LUA_FAST_PREPARE(0);
    if (!imports) {
        s_import_ids_error = "Vita import-ID table is null";
        return 0;
    }
    if (count != ISAAC_VITA_IMPORT_ID_COUNT) {
        s_import_ids_error = "Vita import-ID table count is not 413";
        return 0;
    }

    for (i = 0U; i < count; ++i) {
        const isaac_vita_import_id_entry *entry = &s_import_ids[i];
        const char *actual_binding;
        uint32_t j;

        if (!imports[i].name) {
            s_import_ids_error = "Vita import-ID table contains a null name";
            return 0;
        }
        if (i != 0U && imports[i - 1U].slot_rva >= imports[i].slot_rva) {
            s_import_ids_error =
                "Vita import-ID table is duplicate or out of slot order";
            return 0;
        }
        for (j = 0U; j < i; ++j) {
            if (strcmp(imports[j].name, imports[i].name) == 0) {
                s_import_ids_error =
                    "Vita import-ID table contains a duplicate name";
                return 0;
            }
        }
        if (imports[i].slot_rva != entry->slot_rva ||
            strcmp(imports[i].name, entry->name) != 0) {
            s_import_ids_error =
                "Vita import-ID table differs from the frozen PE contract";
            return 0;
        }
        if (entry->kind == ISAAC_VITA_IMPORT_UNRESOLVED)
            continue;
        actual_binding = binding_name(entry->kind, entry->local_index,
                                      entry->name);
        if (!actual_binding || strcmp(actual_binding, entry->name) != 0) {
            s_import_ids_error =
                "Vita import-ID local index/name binding drifted";
            return 0;
        }
    }

    if (!family_inventories_and_overlaps_are_valid()) {
        s_import_ids_error =
            "Vita import family inventory/name overlap drifted";
        return 0;
    }

    s_import_ids_error = NULL;
    g_isaac_vita_import_ids_ready = 1;
    ISAAC_VITA_LUA_FAST_PREPARE(1);
    return 1;
}

int isaac_vita_fread_import_indexed(CPU *__restrict c, uint32_t crt_index)
{
    int handled = isaac_vita_crt_import_indexed(
        c, crt_index, &g_host_import_calls);

    if (handled)
        isaac_vita_note_archive_fread();
    return handled;
}

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* Diagnostic only (the phase profiler's ph120.i import census); compiled
 * only with the dispatch table so the option-OFF object matches the base. */
int guest_host_import_id_kind(uint32_t import_id)
{
    if (!g_isaac_vita_import_ids_ready ||
        import_id >= ISAAC_VITA_IMPORT_ID_COUNT)
        return -1;
    return (int)s_import_ids[import_id].kind;
}
#endif

/* The two composite kinds keep their stateful semantics behind the uniform
 * family signature.  Both routes pass &g_host_import_calls, which the
 * composite endpoints already own, so the ignored count pointer is exact. */
static int fread_family_indexed(CPU *__restrict c, uint32_t index,
                                unsigned *call_count)
{
    (void)call_count;
    return isaac_vita_fread_import_indexed(c, index);
}

static int shared_loader_family_indexed(CPU *__restrict c, uint32_t index,
                                        unsigned *call_count)
{
    (void)call_count;
    return isaac_vita_shared_loader_import_indexed(c, index);
}

/* The one binding from a row's kind to the endpoint that executes it.  Both
 * guest_host_import_id() and the direct IAT table (guest.c) use this function,
 * so a generated direct site and the classified guest_call route run the same
 * code for the same ID.  NULL: unresolved, compiled out, or invalid kind. */
static guest_import_family_fn family_indexed_fn(uint32_t kind)
{
    switch (kind) {
    case ISAAC_VITA_IMPORT_BASELINE:
        return isaac_vita_baseline_import_indexed;
    case ISAAC_VITA_IMPORT_CRT:
        return isaac_vita_crt_import_indexed;
    case ISAAC_VITA_IMPORT_MATH:
        return isaac_vita_math_import_indexed;
    case ISAAC_VITA_IMPORT_LUA:
#if ISAAC_VITA_LUA
        return isaac_vita_lua_import_indexed;
#else
        return NULL;
#endif
    case ISAAC_VITA_IMPORT_RTTI:
        return isaac_vita_rtti_import_indexed;
    case ISAAC_VITA_IMPORT_EXCEPTION:
        return isaac_vita_exception_import_indexed;
    case ISAAC_VITA_IMPORT_FILE_LOCK:
        return isaac_vita_file_lock_import_indexed;
    case ISAAC_VITA_IMPORT_FILESYSTEM:
        return isaac_vita_filesystem_import_indexed;
    case ISAAC_VITA_IMPORT_FIND:
        return isaac_vita_find_import_indexed;
    case ISAAC_VITA_IMPORT_COM:
        return isaac_vita_com_import_indexed;
    case ISAAC_VITA_IMPORT_POST_COM:
        return isaac_vita_post_com_import_indexed;
    case ISAAC_VITA_IMPORT_HEAP:
        return isaac_vita_heap_import_indexed;
    case ISAAC_VITA_IMPORT_STEAM:
        return isaac_vita_steam_import_indexed;
    case ISAAC_VITA_IMPORT_AUDIO:
#if ISAAC_VITA_AUDIO
        return isaac_vita_audio_import_indexed;
#else
        return NULL;
#endif
    case ISAAC_VITA_IMPORT_GL_WGL:
        return isaac_vita_gl_wgl_import_indexed;
    case ISAAC_VITA_IMPORT_SYNC:
        return isaac_vita_sync_import_indexed;
    case ISAAC_VITA_IMPORT_MEMORY:
        return isaac_vita_memory_import_indexed;
    case ISAAC_VITA_IMPORT_CONSOLE:
        return isaac_vita_console_import_indexed;
    case ISAAC_VITA_IMPORT_USER32:
        return isaac_vita_user32_import_indexed;
    case ISAAC_VITA_IMPORT_STARTUP:
        return isaac_vita_startup_import_indexed;
    case ISAAC_VITA_IMPORT_FLS:
        return isaac_vita_fls_import_indexed;
    case ISAAC_VITA_IMPORT_FREAD:
        return fread_family_indexed;
    case ISAAC_VITA_IMPORT_SHARED_LOADER:
        return shared_loader_family_indexed;
    default:
        return NULL;
    }
}

int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    const isaac_vita_import_id_entry *entry;
    guest_import_family_fn fn;

    if (!g_isaac_vita_import_ids_ready) {
        if (!s_import_ids_error)
            s_import_ids_error = "Vita import-ID table is not ready";
        return -1;
    }
    if (import_id >= ISAAC_VITA_IMPORT_ID_COUNT) {
        s_import_ids_error = "Vita import ID is out of range";
        return -1;
    }

    entry = &s_import_ids[import_id];
    if (entry->kind == ISAAC_VITA_IMPORT_UNRESOLVED)
        return 0;
    fn = family_indexed_fn(entry->kind);
    if (!fn) {
        switch (entry->kind) {
        case ISAAC_VITA_IMPORT_LUA:
#if !ISAAC_VITA_LUA && defined(ISAAC_VITA_LUA_OFF_CLOSE_NOOP)
            /* Lua is compiled out and the generated startup bypass never
             * created a state, but the game's shutdown still calls lua_close
             * on the zeroed LuaEngine member.  Device logs
             * (perf:bundle35-raster720-v1 at 201.549 s) show every controlled
             * exit ending as "guest: FAULT ... Lua5.3.3r.dll!lua_close" with
             * process result 1 instead of the game's own ExitProcess.
             * Closing a state that never existed is a no-op: cdecl, void
             * result, pop only the return address. */
            if (entry->name &&
                    strcmp(entry->name, "Lua5.3.3r.dll!lua_close") == 0) {
                ++g_host_import_calls;
                (void)gpop(c);
                return 1;
            }
#endif
            return 0;
        case ISAAC_VITA_IMPORT_AUDIO:
            /* Audio is deliberately unresolved in an audio-off build. */
            return 0;
        default:
            s_import_ids_error = "Vita import-ID binding kind is invalid";
            return -1;
        }
    }
    ISAAC_VITA_LUA_FAST_BINDING(entry->kind, entry->local_index, fn);
    return fn(c, entry->local_index, &g_host_import_calls);
}

int guest_host_import_id_direct_binding(uint32_t import_id,
                                        guest_import_family_fn *fn,
                                        uint32_t *local_index)
{
    const isaac_vita_import_id_entry *entry;
    guest_import_family_fn bound;

    if (!g_isaac_vita_import_ids_ready ||
        import_id >= ISAAC_VITA_IMPORT_ID_COUNT || !fn || !local_index)
        return 0;
    entry = &s_import_ids[import_id];
    if (entry->kind == ISAAC_VITA_IMPORT_UNRESOLVED)
        return 0;
    bound = family_indexed_fn(entry->kind);
    if (!bound)
        return 0;
    ISAAC_VITA_LUA_FAST_BINDING(entry->kind, entry->local_index, bound);
    *fn = bound;
    *local_index = entry->local_index;
    return 1;
}

const char *guest_host_import_id_error(void)
{
    return s_import_ids_error ? s_import_ids_error :
        "Vita import-ID dispatch failed";
}
