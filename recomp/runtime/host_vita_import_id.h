#ifndef ISAAC_HOST_VITA_IMPORT_ID_H
#define ISAAC_HOST_VITA_IMPORT_ID_H

#include <stdint.h>

#include "guest.h"

/* The dense ID is the index of the exact, slot-RVA-sorted PE import table.
 * This contract is deliberately frozen outside gen_all.py: registration
 * validates every slot and name before the first ID can be dispatched. */
#define ISAAC_VITA_IMPORT_ID_COUNT 413U

/* Copied from the exact rows in host_vita_import_id_map.inc.  The focused
 * dispatch oracle checks all six values against that source-of-truth map;
 * keeping them named prevents guest.c from recalling positional constants. */
#define ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS    60U
#define ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS    61U
#define ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS  6U
#define ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS  5U
#define ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS 0x006060f8U
#define ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS 0x006060fcU

enum isaac_vita_import_binding_kind {
    ISAAC_VITA_IMPORT_UNRESOLVED = 0,
    ISAAC_VITA_IMPORT_BASELINE,
    ISAAC_VITA_IMPORT_CRT,
    ISAAC_VITA_IMPORT_MATH,
    ISAAC_VITA_IMPORT_LUA,
    ISAAC_VITA_IMPORT_RTTI,
    ISAAC_VITA_IMPORT_EXCEPTION,
    ISAAC_VITA_IMPORT_FILE_LOCK,
    ISAAC_VITA_IMPORT_FILESYSTEM,
    ISAAC_VITA_IMPORT_FIND,
    ISAAC_VITA_IMPORT_COM,
    ISAAC_VITA_IMPORT_POST_COM,
    ISAAC_VITA_IMPORT_HEAP,
    ISAAC_VITA_IMPORT_STEAM,
    ISAAC_VITA_IMPORT_AUDIO,
    ISAAC_VITA_IMPORT_GL_WGL,
    ISAAC_VITA_IMPORT_SYNC,
    ISAAC_VITA_IMPORT_MEMORY,
    ISAAC_VITA_IMPORT_CONSOLE,
    ISAAC_VITA_IMPORT_USER32,
    ISAAC_VITA_IMPORT_STARTUP,
    ISAAC_VITA_IMPORT_FLS,
    ISAAC_VITA_IMPORT_FREAD,
    ISAAC_VITA_IMPORT_SHARED_LOADER
};

/* Registration is allocation-free and repeatable.  It fails closed unless
 * all 413 slot/name pairs exactly match the checked-in frozen-PE contract. */
int guest_host_import_ids_register(const guest_import *imports, uint32_t count);
int guest_host_import_id(CPU *__restrict c, uint32_t import_id);
const char *guest_host_import_id_error(void);
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* Binding family (enum isaac_vita_import_binding_kind) of a dense ID once the
 * table is ready, else -1.  Diagnostic only (phase-profile import census);
 * exists only in the dispatch-table owners' compilation. */
int guest_host_import_id_kind(uint32_t import_id);
#endif

/* Direct IAT dispatch (guest.c, ISAAC_VITA_IMPORT_DIRECT).  After a successful
 * registration this returns non-zero together with the exact family endpoint
 * and local index guest_host_import_id() calls for the same ID -- both come
 * from one binding function, so the routes cannot disagree.  Zero for
 * unresolved rows and for families compiled out of this build; those keep
 * guest_call's original classification, faults and special cases. */
int guest_host_import_id_direct_binding(uint32_t import_id,
                                        guest_import_family_fn *fn,
                                        uint32_t *local_index);

/* Read-only outside host_vita_import_id.c.  A direct frozen-IAT seam may use
 * the indexed family entry only after the complete 413-row table and local
 * binding inventory have passed the same registration contract as
 * guest_host_import_id(). */
extern int g_isaac_vita_import_ids_ready;

/* Number of binding kinds (the enum above, UNRESOLVED..SHARED_LOADER). */
#define ISAAC_VITA_IMPORT_KIND_COUNT 24U

#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* Import-kinds view of the phase profiler's per-import census (ph120.ik /
 * ph120.ih).  The census itself is guest.h's g_guest_phase_profile_import_calls
 * (one bounded indexed increment per host import call, recorded by guest.c
 * where the dense ID is already known: guest_call's import branch,
 * guest_try_direct_sync_import_call, guest_import_call and
 * guest_note_authenticated_import_call);
 * this module only knows the frozen table, so it sums a caller-supplied
 * per-ID array per binding kind and copies the fourteen pinned hot imports.
 * hot_ok is 0 when a pinned ID's frozen-table name drifted (then the hot
 * values are meaningless and the reader must fall back to the kind totals). */
enum isaac_vita_import_hot {
    ISAAC_VITA_IMPORT_HOT_ENTER_CS = 0,   /* KERNEL32 EnterCriticalSection */
    ISAAC_VITA_IMPORT_HOT_LEAVE_CS,       /* KERNEL32 LeaveCriticalSection */
    ISAAC_VITA_IMPORT_HOT_TRY_ENTER_CS,   /* KERNEL32 TryEnterCriticalSection */
    ISAAC_VITA_IMPORT_HOT_SLEEP,          /* KERNEL32 Sleep */
    ISAAC_VITA_IMPORT_HOT_MALLOC,         /* crt-heap malloc */
    ISAAC_VITA_IMPORT_HOT_FREE,           /* crt-heap free */
    ISAAC_VITA_IMPORT_HOT_CALLOC,         /* crt-heap calloc */
    ISAAC_VITA_IMPORT_HOT_REALLOC,        /* crt-heap realloc */
    ISAAC_VITA_IMPORT_HOT_MEMCPY,         /* VCRUNTIME140 memcpy */
    ISAAC_VITA_IMPORT_HOT_MEMSET,         /* VCRUNTIME140 memset */
    ISAAC_VITA_IMPORT_HOT_MEMMOVE,        /* VCRUNTIME140 memmove */
    ISAAC_VITA_IMPORT_HOT_TIME_GET_TIME,  /* WINMM timeGetTime */
    ISAAC_VITA_IMPORT_HOT_QPC,            /* KERNEL32 QueryPerformanceCounter */
    ISAAC_VITA_IMPORT_HOT_FLOOR,          /* crt-math floor */
    ISAAC_VITA_IMPORT_HOT_COUNT
};

typedef struct IsaacVitaImportKindSnapshot {
    uint32_t total;                              /* sum over all IDs */
    uint32_t kind[ISAAC_VITA_IMPORT_KIND_COUNT]; /* by binding kind */
    uint32_t hot[ISAAC_VITA_IMPORT_HOT_COUNT];   /* pinned hot IDs */
    uint32_t hot_ok;                             /* pinned names verified */
} IsaacVitaImportKindSnapshot;

/* `calls` holds one count per dense import ID (`count` entries; IDs at or
 * beyond `count` contribute nothing).  The frozen table supplies the binding
 * kind and the hot-pin names. */
void isaac_vita_import_id_calls_snapshot(const uint32_t *calls, uint32_t count,
                                         IsaacVitaImportKindSnapshot *snapshot);
#endif

/* Direct, bounds-checked entry points.  Name dispatchers in the owning module
 * call the same indexed function after their compatibility lookup, so tests of
 * the old API and the production ID path execute one implementation. */
int isaac_vita_baseline_import_indexed(CPU *__restrict c, uint32_t index,
                                       unsigned *call_count);
const char *isaac_vita_baseline_import_name(uint32_t index);
int isaac_vita_crt_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count);
const char *isaac_vita_crt_import_name(uint32_t index);
int isaac_vita_math_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count);
const char *isaac_vita_math_import_name(uint32_t index);
int isaac_vita_lua_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count);
const char *isaac_vita_lua_import_name(uint32_t index);
int isaac_vita_rtti_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count);
const char *isaac_vita_rtti_import_name(uint32_t index);
int isaac_vita_exception_import_indexed(CPU *__restrict c, uint32_t index,
                                        unsigned *call_count);
const char *isaac_vita_exception_import_name(uint32_t index);
int isaac_vita_file_lock_import_indexed(CPU *__restrict c, uint32_t index,
                                        unsigned *call_count);
const char *isaac_vita_file_lock_import_name(uint32_t index);
int isaac_vita_filesystem_import_indexed(CPU *__restrict c, uint32_t index,
                                         unsigned *call_count);
const char *isaac_vita_filesystem_import_name(uint32_t index);
int isaac_vita_find_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count);
const char *isaac_vita_find_import_name(uint32_t index);
int isaac_vita_com_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count);
const char *isaac_vita_com_import_name(uint32_t index);
int isaac_vita_post_com_import_indexed(CPU *__restrict c, uint32_t index,
                                       unsigned *call_count);
const char *isaac_vita_post_com_import_name(uint32_t index);
int isaac_vita_heap_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count);
const char *isaac_vita_heap_import_name(uint32_t index);
int isaac_vita_steam_import_indexed(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count);
const char *isaac_vita_steam_import_name(uint32_t index);
int isaac_vita_audio_import_indexed(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count);
const char *isaac_vita_audio_import_name(uint32_t index);
int isaac_vita_gl_wgl_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count);
const char *isaac_vita_gl_wgl_import_name(uint32_t index);
int isaac_vita_sync_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count);
const char *isaac_vita_sync_import_name(uint32_t index);
int isaac_vita_memory_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count);
const char *isaac_vita_memory_import_name(uint32_t index);
int isaac_vita_console_import_indexed(CPU *__restrict c, uint32_t index,
                                      unsigned *call_count);
const char *isaac_vita_console_import_name(uint32_t index);
int isaac_vita_user32_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count);
const char *isaac_vita_user32_import_name(uint32_t index);
int isaac_vita_startup_import_indexed(CPU *__restrict c, uint32_t index,
                                      unsigned *call_count);
const char *isaac_vita_startup_import_name(uint32_t index);
int isaac_vita_fls_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count);
const char *isaac_vita_fls_import_name(uint32_t index);

/* These two preserve stateful/composite semantics which are not represented
 * by a bare final function pointer. */
int isaac_vita_fread_import_indexed(CPU *__restrict c, uint32_t crt_index);
void isaac_vita_note_archive_fread(void);
/* Start one process-local loading/profile epoch at zero.  The backend calls
 * this before it starts either the independent watchdog or the loading owner;
 * dispatch itself remains single-writer and keeps the hot counter plain. */
void isaac_vita_reset_archive_fread_epoch(void);
int isaac_vita_shared_loader_import_indexed(CPU *__restrict c,
                                            uint32_t shared_index);

#endif
