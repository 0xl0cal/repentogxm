/* ISAAC_VITA_KAGE_BIND_PACKET (RENDER60 W-pkt, bind packet stage 1).
 *
 * Native replay of frozen KAGE Shader::Bind (RVA 005671d0) for the shape
 * the game actually binds - an empty uniform-record list - including the
 * two Shader::SetUniformInt calls (00567430, "Texture0"/"Texture1"), the
 * Image::IsEmpty test (0055f900) and RenderManager::Transform (00565240,
 * the 4x4 "Transform" matrix), with every GL call staged into ONE recorder
 * record (gl_record_backend.c isaac_gl_record_bind_packet_*) instead of
 * 6..14 guest_call crossings, adapters and ring records.
 *
 * What stays identical (proved by host_vita_kage_bind_packet_oracle.c
 * against the translated bodies behind the real recorder and its consumer
 * thread): the GL calls the render thread makes (function, arguments, name
 * and matrix bytes, order, the return RVA each one sees), the producer
 * location-cache transitions, the CPU at the `ret 4` (EAX/ECX/EDX, the
 * callee-saved registers, ESP = entry + 8, xmm0..xmm7 bit for bit, flags),
 * every guest word above the final ESP, the sampler word and the stack low
 * water.  What deliberately differs: the dead stack below the final ESP is
 * not written (the translated bodies leave their frames, arguments and the
 * matrix there), and the dispatch/dynamic-token censuses (ph120.d, ph120.ik
 * dyn) drop by the bypassed crossings, as for the attribute replay.
 *
 * Entry checks (any miss runs the translated body; nothing is done before
 * the last one passes): the bound synthetic stack has the translated
 * frame's 116 bytes below ESP and the 8-byte `ret 4` window above it and
 * is not image-backed; the Shader, the uniform-record vector, the current
 * texture Image, the render target and the current shader do not lie on
 * that stack; the Shader vtable is 0075e0a8 with SetUniformInt in +0x1c;
 * the record list is empty ((end - begin) >> 2 == 0, signed, as `sar`);
 * the Image vtable has Image::IsEmpty at +0x48; the target's +0x28/+0x2c
 * are among the eight frozen trivial getters (k_getters); the six GL slot
 * words hold their registry tokens with the 0x7e-first fact; the recorder
 * admits (installed, game thread, its own six wrappers in the bridge
 * table); the GL ownership word is free for this CPU.
 *
 * Constants: every address below is copied from the frozen corpus
 * (a 2026-10-17 corpus, guest_0167.c: Bind 10959..11205,
 * SetUniformInt 11500..11587, Transform 2574..2800; guest_0166.c:7997
 * IsEmpty; getters guest_0173.c:23133/23147/23659/23673,
 * guest_0006.c:5297, guest_0174.c:729/1482/1496) and the frozen PE. */
#include "host_vita_kage_bind_packet.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gl_bridge.h"
#include "gl_record_backend.h"

#if !defined(ISAAC_VITA_KAGE_BIND_PACKET) || !ISAAC_VITA_KAGE_BIND_PACKET
#error Compile the bind packet only when explicitly enabled
#endif
#if !defined(ISAAC_VITA_GL_THREAD_RECORD) || !ISAAC_VITA_GL_THREAD_RECORD
#error The bind packet stages into the typed GL recorder
#endif
#if !defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
#error The bind packet uses the bridge ownership helpers of SHADER_ATTRIB_FASTPATH
#endif
#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
#error The bind packet replays the GL shim fast dispatch chain
#endif
#if GUEST_STACK_REQUIRED != 1
#error The bind packet requires the bound synthetic stack
#endif
#if defined(ISAAC_VITA_STALL_PROBE)
#error The bind packet makes none of the crossings the stall probe notes
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
/* guest_call publishes its target for the callee's duration and restores
 * the enclosing word on return (guest.c); mirrored per bypassed call. */
# define KBP_PUBLISH(value) (g_kage_guest_last_indirect_target = (value))
#else
# define KBP_PUBLISH(value) ((void)(value))
#endif

void isaac_vita_log(const char *format, ...);
void __real_sub_005671d0(CPU *__restrict c);

#define KBP_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (uint32_t)(rva)))
/* SizeOfImage of the frozen PE (host_vita_shader_sampler_fastpath.c). */
#define KBP_IMAGE_SIZE 0x0085f000u

/* Shader (Bind's ECX). */
#define KBP_SHADER_VTABLE KBP_VA(0x0075e0a8u)       /* sampler fastpath:22 */
#define KBP_SET_UNIFORM_INT KBP_VA(0x00567430u)     /* vtable +0x1c, 11289 */
#define KBP_OFF_RECORDS_LIST 0x1cu                  /* lea eax, [esi+0x1c] */
#define KBP_OFF_PROGRAM 0x28u                       /* push [esi+0x28] */
#define KBP_OFF_SET_UNIFORM_INT 0x1cu               /* call [eax+0x1c] */
/* Current texture Image and its IsEmpty slot. */
#define KBP_CURRENT_IMAGE KBP_VA(0x007c7af4u)       /* 00567272 */
#define KBP_IMAGE_IS_EMPTY KBP_VA(0x0055f900u)      /* guest_0166.c:7997 */
#define KBP_OFF_IS_EMPTY 0x48u                      /* 00567294 */
#define KBP_IMAGE_OFF_HANDLE 0x18u                  /* 0055f900 cmp [ecx+0x18], 0 */
#define KBP_IMAGE_OFF_KIND 0x20u                    /* 0056744a */
#define KBP_IMAGE_OFF_TEX0 0x78u                    /* 0056745e */
#define KBP_IMAGE_OFF_TEX1 0x7cu                    /* 00567459 */
#define KBP_IMAGE_READ_BYTES 0x80u
/* RenderManager (Transform's ECX, 005672b1 mov ecx, 0x987c7a30). */
#define KBP_MANAGER KBP_VA(0x007c7a30u)
#define KBP_MANAGER_SHADER (KBP_MANAGER + 0x14u)    /* 00565372 */
#define KBP_MANAGER_TARGET (KBP_MANAGER + 0x18u)    /* 00565253 */
#define KBP_MANAGER_FLAG (KBP_MANAGER + 0x1cu)      /* 0056525a (byte) */
#define KBP_TARGET_OFF_WIDTH 0x28u                  /* 00565262 */
#define KBP_TARGET_OFF_HEIGHT 0x2cu                 /* 00565280 */
#define KBP_TARGET_READ_BYTES 0x108u                /* largest getter field + 4 */
/* Literals and constants. */
#define KBP_NAME_TEXTURE0 KBP_VA(0x0075e098u)       /* 00567282 push */
#define KBP_NAME_TEXTURE1 KBP_VA(0x0075e03cu)       /* 005672a9 push */
#define KBP_NAME_TRANSFORM KBP_VA(0x00744ac0u)      /* 00565384 push */
#define KBP_COOKIE KBP_VA(0x007aa3b4u)              /* 00565246 */
#define KBP_DEFAULT_WIDTH KBP_VA(0x007aa92cu)       /* 005652b0 (.data) */
#define KBP_DEFAULT_HEIGHT KBP_VA(0x007aa930u)      /* 005652b8 (.data) */
#define KBP_TWO KBP_VA(0x0076a7b0u)                 /* 005652c0 */
#define KBP_U32_BIAS KBP_VA(0x0076c940u)            /* 00565275 [eax*8+...] */
#define KBP_SIGN_MASK KBP_VA(0x0076c9b0u)           /* 0056530a xorps */
#define KBP_GL_TEXTURE0 0x84c0u                     /* 0056743a lea [esi+0x84c0] */
#define KBP_GL_TEXTURE_2D 0x0de1u                   /* 00567461 push 0xde1 */
/* GL slot words (call dword ptr [slot]). */
#define KBP_SLOT_USE_PROGRAM KBP_VA(0x007bf994u)    /* 005671dc */
#define KBP_SLOT_ACTIVE_TEXTURE KBP_VA(0x007c293cu) /* 00567441 */
#define KBP_SLOT_BIND_TEXTURE KBP_VA(0x007c2978u)   /* 00567466 */
#define KBP_SLOT_GET_UNIFORM KBP_VA(0x007bf9bcu)    /* 00567473, 0056538c */
#define KBP_SLOT_UNIFORM1I KBP_VA(0x007bf96cu)      /* 0056747a */
#define KBP_SLOT_MATRIX4 KBP_VA(0x007bf9d0u)        /* 0056539b */
/* Return words the translated call sites push (bare RVAs, GPUSH). */
#define KBP_RET_USE_PROGRAM 0x005671e2u
#define KBP_RET_SET_UNIFORM_INT0 0x0056728cu
#define KBP_RET_IS_EMPTY 0x00567299u
#define KBP_RET_SET_UNIFORM_INT1 0x005672b1u
#define KBP_RET_ACTIVE_TEXTURE 0x00567447u
#define KBP_RET_BIND_TEXTURE 0x0056746cu
#define KBP_RET_GET_UNIFORM_INT 0x00567479u
#define KBP_RET_UNIFORM1I 0x00567480u
#define KBP_RET_GET_UNIFORM_TRANSFORM 0x00565392u
#define KBP_RET_MATRIX4 0x005653a1u
/* Stack map below the entry ESP E (the word holding Bind's return RVA):
 * SetUniformInt's glGetUniformLocation return word E-60, Transform's
 * frame base E-24 (matrix at E-92..E-29), its glGetUniformLocation return
 * word E-108 and the deepest word, glUniformMatrix4fv's return, E-116. */
#define KBP_DEPTH_BYTES 116u
#define KBP_FRAME_BYTES 8u                          /* return word + arg */
#define KBP_COLD_RET_SET_UNIFORM_INT 60u
#define KBP_COLD_RET_TRANSFORM 108u
#define KBP_MATRIX_BELOW_ENTRY 92u

/* The eight getters the frozen PE places in +0x28/+0x2c of the Image
 * vtables whose +0x48 is IsEmpty (0075d2b4, 00765e90, 00766028, 007661c0,
 * 00766258, 007662f0, 00767fe8, 007682ac; the other slot word, 005ec13a,
 * is _purecall and not accepted).  Each is `mov eax, [ecx+off]` or
 * `movzx eax, word [ecx+off]`; `ret`. */
typedef struct kbp_getter {
    uint32_t rva;
    uint16_t offset;
    uint8_t halfword;
} kbp_getter;
static const kbp_getter k_getters[8] = {
    {0x005a0b90u, 0x100u, 0u}, {0x005a0ba0u, 0x104u, 0u},
    {0x005a0dd0u, 0x080u, 1u}, {0x005a0de0u, 0x082u, 1u},
    {0x00032ed0u, 0x080u, 0u}, {0x005a1a70u, 0x084u, 0u},
    {0x005a1ef0u, 0x0c0u, 0u}, {0x005a1f00u, 0x0c4u, 0u},
};

enum {
    KBP_DECLINE_FRAME, KBP_DECLINE_OBJECT, KBP_DECLINE_RECORDS,
    KBP_DECLINE_IMAGE, KBP_DECLINE_TARGET, KBP_DECLINE_TOKENS,
    KBP_DECLINE_RECORDER, KBP_DECLINE_OWNER, KBP_DECLINE_COUNT
};
_Static_assert(KBP_DECLINE_COUNT == 8,
               "isaac_vita_kage_bind_packet_declines reports eight reasons");
static uint32_t s_handled, s_declined, s_cold;
static uint32_t s_decline[KBP_DECLINE_COUNT];
static uint32_t s_next_log = 1u;

/* Registry tokens, resolved once; zero until the first call. */
static uint32_t s_tok_use, s_tok_active, s_tok_bind, s_tok_get, s_tok_int, s_tok_matrix;
static int kbp_tokens_ready(void)
{
    if (s_tok_matrix)
        return 1;
    s_tok_use = guest_gl_resolve("glUseProgram");
    s_tok_active = guest_gl_resolve("glActiveTexture");
    s_tok_bind = guest_gl_resolve("glBindTexture");
    s_tok_get = guest_gl_resolve("glGetUniformLocation");
    s_tok_int = guest_gl_resolve("glUniform1i");
    if (!s_tok_use || !s_tok_active || !s_tok_bind || !s_tok_get || !s_tok_int)
        return 0;
    /* Assigned last: a partially resolved set never reads as ready. */
    s_tok_matrix = guest_gl_resolve("glUniformMatrix4fv");
    return s_tok_matrix != 0u;
}

static int kbp_not_stack(const CPU *c, uint32_t address, uint32_t bytes)
{
    return address && address <= UINT32_MAX - bytes &&
        (address + bytes <= c->stack_floor || address >= c->stack_ceiling);
}

static const kbp_getter *kbp_getter_for(uint32_t va)
{
    uint32_t i;
    for (i = 0u; i < sizeof k_getters / sizeof k_getters[0]; ++i)
        if (va == KBP_VA(k_getters[i].rva))
            return &k_getters[i];
    return NULL;
}

static uint32_t kbp_getter_value(const kbp_getter *g, uint32_t object)
{
    return g->halfword ? (uint32_t)ld16(object + g->offset) :
        ld32(object + g->offset);
}

typedef struct kbp_state {
    CPU *c;
    const guest_gl_backend *backend;
    uint32_t entry;          /* E: entry ESP */
    uint32_t enclosing;      /* sampler word at entry */
} kbp_state;

/* glGetUniformLocation at one of the two call sites: staged when the memo
 * or the producer location cache answers; otherwise the staged calls are
 * published first and the table's own wrapper takes the synchronous path
 * with [esp] holding the call site's return word at the translated slot
 * (record_enqueue2 reads the RVA there), exactly the word the translated
 * body writes at that address. */
static guest_gl_int kbp_location(kbp_state *k, uint32_t ret, uint32_t slot,
                                 uint32_t below_entry, uint32_t program,
                                 uint32_t name)
{
    guest_gl_int location;
    CPU *c = k->c;
    uint32_t esp;

    if (isaac_gl_record_bind_packet_uniform_location(ret, slot, program, name,
                                                     &location))
        return location;
    ++s_cold;
    isaac_gl_record_bind_packet_flush();
    esp = c->esp;
    c->esp = k->entry - below_entry;
    st32(c->esp, ret);
    location = k->backend->glGetUniformLocation(program, name);
    c->esp = esp;
    return location;
}

/* Shader::SetUniformInt(name, unit, image) as 00567430 performs it: the
 * Image fields are read after glActiveTexture; unit 0, or any unit of an
 * Image whose kind is not 2, binds +0x78; unit 1 of a kind-2 Image binds
 * +0x7c; any other unit of a kind-2 Image binds nothing. */
static guest_gl_int kbp_set_uniform_int(kbp_state *k, uint32_t program,
                                        uint32_t name, uint32_t unit,
                                        uint32_t image, uint32_t memo_slot)
{
    uint32_t kind;
    guest_gl_int location;

    KBP_PUBLISH(KBP_SET_UNIFORM_INT);
    KBP_PUBLISH(s_tok_active);
    isaac_gl_record_bind_packet_active_texture(KBP_RET_ACTIVE_TEXTURE,
                                               unit + KBP_GL_TEXTURE0);
    KBP_PUBLISH(KBP_SET_UNIFORM_INT);
    kind = ld32(image + KBP_IMAGE_OFF_KIND);
    if (kind != 2u || unit == 0u) {
        KBP_PUBLISH(s_tok_bind);
        isaac_gl_record_bind_packet_bind_texture(
            KBP_RET_BIND_TEXTURE, KBP_GL_TEXTURE_2D,
            ld32(image + KBP_IMAGE_OFF_TEX0));
        KBP_PUBLISH(KBP_SET_UNIFORM_INT);
    } else if (unit == 1u) {
        KBP_PUBLISH(s_tok_bind);
        isaac_gl_record_bind_packet_bind_texture(
            KBP_RET_BIND_TEXTURE, KBP_GL_TEXTURE_2D,
            ld32(image + KBP_IMAGE_OFF_TEX1));
        KBP_PUBLISH(KBP_SET_UNIFORM_INT);
    }
    KBP_PUBLISH(s_tok_get);
    location = kbp_location(k, KBP_RET_GET_UNIFORM_INT, memo_slot,
                            KBP_COLD_RET_SET_UNIFORM_INT, program, name);
    KBP_PUBLISH(KBP_SET_UNIFORM_INT);
    KBP_PUBLISH(s_tok_int);
    isaac_gl_record_bind_packet_uniform1i(KBP_RET_UNIFORM1I, location,
                                          (guest_gl_int)unit);
    KBP_PUBLISH(KBP_SET_UNIFORM_INT);
    KBP_PUBLISH(k->enclosing);
    return location;
}

/* RenderManager::Transform's arithmetic, statement for statement as the
 * translated body (guest_0167.c:2603..2752) spells it, on the CPU's own
 * xmm registers; `matrix` receives the 16 words it stores at [ebp-0x44].
 * width/height are the getter results (unused on the default path). */
static void kbp_transform_math(CPU *__restrict c, uint32_t target,
                               uint32_t flag, uint32_t width, uint32_t height,
                               uint32_t matrix[16])
{
    uint32_t eax, ecx, w_bits;

    memset(matrix, 0, 16u * sizeof matrix[0]);
    ecx = target;
    if (ecx == 0u || flag != 0u)
        goto defaults;
    eax = width;                                        /* call [vt+0x28] */
    ecx = target;                                       /* mov ecx, [esi+0x18] */
    c->x[0] = (xmm_t){0}; c->x[0].u32[0] = eax;         /* movd xmm0, eax */
    { int32_t _a = c->x[0].i32[0], _b = c->x[0].i32[1];
      c->x[0].d[0] = (double)_a; c->x[0].d[1] = (double)_b; }   /* cvtdq2pd */
    eax >>= 31u;                                        /* shr eax, 0x1f */
    c->x[0].d[0] = (double)(c->x[0].d[0] + ldd(eax * 8u + KBP_U32_BIAS));
    { double _a = c->x[0].d[0], _b = c->x[0].d[1];
      c->x[0] = (xmm_t){0}; c->x[0].f[0] = (float)_a; c->x[0].f[1] = (float)_b; }
    w_bits = c->x[0].u32[0];                            /* movss [ebp-0x48], xmm0 */
    eax = height;                                       /* call [vt+0x2c] */
    ecx = target;
    c->x[1].u32[0] = w_bits;                            /* movss xmm1, [ebp-0x48] */
    c->x[0] = (xmm_t){0}; c->x[0].u32[0] = eax;
    { int32_t _a = c->x[0].i32[0], _b = c->x[0].i32[1];
      c->x[0].d[0] = (double)_a; c->x[0].d[1] = (double)_b; }
    eax >>= 31u;
    c->x[0].d[0] = (double)(c->x[0].d[0] + ldd(eax * 8u + KBP_U32_BIAS));
    { double _a = c->x[0].d[0], _b = c->x[0].d[1];
      c->x[4] = (xmm_t){0}; c->x[4].f[0] = (float)_a; c->x[4].f[1] = (float)_b; }
    goto common;
defaults:
    c->x[1].f[0] = ldf(KBP_DEFAULT_WIDTH);              /* 005652b0 */
    c->x[4].f[0] = ldf(KBP_DEFAULT_HEIGHT);
common:
    c->x[0].f[0] = ldf(KBP_TWO);                        /* 005652c0 */
    GUEST_XMM_MOV(5, 1);
    c->x[3] = (xmm_t){0};
    c->x[1].f[0] = (float)(c->x[1].f[0] + c->x[3].f[0]);
    c->x[5].f[0] = (float)(c->x[5].f[0] - c->x[3].f[0]);
    GUEST_XMM_MOV(6, 0);
    GUEST_XMM_MOV(2, 4);
    c->x[2].f[0] = (float)(c->x[2].f[0] + c->x[3].f[0]);
    GUEST_XMM_LANEOP_M(u32, 4, ^, 1, KBP_SIGN_MASK);
    matrix[10] = 0xba83126fu;                           /* [ebp-0x1c] */
    GUEST_XMM_LANEOP_M(u32, 4, ^, 2, KBP_SIGN_MASK);
    matrix[14] = 0x80000000u;                           /* [ebp-0xc] */
    matrix[15] = 0x3f800000u;                           /* [ebp-8] */
    c->x[6].f[0] = (float)(c->x[6].f[0] / c->x[5].f[0]);
    c->x[1].f[0] = (float)(c->x[1].f[0] / c->x[5].f[0]);
    matrix[0] = c->x[6].u32[0];                         /* [ebp-0x44] */
    matrix[12] = c->x[1].u32[0];                        /* [ebp-0x14] */
    if (ecx != 0u) {
        c->x[4].f[0] = (float)(c->x[4].f[0] - c->x[3].f[0]);
        c->x[0].f[0] = (float)(c->x[0].f[0] / c->x[4].f[0]);
        c->x[2].f[0] = (float)(c->x[2].f[0] / c->x[4].f[0]);
    } else {
        c->x[3].f[0] = (float)(c->x[3].f[0] - c->x[4].f[0]);
        c->x[0].f[0] = (float)(c->x[0].f[0] / c->x[3].f[0]);
        c->x[2].f[0] = (float)(c->x[2].f[0] / c->x[3].f[0]);
    }
    matrix[13] = c->x[2].u32[0];                        /* [ebp-0x10] */
    matrix[5] = c->x[0].u32[0];                         /* [ebp-0x30] */
}

static void kbp_log(void)
{
    uint32_t binds = s_handled + s_declined;
    uint32_t stats[5];

    if (binds != s_next_log)
        return;
    s_next_log = binds < 4096u ? binds * 2u : binds + 4096u;
    isaac_gl_record_bind_packet_stats(stats);
    isaac_vita_log(
        "KAGE VITA BIND PACKET: binds=%u handled=%u declined=%u "
        "(frame=%u object=%u records=%u image=%u target=%u tokens=%u "
        "recorder=%u owner=%u) cold=%u packets=%u staged=%u memo=%u "
        "cache=%u miss=%u\n",
        (unsigned)binds, (unsigned)s_handled, (unsigned)s_declined,
        (unsigned)s_decline[KBP_DECLINE_FRAME],
        (unsigned)s_decline[KBP_DECLINE_OBJECT],
        (unsigned)s_decline[KBP_DECLINE_RECORDS],
        (unsigned)s_decline[KBP_DECLINE_IMAGE],
        (unsigned)s_decline[KBP_DECLINE_TARGET],
        (unsigned)s_decline[KBP_DECLINE_TOKENS],
        (unsigned)s_decline[KBP_DECLINE_RECORDER],
        (unsigned)s_decline[KBP_DECLINE_OWNER], (unsigned)s_cold,
        (unsigned)stats[0], (unsigned)stats[1], (unsigned)stats[2],
        (unsigned)stats[3], (unsigned)stats[4]);
}

static int kbp_decline(int reason)
{
    ++s_declined;
    ++s_decline[reason];
    kbp_log();
    return 0;
}

static int kbp_try(CPU *__restrict c)
{
    kbp_state k;
    uint32_t capacity, offset, self, vtable, list, begin, end;
    uint32_t image, target, flag, shader, program, transform_program = 0u;
    uint32_t width = 0u, height = 0u, notes, low;
    const kbp_getter *get_width = NULL, *get_height = NULL;
    guest_gl_int transform_location = 0;
    uint32_t matrix[16];

    if (!c || !guest_stack_fast_bound(c))
        return kbp_decline(KBP_DECLINE_FRAME);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (capacity < KBP_FRAME_BYTES || offset > capacity - KBP_FRAME_BYTES ||
            offset < KBP_DEPTH_BYTES ||
            !(c->stack_ceiling <= GUEST_IMAGE_BASE ||
              c->stack_floor >= GUEST_IMAGE_BASE + KBP_IMAGE_SIZE))
        return kbp_decline(KBP_DECLINE_FRAME);

    /* Shader object and its record list (005671d7..005671fe). */
    self = c->ecx;
    if (!kbp_not_stack(c, self, KBP_OFF_PROGRAM + 4u))
        return kbp_decline(KBP_DECLINE_OBJECT);
    vtable = ld32(self);
    if (vtable != KBP_SHADER_VTABLE ||
            ld32(vtable + KBP_OFF_SET_UNIFORM_INT) != KBP_SET_UNIFORM_INT)
        return kbp_decline(KBP_DECLINE_OBJECT);
    list = ld32(c->esp + 4u);
    if (!list)
        list = self + KBP_OFF_RECORDS_LIST;
    else if (!kbp_not_stack(c, list, 8u))
        return kbp_decline(KBP_DECLINE_OBJECT);
    begin = ld32(list);
    end = ld32(list + 4u);
    if (((int32_t)(end - begin) >> 2) != 0)
        return kbp_decline(KBP_DECLINE_RECORDS);

    /* Current texture Image (00567272..0056729b). */
    image = ld32(KBP_CURRENT_IMAGE);
    if (image) {
        if (!kbp_not_stack(c, image, KBP_IMAGE_READ_BYTES) ||
                ld32(ld32(image) + KBP_OFF_IS_EMPTY) != KBP_IMAGE_IS_EMPTY)
            return kbp_decline(KBP_DECLINE_IMAGE);
    }

    /* RenderManager state Transform reads (00565253..0056538c). */
    target = ld32(KBP_MANAGER_TARGET);
    flag = ld8(KBP_MANAGER_FLAG);
    shader = ld32(KBP_MANAGER_SHADER);
    if (target && flag == 0u) {
        uint32_t target_vtable;
        if (!kbp_not_stack(c, target, KBP_TARGET_READ_BYTES))
            return kbp_decline(KBP_DECLINE_TARGET);
        target_vtable = ld32(target);
        get_width = kbp_getter_for(ld32(target_vtable + KBP_TARGET_OFF_WIDTH));
        get_height = kbp_getter_for(ld32(target_vtable + KBP_TARGET_OFF_HEIGHT));
        if (!get_width || !get_height)
            return kbp_decline(KBP_DECLINE_TARGET);
    }
    if (shader && !kbp_not_stack(c, shader, KBP_OFF_PROGRAM + 4u))
        return kbp_decline(KBP_DECLINE_TARGET);

    if (!kbp_tokens_ready() || !guest_gl_dynamic_first_holds() ||
            ld32(KBP_SLOT_USE_PROGRAM) != s_tok_use ||
            ld32(KBP_SLOT_ACTIVE_TEXTURE) != s_tok_active ||
            ld32(KBP_SLOT_BIND_TEXTURE) != s_tok_bind ||
            ld32(KBP_SLOT_GET_UNIFORM) != s_tok_get ||
            ld32(KBP_SLOT_UNIFORM1I) != s_tok_int ||
            ld32(KBP_SLOT_MATRIX4) != s_tok_matrix)
        return kbp_decline(KBP_DECLINE_TOKENS);
    k.backend = guest_gl_installed_backend();
    if (!isaac_gl_record_bind_packet_admit(k.backend))
        return kbp_decline(KBP_DECLINE_RECORDER);
    if (!guest_gl_owned_enter(c))
        return kbp_decline(KBP_DECLINE_OWNER);

    /* ---- every check passed: the replay, in the translated order ---- */
    k.c = c;
    k.entry = c->esp;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    k.enclosing = g_kage_guest_last_indirect_target;
#else
    k.enclosing = 0u;
#endif
    program = ld32(self + KBP_OFF_PROGRAM);
    if (!ld32(k.entry + 4u))                    /* 005671ec mov [ebp+8], eax */
        st32(k.entry + 4u, list);
    KBP_PUBLISH(s_tok_use);
    isaac_gl_record_bind_packet_use_program(KBP_RET_USE_PROGRAM, program);
    KBP_PUBLISH(k.enclosing);
    notes = 2u;                                 /* Bind, Transform */
    if (image) {
        (void)kbp_set_uniform_int(&k, ld32(self + KBP_OFF_PROGRAM),
                                  KBP_NAME_TEXTURE0, 0u, image, 0u);
        image = ld32(KBP_CURRENT_IMAGE);        /* 0056728c reload */
        KBP_PUBLISH(KBP_IMAGE_IS_EMPTY);
        KBP_PUBLISH(k.enclosing);
        notes += 2u;
        if (ld32(image + KBP_IMAGE_OFF_HANDLE) == 0u) {
            (void)kbp_set_uniform_int(&k, ld32(self + KBP_OFF_PROGRAM),
                                      KBP_NAME_TEXTURE1, 1u,
                                      ld32(KBP_CURRENT_IMAGE), 1u);
            ++notes;
        }
    }
    /* Transform (005672b1 call 0x565240). */
    if (get_width) {
        KBP_PUBLISH(KBP_VA(get_width->rva));
        width = kbp_getter_value(get_width, target);
        KBP_PUBLISH(k.enclosing);
        KBP_PUBLISH(KBP_VA(get_height->rva));
        height = kbp_getter_value(get_height, target);
        KBP_PUBLISH(k.enclosing);
        notes += 2u;
    }
    kbp_transform_math(c, target, flag, width, height, matrix);
    if (shader) {
        transform_program = ld32(shader + KBP_OFF_PROGRAM);
        KBP_PUBLISH(s_tok_get);
        transform_location = kbp_location(&k, KBP_RET_GET_UNIFORM_TRANSFORM, 2u,
                                          KBP_COLD_RET_TRANSFORM,
                                          transform_program,
                                          KBP_NAME_TRANSFORM);
        KBP_PUBLISH(k.enclosing);
        KBP_PUBLISH(s_tok_matrix);
        isaac_gl_record_bind_packet_uniform_matrix4fv(
            KBP_RET_MATRIX4, transform_location, 1, 0u,
            k.entry - KBP_MATRIX_BELOW_ENTRY, matrix);
        KBP_PUBLISH(k.enclosing);
    }
#if !GUEST_LEAF_INLINE
    ++notes;                                    /* sub_005eacd7 entered */
#endif
    isaac_gl_record_bind_packet_flush();
    guest_gl_owned_leave(c, shader ? s_tok_matrix : s_tok_use);

    /* Stack low water the translated bodies leave.  Raw generated pushes
     * (the production shape) note nothing themselves; the recorder reads
     * every GL call's return word through guest_stack_address, so the
     * lowest of those is the mark: glUniformMatrix4fv's E-116, else
     * SetUniformInt's glGetUniformLocation E-60, else glUseProgram's E-24.
     * Checked pushes note every push: E-116, else the getter call's return
     * word E-104, else Transform's `push esi` E-100. */
#if GUEST_GENERATED_STACK_GUARD
    low = shader ? 116u : get_width ? 104u : 100u;
#else
    low = shader ? 116u : image ? 60u : 24u;
#endif
    guest_stack_note_low(c, k.entry - low);
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    g_guest_function_entries += notes;
#else
    (void)notes;
#endif

    /* Exit state of `ret 4`: EAX from Transform's tail (the location, which
     * the void glUniformMatrix4fv adapter leaves, or [esi+0x14] == 0), ECX
     * the cookie compare operand, EDX the record list's begin word. */
    c->eax = shader ? (uint32_t)transform_location : 0u;
    c->ecx = ld32(KBP_COOKIE);
    c->edx = begin;
    c->esp = k.entry + KBP_FRAME_BYTES;
    ++s_handled;
    kbp_log();
    return 1;
}

void __wrap_sub_005671d0(CPU *__restrict c)
{
    if (!kbp_try(c))
        __real_sub_005671d0(c);
}

void isaac_vita_kage_bind_packet_counts(uint32_t out[3])
{
    out[0] = s_handled;
    out[1] = s_declined;
    out[2] = s_cold;
}

void isaac_vita_kage_bind_packet_declines(uint32_t out[8])
{
    uint32_t i;
    for (i = 0u; i < KBP_DECLINE_COUNT; ++i)
        out[i] = s_decline[i];
}
