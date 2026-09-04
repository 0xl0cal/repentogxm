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
 *   the enable/pointer/disable wrappers exactly as in the translated body. */
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
    for (i = 0u; i < count; ++i) {
        uint32_t name = ld32(attribs + i * 8u);
        uint32_t format = ld32(attribs + i * 8u + 4u);
        uint32_t components = k_format_components[format - 1u];
        guest_gl_int location;

        ATTRIB_SAMPLER_PUBLISH(s_token_get_location);
        ATTRIB_STALL_NOTE(s_token_get_location,
                          ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_GET);
        location = backend->glGetAttribLocation(
            (guest_gl_uint)program, (guest_gl_addr)name);
        ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
        c->eax = (uint32_t)location;
        st32(c->esp + 4u, (uint32_t)location);   /* mov [ebp+8], eax */
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
        base += components * 4u;                  /* add ebx, eax */
        last_format = format;
    }
    guest_gl_owned_leave(c, s_token_pointer);

    /* The last iteration's `mov eax, advance` and `dec ecx` survive the
     * pops; EDX is never written; `ret 0xc`. */
    c->eax = (uint32_t)k_format_components[last_format - 1u] * 4u;
    c->ecx = last_format - 1u;
    c->esp += ATTRIB_FRAME_BYTES;
    return 1;
}

int isaac_vita_shader_attribs_disable_try(CPU *__restrict c)
{
    uint32_t self, count, attribs, program, i;
    const guest_gl_backend *backend;
    uint32_t sampler_enclosing_target;

    if (c == NULL || !attrib_frame_ok(c, ATTRIB_DISABLE_DEPTH_BYTES))
        return 0;
    self = c->ecx;
    if (!self || ld32(self) != ATTRIB_VTABLE)
        return 0;
    count = ld32(self + ISAAC_VITA_SHADER_ATTRIB_OFF_COUNT);
    if (count == 0u) {
        c->esp += ATTRIB_FRAME_BYTES;
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
    for (i = 0u; i < count; ++i) {
        uint32_t name = ld32(attribs + i * 8u);
        guest_gl_int location;

        ATTRIB_SAMPLER_PUBLISH(s_token_get_location);
        ATTRIB_STALL_NOTE(s_token_get_location,
                          ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_GET);
        location = backend->glGetAttribLocation(
            (guest_gl_uint)program, (guest_gl_addr)name);
        ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
        c->eax = (uint32_t)location;
        ATTRIB_SAMPLER_PUBLISH(s_token_disable);
        ATTRIB_STALL_NOTE(s_token_disable,
                          ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_DISABLE);
        backend->glDisableVertexAttribArray((guest_gl_uint)location);
        ATTRIB_SAMPLER_RESTORE(sampler_enclosing_target);
    }
    guest_gl_owned_leave(c, s_token_disable);

    /* EAX already holds the last location (the void adapter leaves it);
     * ECX/EDX are never written; `ret 0xc`. */
    c->esp += ATTRIB_FRAME_BYTES;
    return 1;
}
