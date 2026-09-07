/* Frozen x86 Lua 5.3.3 import ABI -> native vanilla Lua 5.3.3.
 *
 * Lua state and data pointers are intentionally not tokenised: both the
 * frozen PE and Vita are 32-bit, and the guest image/heap use identity
 * addresses.  Native C callbacks are different.  A guest function address is
 * stored in hidden upvalue #1 and every closure enters one common trampoline,
 * which calls the normal generated-function dispatcher (`guest_call`).
 */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "host_vita_heap.h"
#include "host_vita_lua.h"
#include "host_vita_startup.h"
#include "kage_vita_deep_profile.h"
#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* Sampler builds: a Lua error longjmps over guest_call's publish/restore
 * wrapper (guest.c), so each callback frame keeps the enclosing indirect
 * target and the unwind routes below restore it.  Diagnostic only. */
#include "kage_vita_guest_sampler.h"
#endif
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP) || defined(ISAAC_VITA_LUA_GC_PROFILE)
/* Floor-transition GCCOLLECT clamp and per-window GC profile: the lua_gc
 * import routes through host_vita_lua_gc.c (see that file). */
#include "host_vita_lua_gc.h"
#define VITA_LUA_GC_HOOK 1
#endif

#if defined(ISAAC_VITA_LUA_RECEIPT) || \
    (defined(ISAAC_VITA_LUA_ARENA_MB) && ISAAC_VITA_LUA_ARENA_MB > 0)
#include "platform.h"
#endif
#if defined(ISAAC_VITA_LUA_RECEIPT)
#if defined(__vita__)
#include <psp2/io/stat.h>
#else
#include <sys/stat.h>
#endif
#endif
#if defined(__vita__) && defined(ISAAC_VITA_LUA_ARENA_MB) && \
    ISAAC_VITA_LUA_ARENA_MB > 0
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#define VITA_LUA_ARENA_ENABLED 1
#endif
#if defined(__vita__) && defined(ISAAC_VITA_LUA_RECEIPT)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#define VITA_LUA_BEAT_ENABLED 1
#endif

#define VITA_LUA_MAX_STATES 16U
#define VITA_LUA_MAX_CALLBACK_DEPTH 24U
#define VITA_LUA_MAX_REQUIRE_DEPTH 8U
#define VITA_LUA_ERROR_TEXT_MAX 255U
#define VITA_LUA_CALLBACK_RETURN UINT32_C(0xfff15333)

/* ISAAC_VITA_LUA_SCOPE_FASTPATH (default 1; the CMake option of the same name
 * passes =0): behaviour-identical fixed-overhead trims on the per-import and
 * per-callback paths -- the re-entrant scope_enter reads the owner slot before
 * it tries the CAS, vita_lua_callback_frame_push initialises only the fields
 * that are read before they are written, and vita_lua_call_guest drops its
 * acquire load / post-return range re-check (both proven redundant at the
 * site).  0 compiles the pre-knob code byte-for-byte (compile-time escape
 * hatch for bisecting; see recomp/test_vita_lua_scope.py). */
#if !defined(ISAAC_VITA_LUA_SCOPE_FASTPATH)
#define ISAAC_VITA_LUA_SCOPE_FASTPATH 1
#endif

_Static_assert(sizeof(void *) == 4U,
               "the frozen Lua ABI requires a 32-bit native target");
_Static_assert(sizeof(lua_Integer) == 8U,
               "Lua 5.3.3 lua_Integer ABI drifted");
_Static_assert(sizeof(lua_Number) == 8U,
               "Lua 5.3.3 lua_Number ABI drifted");
_Static_assert(sizeof(lua_KContext) == 4U,
               "the frozen x86 lua_KContext ABI requires 32 bits");

typedef void (*vita_lua_handler)(CPU *__restrict c);

typedef struct vita_lua_import_entry {
    const char *name;
    vita_lua_handler handler;
} vita_lua_import_entry;

typedef struct vita_lua_scope {
    CPU *cpu;
    int root;
} vita_lua_scope;

typedef struct vita_lua_allocator {
    CPU *cpu;
    uint32_t target;
    uint32_t guest_ud;
    lua_State *state;
    int used;
} vita_lua_allocator;

typedef struct vita_lua_state_record {
    lua_State *state;
    vita_lua_allocator *allocator;
    uint32_t panic_target;
    int used;
} vita_lua_state_record;

enum vita_lua_callback_error_kind {
    VITA_LUA_CALLBACK_ERROR_NONE = 0,
    VITA_LUA_CALLBACK_ERROR_LUAL_ERROR,
    VITA_LUA_CALLBACK_ERROR_ARGERROR,
    /* luaL_error whose formatted message is already the top of the Lua
     * stack (no native copy, no length cap); the trampoline prefixes
     * luaL_where and raises it. */
    VITA_LUA_CALLBACK_ERROR_LUAL_ERROR_STACK
};

typedef struct vita_lua_callback_frame {
    CPU *cpu;
    lua_State *state;
    uint32_t saved_esp;
    uint32_t saved_ebx;
    uint32_t saved_ebp;
    uint32_t saved_esi;
    uint32_t saved_edi;
    int previous;
    int error_kind;
    int error_argument;
    char error_text[VITA_LUA_ERROR_TEXT_MAX + 1U];
    jmp_buf escape;
    int used;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* g_kage_guest_last_indirect_target when this callback was entered. */
    uint32_t sampler_enclosing_target;
#endif
} vita_lua_callback_frame;

typedef struct vita_lua_require_frame {
    CPU *cpu;
    uint32_t target;
    int previous;
    int used;
} vita_lua_require_frame;

static uintptr_t s_vita_lua_active_cpu;
static vita_lua_allocator s_vita_lua_allocators[VITA_LUA_MAX_STATES];
static vita_lua_state_record s_vita_lua_states[VITA_LUA_MAX_STATES];
static vita_lua_callback_frame
    s_vita_lua_callbacks[VITA_LUA_MAX_CALLBACK_DEPTH];
static vita_lua_require_frame
    s_vita_lua_requires[VITA_LUA_MAX_REQUIRE_DEPTH];
static int s_vita_lua_callback_top = -1;
static int s_vita_lua_require_top = -1;

static uintptr_t vita_lua_active_load(void)
{
#if defined(_MSC_VER)
    return (uintptr_t)_InterlockedCompareExchangePointer(
        (void *volatile *)&s_vita_lua_active_cpu, NULL, NULL);
#elif defined(__GNUC__)
    return __atomic_load_n(&s_vita_lua_active_cpu, __ATOMIC_ACQUIRE);
#else
    return s_vita_lua_active_cpu;
#endif
}

#if ISAAC_VITA_LUA_SCOPE_FASTPATH
/* Plain (relaxed) read of the owner slot.  Sufficient only for the question
 * "is the slot already mine?": the value (uintptr_t)c is stored by exactly
 * one writer, the CAS in vita_lua_scope_enter executed by the thread that
 * runs CPU c (scope_leave / abort_cpu store 0, a foreign CPU stores its own
 * pointer), so a load on c's thread that returns c observes c's own earlier
 * store in program order and no barrier is needed.  Any other value takes
 * the acquire/CAS path unchanged. */
static uintptr_t vita_lua_active_load_relaxed(void)
{
#if defined(_MSC_VER)
    return *(uintptr_t volatile *)&s_vita_lua_active_cpu;
#elif defined(__GNUC__)
    return __atomic_load_n(&s_vita_lua_active_cpu, __ATOMIC_RELAXED);
#else
    return s_vita_lua_active_cpu;
#endif
}
#endif

static int vita_lua_active_compare_exchange(uintptr_t *expected,
                                            uintptr_t desired)
{
#if defined(_MSC_VER)
    uintptr_t observed = (uintptr_t)_InterlockedCompareExchangePointer(
        (void *volatile *)&s_vita_lua_active_cpu,
        (void *)desired, (void *)*expected);
    if (observed == *expected)
        return 1;
    *expected = observed;
    return 0;
#elif defined(__GNUC__)
    return __atomic_compare_exchange_n(
        &s_vita_lua_active_cpu, expected, desired, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#else
    if (s_vita_lua_active_cpu != *expected) {
        *expected = s_vita_lua_active_cpu;
        return 0;
    }
    s_vita_lua_active_cpu = desired;
    return 1;
#endif
}

/* Re-entry by the same translated CPU is required: a native Lua call can
 * invoke a guest CFunction, which can immediately call another Lua import.
 * Only the outermost activation owns/releases the process-wide slot. */
static int vita_lua_scope_enter(CPU *c, vita_lua_scope *scope)
{
    uintptr_t expected = 0U;
    uintptr_t active;

    scope->cpu = c;
    scope->root = 0;
#if ISAAC_VITA_LUA_SCOPE_FASTPATH
    /* Nearly every import is nested inside the outer lua_pcallk scope of the
     * same CPU.  The old path paid a failing CAS (dmb + ldrex + dmb on
     * Cortex-A9) only to land in the `active == c` branch below; a relaxed
     * load that returns c is the same observation (only c's own thread
     * stores c, and it has not stored 0 yet in program order), so the
     * outcome -- re-entrant, root=0 -- is identical for every interleaving.
     * 0 or a foreign owner falls through to the CAS exactly as before. */
    if (vita_lua_active_load_relaxed() == (uintptr_t)c)
        return 1;
#endif
    if (vita_lua_active_compare_exchange(&expected, (uintptr_t)c)) {
        scope->root = 1;
        return 1;
    }
    active = expected;
    if (active == (uintptr_t)c)
        return 1;
    guest_fault(c, 0U, "concurrent Lua bridge entry by another CPU");
    return 0;
}

static void vita_lua_scope_leave(vita_lua_scope *scope)
{
    uintptr_t expected;

    if (!scope->root)
        return;
    expected = (uintptr_t)scope->cpu;
    if (!vita_lua_active_compare_exchange(&expected, 0U))
        guest_fault(scope->cpu, 0U, "Lua bridge owner changed during call");
    scope->root = 0;
}

static uint32_t vita_lua_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static uint64_t vita_lua_arg_u64(CPU *__restrict c, uint32_t index)
{
    return (uint64_t)vita_lua_arg(c, index) |
           ((uint64_t)vita_lua_arg(c, index + 1U) << 32U);
}

static double vita_lua_arg_number(CPU *__restrict c, uint32_t index)
{
    uint64_t bits = vita_lua_arg_u64(c, index);
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static lua_State *vita_lua_arg_state(CPU *__restrict c)
{
    return (lua_State *)(uintptr_t)vita_lua_arg(c, 0U);
}

static void vita_lua_return(CPU *__restrict c)
{
    (void)gpop(c);
}

static void vita_lua_return_pointer(CPU *__restrict c, const void *pointer)
{
    c->eax = (uint32_t)(uintptr_t)pointer;
    vita_lua_return(c);
}

static void vita_lua_return_integer(CPU *__restrict c, lua_Integer value)
{
    uint64_t bits = (uint64_t)value;
    c->eax = (uint32_t)bits;
    c->edx = (uint32_t)(bits >> 32U);
    vita_lua_return(c);
}

/* Native upvalue #1 is bridge-private.  Registry and ordinary stack indices
 * are unchanged; only lua_upvalueindex(n), which is below REGISTRYINDEX,
 * moves down by one slot so guest upvalue #1 remains guest upvalue #1. */
static int vita_lua_index(int index)
{
    return index < LUA_REGISTRYINDEX ? index - 1 : index;
}

static int vita_lua_index_result(int index)
{
    return index < LUA_REGISTRYINDEX ? index + 1 : index;
}

static vita_lua_state_record *vita_lua_find_state(lua_State *state)
{
    uint32_t i;
    for (i = 0U; i < VITA_LUA_MAX_STATES; ++i)
        if (s_vita_lua_states[i].used &&
            s_vita_lua_states[i].state == state)
            return &s_vita_lua_states[i];
    return NULL;
}

static vita_lua_state_record *vita_lua_add_state(
    lua_State *state, vita_lua_allocator *allocator)
{
    uint32_t i;
    if (!state)
        return NULL;
    for (i = 0U; i < VITA_LUA_MAX_STATES; ++i) {
        if (!s_vita_lua_states[i].used) {
            s_vita_lua_states[i].used = 1;
            s_vita_lua_states[i].state = state;
            s_vita_lua_states[i].allocator = allocator;
            s_vita_lua_states[i].panic_target = 0U;
            return &s_vita_lua_states[i];
        }
    }
    return NULL;
}

static vita_lua_allocator *vita_lua_add_allocator(
    CPU *c, uint32_t target, uint32_t guest_ud)
{
    uint32_t i;
    for (i = 0U; i < VITA_LUA_MAX_STATES; ++i) {
        if (!s_vita_lua_allocators[i].used) {
            s_vita_lua_allocators[i].used = 1;
            s_vita_lua_allocators[i].cpu = c;
            s_vita_lua_allocators[i].target = target;
            s_vita_lua_allocators[i].guest_ud = guest_ud;
            s_vita_lua_allocators[i].state = NULL;
            return &s_vita_lua_allocators[i];
        }
    }
    return NULL;
}

static void vita_lua_restore_callback_cpu(vita_lua_callback_frame *frame)
{
    CPU *c = frame->cpu;
    (void)guest_stack_set(c, frame->saved_esp, 0U);
    c->ebx = frame->saved_ebx;
    c->ebp = frame->saved_ebp;
    c->esi = frame->saved_esi;
    c->edi = frame->saved_edi;
}

static int vita_lua_callback_frame_push(CPU *c, lua_State *state)
{
    uint32_t i;
    for (i = 0U; i < VITA_LUA_MAX_CALLBACK_DEPTH; ++i) {
        vita_lua_callback_frame *frame = &s_vita_lua_callbacks[i];
        if (!frame->used) {
#if ISAAC_VITA_LUA_SCOPE_FASTPATH
            /* Field-by-field instead of memset(frame, 0, 392 bytes): every
             * field read before it is written is still initialised here.
             *   cpu/state/saved_esp..saved_edi/previous/used: assigned below.
             *   error_kind: read by vita_lua_call_guest after the callback
             *     (must be NONE unless an error path stored it) -> zeroed.
             *   error_argument: read only together with error_kind ==
             *     ARGERROR, which stores it first (escape_callback) -> zeroed
             *     anyway so the fault/cdecl paths read a defined value.
             *   error_text: read (memcpy 256) only when error_kind != NONE;
             *     LUAL_ERROR/ARGERROR paths store a NUL-terminated string
             *     first, LUAL_ERROR_STACK never uses the copy -> [0] zeroed so
             *     the unused copy is the same empty C string as before.
             *   escape: written by setjmp before any longjmp can target it
             *     (push -> setjmp with no Lua call in between).
             *   sampler_enclosing_target (sampler builds): stored by the
             *     caller right after push, before recover/restore read it. */
            frame->error_kind = VITA_LUA_CALLBACK_ERROR_NONE;
            frame->error_argument = 0;
            frame->error_text[0] = '\0';
#else
            memset(frame, 0, sizeof *frame);
#endif
            frame->used = 1;
            frame->cpu = c;
            frame->state = state;
            frame->saved_esp = c->esp;
            frame->saved_ebx = c->ebx;
            frame->saved_ebp = c->ebp;
            frame->saved_esi = c->esi;
            frame->saved_edi = c->edi;
            frame->previous = s_vita_lua_callback_top;
            s_vita_lua_callback_top = (int)i;
            return (int)i;
        }
    }
    return -1;
}

static void vita_lua_callback_frame_pop(int slot)
{
    vita_lua_callback_frame *frame;
    if (slot < 0 || (uint32_t)slot >= VITA_LUA_MAX_CALLBACK_DEPTH)
        return;
    frame = &s_vita_lua_callbacks[slot];
    s_vita_lua_callback_top = frame->previous;
    frame->used = 0;
}

/* A Lua API such as luaL_checkinteger can raise directly, bypassing our
 * private error request and the native callback trampoline.  The nearest
 * native lua_pcall still returns normally.  Its entry mark distinguishes
 * callback frames abandoned by that Lua longjmp from an outer live callback. */
static int vita_lua_recover_callbacks(CPU *c, int entry_mark)
{
    int recovered = 0;
    while (s_vita_lua_callback_top != entry_mark) {
        int slot = s_vita_lua_callback_top;
        vita_lua_callback_frame *frame;
        if (slot < 0 || (uint32_t)slot >= VITA_LUA_MAX_CALLBACK_DEPTH) {
            guest_fault(c, 0U, "Lua callback recovery chain is corrupt");
            return 0;
        }
        frame = &s_vita_lua_callbacks[slot];
        if (!frame->used || frame->cpu != c) {
            guest_fault(c, 0U, "Lua callback recovery owner mismatch");
            return 0;
        }
        vita_lua_restore_callback_cpu(frame);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        /* Frames pop innermost first, so the outermost abandoned callback's
         * enclosing target is the value left standing. */
        g_kage_guest_last_indirect_target = frame->sampler_enclosing_target;
#endif
        vita_lua_callback_frame_pop(slot);
        recovered = 1;
    }
    (void)recovered;
    return 1;
}

static void vita_lua_copy_error_text(char output[VITA_LUA_ERROR_TEXT_MAX + 1U],
                                     uint32_t guest_text)
{
    uint32_t i;
    if (!guest_text) {
        memcpy(output, "(null)", 7U);
        return;
    }
    for (i = 0U; i < VITA_LUA_ERROR_TEXT_MAX; ++i) {
        output[i] = (char)ld8(guest_text + i);
        if (!output[i])
            return;
    }
    output[VITA_LUA_ERROR_TEXT_MAX] = '\0';
}

/* luaL_error/luaL_argerror must not invoke Lua's longjmp while translated C
 * activations are above the native callback.  Record the request, restore the
 * x86 callback boundary, and first jump to our own native trampoline frame.
 * The trampoline removes its bridge record and only then raises into Lua. */
static int vita_lua_escape_callback(CPU *c, int kind, int argument,
                                    uint32_t guest_text)
{
    vita_lua_callback_frame *frame;
    int slot = s_vita_lua_callback_top;
    if (slot < 0 || (uint32_t)slot >= VITA_LUA_MAX_CALLBACK_DEPTH) {
        guest_fault(c, guest_text,
                    "Lua error escaped without a native callback boundary");
        return 0;
    }
    frame = &s_vita_lua_callbacks[slot];
    if (!frame->used || frame->cpu != c) {
        guest_fault(c, guest_text, "Lua callback error owner mismatch");
        return 0;
    }
    frame->error_kind = kind;
    frame->error_argument = argument;
    vita_lua_copy_error_text(frame->error_text, guest_text);
    vita_lua_restore_callback_cpu(frame);
    longjmp(frame->escape, 1);
}

/* Device receipts (ISAAC_VITA_LUA_RECEIPT).  The game log never names a mod
 * chunk that was skipped before RunScript, so the bridge itself reports every
 * chunk it loads plus every protected call that returns an error.  Bounded:
 * one line per event up to VITA_LUA_RECEIPT_MAX, then a single cap line. */
#if defined(ISAAC_VITA_LUA_RECEIPT)
#define VITA_LUA_RECEIPT_MAX 96U
static unsigned s_vita_lua_receipts;

static int vita_lua_receipt_take(void)
{
    if (s_vita_lua_receipts < VITA_LUA_RECEIPT_MAX) {
        ++s_vita_lua_receipts;
        return 1;
    }
    if (s_vita_lua_receipts++ == VITA_LUA_RECEIPT_MAX)
        isaac_vita_log("[isaac-lua] receipt cap reached (%u lines)",
                       (unsigned)VITA_LUA_RECEIPT_MAX);
    return 0;
}

static const char *vita_lua_receipt_error(lua_State *state, int status)
{
    const char *text;
    if (status == LUA_OK || lua_gettop(state) < 1 ||
        lua_type(state, -1) != LUA_TSTRING)
        return "-";
    text = lua_tostring(state, -1);
    return text ? text : "-";
}

static void vita_lua_receipt_load(lua_State *state, const char *kind,
                                  const char *name, size_t bytes, int status)
{
    if (!vita_lua_receipt_take())
        return;
    isaac_vita_log(
        "[isaac-lua] load kind=%s path=%.160s bytes=%u status=%d mem=%dKB "
        "err=%.120s",
        kind, name ? name : "(null)", (unsigned)bytes, status,
        lua_gc(state, LUA_GCCOUNT, 0), vita_lua_receipt_error(state, status));
}

/* Vita newlib stat() first mallocs a PATH_MAX realpath buffer, which is
 * exactly what fails at the exhausted-heap frontier this receipt is meant to
 * expose; query the metadata allocation-free like host_vita_filesystem.c. */
static size_t vita_lua_receipt_file_bytes(const char *native_path)
{
#if defined(__vita__)
    SceIoStat information;
    if (!native_path)
        return 0U;
    memset(&information, 0, sizeof information);
    if (sceIoGetstat(native_path, &information) < 0)
        return 0U;
    return (size_t)information.st_size;
#else
    struct stat information;
    if (!native_path || stat(native_path, &information) != 0)
        return 0U;
    return (size_t)information.st_size;
#endif
}

static void vita_lua_receipt_pcall(lua_State *state, int status)
{
    if (status == LUA_OK || !vita_lua_receipt_take())
        return;
    isaac_vita_log("[isaac-lua] pcall status=%d mem=%dKB err=%.200s",
                   status, lua_gc(state, LUA_GCCOUNT, 0),
                   vita_lua_receipt_error(state, status));
}
#endif

/* Dedicated Lua arena (ISAAC_VITA_LUA_ARENA_MB).
 *
 * Measured on device (bundles 15/23/24/25, EID enabled): the 81 MiB guest heap
 * is exhausted about 27 s after launch, the 8 MiB overflow mspace takes over
 * (heapovf first-use), the next archive fopen fails with ENOMEM (arcdiag
 * err=12) and the game dereferences the NULL stream.  Vanilla runs never touch
 * the overflow pool.  The frozen LuaEngine allocator (sub_003fca80) is a plain
 * realloc/free, so serving the LuaEngine state from a private USER_RW mspace
 * is transparent to the guest.  The block is reserved before vitaGL init,
 * which otherwise claims every USER_RW byte above its 16 MiB threshold (KAGE
 * VITA MEM: vgl RAM pool 200 MiB, 163 MiB free at init).  Exhausting the
 * arena falls back to the guest allocator per block, so the only failure mode
 * is the status quo. */
#if defined(VITA_LUA_ARENA_ENABLED)
#define VITA_LUA_ARENA_BYTES ((SceSize)ISAAC_VITA_LUA_ARENA_MB * 1024U * 1024U)

typedef struct vita_lua_arena {
    SceUID uid;
    uintptr_t base;
    uintptr_t end;
    SceClibMspace mspace;
    int ready;
    size_t live_bytes;
    size_t peak_bytes;
    unsigned fallbacks;
} vita_lua_arena;

static vita_lua_arena s_vita_lua_arena;

int isaac_vita_lua_arena_reserve(void)
{
    void *base = NULL;
    SceUID uid;
    if (s_vita_lua_arena.ready)
        return 1;
    uid = sceKernelAllocMemBlock("isaac_lua_arena",
                                 SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                 VITA_LUA_ARENA_BYTES, NULL);
    if (uid < 0) {
        isaac_vita_log("[isaac-lua] arena reserve FAILED: alloc=0x%08x bytes=%u",
                       (unsigned)uid, (unsigned)VITA_LUA_ARENA_BYTES);
        return 0;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        isaac_vita_log("[isaac-lua] arena reserve FAILED: no base");
        return 0;
    }
    s_vita_lua_arena.mspace = sceClibMspaceCreate(base, VITA_LUA_ARENA_BYTES);
    if (!s_vita_lua_arena.mspace) {
        (void)sceKernelFreeMemBlock(uid);
        isaac_vita_log("[isaac-lua] arena reserve FAILED: mspace");
        return 0;
    }
    s_vita_lua_arena.uid = uid;
    s_vita_lua_arena.base = (uintptr_t)base;
    s_vita_lua_arena.end = (uintptr_t)base + VITA_LUA_ARENA_BYTES;
    s_vita_lua_arena.ready = 1;
    isaac_vita_log("[isaac-lua] arena reserve: status=ready base=0x%08x bytes=%u",
                   (unsigned)s_vita_lua_arena.base,
                   (unsigned)VITA_LUA_ARENA_BYTES);
    return 1;
}

static int vita_lua_arena_contains(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return s_vita_lua_arena.ready && address >= s_vita_lua_arena.base &&
           address < s_vita_lua_arena.end;
}

static void vita_lua_arena_account(size_t old_size, size_t new_size)
{
    s_vita_lua_arena.live_bytes += new_size;
    s_vita_lua_arena.live_bytes -= old_size;
    if (s_vita_lua_arena.live_bytes > s_vita_lua_arena.peak_bytes)
        s_vita_lua_arena.peak_bytes = s_vita_lua_arena.live_bytes;
}

static void *vita_lua_guest_allocator(void *opaque, void *pointer,
                                      size_t old_size, size_t new_size);
static void *vita_lua_native_allocator(void *opaque, void *pointer,
                                       size_t old_size, size_t new_size);

/* The frozen LuaEngine::Init creates its state through luaL_newstate (the
 * device receipts show no lua_newstate call), whose bridge state carries a
 * null opaque and the bounded native guest-heap allocator.  Spill to whichever
 * allocator the state would have used without the arena. */
static void *vita_lua_arena_fallback(void *opaque, void *pointer,
                                     size_t old_size, size_t new_size)
{
    if (opaque)
        return vita_lua_guest_allocator(opaque, pointer, old_size, new_size);
    return vita_lua_native_allocator(opaque, pointer, old_size, new_size);
}

/* Lua 5.3 allocator contract: (ptr,0) frees, (NULL,n) allocates, (ptr,n)
 * reallocates; old_size is exact for a live block.  Blocks that spilled to the
 * guest heap stay there; arena blocks that cannot grow in place migrate. */
static void *vita_lua_arena_allocator(void *opaque, void *pointer,
                                      size_t old_size, size_t new_size)
{
    void *result;
    if (pointer && !vita_lua_arena_contains(pointer))
        return vita_lua_arena_fallback(opaque, pointer, old_size, new_size);
    if (!new_size) {
        if (pointer) {
            sceClibMspaceFree(s_vita_lua_arena.mspace, pointer);
            vita_lua_arena_account(old_size, 0U);
        }
        return NULL;
    }
    if (pointer)
        result = sceClibMspaceRealloc(s_vita_lua_arena.mspace, pointer,
                                      (SceSize)new_size);
    else
        result = sceClibMspaceMalloc(s_vita_lua_arena.mspace,
                                     (SceSize)new_size);
    if (result) {
        vita_lua_arena_account(pointer ? old_size : 0U, new_size);
        return result;
    }
    ++s_vita_lua_arena.fallbacks;
    result = vita_lua_arena_fallback(opaque, NULL, 0U, new_size);
    if (result && pointer) {
        memcpy(result, pointer, old_size < new_size ? old_size : new_size);
        sceClibMspaceFree(s_vita_lua_arena.mspace, pointer);
        vita_lua_arena_account(old_size, 0U);
    }
    return result;
}
#else
int isaac_vita_lua_arena_reserve(void)
{
    return 0;
}
#endif

#if defined(ISAAC_VITA_LUA_GC_PROFILE) || defined(ISAAC_VITA_HEAP_CENSUS)
/* GC profile (host_vita_lua_gc.c): arena occupancy for the window line.
 * ISAAC_VITA_HEAP_CENSUS: the same three numbers as lua(l,pk,fb) of
 * ph120.mem (kage_vita_phase_profile.c), read outside every lock. */
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks)
{
#if defined(VITA_LUA_ARENA_ENABLED)
    *live_kb = (uint32_t)(s_vita_lua_arena.live_bytes >> 10);
    *peak_kb = (uint32_t)(s_vita_lua_arena.peak_bytes >> 10);
    *fallbacks = (uint32_t)s_vita_lua_arena.fallbacks;
#else
    *live_kb = 0U;
    *peak_kb = 0U;
    *fallbacks = 0U;
#endif
}
#endif

/* Startup beat (ISAAC_VITA_LUA_RECEIPT, device only).
 *
 * Measured 2026-09-02: every LUA=ON eboot of the wf/integration base stops
 * logging 13-16 s after launch, between the ANM2 scratch sessions and the
 * texture uploads, with no guest fault, no coredump and the game log batch
 * still unflushed; LUA=OFF builds of the same base boot.  Nothing in that
 * window logs, so the stall could not be placed.  This watchdog thread starts
 * at the first Lua import (the exact point where a LUA=ON run diverges from
 * LUA=OFF) and, while the loading presenter is active, prints one bounded
 * line every few seconds: the host import counter and the Lua import counter
 * with their deltas (frozen deltas = a stall, moving = slow startup), the
 * last Lua import, whether the guest is inside a Lua import or a Lua->guest
 * callback right now, and a sample of the guest stack: the return-address
 * RVAs nearest ESP, i.e. the translated functions the guest is stuck in.  It
 * never dereferences guest heap memory, never takes a lock and stops itself
 * two beats after the presenter finishes or after VITA_LUA_BEAT_MAX_LINES. */
#if defined(VITA_LUA_BEAT_ENABLED)
#define VITA_LUA_BEAT_MAX_LINES  20U
#define VITA_LUA_BEAT_FIRST_US   UINT64_C(6000000)
#define VITA_LUA_BEAT_PERIOD_US  UINT64_C(4000000)
#define VITA_LUA_BEAT_POLL_US    250000U
#define VITA_LUA_BEAT_STACK_SCAN 96U
#define VITA_LUA_BEAT_RETURNS    8U
/* Frozen PE: .text ends below the IAT (first slot 0x6060f8); anything at or
 * above that is a data pointer, not a translated return address. */
#define VITA_LUA_BEAT_TEXT_END_RVA UINT32_C(0x00606000)

/* The loading presenter is a KAGE-only owner; resolve it weakly so a LUA
 * receipt build without KAGE still links and reports loading=-1. */
__attribute__((weak)) int kage_vita_loading_active(void);
__attribute__((weak)) unsigned kage_vita_loading_stage(void);
__attribute__((weak)) unsigned kage_vita_loading_elapsed_seconds(void);
__attribute__((weak)) unsigned kage_vita_loading_swap_count(void);

static CPU *s_vita_lua_beat_cpu;
static uint32_t s_vita_lua_beat_lua_calls;
static uint32_t s_vita_lua_beat_last_import;
static uint32_t s_vita_lua_beat_started;

static unsigned vita_lua_beat_sample_stack(const CPU *c, uint32_t *out,
                                           unsigned capacity)
{
    unsigned found = 0U;
    unsigned i;
    uint32_t esp;
    uint32_t floor;
    uint32_t ceiling;

    if (!c)
        return 0U;
    esp = *(volatile const uint32_t *)&c->esp;
    floor = c->stack_floor;
    ceiling = c->stack_ceiling;
    if (!floor || floor >= ceiling || esp < floor || esp > ceiling ||
        (esp & 3U) != 0U)
        return 0U;
    for (i = 0U; i < VITA_LUA_BEAT_STACK_SCAN && found < capacity; ++i) {
        uint32_t address = esp + i * 4U;
        uint32_t value;
        if (address > ceiling - 4U)
            break;
        value = *(volatile const uint32_t *)(uintptr_t)address;
        if (value - (uint32_t)GUEST_IMAGE_BASE < VITA_LUA_BEAT_TEXT_END_RVA)
            out[found++] = value - (uint32_t)GUEST_IMAGE_BASE;
    }
    return found;
}

static int vita_lua_beat_thread(SceSize args, void *argp)
{
    uint64_t next = sceKernelGetProcessTimeWide() + VITA_LUA_BEAT_FIRST_US;
    uint32_t last_imports = 0U;
    uint32_t last_lua_calls = 0U;
    unsigned lines = 0U;
    unsigned beats_after_finish = 0U;

    (void)args;
    (void)argp;
    while (lines < VITA_LUA_BEAT_MAX_LINES) {
        uint64_t now;
        CPU *c;
        uint32_t imports;
        uint32_t lua_calls;
        uint32_t last_import;
        uint32_t returns[VITA_LUA_BEAT_RETURNS];
        unsigned count;
        unsigned i;
        int loading;
        const char *last_name;

        (void)sceKernelDelayThread(VITA_LUA_BEAT_POLL_US);
        now = sceKernelGetProcessTimeWide();
        if (now < next)
            continue;
        next = now + VITA_LUA_BEAT_PERIOD_US;

        loading = kage_vita_loading_active ? kage_vita_loading_active() : -1;
        if (loading == 0 && ++beats_after_finish > 2U)
            break;
        c = __atomic_load_n(&s_vita_lua_beat_cpu, __ATOMIC_ACQUIRE);
        imports = __atomic_load_n(&g_host_import_calls, __ATOMIC_RELAXED);
        lua_calls = __atomic_load_n(&s_vita_lua_beat_lua_calls,
                                    __ATOMIC_RELAXED);
        last_import = __atomic_load_n(&s_vita_lua_beat_last_import,
                                      __ATOMIC_RELAXED);
        last_name = isaac_vita_lua_import_name(last_import);
        if (last_name && strncmp(last_name, "Lua5.3.3r.dll!", 14U) == 0)
            last_name += 14;
        memset(returns, 0, sizeof returns);
        count = vita_lua_beat_sample_stack(c, returns, VITA_LUA_BEAT_RETURNS);
        for (i = count; i < VITA_LUA_BEAT_RETURNS; ++i)
            returns[i] = 0U;
        ++lines;
        isaac_vita_log(
            "[isaac-lua] beat n=%u t=%ums loading=%d stage=%u/%us swaps=%u "
            "imports=%u(+%u) lua=%u(+%u) last=%s active=%d cb=%d "
            "esp=0x%08x ret=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x"
#if defined(VITA_LUA_ARENA_ENABLED)
            " arena=%u/%u/%u"
#endif
            ,
            lines, (unsigned)(now / UINT64_C(1000)), loading,
            kage_vita_loading_stage ? kage_vita_loading_stage() : 0U,
            kage_vita_loading_elapsed_seconds
                ? kage_vita_loading_elapsed_seconds() : 0U,
            kage_vita_loading_swap_count
                ? kage_vita_loading_swap_count() : 0U,
            imports, imports - last_imports, lua_calls,
            lua_calls - last_lua_calls, last_name ? last_name : "-",
            vita_lua_active_load() != 0U ? 1 : 0,
            s_vita_lua_callback_top + 1,
            c ? *(volatile const uint32_t *)&c->esp : 0U,
            returns[0], returns[1], returns[2], returns[3],
            returns[4], returns[5], returns[6], returns[7]
#if defined(VITA_LUA_ARENA_ENABLED)
            , (unsigned)s_vita_lua_arena.live_bytes,
            (unsigned)s_vita_lua_arena.peak_bytes,
            s_vita_lua_arena.fallbacks
#endif
            );
        last_imports = imports;
        last_lua_calls = lua_calls;
    }
    return 0;
}

static void vita_lua_beat_note_import(CPU *c, uint32_t index)
{
    __atomic_add_fetch(&s_vita_lua_beat_lua_calls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_vita_lua_beat_last_import, index, __ATOMIC_RELAXED);
    if (s_vita_lua_beat_started)
        return;
    s_vita_lua_beat_started = 1U;
    __atomic_store_n(&s_vita_lua_beat_cpu, c, __ATOMIC_RELEASE);
    {
        SceUID thread = sceKernelCreateThread(
            "isaac_lua_beat", vita_lua_beat_thread, 0x10000100, 0x4000U,
            0U, 0, NULL);
        int started = thread < 0 ? (int)thread
                                 : sceKernelStartThread(thread, 0U, NULL);
        if (thread < 0 || started < 0) {
            if (thread >= 0)
                (void)sceKernelDeleteThread(thread);
            isaac_vita_log("[isaac-lua] beat thread FAILED: create=0x%08x "
                           "start=0x%08x",
                           (unsigned)thread, (unsigned)started);
            return;
        }
        isaac_vita_log("[isaac-lua] beat thread=0x%08x first=%us period=%us "
                       "max_lines=%u",
                       (unsigned)thread,
                       (unsigned)(VITA_LUA_BEAT_FIRST_US / 1000000U),
                       (unsigned)(VITA_LUA_BEAT_PERIOD_US / 1000000U),
                       (unsigned)VITA_LUA_BEAT_MAX_LINES);
    }
}
#endif

static int vita_lua_call_guest(lua_State *state, uint32_t target)
{
    KAGE_VITA_DEEP_SCOPE(KVD_LUA_CALLBACK);
#if ISAAC_VITA_LUA_SCOPE_FASTPATH
    /* The trampoline runs inside the import scope that owns the slot (Lua
     * code executes only under an import of the owning CPU), so the slot
     * holds this thread's own store and a relaxed load returns it; the
     * acquire load (dmb) is repeated only for the empty-slot diagnostic so
     * that path observes exactly what it observed before. */
    uintptr_t active = vita_lua_active_load_relaxed();
#else
    uintptr_t active = vita_lua_active_load();
#endif
    CPU *c;
    vita_lua_callback_frame *frame;
    int slot;
    int result;

    if (!target)
        return luaL_error(state, "%s", "null translated Lua callback");
#if ISAAC_VITA_LUA_SCOPE_FASTPATH
    if (!active)
        active = vita_lua_active_load();
#endif
    if (!active)
        return luaL_error(state, "%s", "Lua callback has no active guest CPU");
    c = (CPU *)active;
    slot = vita_lua_callback_frame_push(c, state);
    if (slot < 0)
        return luaL_error(state, "%s", "translated Lua callback depth exceeded");
    frame = &s_vita_lua_callbacks[slot];
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    frame->sampler_enclosing_target = g_kage_guest_last_indirect_target;
#endif
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    isaac_vita_lua_gc_profile_note_callback(target);
#endif
#if defined(ISAAC_VITA_DEEP_PROFILE)
    const uint32_t deep_callback_mark = kage_vita_deep_depth();
#endif

    if (setjmp(frame->escape) == 0) {
        gpush(c, (uint32_t)(uintptr_t)state);
        gpush(c, VITA_LUA_CALLBACK_RETURN);
        guest_call(c, target);
        if (c->fault) {
            vita_lua_restore_callback_cpu(frame);
            memcpy(frame->error_text, "translated Lua callback fault", 30U);
            frame->error_kind = VITA_LUA_CALLBACK_ERROR_LUAL_ERROR;
        } else if (c->esp != frame->saved_esp - 4U) {
            vita_lua_restore_callback_cpu(frame);
            memcpy(frame->error_text,
                   "translated Lua callback broke cdecl stack", 42U);
            frame->error_kind = VITA_LUA_CALLBACK_ERROR_LUAL_ERROR;
#if ISAAC_VITA_LUA_SCOPE_FASTPATH
        } else {
            /* Pop the callback's own argument word.  The old
             * guest_stack_adjust(c, 4U, target) re-validated a range this
             * branch has already pinned: !c->fault means the two gpush
             * calls above and the callee's ret passed the owner check on
             * this bound CPU (nothing unbinds it mid-callback), and the
             * gpush from saved_esp succeeded only if saved_esp is in
             * [stack_floor + 4, stack_ceiling]; with esp == saved_esp - 4
             * the adjust predicate (offset <= capacity && 4 <= capacity -
             * offset) is therefore always true, so the removed check could
             * never fire and no guest_stack_violation attribution changes.
             * guest_stack_adjust does no low-water note either. */
            c->esp = frame->saved_esp;
        }
#else
        } else if (!guest_stack_adjust(c, 4U, target)) {
            vita_lua_restore_callback_cpu(frame);
            memcpy(frame->error_text,
                   "translated Lua callback stack cleanup failed", 45U);
            frame->error_kind = VITA_LUA_CALLBACK_ERROR_LUAL_ERROR;
        }
#endif
    }
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_unwind(deep_callback_mark);
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* Normal return: guest_call's wrapper already restored this value.
     * longjmp(frame->escape): the wrapper's store was skipped; restore here. */
    g_kage_guest_last_indirect_target = frame->sampler_enclosing_target;
#endif

    if (frame->error_kind != VITA_LUA_CALLBACK_ERROR_NONE) {
        int kind = frame->error_kind;
        int argument = frame->error_argument;
        char message[VITA_LUA_ERROR_TEXT_MAX + 1U];
        memcpy(message, frame->error_text, sizeof message);
        vita_lua_callback_frame_pop(slot);
        if (kind == VITA_LUA_CALLBACK_ERROR_ARGERROR)
            return luaL_argerror(state, argument, message);
        if (kind == VITA_LUA_CALLBACK_ERROR_LUAL_ERROR_STACK) {
            /* Finish vanilla luaL_error at the same C-function level the
             * guest raised from: luaL_where(L, 1) .. formatted message. */
            luaL_where(state, 1);
            lua_insert(state, -2);
            lua_concat(state, 2);
            return lua_error(state);
        }
        return luaL_error(state, "%s", message);
    }

    result = (int32_t)c->eax;
    vita_lua_callback_frame_pop(slot);
    return result;
}

#if defined(ISAAC_VITA_LUA_NATIVE_INDEX)
/* ---- ISAAC_VITA_LUA_NATIVE_INDEX (wf/opt-lua-index) begin -----------------
 * LuaBridge's CFunc::indexMetaMethod (sub_003f95c0) is the __index of every
 * LuaBridge class metatable and namespace table, so each field or method
 * access on game userdata enters the callback trampoline above (frame push,
 * memset, setjmp, two gpush, guest_call) and the translated body then issues
 * 6 (method hit) to 19 (property getter) C-API imports through
 * guest_import_call: 0.4 ms per frame in a quiet room, 1.4-1.8 ms with an EID
 * description shown (perf:wf-prof-v6 ph120.hot/hotc 3f95c0).  This replays
 * the same C-API sequence natively.  Re-derived from the corpus (guest_0120.c
 * sub_003f95c0, IAT slots resolved against the unpacked PE: 0x60621c
 * getmetatable, 0x606258 pushvalue, 0x606194 rawget, 0x606158 iscfunction,
 * 0x606170 settop, 0x60620c type, 0x606220 absindex, 0x6061ac pushstring,
 * 0x606214 rotate, 0x6061c8 callk) it is the stock LuaBridge loop without any
 * __const step:
 *
 *   lua_getmetatable(L, 1)                      (result unchecked)
 *   loop:
 *     pushvalue(2); rawget(-2)
 *     iscfunction(-1)   -> remove(-2); return 1                  [method]
 *     type(-1) != NIL   -> pop 2; throw "not a cfunction"
 *     pop 1
 *     rawgetfield(-1, "__propget") = absindex(-1); pushstring; rawget
 *     type(-1) != TABLE -> pop 2; throw "missing __propget table"
 *     pushvalue(2); rawget(-2); remove(-2)
 *     iscfunction(-1)   -> remove(-2); pushvalue(1); callk(1, 1); return 1
 *                                                               [property]
 *     type(-1) != NIL   -> pop 2; throw "not a cfunction"
 *     pop 1
 *     rawgetfield(-1, "__parent")
 *     type(-1) == TABLE -> remove(-2); loop
 *     type(-1) == NIL   -> return 1                              [nil]
 *     else              -> pop 2; throw "__parent is not a table"
 *
 * Every step before the terminal lua_call only reads or pushes, so any
 * deviation (no metatable, a non-cfunction value, a malformed __propget or
 * __parent) restores the entry stack and runs the unchanged translated body,
 * which produces the original behaviour including its C++ throw.  The [nil]
 * arm deliberately keeps the metatable below the nil, exactly like the
 * translated body; the VERIFY variant compares that residue slot for slot.
 * Only this target is replayed; the sibling registrations (0x3f90b0,
 * 0x3f9210, 0x3f97b0, 0x3fc500, 0x3fc640, 0x3fc830) keep the trampoline. */
void isaac_vita_log(const char *format, ...);
static int vita_lua_recover_requires(CPU *c, int entry_mark);

#define VITA_LUA_NATIVE_INDEX_RVA UINT32_C(0x003f95c0)
#define VITA_LUA_NATIVE_INDEX_VERIFY_BURST 1000U
#define VITA_LUA_NATIVE_INDEX_VERIFY_PERIOD 4096U
/* One stats line per 65536 native hits (roughly 10-40 s of play at the
 * measured 50-160 hits per frame) plus one when the VERIFY burst completes;
 * the first few fallbacks are logged individually with their reason. */
#define VITA_LUA_NATIVE_INDEX_REPORT_MASK UINT32_C(0xffff)
#define VITA_LUA_NATIVE_INDEX_FALLBACK_LOGS 8U

enum vita_lua_native_index_hit {
    VITA_LUA_NATIVE_INDEX_HIT_METHOD = 0,
    VITA_LUA_NATIVE_INDEX_HIT_PROPERTY,
    VITA_LUA_NATIVE_INDEX_HIT_NIL,
    VITA_LUA_NATIVE_INDEX_HIT_COUNT
};

enum vita_lua_native_index_fallback {
    VITA_LUA_NATIVE_INDEX_FALLBACK_NO_METATABLE = 0,
    VITA_LUA_NATIVE_INDEX_FALLBACK_NOT_CFUNCTION,
    VITA_LUA_NATIVE_INDEX_FALLBACK_PROPGET_NOT_TABLE,
    VITA_LUA_NATIVE_INDEX_FALLBACK_PROPGET_NOT_CFUNCTION,
    VITA_LUA_NATIVE_INDEX_FALLBACK_PARENT_NOT_TABLE,
    VITA_LUA_NATIVE_INDEX_FALLBACK_DISABLED,
    VITA_LUA_NATIVE_INDEX_FALLBACK_COUNT
};

typedef struct vita_lua_native_index_stats {
    uint32_t hits;
    uint32_t kinds[VITA_LUA_NATIVE_INDEX_HIT_COUNT];
    uint32_t parent_hops;
    uint32_t fallbacks;
    uint32_t reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_COUNT];
    uint32_t verify_runs;
    uint32_t verify_mismatches;
    uint32_t reports;
    int disabled;
} vita_lua_native_index_stats;

static const char *const s_vita_lua_native_index_fallback_names
    [VITA_LUA_NATIVE_INDEX_FALLBACK_COUNT] = {
    "no-metatable", "not-cfunction", "propget-not-table",
    "propget-not-cfunction", "parent-not-table", "disabled"
};
static vita_lua_native_index_stats s_vita_lua_native_index;

void isaac_vita_lua_native_index_report(void)
{
    const vita_lua_native_index_stats *s = &s_vita_lua_native_index;
    isaac_vita_log("[isaac-lua] native-index win=%u hits=%u "
                   "kind(method,property,nil)=%u,%u,%u hops=%u "
                   "fallbacks=%u reason(nomt,notcf,propget,propcf,parent,off)="
                   "%u,%u,%u,%u,%u,%u verify(runs,mismatch)=%u,%u disabled=%d",
                   (unsigned)s->reports, (unsigned)s->hits,
                   (unsigned)s->kinds[VITA_LUA_NATIVE_INDEX_HIT_METHOD],
                   (unsigned)s->kinds[VITA_LUA_NATIVE_INDEX_HIT_PROPERTY],
                   (unsigned)s->kinds[VITA_LUA_NATIVE_INDEX_HIT_NIL],
                   (unsigned)s->parent_hops, (unsigned)s->fallbacks,
                   (unsigned)s->reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_NO_METATABLE],
                   (unsigned)s->reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_NOT_CFUNCTION],
                   (unsigned)s->reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_PROPGET_NOT_TABLE],
                   (unsigned)s->reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_PROPGET_NOT_CFUNCTION],
                   (unsigned)s->reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_PARENT_NOT_TABLE],
                   (unsigned)s->reasons[VITA_LUA_NATIVE_INDEX_FALLBACK_DISABLED],
                   (unsigned)s->verify_runs, (unsigned)s->verify_mismatches,
                   s->disabled);
    ++s_vita_lua_native_index.reports;
}

/* The key is only rendered when it already is a string: lua_tostring would
 * otherwise convert a number key in place. */
static const char *vita_lua_native_index_key_text(lua_State *L)
{
    return lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "-";
}

static int vita_lua_native_index_fallback(lua_State *L, uint32_t target,
                                          int base, int reason)
{
    vita_lua_native_index_stats *s = &s_vita_lua_native_index;
    ++s->fallbacks;
    ++s->reasons[reason];
    if (s->fallbacks <= VITA_LUA_NATIVE_INDEX_FALLBACK_LOGS)
        isaac_vita_log("[isaac-lua] native-index fallback: reason=%s obj=%s "
                       "key=%s:%.48s base=%d count=%u",
                       s_vita_lua_native_index_fallback_names[reason],
                       lua_typename(L, lua_type(L, 1)),
                       lua_typename(L, lua_type(L, 2)),
                       vita_lua_native_index_key_text(L), base,
                       (unsigned)s->fallbacks);
    /* Every native step so far only pushed above the entry top. */
    lua_settop(L, base);
    return vita_lua_call_guest(L, target);
}

#if defined(ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY)
static const char *const
    s_vita_lua_native_index_hit_names[VITA_LUA_NATIVE_INDEX_HIT_COUNT] = {
    "method", "property", "nil"
};
static uint32_t s_vita_lua_native_index_verify_target;
static int s_vita_lua_native_index_verify_count;

/* Runs the translated body on a private frame holding copies of the entry
 * arguments and hands back its whole stack residue (not only the declared
 * result), so the caller can compare every slot against the native residue. */
static int vita_lua_native_index_verify_body(lua_State *L)
{
    int base = lua_gettop(L);
    int count = vita_lua_call_guest(
        L, s_vita_lua_native_index_verify_target);
    int top = lua_gettop(L);
    s_vita_lua_native_index_verify_count = count;
    return top > base ? top - base : 0;
}

static int vita_lua_native_index_same(lua_State *L, int a, int b)
{
    int type = lua_type(L, a);
    size_t length;
    int has_a;
    int has_b;
    int same;

    if (type != lua_type(L, b))
        return 0;
    if (type == LUA_TNUMBER && lua_isinteger(L, a) != lua_isinteger(L, b))
        return 0;
    if (lua_rawequal(L, a, b))
        return 1;
    if (type != LUA_TUSERDATA)
        return 0;
    /* A by-value getter (Vector, Color, ...) boxes a fresh copy on every
     * call; equal bytes under the same metatable is the same answer. */
    length = lua_rawlen(L, a);
    if (length != lua_rawlen(L, b))
        return 0;
    if (length &&
        memcmp(lua_touserdata(L, a), lua_touserdata(L, b), length) != 0)
        return 0;
    has_a = lua_getmetatable(L, a);
    has_b = lua_getmetatable(L, b);
    same = has_a == has_b && (!has_a || lua_rawequal(L, -1, -2));
    lua_pop(L, has_a + has_b);
    return same;
}

static void vita_lua_native_index_verify(lua_State *L, uint32_t target,
                                         int base, int kind)
{
    vita_lua_native_index_stats *s = &s_vita_lua_native_index;
    CPU *c = (CPU *)vita_lua_active_load();
    int native_top = lua_gettop(L);
    int callback_mark;
    int require_mark;
    int status = LUA_OK;
    int results = 0;
    int slot = 0;
    int i;
    const char *why = NULL;

    ++s->verify_runs;
    if (!c) {
        why = "no active guest cpu";
    } else if (!lua_checkstack(L, base + 4)) {
        why = "no stack for the translated frame";
    } else {
        s_vita_lua_native_index_verify_target = target;
        s_vita_lua_native_index_verify_count = -1;
        lua_pushcfunction(L, vita_lua_native_index_verify_body);
        for (i = 1; i <= base; ++i)
            lua_pushvalue(L, i);
        callback_mark = s_vita_lua_callback_top;
        require_mark = s_vita_lua_require_top;
#if defined(ISAAC_VITA_DEEP_PROFILE)
        const uint32_t deep_verify_mark = kage_vita_deep_depth();
#endif
        status = lua_pcall(L, base, LUA_MULTRET, 0);
#if defined(ISAAC_VITA_DEEP_PROFILE)
        kage_vita_deep_unwind(deep_verify_mark);
#endif
        if (!vita_lua_recover_callbacks(c, callback_mark) ||
            !vita_lua_recover_requires(c, require_mark)) {
            why = "callback recovery";
        } else if (status != LUA_OK) {
            why = "translated body raised";
        } else {
            results = lua_gettop(L) - native_top;
            if (results != native_top - base) {
                why = "stack depth";
            } else if (s_vita_lua_native_index_verify_count != 1) {
                why = "result count";
            } else {
                for (i = 1; i <= results; ++i) {
                    if (!vita_lua_native_index_same(L, base + i,
                                                    native_top + i)) {
                        why = "slot value";
                        slot = i;
                        break;
                    }
                }
            }
        }
    }
    if (why) {
        ++s->verify_mismatches;
        s->disabled = 1;
        isaac_vita_log("[isaac-lua] native-index verify mismatch: %s kind=%s "
                       "hit=%u base=%d native=%d translated=%d count=%d "
                       "slot=%d key=%s:%.48s error=%.96s -- replay disabled",
                       why, s_vita_lua_native_index_hit_names[kind],
                       (unsigned)s->hits, base, native_top - base, results,
                       s_vita_lua_native_index_verify_count, slot,
                       lua_typename(L, lua_type(L, 2)),
                       vita_lua_native_index_key_text(L),
                       status != LUA_OK && lua_type(L, -1) == LUA_TSTRING
                           ? lua_tostring(L, -1) : "-");
    }
    lua_settop(L, native_top);
}
#endif /* ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY */

static int vita_lua_native_index(lua_State *L, uint32_t target)
{
    vita_lua_native_index_stats *s = &s_vita_lua_native_index;
    int base = lua_gettop(L);
    int kind = VITA_LUA_NATIVE_INDEX_HIT_NIL;

    if (s->disabled)
        return vita_lua_native_index_fallback(
            L, target, base, VITA_LUA_NATIVE_INDEX_FALLBACK_DISABLED);
    if (!lua_getmetatable(L, 1))
        return vita_lua_native_index_fallback(
            L, target, base, VITA_LUA_NATIVE_INDEX_FALLBACK_NO_METATABLE);
    for (;;) {
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        if (lua_iscfunction(L, -1)) {
            lua_remove(L, -2);
            kind = VITA_LUA_NATIVE_INDEX_HIT_METHOD;
            break;
        }
        if (!lua_isnil(L, -1))
            return vita_lua_native_index_fallback(
                L, target, base, VITA_LUA_NATIVE_INDEX_FALLBACK_NOT_CFUNCTION);
        lua_pop(L, 1);
        lua_pushstring(L, "__propget");
        lua_rawget(L, -2);
        if (!lua_istable(L, -1))
            return vita_lua_native_index_fallback(
                L, target, base,
                VITA_LUA_NATIVE_INDEX_FALLBACK_PROPGET_NOT_TABLE);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        if (lua_iscfunction(L, -1)) {
            lua_remove(L, -2);
            lua_pushvalue(L, 1);
            lua_call(L, 1, 1);
            kind = VITA_LUA_NATIVE_INDEX_HIT_PROPERTY;
            break;
        }
        if (!lua_isnil(L, -1))
            return vita_lua_native_index_fallback(
                L, target, base,
                VITA_LUA_NATIVE_INDEX_FALLBACK_PROPGET_NOT_CFUNCTION);
        lua_pop(L, 1);
        lua_pushstring(L, "__parent");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_remove(L, -2);
            ++s->parent_hops;
            continue;
        }
        if (lua_isnil(L, -1)) {
            kind = VITA_LUA_NATIVE_INDEX_HIT_NIL;
            break;
        }
        return vita_lua_native_index_fallback(
            L, target, base, VITA_LUA_NATIVE_INDEX_FALLBACK_PARENT_NOT_TABLE);
    }
    ++s->hits;
    ++s->kinds[kind];
#if defined(ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY)
    if (s->hits <= VITA_LUA_NATIVE_INDEX_VERIFY_BURST ||
        (s->hits % VITA_LUA_NATIVE_INDEX_VERIFY_PERIOD) == 0U)
        vita_lua_native_index_verify(L, target, base, kind);
#endif
    if ((s->hits & VITA_LUA_NATIVE_INDEX_REPORT_MASK) == 0U ||
        s->hits == VITA_LUA_NATIVE_INDEX_VERIFY_BURST)
        isaac_vita_lua_native_index_report();
    return 1;
}
/* ---- ISAAC_VITA_LUA_NATIVE_INDEX end ------------------------------------ */
#endif

static int vita_lua_guest_cfunction(lua_State *state)
{
    lua_Integer encoded = lua_tointeger(state, lua_upvalueindex(1));
#if defined(ISAAC_VITA_LUA_NATIVE_INDEX)
    if (guest_direct_translated_target((uint32_t)(uint64_t)encoded,
                                       VITA_LUA_NATIVE_INDEX_RVA))
        return vita_lua_native_index(state, (uint32_t)(uint64_t)encoded);
#endif
    return vita_lua_call_guest(state, (uint32_t)(uint64_t)encoded);
}

static int vita_lua_guest_openfunction(lua_State *state)
{
    int slot = s_vita_lua_require_top;
    if (slot < 0 || (uint32_t)slot >= VITA_LUA_MAX_REQUIRE_DEPTH ||
        !s_vita_lua_requires[slot].used)
        return luaL_error(state, "%s", "Lua requiref callback has no target");
    return vita_lua_call_guest(state, s_vita_lua_requires[slot].target);
}

static int vita_lua_guest_panic(lua_State *state)
{
    vita_lua_state_record *record = vita_lua_find_state(state);
    if (!record || !record->panic_target)
        return 0;
    return vita_lua_call_guest(state, record->panic_target);
}

static void *vita_lua_guest_allocator(void *opaque, void *pointer,
                                      size_t old_size, size_t new_size)
{
    vita_lua_allocator *allocator = (vita_lua_allocator *)opaque;
    CPU *c;
    uint32_t saved_esp;
    uint32_t result;

    if (!allocator || !allocator->used ||
        old_size > UINT32_MAX || new_size > UINT32_MAX ||
        (uintptr_t)pointer > UINT32_MAX)
        return NULL;
    c = allocator->cpu;
    if (!c || vita_lua_active_load() != (uintptr_t)c)
        return NULL;
    saved_esp = c->esp;
    gpush(c, (uint32_t)new_size);
    gpush(c, (uint32_t)old_size);
    gpush(c, (uint32_t)(uintptr_t)pointer);
    gpush(c, allocator->guest_ud);
    gpush(c, VITA_LUA_CALLBACK_RETURN);
    guest_call(c, allocator->target);
    if (c->fault || c->esp != saved_esp - 16U) {
        (void)guest_stack_set(c, saved_esp, allocator->target);
        return NULL;
    }
    result = c->eax;
    if (!guest_stack_adjust(c, 16U, allocator->target))
        return NULL;
    return (void *)(uintptr_t)result;
}

/* luaL_newstate's stock lauxlib allocator calls the process realloc/free
 * directly.  That would bypass the port's single guest-visible heap owner.
 * Reproduce luaL_newstate on top of lua_newstate with the established heap
 * boundary; startup installs the frozen guest panic function immediately. */
static void *vita_lua_native_allocator(void *opaque, void *pointer,
                                       size_t old_size, size_t new_size)
{
    int valid_owner = 1;
    (void)opaque;
    (void)old_size;
    if (!new_size) {
        if (pointer)
            (void)isaac_vita_guest_free(pointer);
        return NULL;
    }
    if (!pointer)
        return isaac_vita_guest_malloc(new_size);
    return isaac_vita_guest_realloc(pointer, new_size, &valid_owner);
}

static int vita_lua_require_push(CPU *c, uint32_t target)
{
    uint32_t i;
    for (i = 0U; i < VITA_LUA_MAX_REQUIRE_DEPTH; ++i) {
        vita_lua_require_frame *frame = &s_vita_lua_requires[i];
        if (!frame->used) {
            frame->used = 1;
            frame->cpu = c;
            frame->target = target;
            frame->previous = s_vita_lua_require_top;
            s_vita_lua_require_top = (int)i;
            return (int)i;
        }
    }
    return -1;
}

/* LuaEngine::Init passes seven odd-address import thunks, not IAT slots and
 * not generated function roots, as luaL_requiref openers.  They are frozen
 * one-instruction thunks and map exactly to the native library openers. */
static lua_CFunction vita_lua_builtin_openfunction(uint32_t target)
{
    if (target >= (uint32_t)GUEST_IMAGE_BASE)
        target -= (uint32_t)GUEST_IMAGE_BASE;
    switch (target) {
    case 0x005e159dU: return luaopen_table;
    case 0x005e15a3U: return luaopen_base;
    case 0x005e15a9U: return luaopen_utf8;
    case 0x005e15afU: return luaopen_string;
    case 0x005e15b5U: return luaopen_math;
    case 0x005e15bbU: return luaopen_debug;
    case 0x005e15c1U: return luaopen_coroutine;
    default: return NULL;
    }
}

static void vita_lua_require_pop(int slot)
{
    if (slot < 0 || (uint32_t)slot >= VITA_LUA_MAX_REQUIRE_DEPTH)
        return;
    s_vita_lua_require_top = s_vita_lua_requires[slot].previous;
    s_vita_lua_requires[slot].used = 0;
}

static int vita_lua_recover_requires(CPU *c, int entry_mark)
{
    while (s_vita_lua_require_top != entry_mark) {
        int slot = s_vita_lua_require_top;
        vita_lua_require_frame *frame;
        if (slot < 0 || (uint32_t)slot >= VITA_LUA_MAX_REQUIRE_DEPTH) {
            guest_fault(c, 0U, "Lua requiref recovery chain is corrupt");
            return 0;
        }
        frame = &s_vita_lua_requires[slot];
        if (!frame->used || frame->cpu != c) {
            guest_fault(c, 0U, "Lua requiref recovery owner mismatch");
            return 0;
        }
        vita_lua_require_pop(slot);
    }
    return 1;
}

static void vita_lua_close_import(CPU *__restrict c)
{
    lua_State *state = vita_lua_arg_state(c);
    vita_lua_state_record *record = vita_lua_find_state(state);
    vita_lua_allocator *allocator = record ? record->allocator : NULL;
    vita_lua_scope scope;
    if (!vita_lua_scope_enter(c, &scope))
        return;
    lua_close(state);
    vita_lua_scope_leave(&scope);
#if defined(ISAAC_VITA_LUA_RECEIPT)
    if (vita_lua_receipt_take())
#if defined(VITA_LUA_ARENA_ENABLED)
        isaac_vita_log("[isaac-lua] close state=0x%08x arena live=%u peak=%u "
                       "fallbacks=%u",
                       (unsigned)(uintptr_t)state,
                       (unsigned)s_vita_lua_arena.live_bytes,
                       (unsigned)s_vita_lua_arena.peak_bytes,
                       s_vita_lua_arena.fallbacks);
#else
        isaac_vita_log("[isaac-lua] close state=0x%08x",
                       (unsigned)(uintptr_t)state);
#endif
#endif
    if (record)
        memset(record, 0, sizeof *record);
    if (allocator)
        memset(allocator, 0, sizeof *allocator);
    vita_lua_return(c);
}

#define VITA_LUA_BEGIN()                                                   \
    lua_State *state = vita_lua_arg_state(c);                              \
    vita_lua_scope scope;                                                   \
    if (!vita_lua_scope_enter(c, &scope))                                  \
        return
#define VITA_LUA_END()                                                     \
    do { vita_lua_scope_leave(&scope); vita_lua_return(c); } while (0)

static void vita_lua_rawset_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_rawset(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_iscfunction_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_iscfunction(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_getfield_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_getfield(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        (const char *)(uintptr_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_typename_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    result = lua_typename(state, (int32_t)vita_lua_arg(c, 1U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_toboolean_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_toboolean(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_setmetatable_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_setmetatable(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_compare_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_compare(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        vita_lua_index((int32_t)vita_lua_arg(c, 2U)),
        (int32_t)vita_lua_arg(c, 3U));
    VITA_LUA_END();
}

static void vita_lua_settop_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_settop(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

#define VITA_LUA_OPEN_IMPORT(name)                                        \
    static void vita_lua_##name##_import(CPU *__restrict c)               \
    {                                                                      \
        VITA_LUA_BEGIN();                                                  \
        c->eax = (uint32_t)luaopen_##name(state);                          \
        VITA_LUA_END();                                                    \
    }

VITA_LUA_OPEN_IMPORT(debug)
VITA_LUA_OPEN_IMPORT(coroutine)
VITA_LUA_OPEN_IMPORT(math)
VITA_LUA_OPEN_IMPORT(string)
VITA_LUA_OPEN_IMPORT(utf8)
VITA_LUA_OPEN_IMPORT(base)
VITA_LUA_OPEN_IMPORT(table)

static void vita_lua_openlibs_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    luaL_openlibs(state);
    VITA_LUA_END();
}

static void vita_lua_tolstring_aux_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    result = luaL_tolstring(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        (size_t *)(uintptr_t)vita_lua_arg(c, 2U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_requiref_import(CPU *__restrict c)
{
    int slot;
    lua_CFunction openfunction;
    VITA_LUA_BEGIN();
    openfunction = vita_lua_builtin_openfunction(vita_lua_arg(c, 2U));
    if (openfunction) {
        luaL_requiref(state,
                      (const char *)(uintptr_t)vita_lua_arg(c, 1U),
                      openfunction, (int32_t)vita_lua_arg(c, 3U));
        VITA_LUA_END();
        return;
    }
    slot = vita_lua_require_push(c, vita_lua_arg(c, 2U));
    if (slot < 0) {
        vita_lua_scope_leave(&scope);
        guest_fault(c, vita_lua_arg(c, 2U), "Lua requiref depth exceeded");
        return;
    }
    luaL_requiref(state,
                  (const char *)(uintptr_t)vita_lua_arg(c, 1U),
                  vita_lua_guest_openfunction,
                  (int32_t)vita_lua_arg(c, 3U));
    vita_lua_require_pop(slot);
    VITA_LUA_END();
}

static void vita_lua_argerror_import(CPU *__restrict c)
{
    (void)vita_lua_escape_callback(
        c, VITA_LUA_CALLBACK_ERROR_ARGERROR,
        (int32_t)vita_lua_arg(c, 1U), vita_lua_arg(c, 2U));
}

static void vita_lua_pushnil_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_pushnil(state);
    VITA_LUA_END();
}

static void vita_lua_len_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_len(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_rawget_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_rawget(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_gettop_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_gettop(state);
    VITA_LUA_END();
}

static void vita_lua_pushnumber_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_pushnumber(state, vita_lua_arg_number(c, 1U));
    VITA_LUA_END();
}

static void vita_lua_checknumber_import(CPU *__restrict c)
{
    lua_Number result;
    VITA_LUA_BEGIN();
    result = luaL_checknumber(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    vita_lua_scope_leave(&scope);
    fpush(c, result);
    vita_lua_return(c);
}

static void vita_lua_getstack_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_getstack(
        state, (int32_t)vita_lua_arg(c, 1U),
        (lua_Debug *)(uintptr_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_pushstring_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    result = lua_pushstring(
        state, (const char *)(uintptr_t)vita_lua_arg(c, 1U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_touserdata_import(CPU *__restrict c)
{
    void *result;
    VITA_LUA_BEGIN();
    result = lua_touserdata(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

/* Guest varargs cannot be forwarded as a native va_list, but the x86 cdecl
 * layout is fixed: every vararg occupies 4-byte stack slots after the format
 * pointer (slot 2 for luaL_error/lua_pushfstring), int/long/pointer
 * conversions take one slot, lua_Integer and lua_Number take two adjacent
 * slots without padding.  Each conversion is rendered by the native
 * lua_pushfstring so the text is byte-identical to the frozen DLL's
 * luaO_pushvfstring (5.3.3 set: s c d I f p U %%), and the pieces are
 * concatenated on the Lua stack, so the result has no length cap.  Device
 * proof of need (2026-09-03 game log): the frozen require raises
 * "module '%s' not found:%s" and EID parses that text for its mod path; the
 * old literal-only formatter turned it into a fixed diagnostic and EID
 * failed at main.lua:156 with every language pack reporting an error.
 *
 * Returns 1 with the formatted string on top of the stack.  A malformed
 * conversion leaves vanilla's "invalid option" diagnostic on top instead and
 * returns 0; callers raise it the way luaG_runerror would have. */
#define VITA_LUA_FORMAT_FOLD_PIECES 8

static int vita_lua_push_guest_format(CPU *__restrict c, lua_State *state,
                                      uint32_t guest_format, uint32_t slot)
{
    const char *cursor = guest_format
        ? (const char *)(uintptr_t)guest_format : "(null)";
    int pieces = 0;

    for (;;) {
        const char *percent = strchr(cursor, '%');
        if (!percent)
            break;
        if (pieces > 1 && (pieces >= VITA_LUA_FORMAT_FOLD_PIECES ||
                           !lua_checkstack(state, 3))) {
            lua_concat(state, pieces);
            pieces = 1;
        }
        if (percent > cursor) {
            lua_pushlstring(state, cursor, (size_t)(percent - cursor));
            ++pieces;
        }
        switch (percent[1]) {
        case 's':
            lua_pushfstring(state, "%s",
                            (const char *)(uintptr_t)vita_lua_arg(c, slot));
            slot += 1U;
            break;
        case 'c':
            lua_pushfstring(state, "%c", (int)(int32_t)vita_lua_arg(c, slot));
            slot += 1U;
            break;
        case 'd':
            lua_pushfstring(state, "%d", (int)(int32_t)vita_lua_arg(c, slot));
            slot += 1U;
            break;
        case 'I':
            lua_pushfstring(state, "%I",
                            (lua_Integer)vita_lua_arg_u64(c, slot));
            slot += 2U;
            break;
        case 'f':
            lua_pushfstring(state, "%f", vita_lua_arg_number(c, slot));
            slot += 2U;
            break;
        case 'p':
            lua_pushfstring(state, "%p",
                            (void *)(uintptr_t)vita_lua_arg(c, slot));
            slot += 1U;
            break;
        case 'U':
            lua_pushfstring(state, "%U", (long)(int32_t)vita_lua_arg(c, slot));
            slot += 1U;
            break;
        case '%':
            lua_pushliteral(state, "%");
            break;
        default:
            lua_pop(state, pieces);
            lua_pushfstring(state, "invalid option '%%%c' to 'lua_pushfstring'",
                            (int)(unsigned char)percent[1]);
            return 0;
        }
        ++pieces;
        cursor = percent + 2;
    }
    if (*cursor || pieces == 0) {
        lua_pushstring(state, cursor);
        ++pieces;
    }
    if (pieces > 1)
        lua_concat(state, pieces);
    return 1;
}

static void vita_lua_pushfstring_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    if (!vita_lua_push_guest_format(c, state, vita_lua_arg(c, 1U), 2U)) {
        /* Vanilla luaO_pushvfstring raises this through luaG_runerror; the
         * nearest native lua_pcall recovers the abandoned callback frames. */
        vita_lua_scope_leave(&scope);
        (void)lua_error(state);
        return;
    }
    result = lua_tostring(state, -1);
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_atpanic_import(CPU *__restrict c)
{
    uint32_t target = vita_lua_arg(c, 1U);
    uint32_t previous = 0U;
    vita_lua_state_record *record;
    VITA_LUA_BEGIN();
    record = vita_lua_find_state(state);
    if (!record) {
        vita_lua_scope_leave(&scope);
        guest_fault(c, (uint32_t)(uintptr_t)state,
                    "lua_atpanic received an unregistered state");
        return;
    }
    previous = record->panic_target;
    record->panic_target = target;
    (void)lua_atpanic(state, target ? vita_lua_guest_panic : NULL);
    c->eax = previous;
    VITA_LUA_END();
}

static void vita_lua_getglobal_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_getglobal(
        state, (const char *)(uintptr_t)vita_lua_arg(c, 1U));
    VITA_LUA_END();
}

static void vita_lua_isstring_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_isstring(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_callk_import(CPU *__restrict c)
{
    if (vita_lua_arg(c, 4U)) {
        guest_fault(c, vita_lua_arg(c, 4U),
                    "non-zero lua_callk continuation is not implemented");
        return;
    }
    VITA_LUA_BEGIN();
    lua_callk(state, (int32_t)vita_lua_arg(c, 1U),
              (int32_t)vita_lua_arg(c, 2U),
              (lua_KContext)(int32_t)vita_lua_arg(c, 3U), NULL);
    VITA_LUA_END();
}

static void vita_lua_rawgetp_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_rawgetp(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        (const void *)(uintptr_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_loadbufferx_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)luaL_loadbufferx(
        state, (const char *)(uintptr_t)vita_lua_arg(c, 1U),
        (size_t)vita_lua_arg(c, 2U),
        (const char *)(uintptr_t)vita_lua_arg(c, 3U),
        (const char *)(uintptr_t)vita_lua_arg(c, 4U));
#if defined(ISAAC_VITA_LUA_RECEIPT)
    vita_lua_receipt_load(state, "buffer",
                          (const char *)(uintptr_t)vita_lua_arg(c, 3U),
                          (size_t)vita_lua_arg(c, 2U), (int)c->eax);
#endif
    VITA_LUA_END();
}

static void vita_lua_tolstring_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    result = lua_tolstring(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        (size_t *)(uintptr_t)vita_lua_arg(c, 2U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_isuserdata_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_isuserdata(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_pushboolean_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_pushboolean(state, (int32_t)vita_lua_arg(c, 1U));
    VITA_LUA_END();
}

static void vita_lua_newstate_import(CPU *__restrict c)
{
    uint32_t target = vita_lua_arg(c, 0U);
    vita_lua_allocator *allocator;
    lua_State *state;
    vita_lua_scope scope;
    if (!target) {
        guest_fault(c, 0U, "lua_newstate received a null guest allocator");
        return;
    }
    if (!vita_lua_scope_enter(c, &scope))
        return;
    allocator = vita_lua_add_allocator(c, target, vita_lua_arg(c, 1U));
    if (!allocator) {
        vita_lua_scope_leave(&scope);
        guest_fault(c, target, "Lua guest allocator registry is full");
        return;
    }
#if defined(VITA_LUA_ARENA_ENABLED)
    state = lua_newstate(s_vita_lua_arena.ready ? vita_lua_arena_allocator
                                                : vita_lua_guest_allocator,
                         allocator);
#else
    state = lua_newstate(vita_lua_guest_allocator, allocator);
#endif
#if defined(ISAAC_VITA_LUA_RECEIPT)
    if (vita_lua_receipt_take())
        isaac_vita_log("[isaac-lua] newstate state=0x%08x alloc=0x%08x arena=%d",
                       (unsigned)(uintptr_t)state, (unsigned)target,
#if defined(VITA_LUA_ARENA_ENABLED)
                       s_vita_lua_arena.ready
#else
                       0
#endif
                       );
#endif
    if (state && vita_lua_add_state(state, allocator)) {
        allocator->state = state;
    } else {
        if (state)
            lua_close(state);
        memset(allocator, 0, sizeof *allocator);
        state = NULL;
    }
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, state);
}

static void vita_lua_setglobal_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_setglobal(state, (const char *)(uintptr_t)vita_lua_arg(c, 1U));
    VITA_LUA_END();
}

static void vita_lua_newuserdata_import(CPU *__restrict c)
{
    void *result;
    VITA_LUA_BEGIN();
    result = lua_newuserdata(state, (size_t)vita_lua_arg(c, 1U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_newstate_aux_import(CPU *__restrict c)
{
    lua_State *state;
    vita_lua_scope scope;
    if (!vita_lua_scope_enter(c, &scope))
        return;
    /* This is the state LuaEngine::Init actually creates on device (the
     * lua_newstate route above never fires), so the private arena must serve
     * it too or ISAAC_VITA_LUA_ARENA_MB changes nothing. */
#if defined(VITA_LUA_ARENA_ENABLED)
    state = lua_newstate(s_vita_lua_arena.ready ? vita_lua_arena_allocator
                                                : vita_lua_native_allocator,
                         NULL);
#else
    state = lua_newstate(vita_lua_native_allocator, NULL);
#endif
#if defined(ISAAC_VITA_LUA_RECEIPT)
    if (vita_lua_receipt_take())
        isaac_vita_log("[isaac-lua] newstate state=0x%08x alloc=luaL_newstate "
                       "arena=%d",
                       (unsigned)(uintptr_t)state,
#if defined(VITA_LUA_ARENA_ENABLED)
                       s_vita_lua_arena.ready
#else
                       0
#endif
                       );
#endif
    if (state && !vita_lua_add_state(state, NULL)) {
        lua_close(state);
        state = NULL;
    }
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, state);
}

static void vita_lua_checkinteger_import(CPU *__restrict c)
{
    lua_Integer result;
    VITA_LUA_BEGIN();
    result = luaL_checkinteger(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    vita_lua_scope_leave(&scope);
    vita_lua_return_integer(c, result);
}

/* luaL_error never returns to its caller.  Format the guest varargs onto the
 * Lua stack while the x86 frame is still live, then take the private escape
 * to the native callback trampoline, which prefixes luaL_where and raises.
 * Without a translated callback boundary there is nothing to unwind to
 * (vanilla would panic), so that stays a guest fault. */
static void vita_lua_error_import(CPU *__restrict c)
{
    uint32_t format = vita_lua_arg(c, 1U);
    lua_State *state = vita_lua_arg_state(c);
    vita_lua_callback_frame *frame =
        s_vita_lua_callback_top >= 0
            ? &s_vita_lua_callbacks[s_vita_lua_callback_top] : NULL;
    vita_lua_scope scope;

    if (!frame || !frame->used || frame->cpu != c) {
        guest_fault(c, format, "luaL_error has no translated callback boundary");
        return;
    }
    if (frame->state != state) {
        guest_fault(c, (uint32_t)(uintptr_t)state,
                    "luaL_error state differs from the callback state");
        return;
    }
    if (!vita_lua_scope_enter(c, &scope))
        return;
    /* A malformed conversion leaves vanilla's own diagnostic on the stack;
     * raising that is what luaO_pushvfstring would have done. */
    (void)vita_lua_push_guest_format(c, state, format, 2U);
    vita_lua_scope_leave(&scope);
    frame->error_kind = VITA_LUA_CALLBACK_ERROR_LUAL_ERROR_STACK;
    vita_lua_restore_callback_cpu(frame);
    longjmp(frame->escape, 1);
}

static void vita_lua_rawsetp_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_rawsetp(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
                (const void *)(uintptr_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_setfield_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_setfield(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
                 (const char *)(uintptr_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_gettable_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_gettable(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_type_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_type(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_rawequal_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_rawequal(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        vita_lua_index((int32_t)vita_lua_arg(c, 2U)));
    VITA_LUA_END();
}

static void vita_lua_rotate_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_rotate(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
               (int32_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_checklstring_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    result = luaL_checklstring(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        (size_t *)(uintptr_t)vita_lua_arg(c, 2U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_getmetatable_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_getmetatable(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_absindex_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)vita_lua_index_result(lua_absindex(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U))));
    VITA_LUA_END();
}

static void vita_lua_pushlstring_import(CPU *__restrict c)
{
    const char *result;
    VITA_LUA_BEGIN();
    result = lua_pushlstring(
        state, (const char *)(uintptr_t)vita_lua_arg(c, 1U),
        (size_t)vita_lua_arg(c, 2U));
    vita_lua_scope_leave(&scope);
    vita_lua_return_pointer(c, result);
}

static void vita_lua_getinfo_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_getinfo(
        state, (const char *)(uintptr_t)vita_lua_arg(c, 1U),
        (lua_Debug *)(uintptr_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_copy_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_copy(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
             vita_lua_index((int32_t)vita_lua_arg(c, 2U)));
    VITA_LUA_END();
}

static int vita_lua_build_core_package_path(
    const char *native_filename, char *result, size_t capacity,
    size_t *result_length)
{
    static const char posix_marker[] = "/mods/";
    static const char windows_marker[] = "\\mods\\";
    static const char posix_suffix[] = "/resources/scripts/?.lua";
    static const char windows_suffix[] = "\\resources\\scripts\\?.lua";
    const char *marker = strstr(native_filename, posix_marker);
    const char *suffix = posix_suffix;
    size_t root_length;
    size_t suffix_length;

    if (!marker) {
        marker = strstr(native_filename, windows_marker);
        suffix = windows_suffix;
    }
    if (!marker)
        return 0;
    root_length = (size_t)(marker - native_filename);
    suffix_length = strlen(suffix);
    if (root_length + suffix_length + 1U > capacity)
        return 0;
    memcpy(result, native_filename, root_length);
    memcpy(result + root_length, suffix, suffix_length + 1U);
    *result_length = root_length + suffix_length;
    return 1;
}

/* LuaEngine::RunScript temporarily prepends
 *
 *     dirname(script) + "/?.lua;"
 *
 * to package.path before it calls luaL_loadfilex, then restores the previous
 * path after the protected script call.  On Vita that dirname is deliberately
 * guest-visible (for example `ux0:data/isaacr001/mods/836319872`), while
 * vanilla Lua's package searcher opens files directly and therefore needs the
 * canonical native spelling (`ux0:/data/...`).  Rebase only that exact leading
 * template from the script path which just crossed the confined path mapper.
 * The one frozen tail remains a Windows path even in the Vita translation:
 *
 *     .\resources\scripts\?.lua
 *
 * Rebase that exact tail to the sibling resources directory under the same
 * already-confined native data root.  Any other tail is preserved byte-for-
 * byte, as is the engine's later package.path restore.
 */
static void vita_lua_rebase_script_package_path(
    lua_State *state, const char *guest_filename, const char *native_filename)
{
    static const char module_pattern[] = "/?.lua;";
    static const char frozen_core_pattern[] = ".\\resources\\scripts\\?.lua";
    char native_head[ISAAC_VITA_STARTUP_PATH_MAX + sizeof module_pattern];
    char native_core[ISAAC_VITA_STARTUP_PATH_MAX +
                     sizeof "/resources/scripts/?.lua"];
    const char *guest_separator;
    const char *native_separator;
    const char *current;
    const char *tail;
    size_t guest_directory_length;
    size_t native_directory_length;
    size_t guest_head_length;
    size_t native_head_length;
    size_t current_length;
    size_t tail_length;
    int stack_top;
    int package_index;

    guest_separator = strrchr(guest_filename, '/');
    native_separator = strrchr(native_filename, '/');
    if (!native_separator)
        native_separator = strrchr(native_filename, '\\');
    if (!guest_separator || !native_separator)
        return;
    guest_directory_length = (size_t)(guest_separator - guest_filename);
    native_directory_length = (size_t)(native_separator - native_filename);
    guest_head_length = guest_directory_length + sizeof module_pattern - 1U;
    native_head_length = native_directory_length + sizeof module_pattern - 1U;
    if (native_head_length + 1U > sizeof native_head)
        return;

    stack_top = lua_gettop(state);
    if (lua_getglobal(state, "package") != LUA_TTABLE) {
        lua_settop(state, stack_top);
        return;
    }
    package_index = lua_gettop(state);
    if (lua_getfield(state, package_index, "path") != LUA_TSTRING) {
        lua_settop(state, stack_top);
        return;
    }
    current = lua_tolstring(state, -1, &current_length);
    if (!current || current_length < guest_head_length ||
        memcmp(current, guest_filename, guest_directory_length) != 0 ||
        memcmp(current + guest_directory_length, module_pattern,
               sizeof module_pattern - 1U) != 0) {
        lua_settop(state, stack_top);
        return;
    }

    memcpy(native_head, native_filename, native_directory_length);
    memcpy(native_head + native_directory_length, module_pattern,
           sizeof module_pattern);
    tail = current + guest_head_length;
    tail_length = current_length - guest_head_length;
    if (tail_length == sizeof frozen_core_pattern - 1U &&
        memcmp(tail, frozen_core_pattern,
               sizeof frozen_core_pattern - 1U) == 0 &&
        vita_lua_build_core_package_path(
            native_filename, native_core, sizeof native_core,
            &tail_length)) {
        tail = native_core;
    }
    lua_pushlstring(state, native_head, native_head_length);
    lua_pushlstring(state, tail, tail_length);
    lua_concat(state, 2);
    lua_setfield(state, package_index, "path");
    lua_settop(state, stack_top);
}

static void vita_lua_loadfilex_import(CPU *__restrict c)
{
    uint32_t guest_filename = vita_lua_arg(c, 1U);
    const char *filename = NULL;
    char native_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];

    /* luaL_loadfilex(NULL, ...) means stdin.  Preserve that API branch; every
     * actual guest filename crosses the same confined Vita path boundary as
     * fopen/access/remove before vanilla Lua opens it.  Map before acquiring
     * the Lua owner so a non-local mapper fault cannot strand that owner. */
    if (guest_filename) {
        if (!isaac_vita_startup_map_path(
                c, guest_filename, native_path, sizeof native_path))
            return;
        filename = native_path;
    }
    VITA_LUA_BEGIN();
    if (filename) {
        vita_lua_rebase_script_package_path(
            state, (const char *)(uintptr_t)guest_filename, filename);
    }
    c->eax = (uint32_t)luaL_loadfilex(
        state, filename,
        (const char *)(uintptr_t)vita_lua_arg(c, 2U));
#if defined(ISAAC_VITA_LUA_RECEIPT)
    vita_lua_receipt_load(state, "file", filename ? filename : "(stdin)",
                          vita_lua_receipt_file_bytes(filename), (int)c->eax);
#endif
    VITA_LUA_END();
}

#if defined(ISAAC_VITA_LUA_GCSTEP_CLAMP_KB) && ISAAC_VITA_LUA_GCSTEP_CLAMP_KB > 0
/* LuaEngine::Update paces the collector itself (0x4022d9..0x4023c6): every
 * frame it reads LUA_GCCOUNT, keeps a ten-frame ring of deltas and calls
 * lua_gc(L, LUA_GCSTEP, step) where step grows by the average growth while
 * memory rises (cap 10,000 KB), holds for 600 frames, then decays 1 %/frame
 * to a floor of 50 KB.  lua_gc(GCSTEP, data) adds `data` KB of debt and runs
 * luaC_step until it is paid or the cycle completes, so with a mod that
 * allocates every frame (EID: 11 MB heap) the step ramps until the whole heap
 * is marked every frame -- 3-5 ms on a PC, 30-76 ms here (perf:wf-flags-v2,
 * EID on: render p95 76 ms, update p95 31 ms, 14 FPS).  Clamp that one site;
 * Lua's own allocation-debt pacing (pause 200 %) still bounds the heap. */
#define VITA_LUA_ENGINE_GCSTEP_RETURN_RVA 0x004023c6U
static uint32_t s_vita_lua_gcstep_clamps;
static int32_t s_vita_lua_gcstep_max_seen;

static int vita_lua_gcstep_clamp(CPU *__restrict c, int what, int data)
{
    uint32_t stack_address;

    if (what != LUA_GCSTEP || data <= ISAAC_VITA_LUA_GCSTEP_CLAMP_KB)
        return data;
    stack_address = guest_stack_address(c, c->esp, 4U, 0U);
    if (!stack_address ||
        ld32(stack_address) != VITA_LUA_ENGINE_GCSTEP_RETURN_RVA)
        return data;
    if (data > s_vita_lua_gcstep_max_seen)
        s_vita_lua_gcstep_max_seen = data;
    if (s_vita_lua_gcstep_clamps < 3U ||
        (s_vita_lua_gcstep_clamps % 1800U) == 0U)
        isaac_vita_log("[isaac-lua] gcstep clamp: requested=%d KB -> %d KB "
                       "count=%u max=%d",
                       data, (int)ISAAC_VITA_LUA_GCSTEP_CLAMP_KB,
                       (unsigned)s_vita_lua_gcstep_clamps,
                       (int)s_vita_lua_gcstep_max_seen);
    if (s_vita_lua_gcstep_clamps != UINT32_MAX)
        ++s_vita_lua_gcstep_clamps;
    return ISAAC_VITA_LUA_GCSTEP_CLAMP_KB;
}
#endif

static void vita_lua_gc_import(CPU *__restrict c)
{
    KAGE_VITA_DEEP_SCOPE(KVD_LUA_GC);
    int what = (int32_t)vita_lua_arg(c, 1U);
    int data = (int32_t)vita_lua_arg(c, 2U);
#if defined(VITA_LUA_GC_HOOK)
    int requested = data;
#endif
    VITA_LUA_BEGIN();
#if defined(ISAAC_VITA_LUA_GCSTEP_CLAMP_KB) && ISAAC_VITA_LUA_GCSTEP_CLAMP_KB > 0
    data = vita_lua_gcstep_clamp(c, what, data);
#endif
#if defined(VITA_LUA_GC_HOOK)
    /* GCCOLLECT clamp (site 0x3b5870) and GC profile, host_vita_lua_gc.c. */
    c->eax = (uint32_t)isaac_vita_lua_gc_call(c, state, what, requested, data);
#else
    c->eax = (uint32_t)lua_gc(state, what, data);
#endif
    VITA_LUA_END();
}

static void vita_lua_pcallk_import(CPU *__restrict c)
{
    KAGE_VITA_DEEP_SCOPE(KVD_LUA_PCALL);
#if defined(ISAAC_VITA_DEEP_PROFILE)
    uint32_t deep_mark = kage_vita_deep_depth();
#endif
    int callback_mark;
    int require_mark;
    int result;
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    uint32_t gc_depth_mark;
#endif
    if (vita_lua_arg(c, 5U)) {
        guest_fault(c, vita_lua_arg(c, 5U),
                    "non-zero lua_pcallk continuation is not implemented");
        return;
    }
    VITA_LUA_BEGIN();
    callback_mark = s_vita_lua_callback_top;
    require_mark = s_vita_lua_require_top;
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    gc_depth_mark = isaac_vita_lua_gc_profile_depth();
#endif
    result = lua_pcallk(
        state, (int32_t)vita_lua_arg(c, 1U),
        (int32_t)vita_lua_arg(c, 2U),
        vita_lua_index((int32_t)vita_lua_arg(c, 3U)),
        (lua_KContext)(int32_t)vita_lua_arg(c, 4U), NULL);
#if defined(ISAAC_VITA_DEEP_PROFILE)
    /* Lua errors can bypass ordinary C cleanup in callbacks/GC. Discard
     * those abandoned diagnostic scopes, keeping incomplete time explicit. */
    kage_vita_deep_unwind(deep_mark);
#endif
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    /* GC profile (host_vita_lua_gc.c): a Lua error inside lua_gc longjmps
     * past its import frame; restore the nesting depth like the marks. */
    isaac_vita_lua_gc_profile_unwind(gc_depth_mark);
#endif
    if (!vita_lua_recover_callbacks(c, callback_mark)) {
        vita_lua_scope_leave(&scope);
        return;
    }
    if (!vita_lua_recover_requires(c, require_mark)) {
        vita_lua_scope_leave(&scope);
        return;
    }
#if defined(ISAAC_VITA_LUA_RECEIPT)
    vita_lua_receipt_pcall(state, result);
#endif
    c->eax = (uint32_t)result;
    VITA_LUA_END();
}

static void vita_lua_pushinteger_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_pushinteger(state, (lua_Integer)vita_lua_arg_u64(c, 1U));
    VITA_LUA_END();
}

static void vita_lua_pushcclosure_import(CPU *__restrict c)
{
    uint32_t target = vita_lua_arg(c, 1U);
    int upvalues = (int32_t)vita_lua_arg(c, 2U);
    if (!target || upvalues < 0 || upvalues > 254) {
        guest_fault(c, target, "invalid translated lua_pushcclosure arguments");
        return;
    }
    VITA_LUA_BEGIN();
    /* Insert bridge-private target below the guest's existing upvalues. */
    lua_pushinteger(state, (lua_Integer)(uint64_t)target);
    if (upvalues != 0)
        lua_rotate(state, -(upvalues + 1), 1);
    lua_pushcclosure(state, vita_lua_guest_cfunction, upvalues + 1);
    VITA_LUA_END();
}

static void vita_lua_unref_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    luaL_unref(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
               (int32_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_rawgeti_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)lua_rawgeti(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)),
        (lua_Integer)vita_lua_arg_u64(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_createtable_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_createtable(state, (int32_t)vita_lua_arg(c, 1U),
                    (int32_t)vita_lua_arg(c, 2U));
    VITA_LUA_END();
}

static void vita_lua_ref_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    c->eax = (uint32_t)luaL_ref(
        state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static void vita_lua_pushvalue_import(CPU *__restrict c)
{
    VITA_LUA_BEGIN();
    lua_pushvalue(state, vita_lua_index((int32_t)vita_lua_arg(c, 1U)));
    VITA_LUA_END();
}

static const vita_lua_import_entry s_vita_lua_imports[] = {
    { "Lua5.3.3r.dll!lua_close",         vita_lua_close_import },
    { "Lua5.3.3r.dll!lua_rawset",        vita_lua_rawset_import },
    { "Lua5.3.3r.dll!lua_iscfunction",   vita_lua_iscfunction_import },
    { "Lua5.3.3r.dll!lua_getfield",      vita_lua_getfield_import },
    { "Lua5.3.3r.dll!lua_typename",      vita_lua_typename_import },
    { "Lua5.3.3r.dll!lua_toboolean",     vita_lua_toboolean_import },
    { "Lua5.3.3r.dll!lua_setmetatable",  vita_lua_setmetatable_import },
    { "Lua5.3.3r.dll!lua_compare",       vita_lua_compare_import },
    { "Lua5.3.3r.dll!lua_settop",        vita_lua_settop_import },
    { "Lua5.3.3r.dll!luaopen_debug",     vita_lua_debug_import },
    { "Lua5.3.3r.dll!luaL_openlibs",     vita_lua_openlibs_import },
    { "Lua5.3.3r.dll!luaL_tolstring",    vita_lua_tolstring_aux_import },
    { "Lua5.3.3r.dll!luaL_requiref",     vita_lua_requiref_import },
    { "Lua5.3.3r.dll!luaopen_coroutine", vita_lua_coroutine_import },
    { "Lua5.3.3r.dll!luaL_argerror",     vita_lua_argerror_import },
    { "Lua5.3.3r.dll!lua_pushnil",       vita_lua_pushnil_import },
    { "Lua5.3.3r.dll!lua_len",           vita_lua_len_import },
    { "Lua5.3.3r.dll!lua_rawget",        vita_lua_rawget_import },
    { "Lua5.3.3r.dll!lua_gettop",        vita_lua_gettop_import },
    { "Lua5.3.3r.dll!lua_pushnumber",     vita_lua_pushnumber_import },
    { "Lua5.3.3r.dll!luaL_checknumber",  vita_lua_checknumber_import },
    { "Lua5.3.3r.dll!lua_getstack",      vita_lua_getstack_import },
    { "Lua5.3.3r.dll!luaopen_math",      vita_lua_math_import },
    { "Lua5.3.3r.dll!lua_pushstring",    vita_lua_pushstring_import },
    { "Lua5.3.3r.dll!lua_touserdata",    vita_lua_touserdata_import },
    { "Lua5.3.3r.dll!lua_pushfstring",   vita_lua_pushfstring_import },
    { "Lua5.3.3r.dll!lua_atpanic",       vita_lua_atpanic_import },
    { "Lua5.3.3r.dll!lua_getglobal",     vita_lua_getglobal_import },
    { "Lua5.3.3r.dll!lua_isstring",      vita_lua_isstring_import },
    { "Lua5.3.3r.dll!luaopen_string",    vita_lua_string_import },
    { "Lua5.3.3r.dll!lua_callk",         vita_lua_callk_import },
    { "Lua5.3.3r.dll!lua_rawgetp",       vita_lua_rawgetp_import },
    { "Lua5.3.3r.dll!luaL_loadbufferx",  vita_lua_loadbufferx_import },
    { "Lua5.3.3r.dll!lua_tolstring",     vita_lua_tolstring_import },
    { "Lua5.3.3r.dll!lua_isuserdata",    vita_lua_isuserdata_import },
    { "Lua5.3.3r.dll!lua_pushboolean",   vita_lua_pushboolean_import },
    { "Lua5.3.3r.dll!lua_newstate",      vita_lua_newstate_import },
    { "Lua5.3.3r.dll!lua_setglobal",     vita_lua_setglobal_import },
    { "Lua5.3.3r.dll!lua_newuserdata",   vita_lua_newuserdata_import },
    { "Lua5.3.3r.dll!luaL_newstate",     vita_lua_newstate_aux_import },
    { "Lua5.3.3r.dll!luaL_checkinteger", vita_lua_checkinteger_import },
    { "Lua5.3.3r.dll!luaL_error",        vita_lua_error_import },
    { "Lua5.3.3r.dll!lua_rawsetp",       vita_lua_rawsetp_import },
    { "Lua5.3.3r.dll!luaopen_utf8",      vita_lua_utf8_import },
    { "Lua5.3.3r.dll!lua_setfield",      vita_lua_setfield_import },
    { "Lua5.3.3r.dll!lua_gettable",      vita_lua_gettable_import },
    { "Lua5.3.3r.dll!luaopen_base",      vita_lua_base_import },
    { "Lua5.3.3r.dll!lua_type",          vita_lua_type_import },
    { "Lua5.3.3r.dll!lua_rawequal",      vita_lua_rawequal_import },
    { "Lua5.3.3r.dll!lua_rotate",        vita_lua_rotate_import },
    { "Lua5.3.3r.dll!luaL_checklstring", vita_lua_checklstring_import },
    { "Lua5.3.3r.dll!lua_getmetatable",  vita_lua_getmetatable_import },
    { "Lua5.3.3r.dll!lua_absindex",      vita_lua_absindex_import },
    { "Lua5.3.3r.dll!lua_pushlstring",   vita_lua_pushlstring_import },
    { "Lua5.3.3r.dll!lua_getinfo",       vita_lua_getinfo_import },
    { "Lua5.3.3r.dll!lua_copy",          vita_lua_copy_import },
    { "Lua5.3.3r.dll!luaopen_table",     vita_lua_table_import },
    { "Lua5.3.3r.dll!luaL_loadfilex",    vita_lua_loadfilex_import },
    { "Lua5.3.3r.dll!lua_gc",            vita_lua_gc_import },
    { "Lua5.3.3r.dll!lua_pcallk",        vita_lua_pcallk_import },
    { "Lua5.3.3r.dll!lua_pushinteger",   vita_lua_pushinteger_import },
    { "Lua5.3.3r.dll!lua_pushcclosure",  vita_lua_pushcclosure_import },
    { "Lua5.3.3r.dll!luaL_unref",        vita_lua_unref_import },
    { "Lua5.3.3r.dll!lua_rawgeti",       vita_lua_rawgeti_import },
    { "Lua5.3.3r.dll!lua_createtable",   vita_lua_createtable_import },
    { "Lua5.3.3r.dll!luaL_ref",          vita_lua_ref_import },
    { "Lua5.3.3r.dll!lua_pushvalue",     vita_lua_pushvalue_import }
};

_Static_assert(sizeof s_vita_lua_imports / sizeof s_vita_lua_imports[0] ==
                   ISAAC_VITA_LUA_IMPORT_COUNT,
               "frozen Lua import inventory drifted");

const char *isaac_vita_lua_import_name(uint32_t index)
{
    return index < ISAAC_VITA_LUA_IMPORT_COUNT
        ? s_vita_lua_imports[index].name : NULL;
}

int isaac_vita_lua_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count)
{
    if (index >= ISAAC_VITA_LUA_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
#if defined(VITA_LUA_BEAT_ENABLED)
    vita_lua_beat_note_import(c, index);
#endif
    s_vita_lua_imports[index].handler(c);
    return 1;
}

#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
/* ---- ISAAC_VITA_LUA_IMPORT_FASTDISPATCH (wf/opt-lua-direct) begin -------
 * Typed direct endpoints for the thirteen hottest Lua C-API imports (the
 * LuaBridge Userdata::getClass / callback-invoker family: sub_003f8e30,
 * sub_003f95c0, sub_0040eef0; ~2.0 ms/frame of host time under them with EID
 * in the perf:wf-prof-v6 sampler).  Each endpoint has the family signature
 * (guest_import_family_fn), so a generated GUEST_IMPORT_CALL site reaches it
 * with one blx from guest_import_call and guest_call's classified route
 * reaches it through guest_host_import_id (host_vita_import_id.c binds the
 * same pointer for both), bypassing isaac_vita_lua_import_indexed's second
 * indirect call and the generic handler's per-argument noinline
 * guest_stack_address reads (two to four per call) plus gpop_at.
 *
 * Contract, per call and identical to the generic handler: one
 * guest_stack_contains(c, esp, 4 + 4*nargs) covers every slot the generic
 * handler validated individually (return word, state, arguments); on any
 * rejection the call takes the unchanged generic handler, so every fault
 * text, order and CPU side effect on a bad stack is the generic one.  The
 * host call census (++*call_count), the beat receipt, the low-water note of
 * the lowest slot the generic path touched (esp+4), EAX and the popped return
 * word are produced exactly as before; EDX and the x87 ring are untouched by
 * all thirteen APIs in both spellings.
 *
 * Owner word: the Vita runtime has exactly one guest CPU (entry_vita.c; see
 * gl_bridge.c's ISAAC_VITA_GL_SHIM_FASTDISPATCH note -- the beat, sampler,
 * audio and save threads never own a CPU and never enter this bridge), so
 * the root/nested/foreign state machine of vita_lua_scope_enter/leave runs
 * here on plain loads and stores with the same transitions and fault texts;
 * no dmb/ldrex/strex remains on the typed path.  The word itself is shared
 * with the atomic spelling the other 54 handlers keep, so nesting a typed
 * call inside a generic pcall (or a generic import inside a callback raised
 * from a typed pushstring's GC step) sees one consistent owner. */
#define VITA_LUA_FAST_COUNT 13U

/* `active` is the owner word as entered (0 = this call is the root).  The
 * store is unconditional because in the nested case the word already holds
 * this CPU, and leave restores the entered value, so neither path needs a
 * root flag the compiler would tail-duplicate the Lua call over. */
static int vita_lua_fast_scope_enter(CPU *__restrict c, uintptr_t *active)
{
    volatile uintptr_t *word = &s_vita_lua_active_cpu;
    uintptr_t entered = *word;

    *active = entered;
    if (entered != 0U && entered != (uintptr_t)c) {
        guest_fault(c, 0U, "concurrent Lua bridge entry by another CPU");
        return 0;
    }
    *word = (uintptr_t)c;
    return 1;
}

static void vita_lua_fast_scope_leave(CPU *__restrict c, uintptr_t active)
{
    volatile uintptr_t *word = &s_vita_lua_active_cpu;

    /* Root only: the generic leave faults instead of clearing when the word
     * no longer names this CPU (an unwound activation left it behind). */
    if (active == 0U && *word != (uintptr_t)c) {
        guest_fault(c, 0U, "Lua bridge owner changed during call");
        return;
    }
    *word = active;
}

/* Cold: the frame the generic handler would reject (or an unbound CPU) goes
 * to that handler itself so every fault text and order stays the generic one. */
GUEST_STACK_COLD_NOINLINE static int vita_lua_fast_fallback(
    CPU *__restrict c, uint32_t index, unsigned *call_count)
{
    return isaac_vita_lua_import_indexed(c, index, call_count);
}

#if defined(VITA_LUA_BEAT_ENABLED)
#define VITA_LUA_FAST_BEAT(c, index) vita_lua_beat_note_import((c), (index))
#else
#define VITA_LUA_FAST_BEAT(c, index) ((void)0)
#endif
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE)
/* Host oracles only (never a production definition): counts typed bodies
 * that passed the range check, so a differential run can prove it compared
 * the typed spelling and not the fallback. */
unsigned g_isaac_vita_lua_fast_typed_runs;
#define VITA_LUA_FAST_TYPED_RUN() (++g_isaac_vita_lua_fast_typed_runs)
#else
#define VITA_LUA_FAST_TYPED_RUN() ((void)0)
#endif

/* `frame` is the byte span the generic handler reads: the return word plus
 * 4 bytes per argument slot (lua_Integer/lua_Number take two slots). */
#define VITA_LUA_FAST_BEGIN(frame)                                         \
    uint32_t esp = c->esp;                                                 \
    lua_State *state;                                                      \
    uintptr_t active;                                                      \
    if (GUEST_STACK_UNLIKELY(!guest_stack_contains(c, esp, (frame))))      \
        return vita_lua_fast_fallback(c, index, call_count);               \
    VITA_LUA_FAST_TYPED_RUN();                                             \
    if (call_count)                                                        \
        ++*call_count;                                                     \
    VITA_LUA_FAST_BEAT(c, index);                                          \
    guest_stack_note_low(c, esp + 4U);                                     \
    state = (lua_State *)(uintptr_t)ld32(esp + 4U);                        \
    if (!vita_lua_fast_scope_enter(c, &active))                            \
        return 1

/* The generic handler pops the return word through gpop_at after the Lua
 * call; a callback raised from inside the call (pushstring's GC step) that
 * left ESP moved is the only way that pop can observe another address, and
 * then it must run the same checked pop. */
#define VITA_LUA_FAST_END_STORE(store)                                     \
    do {                                                                   \
        vita_lua_fast_scope_leave(c, active);                              \
        store;                                                             \
        if (GUEST_STACK_UNLIKELY(c->esp != esp))                           \
            (void)gpop(c);                                                 \
        else                                                               \
            c->esp = esp + 4U;                                             \
        return 1;                                                          \
    } while (0)
/* Most generic handlers assign EAX before VITA_LUA_END (before the owner
 * release); lua_touserdata and lua_pushstring go through
 * vita_lua_return_pointer, which assigns EAX only after the release
 * succeeded.  The typed spellings keep each import's own order, so the EAX
 * a fault raised by the release leaves behind (reachable when a __gc
 * callback raised from pushstring cleared the owner word) is the generic
 * one -- the hostile differential oracle pins it. */
#define VITA_LUA_FAST_END() VITA_LUA_FAST_END_STORE((void)0)

static int vita_lua_fast_rawgetp(CPU *__restrict c, uint32_t index,
                                 unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(16U);
    c->eax = (uint32_t)lua_rawgetp(
        state, vita_lua_index((int32_t)ld32(esp + 8U)),
        (const void *)(uintptr_t)ld32(esp + 12U));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_getmetatable(CPU *__restrict c, uint32_t index,
                                      unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(12U);
    c->eax = (uint32_t)lua_getmetatable(
        state, vita_lua_index((int32_t)ld32(esp + 8U)));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_type(CPU *__restrict c, uint32_t index,
                              unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(12U);
    c->eax = (uint32_t)lua_type(
        state, vita_lua_index((int32_t)ld32(esp + 8U)));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_touserdata(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count)
{
    void *result;
    VITA_LUA_FAST_BEGIN(12U);
    result = lua_touserdata(state, vita_lua_index((int32_t)ld32(esp + 8U)));
    VITA_LUA_FAST_END_STORE(c->eax = (uint32_t)(uintptr_t)result);
}

static int vita_lua_fast_pushvalue(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(12U);
    lua_pushvalue(state, vita_lua_index((int32_t)ld32(esp + 8U)));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_rawget(CPU *__restrict c, uint32_t index,
                                unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(12U);
    c->eax = (uint32_t)lua_rawget(
        state, vita_lua_index((int32_t)ld32(esp + 8U)));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_rawgeti(CPU *__restrict c, uint32_t index,
                                 unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(20U);
    c->eax = (uint32_t)lua_rawgeti(
        state, vita_lua_index((int32_t)ld32(esp + 8U)),
        (lua_Integer)ld64(esp + 12U));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_settop(CPU *__restrict c, uint32_t index,
                                unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(12U);
    lua_settop(state, vita_lua_index((int32_t)ld32(esp + 8U)));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_gettop(CPU *__restrict c, uint32_t index,
                                unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(8U);
    c->eax = (uint32_t)lua_gettop(state);
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_pushnil(CPU *__restrict c, uint32_t index,
                                 unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(8U);
    lua_pushnil(state);
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_pushinteger(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(16U);
    lua_pushinteger(state, (lua_Integer)ld64(esp + 8U));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_pushnumber(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count)
{
    VITA_LUA_FAST_BEGIN(16U);
    lua_pushnumber(state, ldd(esp + 8U));
    VITA_LUA_FAST_END();
}

static int vita_lua_fast_pushstring(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count)
{
    const char *result;
    VITA_LUA_FAST_BEGIN(12U);
    result = lua_pushstring(state, (const char *)(uintptr_t)ld32(esp + 8U));
    VITA_LUA_FAST_END_STORE(c->eax = (uint32_t)(uintptr_t)result);
}

#undef VITA_LUA_FAST_BEGIN
#undef VITA_LUA_FAST_END
#undef VITA_LUA_FAST_END_STORE
#undef VITA_LUA_FAST_BEAT
#undef VITA_LUA_FAST_TYPED_RUN

typedef struct vita_lua_fast_thunk {
    const char *name;
    guest_import_family_fn fn;
} vita_lua_fast_thunk;

/* Bound by import name against the frozen inventory above at registration,
 * never by a positional local index, so an inventory edit cannot silently
 * retarget a typed endpoint. */
static const vita_lua_fast_thunk s_vita_lua_fast_thunks[VITA_LUA_FAST_COUNT] = {
    { "Lua5.3.3r.dll!lua_rawgetp",      vita_lua_fast_rawgetp },
    { "Lua5.3.3r.dll!lua_getmetatable", vita_lua_fast_getmetatable },
    { "Lua5.3.3r.dll!lua_type",         vita_lua_fast_type },
    { "Lua5.3.3r.dll!lua_touserdata",   vita_lua_fast_touserdata },
    { "Lua5.3.3r.dll!lua_pushvalue",    vita_lua_fast_pushvalue },
    { "Lua5.3.3r.dll!lua_rawget",       vita_lua_fast_rawget },
    { "Lua5.3.3r.dll!lua_rawgeti",      vita_lua_fast_rawgeti },
    { "Lua5.3.3r.dll!lua_settop",       vita_lua_fast_settop },
    { "Lua5.3.3r.dll!lua_gettop",       vita_lua_fast_gettop },
    { "Lua5.3.3r.dll!lua_pushnil",      vita_lua_fast_pushnil },
    { "Lua5.3.3r.dll!lua_pushinteger",  vita_lua_fast_pushinteger },
    { "Lua5.3.3r.dll!lua_pushnumber",   vita_lua_fast_pushnumber },
    { "Lua5.3.3r.dll!lua_pushstring",   vita_lua_fast_pushstring }
};

guest_import_family_fn
    g_isaac_vita_lua_import_fast_by_index[ISAAC_VITA_LUA_IMPORT_COUNT];

uint32_t isaac_vita_lua_import_fast_prepare(int enable)
{
    uint32_t bound = 0U;
    uint32_t i, j;

    memset(g_isaac_vita_lua_import_fast_by_index, 0,
           sizeof g_isaac_vita_lua_import_fast_by_index);
    if (!enable)
        return 0U;
    for (i = 0U; i < VITA_LUA_FAST_COUNT; ++i) {
        for (j = 0U; j < ISAAC_VITA_LUA_IMPORT_COUNT; ++j) {
            if (strcmp(s_vita_lua_imports[j].name,
                       s_vita_lua_fast_thunks[i].name) != 0)
                continue;
            g_isaac_vita_lua_import_fast_by_index[j] =
                s_vita_lua_fast_thunks[i].fn;
            ++bound;
            break;
        }
    }
    if (bound != VITA_LUA_FAST_COUNT) {
        /* An inventory rename would leave a typed endpoint unbound: fail
         * closed to the generic handlers for every import. */
        memset(g_isaac_vita_lua_import_fast_by_index, 0,
               sizeof g_isaac_vita_lua_import_fast_by_index);
        return 0U;
    }
    return bound;
}
/* ---- ISAAC_VITA_LUA_IMPORT_FASTDISPATCH end ----------------------------- */
#endif

void isaac_vita_lua_abort_cpu(CPU *c)
{
    uint32_t i;
    uintptr_t expected;
    for (i = 0U; i < VITA_LUA_MAX_CALLBACK_DEPTH; ++i) {
        if (s_vita_lua_callbacks[i].used &&
            s_vita_lua_callbacks[i].cpu == c)
            s_vita_lua_callbacks[i].used = 0;
    }
    for (i = 0U; i < VITA_LUA_MAX_REQUIRE_DEPTH; ++i) {
        if (s_vita_lua_requires[i].used &&
            s_vita_lua_requires[i].cpu == c)
            s_vita_lua_requires[i].used = 0;
    }
    /* A fault during lua_newstate can escape before the partially-created
     * state is registered.  Only those unbound contexts are safe to release;
     * live states retain their allocator across later guest runs. */
    for (i = 0U; i < VITA_LUA_MAX_STATES; ++i) {
        if (s_vita_lua_allocators[i].used &&
            s_vita_lua_allocators[i].cpu == c &&
            !s_vita_lua_allocators[i].state)
            memset(&s_vita_lua_allocators[i], 0,
                   sizeof s_vita_lua_allocators[i]);
    }
    s_vita_lua_callback_top = -1;
    s_vita_lua_require_top = -1;
    expected = (uintptr_t)c;
    (void)vita_lua_active_compare_exchange(&expected, 0U);
}

#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
/* ---- native getClass/getExact seam owner (host_vita_lua_getclass.c) -----
 * The seam runs Lua API calls on the game thread outside any import handler,
 * so it holds the same process-wide bridge owner an import holds: a GC step
 * reached through lua_pushstring can enter a guest __gc closure, and
 * vita_lua_call_guest reads the active CPU from that slot.  Same protocol as
 * vita_lua_scope_enter/leave -- the outermost activation owns and releases
 * the slot -- minus the contention fault: the seam falls back instead and the
 * translated body's first import raises the established fault text. */
int isaac_vita_lua_seam_enter(CPU *__restrict c, int *root)
{
    uintptr_t expected = 0U;
    *root = 0;
#if ISAAC_VITA_LUA_SCOPE_FASTPATH
    /* Same nested-owner proof as vita_lua_scope_enter above: getClass and
     * getExact commonly re-enter from an import-owned callback. Only c's
     * own thread publishes c, so observing that owner needs no failing CAS.
     * Keep NULL on the original path: CAS(0, 0) succeeds with root=1. */
    if (c != NULL && vita_lua_active_load_relaxed() == (uintptr_t)c)
        return 1;
#endif
    if (vita_lua_active_compare_exchange(&expected, (uintptr_t)c)) {
        *root = 1;
        return 1;
    }
    return expected == (uintptr_t)c;
}

void isaac_vita_lua_seam_leave(CPU *__restrict c, int root)
{
    vita_lua_scope scope;
    scope.cpu = c;
    scope.root = root;
    vita_lua_scope_leave(&scope);
}
#endif

#undef VITA_LUA_OPEN_IMPORT
#undef VITA_LUA_BEGIN
#undef VITA_LUA_END
