/* Generic guest-stack adapters for the generated OpenGL surface.
 *
 * Platform code supplies typed callbacks through guest_gl_install_backend().
 * This file owns x86 argument decoding, stdcall cleanup, return registers,
 * exact name/token lookup, and loud missing-backend behaviour.  It never asks
 * the native dynamic loader for an untyped function pointer.
 */
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
# include <intrin.h>
#endif

#include "gl_bridge.h"

#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
/* ---- ISAAC_VITA_GL_WRAPPER_TIME (wf/cpu-render-20260906) begin ----------
 * Wrapper entry/return clock around every adapter run (guest_gl_run_owned);
 * bucket per registry entry, classified once at backend install by the
 * frozen name (gl_vita_backend.h legend).  The clock is the one ph120.gt
 * uses (sceKernelGetProcessTimeWide), so w minus gt is subtractable. */
# if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
#  error ISAAC_VITA_GL_WRAPPER_TIME needs the entry-indexed fast dispatch (ISAAC_VITA_GL_SHIM_FASTDISPATCH)
# endif
# include "gl_vita_backend.h"
# if defined(__vita__)
#  include <psp2/kernel/processmgr.h>
# else
uint64_t sceKernelGetProcessTimeWide(void);
# endif
IsaacVitaGlWrapperTimeProfile g_isaac_vita_gl_wrapper_time;
uint64_t g_isaac_vita_gl_wrapper_entry_at;
uint64_t g_isaac_vita_gl_draw_body_ended_at;
static uint8_t s_guest_gl_wrapper_bucket[GUEST_GL_SURFACE_COUNT];
static uint8_t s_guest_gl_uniform_kind[GUEST_GL_SURFACE_COUNT];
static inline uint64_t guest_gl_wrapper_time_now(void)
{
    return sceKernelGetProcessTimeWide();
}
/* ---- ISAAC_VITA_GL_WRAPPER_TIME end ------------------------------------ */
#endif

typedef void (*guest_gl_adapter_fn)(CPU *__restrict);

typedef struct guest_gl_entry {
    uint32_t             token;
    const char          *name;
    uint16_t             x86_stack_bytes;
    uint8_t              argument_count;
    guest_gl_return_kind return_kind;
    guest_gl_adapter_fn  adapter;
} guest_gl_entry;

static guest_gl_backend s_guest_gl_backend;
static uintptr_t s_guest_gl_active_state;

#define GUEST_GL_ACTIVE_IDLE    ((uintptr_t)0u)
#define GUEST_GL_ACTIVE_FAULTED ((uintptr_t)1u)

#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
/* ISAAC_VITA_GL_SHIM_FASTDISPATCH: the Vita runtime has exactly one guest CPU
 * (entry_vita.c isaac_vita_run_first_fault's `cpu`, bound to the main thread)
 * and every typed GL entry is a guest_call from translated code on that CPU:
 * the audio worker (core 2), the native-vorbis worker, the async save writer,
 * the Lua beat thread and the diagnostic sampler/stall threads never own a
 * CPU and never reach gl_bridge (vitaGL itself is main-thread-only).  The
 * ownership word is therefore a plain load/store state machine with the same
 * IDLE/FAULTED/owner transitions as the atomic version below, and the first
 * dispatching CPU is pinned: any other CPU pointer entering later is the
 * fail-closed "foreign thread" trap the atomic protocol only detected by
 * accident.  No dmb/ldrex/strex remains on the per-call path. */
static CPU *s_guest_gl_owner_cpu;

static inline uintptr_t guest_gl_active_load(void)
{
    return s_guest_gl_active_state;
}

/* Same result contract as the atomic compare/exchange: one when the word
 * held *expected and now holds desired, else zero with *expected updated. */
static inline int guest_gl_active_compare_exchange(
    uintptr_t *expected, uintptr_t desired)
{
    if (s_guest_gl_active_state != *expected) {
        *expected = s_guest_gl_active_state;
        return 0;
    }
    s_guest_gl_active_state = desired;
    return 1;
}

static inline const guest_gl_entry *guest_gl_entry_for_token(uint32_t token);
#else
/* Dispatch is process-global because native GL contexts are thread-affine.
 * The compare/exchange guard rejects nesting and accidental concurrent use;
 * it is not a scheduler and deliberately does not move work between threads.
 * GCC emits lock-free ARMv7 ldrex/strex for this pointer-sized operation, and
 * MSVC uses the corresponding pointer intrinsics. */
static uintptr_t guest_gl_active_load(void)
{
#if defined(_MSC_VER)
    return (uintptr_t)_InterlockedCompareExchangePointer(
        (void *volatile *)&s_guest_gl_active_state, NULL, NULL);
#elif defined(__GNUC__)
    return __atomic_load_n(&s_guest_gl_active_state, __ATOMIC_ACQUIRE);
#else
    return s_guest_gl_active_state;
#endif
}

static int guest_gl_active_compare_exchange(
    uintptr_t *expected, uintptr_t desired)
{
#if defined(_MSC_VER)
    uintptr_t observed = (uintptr_t)_InterlockedCompareExchangePointer(
        (void *volatile *)&s_guest_gl_active_state,
        (void *)desired, (void *)*expected);
    if (observed == *expected)
        return 1;
    *expected = observed;
    return 0;
#elif defined(__GNUC__)
    return __atomic_compare_exchange_n(
        &s_guest_gl_active_state, expected, desired, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#else
    if (s_guest_gl_active_state != *expected) {
        *expected = s_guest_gl_active_state;
        return 0;
    }
    s_guest_gl_active_state = desired;
    return 1;
#endif
}
#endif /* ISAAC_VITA_GL_SHIM_FASTDISPATCH */

#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
static int guest_gl_active_acquire(CPU *cpu, uintptr_t *observed)
{
    uintptr_t expected = GUEST_GL_ACTIVE_IDLE;

    if (guest_gl_active_compare_exchange(&expected, (uintptr_t)cpu))
        return 1;
    if (expected == GUEST_GL_ACTIVE_FAULTED &&
            guest_gl_active_compare_exchange(
                &expected, (uintptr_t)cpu))
        return 1;
    if (observed)
        *observed = expected;
    return 0;
}
#endif

static uint32_t guest_gl_arg_u32(CPU *__restrict c, uint32_t byte_offset)
{
    /* ESP points at the guest return address during an APIENTRY call. */
    return ld32(guest_stack_address(
        c, c->esp + 4u + byte_offset, 4U, 0U));
}

static guest_gl_float guest_gl_arg_f32(
    CPU *__restrict c, uint32_t byte_offset)
{
    uint32_t bits = guest_gl_arg_u32(c, byte_offset);
    guest_gl_float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static guest_gl_double guest_gl_arg_f64(
    CPU *__restrict c, uint32_t byte_offset)
{
    uint64_t bits = (uint64_t)guest_gl_arg_u32(c, byte_offset) |
                    ((uint64_t)guest_gl_arg_u32(c, byte_offset + 4u) << 32);
    guest_gl_double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void guest_gl_stdcall_return(
    CPU *__restrict c, uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void guest_gl_unsupported(
    CPU *__restrict c, uint32_t token, const char *message)
{
    uintptr_t expected = (uintptr_t)c;

    (void)guest_gl_active_compare_exchange(
        &expected, GUEST_GL_ACTIVE_FAULTED);
    guest_fault(c, token, message);
}

#if defined(ISAAC_GL_SHIM_FASTDISPATCH_ORACLE)
/* Oracle only (recomp/gl_shim_fastdispatch_oracle.c): a recording
 * guest_fault that returns out of an adapter -- a stack fault while decoding
 * the frame, which production never survives -- leaves the owner word at
 * the CPU; the differential oracle clears it between its short-frame cases. */
void guest_gl_oracle_reset_active(void)
{
    s_guest_gl_active_state = GUEST_GL_ACTIVE_IDLE;
}
#endif

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS) && \
    !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
# error ISAAC_VITA_GL_SHIM_TABLE_TOKENS requires ISAAC_VITA_GL_SHIM_FASTDISPATCH
#endif
#if defined(ISAAC_VITA_GL_SHIM_RAW_ARGS) && \
    !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
# error ISAAC_VITA_GL_SHIM_RAW_ARGS requires ISAAC_VITA_GL_SHIM_FASTDISPATCH
#endif

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS) || \
    defined(ISAAC_VITA_GL_SHIM_RAW_ARGS)
#if defined(__GNUC__) || defined(__clang__)
# define GUEST_GL_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
# define GUEST_GL_NOINLINE __declspec(noinline)
#else
# define GUEST_GL_NOINLINE
#endif
#endif
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* Defined after guest_gl_run_owned; the generated trampolines call it. */
static GUEST_GL_NOINLINE void guest_gl_table_dispatch(
    CPU *__restrict c, const guest_gl_entry *entry);
#endif

#if defined(ISAAC_VITA_GL_SHIM_RAW_ARGS)
/* ---- ISAAC_VITA_GL_SHIM_RAW_ARGS (wf/opt-gltok) begin --------------------
 * The generated adapters (gl_surface_generated.inc, RAW_ARGS arm) validate
 * the whole stdcall frame once instead of entering the noinline validator
 * for every argument word and again for the return word and the cleanup.
 * For a frame of frame_bytes (return word + argument bytes) the per-word
 * checks the legacy adapter makes -- guest_stack_address(esp + 4 + 4k, 4)
 * for each argument word, then gpop_at(esp) and
 * guest_stack_adjust(argument_bytes) -- each say "this word lies inside the
 * bound stack", and their conjunction is exactly
 * offset + frame_bytes <= capacity with offset = esp - floor, evaluated
 * below with the same wrap-safe arithmetic.  A frame that passes reads its
 * words with plain ld32 and retires with one esp store; a frame that fails
 * takes the legacy sequence unchanged, so the first rejected word faults
 * with the same kind, address, size and text (gl_shim_fastdispatch_oracle.c
 * phase 9 sweeps every argument position of every token plus a floor cut
 * under the return word).  The retirement stores esp once when esp is still
 * the checked frame (the validated [esp, esp + 4 + argument bytes) is what
 * leaves the stack, and its end is inside the ceiling); a nested dispatch
 * that moved esp -- backends never receive the CPU, the oracle's callback
 * case restores it -- takes the legacy gpop + adjust pair. */
static inline int guest_gl_frame_bound(const CPU *c, uint32_t frame_bytes)
{
    uint32_t capacity, offset;

    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return 0;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    return offset <= capacity && frame_bytes <= capacity - offset;
}

#if defined(ISAAC_GL_SHIM_FASTDISPATCH_ORACLE)
/* Oracle census only: frames the fast check accepted. */
unsigned g_guest_gl_oracle_raw_frames;
#endif

/* Non-zero when the whole frame (return word + argument bytes at esp) is
 * inside the bound stack; mirrors the low-water note the first legacy
 * argument read (esp + 4) would have made. */
static inline int guest_gl_frame_ok(CPU *__restrict c, uint32_t argument_bytes)
{
    if (!guest_gl_frame_bound(c, 4u + argument_bytes))
        return 0;
#if defined(ISAAC_GL_SHIM_FASTDISPATCH_ORACLE)
    ++g_guest_gl_oracle_raw_frames;
#endif
    guest_stack_note_low(c, c->esp + 4u);
    return 1;
}

static inline guest_gl_float guest_gl_f32_from_bits(uint32_t bits)
{
    guest_gl_float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline guest_gl_double guest_gl_f64_from_bits(uint32_t low,
                                                     uint32_t high)
{
    uint64_t bits = (uint64_t)low | ((uint64_t)high << 32);
    guest_gl_double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* stdcall retirement: while esp is still the frame guest_gl_frame_ok
 * validated (`frame` = that esp + 4), the return word and the arguments
 * leave the stack in one esp store whose result, frame + argument bytes, the
 * check already placed inside the ceiling; otherwise the legacy gpop +
 * guest_stack_adjust pair runs and faults where it faults. */
static inline void guest_gl_stdcall_return_raw(CPU *__restrict c,
                                               uint32_t frame,
                                               uint32_t argument_bytes)
{
    if (c->esp + 4u == frame) {
        c->esp = frame + argument_bytes;
        return;
    }
    guest_gl_stdcall_return(c, argument_bytes);
}
/* ---- ISAAC_VITA_GL_SHIM_RAW_ARGS end ------------------------------ */
#endif

#include "gl_surface_generated.inc"

#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
/* O(1) exact lookup: multiplicative slot -> generated index -> exact token
 * compare.  Returns NULL for every token outside the frozen registry, which
 * is precisely the set for which the linear scan of s_guest_gl_symbols and
 * the default case of guest_gl_dispatch_generated answer "not ours". */
static inline const guest_gl_entry *guest_gl_entry_for_token(uint32_t token)
{
    uint32_t index = s_guest_gl_token_index[GUEST_GL_TOKEN_SLOT(token)];
    const guest_gl_entry *entry;

    if (!index)
        return NULL;
    entry = &s_guest_gl_entries[index - 1u];
    return entry->token == token ? entry : NULL;
}

int guest_gl_token_is_registered(uint32_t token)
{
    return guest_gl_entry_for_token(token) != NULL;
}

_Static_assert(sizeof s_guest_gl_entries / sizeof s_guest_gl_entries[0] ==
               GUEST_GL_SURFACE_COUNT &&
               GUEST_GL_SURFACE_COUNT < 256u,
               "token index bytes must address every generated entry");
#elif defined(ISAAC_GL_SHIM_FASTDISPATCH_ORACLE)
/* Legacy spelling, compiled only for recomp/gl_shim_fastdispatch_oracle.c:
 * a production build without the option is instruction-identical to the
 * base revision (recomp/vita/test_gl_shim_fastdispatch.sh off-identity). */
int guest_gl_token_is_registered(uint32_t token)
{
    size_t i;

    for (i = 0u; i < GUEST_GL_SURFACE_COUNT; ++i)
        if (s_guest_gl_symbols[i].token == token)
            return 1;
    return 0;
}
#endif

void guest_gl_install_backend(const guest_gl_backend *backend)
{
    if (backend)
        s_guest_gl_backend = *backend;
    else
        memset(&s_guest_gl_backend, 0, sizeof s_guest_gl_backend);
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Entry index -> bucket, from the frozen names; install-time only. */
    if (backend) {
        size_t i;

        for (i = 0u; i < GUEST_GL_SURFACE_COUNT; ++i) {
            s_guest_gl_wrapper_bucket[i] =
                isaac_vita_gl_wrapper_bucket_for_name(
                    s_guest_gl_entries[i].name);
            s_guest_gl_uniform_kind[i] =
                isaac_vita_gl_uniform_kind_for_name(
                    s_guest_gl_entries[i].name);
        }
    }
#endif
#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
    /* A backend teardown is the only legitimate owner change: the next
     * dispatching CPU re-pins.  Nothing else ever clears the pin. */
    if (!backend)
        s_guest_gl_owner_cpu = NULL;
#endif
}

uint32_t guest_gl_resolve(const char *name)
{
    size_t lo = 0u;
    size_t hi = sizeof s_guest_gl_entries / sizeof s_guest_gl_entries[0];
    if (!name)
        return 0u;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        int relation = strcmp(name, s_guest_gl_entries[mid].name);
        if (relation == 0)
            return s_guest_gl_entries[mid].token;
        if (relation > 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return 0u;
}

uint32_t guest_gl_resolve_guest(
    uint32_t guest_name,
    char copied_name[GUEST_GL_PROCEDURE_NAME_MAX + 1U])
{
    uint32_t index;

    if (!copied_name)
        return 0u;
    copied_name[0] = '\0';
    /* Reject NULL and ordinal-shaped values before the first guest read. */
    if (!guest_name || (guest_name >> 16u) == 0u)
        return 0u;

    for (index = 0u; index <= GUEST_GL_PROCEDURE_NAME_MAX; ++index) {
        uint8_t value;
        if (guest_name > UINT32_MAX - index) {
            copied_name[0] = '\0';
            return 0u;
        }
        value = ld8(guest_name + index);
        if (!value) {
            copied_name[index] = '\0';
            return index ? guest_gl_resolve(copied_name) : 0u;
        }
        /* GL entry points are printable ASCII.  Reject control/high bytes and
         * an unterminated 64-byte spelling instead of logging a partial name. */
        if (value < 0x20u || value > 0x7eu ||
                index == GUEST_GL_PROCEDURE_NAME_MAX) {
            copied_name[0] = '\0';
            return 0u;
        }
        copied_name[index] = (char)value;
    }
    copied_name[0] = '\0';
    return 0u;
}

int guest_gl_resolve_provider(
    CPU *__restrict c,
    char copied_name[GUEST_GL_PROCEDURE_NAME_MAX + 1U])
{
    uint32_t token;

    if (!c)
        return 0;
    token = guest_gl_resolve_guest(c->ecx, copied_name);
    if (!token)
        return 0;
    c->eax = token;
    (void)gpop(c);
    return 1;
}

uint32_t guest_gl_backend_return_rva(void)
{
#define GUEST_GL_FROZEN_IMAGE_SIZE 0x0085f000u
    uintptr_t active = guest_gl_active_load();
    CPU *cpu;
    uint32_t address;
    uint32_t return_address;

    if (active <= GUEST_GL_ACTIVE_FAULTED)
        return 0u;
    cpu = (CPU *)active;
    address = guest_stack_address(cpu, cpu->esp, 4u, 0u);
    if (!address)
        return 0u;
    return_address = ld32(address);
    /* Generated CALL lowering pushes canonical RVAs, not relocated image
     * addresses.  Real guest pointers can still arrive relocated, so accept
     * exactly either representation and normalize only at this boundary.
     * The subtraction range check is deliberately retained for wrap-safe VA
     * validation; every other 32-bit word fails closed to zero. */
    if (return_address < GUEST_GL_FROZEN_IMAGE_SIZE)
        return return_address;
    if (return_address - (uint32_t)GUEST_IMAGE_BASE >=
            GUEST_GL_FROZEN_IMAGE_SIZE)
        return 0u;
    return return_address - (uint32_t)GUEST_IMAGE_BASE;
#undef GUEST_GL_FROZEN_IMAGE_SIZE
}

#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
/* The ownership protocol of the legacy compare/exchange version, spelled as
 * plain loads and stores on the single GL thread (see the file header note):
 * nested entry faults (same CPU: pointer-free sentinel first), the first CPU
 * to dispatch is pinned and any other CPU is a fail-closed fault, the adapter
 * runs with the word set to the CPU, and the word returns to IDLE from either
 * the CPU or the FAULTED sentinel a returning oracle fault leaves behind.
 * `entry` NULL is the legacy "unknown token" pass through the switch
 * default: nothing is dispatched, the word returns to IDLE (consuming a
 * returning oracle's FAULTED sentinel exactly as the legacy acquire did) and
 * the CPU pin is not taken, so a stray token cannot claim the GL context. */
static inline int guest_gl_run_owned(
    CPU *__restrict c, uint32_t token, const guest_gl_entry *entry)
{
    uintptr_t active = s_guest_gl_active_state;

    if (active > GUEST_GL_ACTIVE_FAULTED) {
        /* A nested dispatch using the same CPU unwinds the whole active guest
         * scope, so replace its pointer with a pointer-free fault sentinel
         * before the non-local guest fault.  A genuinely concurrent caller
         * faults its own CPU and leaves the current owner intact. */
        if (active == (uintptr_t)c)
            s_guest_gl_active_state = GUEST_GL_ACTIVE_FAULTED;
        guest_fault(c, token, "nested or concurrent guest GL dispatch");
        return 1;
    }
    if (!entry) {
        s_guest_gl_active_state = GUEST_GL_ACTIVE_IDLE;
        return 0;
    }
    /* Fail-closed thread pin: the first CPU to dispatch owns the GL context
     * for the lifetime of the installed backend. */
    if (s_guest_gl_owner_cpu != c) {
        if (s_guest_gl_owner_cpu) {
            guest_fault(c, token,
                        "guest GL dispatch from a foreign CPU/thread");
            return 1;
        }
        s_guest_gl_owner_cpu = c;
    }
    s_guest_gl_active_state = (uintptr_t)c;
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    {
        /* One clock read before the adapter, one after: argument decode,
         * the typed wrapper (with its gt bracket inside) and the stdcall
         * retirement are the wrapper cost the guest sampler sees as `ext`. */
        uint64_t wrapper_started_at = guest_gl_wrapper_time_now();
        uint64_t wrapper_ended_at;
        size_t index = (size_t)(entry - s_guest_gl_entries);

        /* The draw wrapper charges its ph120.gd `disp` from this read. */
        g_isaac_vita_gl_wrapper_entry_at = wrapper_started_at;
        entry->adapter(c);
        wrapper_ended_at = guest_gl_wrapper_time_now();
        isaac_vita_gl_wrapper_time_add(
            &g_isaac_vita_gl_wrapper_time.wrapper[
                s_guest_gl_wrapper_bucket[index]],
            wrapper_started_at, wrapper_ended_at);
        /* Same two reads, finer keys: the draw tail (gt END read -> here)
         * and the ph120.gu uniform entry point; no further clock. */
        if (s_guest_gl_wrapper_bucket[index] == ISAAC_VITA_GL_WRAPPER_DRAW)
            isaac_vita_gl_wrapper_time_add(
                &g_isaac_vita_gl_wrapper_time.draw[
                    ISAAC_VITA_GL_DRAW_SPLIT_TAIL],
                g_isaac_vita_gl_draw_body_ended_at, wrapper_ended_at);
        else if (s_guest_gl_uniform_kind[index] !=
                 ISAAC_VITA_GL_UNIFORM_NONE)
            isaac_vita_gl_wrapper_time_add(
                &g_isaac_vita_gl_wrapper_time.uniform[
                    s_guest_gl_uniform_kind[index]],
                wrapper_started_at, wrapper_ended_at);
    }
#else
    entry->adapter(c);
#endif
    active = s_guest_gl_active_state;
    if (active == (uintptr_t)c || active == GUEST_GL_ACTIVE_FAULTED) {
        /* Production guest_fault never returns.  Recording oracles may
         * return; consume their fault sentinel without overwriting the exact
         * first fault. */
        s_guest_gl_active_state = GUEST_GL_ACTIVE_IDLE;
        return 1;
    }
    guest_fault(c, token, "guest GL dispatch ownership changed");
    return 1;
}

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* ---- ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok) begin ----------------
 * Target of a dispatch-table hit for a registry token; the generated
 * guest_gl_table_<name> trampolines bind the entry.  guest.c places a token
 * in the table only while g_gl_dynamic_first holds, i.e. exactly when
 * guest_call_slow's 0x7e block would have taken it; that block noted the
 * probe miss (dispatch_slow), then isaac_vita_gl_dynamic_counted ->
 * guest_gl_dispatch_counted re-derived registry membership, counted the
 * dynamic call and ran the owned adapter.  Membership is already decided by
 * the exact key compare in the probe, so what remains is the census (the
 * dispatch_gl counter takes the place of the dispatch_slow increment; the
 * dynamic-call counter is the same word) and the same ownership-guarded
 * adapter run, nested/foreign faults included. */
static GUEST_GL_NOINLINE void guest_gl_table_dispatch(
    CPU *__restrict c, const guest_gl_entry *entry)
{
    GUEST_PHASE_PROFILE_NOTE_DISPATCH_GL();
    ++g_host_dynamic_calls;
    (void)guest_gl_run_owned(c, entry->token, entry);
}

uint32_t guest_gl_table_tokens(const uint32_t **tokens,
                               const guest_fn **functions)
{
    if (tokens)
        *tokens = s_guest_gl_table_tokens;
    if (functions)
        *functions = s_guest_gl_table_functions;
    return GUEST_GL_SURFACE_COUNT;
}
/* ---- ISAAC_VITA_GL_SHIM_TABLE_TOKENS end ---------------------------- */
#endif

int guest_gl_dispatch(CPU *__restrict c, uint32_t token)
{
    if (!c)
        return 0;
    /* Ownership is examined before the registry verdict is acted on, exactly
     * as the atomic acquire ran before guest_gl_dispatch_generated. */
    return guest_gl_run_owned(c, token, guest_gl_entry_for_token(token));
}

int guest_gl_dispatch_counted(CPU *__restrict c, uint32_t token,
                              unsigned *call_count)
{
    const guest_gl_entry *entry;

    if (!c)
        return 0;
    /* One lookup answers what the legacy path asked twice (the 73-entry
     * scan in host_vita_gl.c, then the 73-case switch).  An unregistered
     * token leaves the CPU, the counter and the ownership word untouched. */
    entry = guest_gl_entry_for_token(token);
    if (!entry)
        return 0;
    if (call_count)
        ++*call_count;
    (void)guest_gl_run_owned(c, token, entry);
    return 1;
}
#else
int guest_gl_dispatch(CPU *__restrict c, uint32_t token)
{
    uintptr_t expected;
    uintptr_t observed = GUEST_GL_ACTIVE_IDLE;
    int dispatched;

    if (!c)
        return 0;
    if (!guest_gl_active_acquire(c, &observed)) {
        /* A nested dispatch using the same CPU unwinds the whole active guest
         * scope, so replace its pointer with a pointer-free fault sentinel
         * before the non-local guest fault.  A genuinely concurrent caller
         * faults its own CPU and leaves the current owner intact. */
        if (observed == (uintptr_t)c) {
            expected = (uintptr_t)c;
            (void)guest_gl_active_compare_exchange(
                &expected, GUEST_GL_ACTIVE_FAULTED);
        }
        guest_fault(c, token, "nested or concurrent guest GL dispatch");
        return 1;
    }
    dispatched = guest_gl_dispatch_generated(c, token);
    expected = (uintptr_t)c;
    if (guest_gl_active_compare_exchange(
            &expected, GUEST_GL_ACTIVE_IDLE))
        return dispatched;
    /* Production guest_fault never returns.  Recording oracles may return;
     * consume their fault sentinel without overwriting the exact first fault. */
    expected = GUEST_GL_ACTIVE_FAULTED;
    if (guest_gl_active_compare_exchange(
            &expected, GUEST_GL_ACTIVE_IDLE))
        return dispatched;
    guest_fault(c, token, "guest GL dispatch ownership changed");
    return 1;
}

#if defined(ISAAC_GL_SHIM_FASTDISPATCH_ORACLE)
int guest_gl_dispatch_counted(CPU *__restrict c, uint32_t token,
                              unsigned *call_count)
{
    /* The historical host_vita_gl.c sequence, spelled here only for the
     * differential oracle (production legacy builds keep it inline in
     * host_vita_gl.c): exact membership first (no side effect for a foreign
     * token), count before the handler, then the guarded dispatch. */
    if (!c)
        return 0;
    if (!guest_gl_token_is_registered(token))
        return 0;
    if (call_count)
        ++*call_count;
    if (!guest_gl_dispatch(c, token)) {
        guest_fault(c, token,
                    "typed Vita GL token was rejected by gl_bridge");
    }
    return 1;
}
#endif
#endif /* ISAAC_VITA_GL_SHIM_FASTDISPATCH */

#if defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH (wf/opt-attrib) begin ---------- */
#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
#error ISAAC_VITA_SHADER_ATTRIB_FASTPATH requires ISAAC_VITA_GL_SHIM_FASTDISPATCH
#endif
const guest_gl_backend *guest_gl_installed_backend(void)
{
    return &s_guest_gl_backend;
}

int guest_gl_owned_enter(CPU *__restrict c)
{
    /* guest_gl_run_owned's pre-adapter half without its faults: a busy word
     * (nested or concurrent dispatch) or a foreign pinned CPU makes the
     * replay decline, and the translated body's guest_call then raises
     * exactly the established fault.  A free word pins `c` and is set to
     * `c` for the duration of the replay, as run_owned does per adapter. */
    if (s_guest_gl_active_state > GUEST_GL_ACTIVE_FAULTED)
        return 0;
    if (s_guest_gl_owner_cpu != c) {
        if (s_guest_gl_owner_cpu)
            return 0;
        s_guest_gl_owner_cpu = c;
    }
    s_guest_gl_active_state = (uintptr_t)c;
    return 1;
}

void guest_gl_owned_leave(CPU *__restrict c, uint32_t token)
{
    uintptr_t active = s_guest_gl_active_state;

    if (active == (uintptr_t)c || active == GUEST_GL_ACTIVE_FAULTED) {
        s_guest_gl_active_state = GUEST_GL_ACTIVE_IDLE;
        return;
    }
    guest_fault(c, token, "guest GL dispatch ownership changed");
}
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH end ---------------------------- */
#endif

GUEST_NORETURN void guest_gl_backend_fault(
    uint32_t address, const char *message)
{
    uintptr_t state = guest_gl_active_load();
    CPU *cpu;

    /* Calling this outside a typed dispatch is a host-side contract breach;
     * there is no guest CPU on which a recoverable fault can be recorded. */
    if (state <= GUEST_GL_ACTIVE_FAULTED)
        abort();
    cpu = (CPU *)state;
    if (!guest_gl_active_compare_exchange(
            &state, GUEST_GL_ACTIVE_FAULTED))
        abort();
    guest_fault(cpu, address, message);
    abort();
}

const guest_gl_symbol *guest_gl_symbols(size_t *count)
{
    if (count)
        *count = GUEST_GL_SURFACE_COUNT;
    return s_guest_gl_symbols;
}
