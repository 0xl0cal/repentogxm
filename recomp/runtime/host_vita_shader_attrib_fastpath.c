/* ISAAC_VITA_SHADER_ATTRIB_FASTPATH (wf/opt-attrib): host replay of
 * KAGE::Graphics::Shader::EnableAttribs / DisableAttribs.
 *
 * What the translated bodies cost.  Each of the 3 (enable) or 2 (disable)
 * GL calls per attribute is a guest_call on a 0x7e token read from a
 * wglGetProcAddress-filled function-pointer slot: inline dispatch probe ->
 * guest_call_slow (0x7e gate) -> isaac_vita_gl_dynamic_counted ->
 * guest_gl_dispatch_counted -> guest_gl_run_owned -> generated adapter ->
 * N x guest_stack_address (noinline) -> gpop_at + guest_stack_adjust; about
 * 184 + 35/argument instructions before the typed wrapper runs.  Mid rooms
 * issue ~1410 of these crossings per frame (293 attributes bound and
 * unbound across 43-47 draws).
 *
 * What this file does.  After authenticating the exact shape the bodies
 * assume (Shader vtable, count bound, non-null attribute names and program,
 * every format in 1..8, the four slot words still holding the registry
 * tokens the generated adapters are keyed on, the 0x7e-first startup fact,
 * an installed typed backend and an idle GL ownership word owned by this
 * CPU), it calls the SAME typed backend wrappers the adapters call, in the
 * SAME order with the SAME arguments, then performs the thiscall return.
 * The wrappers own every GL-side receipt (ph120.c gl(a,v,...) counts, the
 * location cache, the typed state cache, GL time buckets), so those stay
 * identical by construction.  What the replay deliberately does not pay is
 * the guest_call classification: the dispatch census (ph120.d d(c,s) and
 * therefore the derived ph120.c g(c)) and the dynamic-token census
 * (ph120.ik dyn) drop by exactly the number of bypassed crossings, which
 * keeps the ph120.ik identity g(c)-g(l)-imports-dyn at zero.
 *
 * x86 facts replayed (recomp/gen_all.py pins the bytes, CFG and tables):
 *   EnableAttribs   push ebp/ebx/esi/edi; ebx=[ebp+8] (base); edi=ecx;
 *                   for i<[this+0xc]: loc=glGetAttribLocation([this+0x28],
 *                   attribs[i].name); [ebp+8]=loc; glEnableVertexAttribArray
 *                   (loc); ecx=fmt-1 (jump table -> eax=ncomp);
 *                   glVertexAttribPointer(loc, ncomp, GL_FLOAT, 0,
 *                   [ebp+0x10], ebx); eax=advance (second table); ebx+=eax;
 *                   pop edi/esi/ebx/ebp; ret 0xc.  Exit: EAX = last advance,
 *                   ECX = last format - 1 (count 0: both untouched).
 *   DisableAttribs  push esi/edi; edi=ecx; for i<count: loc=
 *                   glGetAttribLocation(program, name); glDisableVertexAttrib
 *                   Array(loc); pop edi/esi; ret 0xc.  Exit: EAX = last loc,
 *                   ECX untouched.
 *   Both leave EDX untouched (the adapters never write it) and their lazy
 *   flags are dead at RET (the emitted bodies publish no flags there).
 *   glGetAttribLocation returning -1 is not branched on: 0xffffffff reaches
 *   the enable/pointer/disable wrappers exactly as in the translated body.
 *
 * ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO (default OFF, see the block below)
 * additionally remembers the glGetAttribLocation answers per Shader object
 * and skips that wrapper crossing while the object, its program and the
 * backend's program-state generation still read exactly as memoised. */
#include "host_vita_shader_attrib_fastpath.h"

#include <stddef.h>
#include <stdint.h>

#include "gl_bridge.h"

#if !defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH) || \
    !ISAAC_VITA_SHADER_ATTRIB_FASTPATH
#error The Shader attribute fast path must only be compiled when enabled
#endif
#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
#error The Shader attribute fast path replays the GL shim fast dispatch chain
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif
#if defined(__vita__) && defined(ISAAC_VITA_STALL_PROBE)
#include "kage_vita_stall_probe.h"
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
/* ph120.gw k(en,di): entry-to-`ret 0xc` time of a HANDLED replay on the gt
 * clock (gl_vita_backend.h legend).  A rejected replay is not timed: the
 * translated body then runs and its wrappers land in the w buckets. */
# include "gl_vita_backend.h"
# if defined(__vita__)
#  include <psp2/kernel/processmgr.h>
# else
uint64_t sceKernelGetProcessTimeWide(void);
# endif
# define ATTRIB_TIME_DECL \
    uint64_t attrib_started_at = sceKernelGetProcessTimeWide(); \
    uint64_t attrib_split_at = attrib_started_at
/* ph120.gd en/di(own,loc,gl): a running cursor from the entry read, one
 * read per boundary.  MARK closes `part` at a fresh read; HANDLED closes the
 * slot total and whatever is open into `own` on one final read. */
# define ATTRIB_TIME_MARK(index, part) \
    do { \
        uint64_t attrib_now = sceKernelGetProcessTimeWide(); \
        isaac_vita_gl_wrapper_time_add( \
            &g_isaac_vita_gl_wrapper_time.replay[(index)][(part)], \
            attrib_split_at, attrib_now); \
        attrib_split_at = attrib_now; \
    } while (0)
# define ATTRIB_TIME_HANDLED(index) \
    do { \
        uint64_t attrib_now = sceKernelGetProcessTimeWide(); \
        isaac_vita_gl_wrapper_time_add( \
            &g_isaac_vita_gl_wrapper_time.slot[(index)], \
            attrib_started_at, attrib_now); \
        isaac_vita_gl_wrapper_time_add( \
            &g_isaac_vita_gl_wrapper_time.replay[(index)] \
                                                [ISAAC_VITA_GL_REPLAY_OWN], \
            attrib_split_at, attrib_now); \
    } while (0)
#else
# define ATTRIB_TIME_DECL ((void)0)
# define ATTRIB_TIME_MARK(index, part) ((void)0)
# define ATTRIB_TIME_HANDLED(index) ((void)0)
#endif

#if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
/* ph120.gt mode=sdk32 scope=attrib (gl_vita_backend.h legend): every 32nd
 * HANDLED replay of each kind is timed from `_try` entry to its `ret 0xc`
 * retirement on the sdk32 clock; the ordinal is the kind's HANDLED count
 * and the residue is the phase profiler's window & 31, so the selection
 * needs one load, one compare and one increment per replay and no clock
 * read on the 31 unselected ones.  Whether the selected ordinal is handled
 * is not known at entry, so a rejected attempt at that ordinal costs one
 * clock read and nothing else (the ordinal stays, the next attempt is
 * selected again).  ISAAC_VITA_GL_WRAPPER_TIME rejects SDK_SPARSE, so the
 * two timers never coexist. */
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
#  error ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB is the sdk32 replay timer; ISAAC_VITA_GL_WRAPPER_TIME rejects GL_TIME_SDK_SPARSE
# endif
# include "gl_vita_backend.h"
# if defined(__vita__)
#  include <psp2/kernel/processmgr.h>
# else
uint64_t sceKernelGetProcessTimeWide(void);
# endif
static IsaacVitaAttribReplaySparse s_attrib_sparse;

void isaac_vita_attrib_replay_sparse_take(
    IsaacVitaAttribReplaySparse *out, uint32_t next_window)
{
    if (out)
        *out = s_attrib_sparse;
    s_attrib_sparse = (IsaacVitaAttribReplaySparse){
        .residue = next_window & 31u };
}

static void attrib_sparse_add(IsaacVitaAttribReplaySparseBucket *bucket,
                              uint64_t started_at, uint64_t ended_at)
{
    uint64_t elapsed;

    if (ended_at < started_at) {
        ++s_attrib_sparse.bad_clock;
        return;
    }
    elapsed = ended_at - started_at;
    if (elapsed > UINT32_MAX)
        elapsed = UINT32_MAX;
    ++bucket->calls;
    bucket->us += (uint32_t)elapsed;
    if ((uint32_t)elapsed > bucket->max_us)
        bucket->max_us = (uint32_t)elapsed;
}
# define ATTRIB_SPARSE_DECL(kind) \
    const int attrib_sparse_selected = \
        (s_attrib_sparse.kind##_replays & 31u) == s_attrib_sparse.residue; \
    const uint64_t attrib_sparse_started_at = \
        attrib_sparse_selected ? sceKernelGetProcessTimeWide() : 0u
# define ATTRIB_SPARSE_HANDLED(kind) \
    do { \
        ++s_attrib_sparse.kind##_replays; \
        if (attrib_sparse_selected) \
            attrib_sparse_add(&s_attrib_sparse.kind, \
                              attrib_sparse_started_at, \
                              sceKernelGetProcessTimeWide()); \
    } while (0)
#else
# define ATTRIB_SPARSE_DECL(kind) ((void)0)
# define ATTRIB_SPARSE_HANDLED(kind) ((void)0)
#endif

#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
/* ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE (default OFF): after every decline
 * check has passed and the ownership word is pinned, hand the whole
 * attribute set to gl_vita_backend_attribs_replay_enable/_disable
 * (gl_vita_backend.c) in one native call instead of calling the three (two)
 * typed wrappers per attribute through the backend table.  The batch runs
 * the wrapper bodies statement for statement in the per-call order (see the
 * contract in gl_vita_backend.h), so the vitaGL call sequence, the
 * typed-state/coalescer/fill-census/location-cache writes and every ph120
 * counter are the per-call ones; the batch returns 0 without a side effect
 * when the installed table is not the Vita backend's own wrappers and the
 * replay then runs its unchanged per-call loop.  The memo integration is the
 * per-call one: a hit supplies the remembered locations (no lookups), a
 * miss/fill lets the batch look every name up and notes the answers it
 * returns, VERIFY always looks up and compares afterwards.  The ph120.gd
 * per-attribute split and the stall-probe breadcrumbs describe crossings
 * this path does not make, so both are rejected at compile time (CMake
 * FATALs on the same combinations). */
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
#  error ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE has no per-attribute split for ISAAC_VITA_GL_WRAPPER_TIME
# endif
# if defined(__vita__) && defined(ISAAC_VITA_STALL_PROBE)
#  error ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE makes no per-wrapper crossings for the stall-probe breadcrumbs
# endif
# include "gl_vita_backend.h"
#endif

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
/* ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO: remember the glGetAttribLocation
 * answers per Shader object so a replay whose object still reads exactly as
 * memoised issues no lookup crossing at all (~7 per replay, ~1000 per
 * present in heavy rooms, each a typed-wrapper crossing through the shim
 * location cache).
 *
 * Why a hit is exact.  The typed wrapper (gl_vita_backend.c
 * vita_glGetAttribLocation) answers as a pure function of (program name,
 * attribute name bytes) between two mutations of program/shader state -
 * the same fact the shim location cache rests on - and every guest-reachable
 * mutator bumps g_isaac_vita_gl_location_generation before it runs
 * (glAttachShader, glCompileShader, glCreateProgram, glDeleteProgram,
 * glDeleteShader, glLinkProgram, glShaderSource, backend install/uninstall;
 * glBindAttribLocation is not in the guest GL surface).  An entry is filled
 * from the wrapper's own answers under one generation and is used only
 * while the generation, the object address, its program word, its attribute
 * table address and count, and every attribute's name address AND name
 * bytes (read through the current string to its NUL, so a shortened,
 * lengthened or rewritten name misses) still match.  Anything else - a
 * foreign slot occupant, an entry not yet filled, a name the shim cache
 * would not memoise either (empty, >63 bytes, non-printable) - takes the
 * unchanged per-attribute lookup path and refills.  Every decline check of
 * the replay runs before the memo is consulted, so the memo never changes
 * a verdict; a HANDLED replay's wrapper sequence is today's minus exactly
 * the skipped glGetAttribLocation calls (their ph120.a loc(a) / ph120.c
 * gl count, the sampler publish and the stall-probe breadcrumb of a
 * skipped call go with it).  EAX and [ebp+8] are still written from the
 * remembered location exactly as the x86 does.
 * VERIFY: every hit still performs the wrapper lookup (the build then
 * issues exactly today's calls), a disagreement is counted into ph120.a
 * loc(...,m) via isaac_vita_gl_location_memo_note_mismatch, and the
 * looked-up value is what the replay uses and re-memoises. */
# include "gl_vita_backend.h"
# define ATTRIB_MEMO_ENTRIES 16u
# define ATTRIB_MEMO_NAME_MAX 63u   /* == gl_vita_backend.c GL_VITA_LOCATION_NAME_MAX */

typedef struct attrib_memo_name {
    uint32_t address;
    guest_gl_int location;
    uint8_t length;              /* 1..ATTRIB_MEMO_NAME_MAX, bytes never NUL */
    char bytes[ATTRIB_MEMO_NAME_MAX + 1u];
} attrib_memo_name;

typedef struct attrib_memo_entry {
    uint32_t generation;         /* 0 = never filled, or a fill in progress */
    uint32_t self;
    uint32_t program;
    uint32_t attribs;
    uint32_t count;
    attrib_memo_name names[ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT];
} attrib_memo_entry;

_Static_assert((ATTRIB_MEMO_ENTRIES & (ATTRIB_MEMO_ENTRIES - 1u)) == 0u,
               "attrib memo entry count must be a power of two");

static attrib_memo_entry s_attrib_memo[ATTRIB_MEMO_ENTRIES];

/* Direct-mapped by the object address; a collision is only a miss. */
static attrib_memo_entry *attrib_memo_slot(uint32_t self)
{
    return &s_attrib_memo[((self >> 4) ^ (self >> 9)) &
                          (ATTRIB_MEMO_ENTRIES - 1u)];
}

static int attrib_memo_hit(const attrib_memo_entry *entry,
                           uint32_t generation, uint32_t self,
                           uint32_t program, uint32_t attribs, uint32_t count)
{
    uint32_t i;

    if (entry->generation != generation || entry->self != self ||
            entry->program != program || entry->attribs != attribs ||
            entry->count != count)
        return 0;
    for (i = 0u; i < count; ++i) {
        const attrib_memo_name *name = &entry->names[i];
        uint32_t address = ld32(attribs + i * 8u);
        uint32_t j;

        if (address != name->address)
            return 0;
        /* Remembered bytes are never NUL, so a shorter current string
         * mismatches at its terminator; the final read rejects a longer one.
         * Reads stop at the first mismatch, never past the current NUL. */
        for (j = 0u; j < name->length; ++j)
            if (ld8(address + j) != (uint8_t)name->bytes[j])
                return 0;
        if (ld8(address + j) != 0u)
            return 0;
    }
    return 1;
}

/* Remember one wrapper answer while filling.  0 = not memoisable; the fill
 * is abandoned and the slot stays empty (generation 0). */
static int attrib_memo_note(attrib_memo_entry *entry, uint32_t index,
                            uint32_t address, guest_gl_int location)
{
    attrib_memo_name *name = &entry->names[index];
    uint32_t length = 0u;

    for (;;) {
        uint8_t byte = ld8(address + length);
        if (!byte)
            break;
        if (byte < 0x20u || byte > 0x7eu || length == ATTRIB_MEMO_NAME_MAX)
            return 0;
        name->bytes[length++] = (char)byte;
    }
    if (!length)
        return 0;
    name->bytes[length] = '\0';
    name->length = (uint8_t)length;
    name->address = address;
    name->location = location;
    return 1;
}

# define ATTRIB_MEMO_DECL \
    attrib_memo_entry *memo; \
    uint32_t memo_generation; \
    int memo_hit; \
    int memo_fill
/* After the last decline check: one generation read, one slot, hit or
 * fill.  A fill empties the slot first so an abandoned fill leaves no
 * half-written entry behind. */
# define ATTRIB_MEMO_BEGIN(self, program, attribs, count) \
    do { \
        memo_generation = g_isaac_vita_gl_location_generation; \
        memo = attrib_memo_slot(self); \
        memo_hit = attrib_memo_hit(memo, memo_generation, (self), \
                                   (program), (attribs), (count)); \
        memo_fill = !memo_hit; \
        if (memo_fill) \
            memo->generation = 0u; \
    } while (0)
# define ATTRIB_MEMO_NOTE(index, address, location) \
    do { \
        if (memo_fill && \
                !attrib_memo_note(memo, (index), (address), (location))) \
            memo_fill = 0; \
    } while (0)
# define ATTRIB_MEMO_COMMIT(self, program, attribs, count) \
    do { \
        if (memo_fill) { \
            memo->self = (self); \
            memo->program = (program); \
            memo->attribs = (attribs); \
            memo->count = (count); \
            memo->generation = memo_generation; \
        } \
    } while (0)
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
#  define ATTRIB_MEMO_VERIFY(index, looked_up, location) \
    do { \
        if ((looked_up) != (location)) { \
            isaac_vita_gl_location_memo_note_mismatch(); \
            (location) = (looked_up); \
            memo->names[(index)].location = (looked_up); \
        } \
    } while (0)
# endif
#endif

/* Relocated image words the bodies read (GUEST_IMAGE_BASE + RVA); macros
 * rather than enumerators because the values exceed INT_MAX. */
#define ATTRIB_VTABLE \
    ((uint32_t)(GUEST_IMAGE_BASE + ISAAC_VITA_SHADER_ATTRIB_VTABLE_RVA))
#define ATTRIB_SLOT_GET \
    ((uint32_t)(GUEST_IMAGE_BASE + \
                ISAAC_VITA_SHADER_ATTRIB_SLOT_GET_LOCATION_RVA))
#define ATTRIB_SLOT_ENABLE \
    ((uint32_t)(GUEST_IMAGE_BASE + ISAAC_VITA_SHADER_ATTRIB_SLOT_ENABLE_RVA))
#define ATTRIB_SLOT_POINTER \
    ((uint32_t)(GUEST_IMAGE_BASE + ISAAC_VITA_SHADER_ATTRIB_SLOT_POINTER_RVA))
#define ATTRIB_SLOT_DISABLE \
    ((uint32_t)(GUEST_IMAGE_BASE + ISAAC_VITA_SHADER_ATTRIB_SLOT_DISABLE_RVA))
/* Return word + three thiscall arguments. */
#define ATTRIB_FRAME_BYTES 16u
/* Deepest stack word the translated bodies touch below the entry ESP: four
 * saved registers plus the glVertexAttribPointer frame (six arguments and a
 * return word) for EnableAttribs, two saved registers plus the
 * glGetAttribLocation frame for DisableAttribs.  The generated adapters read
 * every argument through the checked guest_stack_address, so a shallower
 * stack faults in the translated body; the replay then declines instead of
 * completing what the original could not. */
#define ATTRIB_ENABLE_DEPTH_BYTES (16u + 28u)
#define ATTRIB_DISABLE_DEPTH_BYTES (8u + 12u)

/* Frozen format -> component count (jump table at RVA 0x5673ac); the byte
 * advance table at 0x5673cc is 4 * this in every slot. */
static const uint8_t k_format_components[ISAAC_VITA_SHADER_ATTRIB_FORMAT_COUNT]
    = { 1u, 2u, 3u, 4u, 3u, 4u, 2u, 1u };

/* Registry tokens, resolved once through the frozen typed registry (the
 * same table the adapters are keyed on); zero until the first call. */
static uint32_t s_token_get_location;
static uint32_t s_token_enable;
static uint32_t s_token_pointer;
static uint32_t s_token_disable;

static int attrib_tokens_ready(void)
{
    if (s_token_get_location)
        return 1;
    s_token_enable = guest_gl_resolve("glEnableVertexAttribArray");
    s_token_pointer = guest_gl_resolve("glVertexAttribPointer");
    s_token_disable = guest_gl_resolve("glDisableVertexAttribArray");
    if (!s_token_enable || !s_token_pointer || !s_token_disable)
        return 0;
    /* Assigned last: a partially resolved set never reads as ready. */
    s_token_get_location = guest_gl_resolve("glGetAttribLocation");
    return s_token_get_location != 0u;
}

#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* guest_call publishes the indirect target (here the 0x7e token) for the
 * duration of the callee and restores the enclosing target when it returns
 * (kage_vita_guest_sampler.h); mirror both stores around each wrapper. */
# define ATTRIB_SAMPLER_PUBLISH(token) \
    (g_kage_guest_last_indirect_target = (token))
# define ATTRIB_SAMPLER_RESTORE(value) \
    (g_kage_guest_last_indirect_target = (value))
#else
# define ATTRIB_SAMPLER_PUBLISH(token) ((void)(token))
# define ATTRIB_SAMPLER_RESTORE(value) ((void)(value))
#endif

#if defined(__vita__) && defined(ISAAC_VITA_STALL_PROBE)
/* guest_call_slow notes every registered 0x7e dispatch with the token and
 * the return word; a 0x7e token is outside the image so image_rva() is the
 * identity on it. */
# define ATTRIB_STALL_NOTE(token, return_rva) \
    KAGE_VITA_STALL_NOTE_DISPATCH( \
        KAGE_VITA_STALL_DISPATCH_INDIRECT, (token), (return_rva))
#else
# define ATTRIB_STALL_NOTE(token, return_rva) ((void)0)
#endif

/* Shared entry checks.  Zero leaves everything untouched. */
static int attrib_frame_ok(const CPU *c, uint32_t depth)
{
    uint32_t capacity, offset;

    /* The return word and the three arguments must lie inside the bound
     * synthetic stack (the raw `ret 0xc` below assumes that window) and the
     * translated body's deepest frame must fit below ESP (otherwise its
     * checked adapter reads fault).  Anything else is the translated body's
     * fault to raise. */
    if (!guest_stack_fast_bound(c))
        return 0;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (capacity < ATTRIB_FRAME_BYTES ||
            offset > capacity - ATTRIB_FRAME_BYTES || offset < depth)
        return 0;
    return 1;
}

static int attrib_object_ok(uint32_t self, uint32_t count,
                            uint32_t *attribs_out, uint32_t *program_out,
                            int need_format)
{
    uint32_t attribs, program, i;

    if (count > ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT)
        return 0;
    attribs = ld32(self + ISAAC_VITA_SHADER_ATTRIB_OFF_ATTRIBS);
    program = ld32(self + ISAAC_VITA_SHADER_ATTRIB_OFF_PROGRAM);
    if (!attribs || !program)
        return 0;
    /* Every (name, format) pair is validated before the first GL call: once
     * a wrapper has run there is no exact fallback any more. */
    for (i = 0u; i < count; ++i) {
        uint32_t name = ld32(attribs + i * 8u);
        uint32_t format = ld32(attribs + i * 8u + 4u);
        if (!name)
            return 0;
        if (need_format &&
                format - 1u >= ISAAC_VITA_SHADER_ATTRIB_FORMAT_COUNT)
            return 0;
    }
    *attribs_out = attribs;
    *program_out = program;
    return 1;
}

int isaac_vita_shader_attribs_enable_try(CPU *__restrict c)
{
    uint32_t self, count, attribs, program, base, stride, i;
    uint32_t last_format = 0u;
    const guest_gl_backend *backend;
    uint32_t sampler_enclosing_target;
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    ATTRIB_MEMO_DECL;
#endif
    ATTRIB_TIME_DECL;
    ATTRIB_SPARSE_DECL(enable);

    if (c == NULL || !attrib_frame_ok(c, ATTRIB_ENABLE_DEPTH_BYTES))
        return 0;
    self = c->ecx;
    if (!self || ld32(self) != ATTRIB_VTABLE)
        return 0;
    count = ld32(self + ISAAC_VITA_SHADER_ATTRIB_OFF_COUNT);
    if (count == 0u) {
        /* `cmp [edi+0xc], esi; jbe epilogue`: no call, every register the
         * body touched is restored by its pops; only the thiscall return. */
        c->esp += ATTRIB_FRAME_BYTES;
        ATTRIB_TIME_HANDLED(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE);
        ATTRIB_SPARSE_HANDLED(enable);
        return 1;
    }
    if (!attrib_object_ok(self, count, &attribs, &program, 1))
        return 0;
    if (!attrib_tokens_ready() ||
            ld32(ATTRIB_SLOT_GET) != s_token_get_location ||
            ld32(ATTRIB_SLOT_ENABLE) != s_token_enable ||
            ld32(ATTRIB_SLOT_POINTER) != s_token_pointer ||
            !guest_gl_dynamic_first_holds())
        return 0;
    backend = guest_gl_installed_backend();
    if (!backend->glGetAttribLocation ||
            !backend->glEnableVertexAttribArray ||
            !backend->glVertexAttribPointer)
        return 0;
    if (!guest_gl_owned_enter(c))
        return 0;

    /* `mov ebx, [ebp+8]` once at entry (the slot is overwritten with each
     * location below, as the x86 does) and `push [ebp+0x10]` per call. */
    base = ld32(c->esp + 4u);
    stride = ld32(c->esp + 12u);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
#else
    sampler_enclosing_target = 0u;
    (void)sampler_enclosing_target;
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    /* Inside `own` of the ph120.gd split: the whole-object hit test. */
    ATTRIB_MEMO_BEGIN(self, program, attribs, count);
#endif
    ATTRIB_TIME_MARK(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE,
                     ISAAC_VITA_GL_REPLAY_OWN);
#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
    {
        guest_gl_addr names[ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT];
        guest_gl_int locations[ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT];
        uint8_t components[ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT];
        const guest_gl_addr *lookup = names;
        IsaacVitaAttribReplayTokens tokens;

        tokens.get_location = s_token_get_location;
        tokens.toggle = s_token_enable;
        tokens.pointer = s_token_pointer;
        tokens.enclosing = sampler_enclosing_target;
        for (i = 0u; i < count; ++i) {
            uint32_t format = ld32(attribs + i * 8u + 4u);
            names[i] = (guest_gl_addr)ld32(attribs + i * 8u);
            components[i] = k_format_components[format - 1u];
            last_format = format;
        }
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
        if (memo_hit) {
            for (i = 0u; i < count; ++i)
                locations[i] = memo->names[i].location;
#  if !defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
            lookup = NULL;   /* remembered locations, no lookups */
#  endif
        }
# endif
        if (gl_vita_backend_attribs_replay_enable(
                backend, (guest_gl_uint)program, count, lookup, locations,
                components, (guest_gl_sizei)stride, (guest_gl_addr)base,
                &tokens)) {
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
            if (memo_hit) {
#  if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
                /* The batch used the looked-up values; count and refresh
                 * every disagreement with the remembered ones. */
                for (i = 0u; i < count; ++i) {
                    guest_gl_int location = memo->names[i].location;
                    ATTRIB_MEMO_VERIFY(i, locations[i], location);
                }
#  endif
            } else {
                for (i = 0u; i < count; ++i)
                    ATTRIB_MEMO_NOTE(i, names[i], locations[i]);
            }
# endif
            /* The last iteration's `mov [ebp+8], eax` survives. */
            st32(c->esp + 4u, (uint32_t)locations[count - 1u]);
            goto direct_state_retire;
        }
        /* Declined (foreign or partial table): the per-call path below. */
        last_format = 0u;
    }
#endif
    for (i = 0u; i < count; ++i) {
        uint32_t name = ld32(attribs + i * 8u);
        uint32_t format = ld32(attribs + i * 8u + 4u);
        uint32_t components = k_format_components[format - 1u];
        guest_gl_int location;

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
        if (memo_hit) {
            location = memo->names[i].location;
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
            {
                guest_gl_int looked_up;

                ATTRIB_SAMPLER_PUBLISH(s_token_get_location);
                ATTRIB_STALL_NOTE(s_token_get_location,
                                  ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_GET);
                looked_up = backend->glGetAttribLocation(
                    (guest_gl_uint)program, (guest_gl_addr)name);
                ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
                ATTRIB_MEMO_VERIFY(i, looked_up, location);
            }
# endif
        } else
#endif
        {
            ATTRIB_SAMPLER_PUBLISH(s_token_get_location);
            ATTRIB_STALL_NOTE(s_token_get_location,
                              ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_GET);
            location = backend->glGetAttribLocation(
                (guest_gl_uint)program, (guest_gl_addr)name);
            ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
            ATTRIB_MEMO_NOTE(i, name, location);
#endif
        }
        c->eax = (uint32_t)location;
        st32(c->esp + 4u, (uint32_t)location);   /* mov [ebp+8], eax */
        ATTRIB_TIME_MARK(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE,
                         ISAAC_VITA_GL_REPLAY_LOC);
        ATTRIB_SAMPLER_PUBLISH(s_token_enable);
        ATTRIB_STALL_NOTE(s_token_enable,
                          ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_ENABLE);
        backend->glEnableVertexAttribArray((guest_gl_uint)location);
        ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
        ATTRIB_SAMPLER_PUBLISH(s_token_pointer);
        ATTRIB_STALL_NOTE(s_token_pointer,
                          ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_POINTER);
        backend->glVertexAttribPointer(
            (guest_gl_uint)location, (guest_gl_int)components,
            (guest_gl_enum)ISAAC_VITA_SHADER_ATTRIB_GL_FLOAT,
            (guest_gl_boolean)0u, (guest_gl_sizei)stride,
            (guest_gl_addr)base);
        ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
        ATTRIB_TIME_MARK(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE,
                         ISAAC_VITA_GL_REPLAY_GL);
        base += components * 4u;                  /* add ebx, eax */
        last_format = format;
    }
#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
direct_state_retire:
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    ATTRIB_MEMO_COMMIT(self, program, attribs, count);
#endif
    guest_gl_owned_leave(c, s_token_pointer);

    /* The last iteration's `mov eax, advance` and `dec ecx` survive the
     * pops; EDX is never written; `ret 0xc`. */
    c->eax = (uint32_t)k_format_components[last_format - 1u] * 4u;
    c->ecx = last_format - 1u;
    c->esp += ATTRIB_FRAME_BYTES;
    ATTRIB_TIME_HANDLED(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE);
    ATTRIB_SPARSE_HANDLED(enable);
    return 1;
}

int isaac_vita_shader_attribs_disable_try(CPU *__restrict c)
{
    uint32_t self, count, attribs, program, i;
    const guest_gl_backend *backend;
    uint32_t sampler_enclosing_target;
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    ATTRIB_MEMO_DECL;
#endif
    ATTRIB_TIME_DECL;
    ATTRIB_SPARSE_DECL(disable);

    if (c == NULL || !attrib_frame_ok(c, ATTRIB_DISABLE_DEPTH_BYTES))
        return 0;
    self = c->ecx;
    if (!self || ld32(self) != ATTRIB_VTABLE)
        return 0;
    count = ld32(self + ISAAC_VITA_SHADER_ATTRIB_OFF_COUNT);
    if (count == 0u) {
        c->esp += ATTRIB_FRAME_BYTES;
        ATTRIB_TIME_HANDLED(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE);
        ATTRIB_SPARSE_HANDLED(disable);
        return 1;
    }
    if (!attrib_object_ok(self, count, &attribs, &program, 0))
        return 0;
    if (!attrib_tokens_ready() ||
            ld32(ATTRIB_SLOT_GET) != s_token_get_location ||
            ld32(ATTRIB_SLOT_DISABLE) != s_token_disable ||
            !guest_gl_dynamic_first_holds())
        return 0;
    backend = guest_gl_installed_backend();
    if (!backend->glGetAttribLocation ||
            !backend->glDisableVertexAttribArray)
        return 0;
    if (!guest_gl_owned_enter(c))
        return 0;

#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
#else
    sampler_enclosing_target = 0u;
    (void)sampler_enclosing_target;
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    /* Enable and Disable share the memo: same object, same answers. */
    ATTRIB_MEMO_BEGIN(self, program, attribs, count);
#endif
    ATTRIB_TIME_MARK(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE,
                     ISAAC_VITA_GL_REPLAY_OWN);
#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
    {
        guest_gl_addr names[ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT];
        guest_gl_int locations[ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT];
        const guest_gl_addr *lookup = names;
        IsaacVitaAttribReplayTokens tokens;

        tokens.get_location = s_token_get_location;
        tokens.toggle = s_token_disable;
        tokens.pointer = 0u;
        tokens.enclosing = sampler_enclosing_target;
        for (i = 0u; i < count; ++i)
            names[i] = (guest_gl_addr)ld32(attribs + i * 8u);
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
        if (memo_hit) {
            for (i = 0u; i < count; ++i)
                locations[i] = memo->names[i].location;
#  if !defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
            lookup = NULL;
#  endif
        }
# endif
        if (gl_vita_backend_attribs_replay_disable(
                backend, (guest_gl_uint)program, count, lookup, locations,
                &tokens)) {
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
            if (memo_hit) {
#  if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
                for (i = 0u; i < count; ++i) {
                    guest_gl_int location = memo->names[i].location;
                    ATTRIB_MEMO_VERIFY(i, locations[i], location);
                }
#  endif
            } else {
                for (i = 0u; i < count; ++i)
                    ATTRIB_MEMO_NOTE(i, names[i], locations[i]);
            }
# endif
            /* The void adapter leaves EAX at the last location. */
            c->eax = (uint32_t)locations[count - 1u];
            goto direct_state_retire;
        }
    }
#endif
    for (i = 0u; i < count; ++i) {
        uint32_t name = ld32(attribs + i * 8u);
        guest_gl_int location;

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
        if (memo_hit) {
            location = memo->names[i].location;
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
            {
                guest_gl_int looked_up;

                ATTRIB_SAMPLER_PUBLISH(s_token_get_location);
                ATTRIB_STALL_NOTE(s_token_get_location,
                                  ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_GET);
                looked_up = backend->glGetAttribLocation(
                    (guest_gl_uint)program, (guest_gl_addr)name);
                ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
                ATTRIB_MEMO_VERIFY(i, looked_up, location);
            }
# endif
        } else
#endif
        {
            ATTRIB_SAMPLER_PUBLISH(s_token_get_location);
            ATTRIB_STALL_NOTE(s_token_get_location,
                              ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_GET);
            location = backend->glGetAttribLocation(
                (guest_gl_uint)program, (guest_gl_addr)name);
            ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
            ATTRIB_MEMO_NOTE(i, name, location);
#endif
        }
        c->eax = (uint32_t)location;
        ATTRIB_TIME_MARK(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE,
                         ISAAC_VITA_GL_REPLAY_LOC);
        ATTRIB_SAMPLER_PUBLISH(s_token_disable);
        ATTRIB_STALL_NOTE(s_token_disable,
                          ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_DISABLE);
        backend->glDisableVertexAttribArray((guest_gl_uint)location);
        ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
        ATTRIB_TIME_MARK(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE,
                         ISAAC_VITA_GL_REPLAY_GL);
    }
#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
direct_state_retire:
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    ATTRIB_MEMO_COMMIT(self, program, attribs, count);
#endif
    guest_gl_owned_leave(c, s_token_disable);

    /* EAX already holds the last location (the void adapter leaves it);
     * ECX/EDX are never written; `ret 0xc`. */
    c->esp += ATTRIB_FRAME_BYTES;
    ATTRIB_TIME_HANDLED(ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE);
    ATTRIB_SPARSE_HANDLED(disable);
    return 1;
}
