#ifndef ISAAC_HOST_VITA_KAGE_QUAD_FASTPATH_H
#define ISAAC_HOST_VITA_KAGE_QUAD_FASTPATH_H

#include <stdint.h>

#include "guest.h"

/* ISAAC_VITA_KAGE_QUAD_FASTPATH (wf/cpu-render): host owner of the
 * authenticated KAGE::Graphics::Image::PushQuad body.
 *
 *   sub_0055f990  Image::PushQuad(this=ecx, uv, quad, c0, c1, c2, c3)
 *                 cull against the render target size (GetWidth/GetHeight
 *                 through the target's vtable, or 480x270); when the image
 *                 is not batched select the blend state and the batch
 *                 (sub_005607a0); pixel-snap the quad (four floor imports),
 *                 reserve 6 indices + 4 vertices in the batch rings
 *                 (sub_0055ee80), write the interleaved vertices per
 *                 attribute format (sub_00561e20 sizes for the formats it
 *                 does not fill), step the global depth cursor, push the
 *                 image onto the dirty vector (sub_00026fb0 when it grows);
 *                 when not batched reset the blend state (sub_00561ff0 when
 *                 [this+0x70]) and the image's batch words; ret 0x18.
 *
 * `_try` is called by the generated body marker at the root instruction
 * (gen_all.py VITA_KAGE_QUAD_FASTPATH_*) with the CPU exactly as the caller's
 * `call sub_0055f990` left it: ECX = this, ESP at the pushed return word,
 * the six stdcall arguments above it.  HANDLED (1) means every guest memory
 * write of the translated body above the final ESP was performed with the
 * same bytes (indices, vertices, ring cursors, depth cursor, dirty vector,
 * image flags and batch words), the same imports were accounted (four floor
 * calls with the receipts of the floor thunk fast path, the two getters
 * through guest_call), the translated tail callees the body reaches by
 * `call rel32` (batch select, growing ring reserves, dirty-vector growth,
 * blend reset) were entered by direct call with the site's exact register
 * and stack state, and the `ret 0x18` was performed (ESP += 28, EAX/ECX/EDX
 * as the x86 leaves them, EBX/ESI/EDI/EBP as the pops restore them; lazy
 * flags untouched: the body's flags are dead at its RETs).  REJECTED (0)
 * leaves the CPU, guest memory and every census untouched so the translated
 * body that follows the marker decides; every decline is taken before the
 * first side effect (the cull getters, the batch select).
 *
 * ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE runs the decline checks, counts the
 * verdicts and always returns 0 (the translated body runs). */
int isaac_vita_kage_quad_try(CPU *__restrict c);

/* Decline reasons, in the order of the ph120.kq d(...) legend.  Every one is
 * decided from reads alone before the first side effect; the states the
 * x86 would fault on (null pointers it dereferences) are declined when they
 * are visible at entry and fault with a message when they can only appear
 * after the batch select. */
enum {
    ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME = 0,       /* f:  CPU/stack window */
    ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT, /* oa: null this/quad/formats/uv/colour */
    ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BATCH,    /* ob: null batch or ring records */
    ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER,   /* on: null ring buffer the x86 stores through */
    ISAAC_VITA_KAGE_QUAD_DECLINE_COUNT_BOUND,     /* c:  attribute count > 16 */
    ISAAC_VITA_KAGE_QUAD_DECLINE_FORMAT,          /* a:  attribute format outside 1..8 */
    ISAAC_VITA_KAGE_QUAD_DECLINE_STRIDE,          /* s:  format sizes do not sum to the stride */
    ISAAC_VITA_KAGE_QUAD_DECLINE_FLOOR,           /* g:  floor import route not authenticated */
    ISAAC_VITA_KAGE_QUAD_DECLINE_GETTER,          /* t:  target vtable/getter slot null */
    ISAAC_VITA_KAGE_QUAD_DECLINE_REASONS
};

typedef struct IsaacVitaKageQuadCounters {
    uint32_t mode;          /* 1 = fast path retires handled quads, 2 = OBSERVE */
    uint32_t calls;         /* `_try` entries */
    uint32_t handled;       /* every check passed (OBSERVE: would have replayed) */
    uint32_t culled;        /* x: handled quads that took the cull exit */
    uint32_t constant_cull; /* k: handled quads culled against 480x270 */
    uint32_t getters;       /* g: handled quads that called GetWidth/GetHeight */
    uint32_t snapped;       /* s: handled quads that took the snap path (4 floors) */
    uint32_t unbatched;     /* b: handled quads that entered the batch select */
    uint32_t translucent;   /* t: unbatched quads with alpha sum < 4 (bool 1) */
    uint32_t fresh_records; /* f: ring records with no buffer and capacity 0 */
    uint32_t ring_growths;  /* r: ring reserves delegated to the translated Ring::Alloc */
    uint32_t dirty_growths; /* d: dirty pushes delegated to the translated growth */
    uint32_t declined[ISAAC_VITA_KAGE_QUAD_DECLINE_REASONS];
} IsaacVitaKageQuadCounters;

/* Take-and-zero for the phase profiler (ph120.kq); NULL discards. */
void isaac_vita_kage_quad_fastpath_take(IsaacVitaKageQuadCounters *out);

/* Frozen facts shared with the oracle (recomp/runtime/
 * host_vita_kage_quad_fastpath_oracle.c).  RVAs are image-relative. */
#define ISAAC_VITA_KAGE_QUAD_ROOT_RVA             0x0055F990u
#define ISAAC_VITA_KAGE_QUAD_RING_ALLOC_RVA       0x0055EE80u
#define ISAAC_VITA_KAGE_QUAD_FORMAT_SIZE_RVA      0x00561E20u
#define ISAAC_VITA_KAGE_QUAD_SNAP_REBUILD_RVA     0x0055E590u
#define ISAAC_VITA_KAGE_QUAD_FLOOR_THUNK_RVA      0x005EC3B2u
#define ISAAC_VITA_KAGE_QUAD_FLOOR_IAT_RVA        0x00606524u
#define ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID      313u
/* Translated tail callees entered by direct call (their `ret N`). */
#define ISAAC_VITA_KAGE_QUAD_BATCH_SELECT_RVA     0x005607A0u  /* ret 4 */
#define ISAAC_VITA_KAGE_QUAD_BLEND_RESET_RVA      0x00561FF0u  /* ret 4 */
#define ISAAC_VITA_KAGE_QUAD_DIRTY_GROW_RVA       0x00026FB0u  /* ret 8 */
/* Return words the translated `call eax` getter sites push (RVAs). */
#define ISAAC_VITA_KAGE_QUAD_RET_GET_WIDTH        0x0055F9F5u
#define ISAAC_VITA_KAGE_QUAD_RET_GET_HEIGHT       0x0055FA1Eu
/* Return words the translated `call rel32` tail sites push (RVAs). */
#define ISAAC_VITA_KAGE_QUAD_RET_BATCH_SELECT     0x0055FB74u
#define ISAAC_VITA_KAGE_QUAD_RET_RING_INDEX       0x0055FD59u
#define ISAAC_VITA_KAGE_QUAD_RET_RING_VERTEX      0x0055FD6Cu
#define ISAAC_VITA_KAGE_QUAD_RET_DIRTY_GROW       0x005601F3u
#define ISAAC_VITA_KAGE_QUAD_RET_BLEND_RESET      0x00560218u
/* Globals (RVAs). */
#define ISAAC_VITA_KAGE_QUAD_G_RENDERER           0x007C7A30u  /* Renderer singleton */
#define ISAAC_VITA_KAGE_QUAD_G_CULL_TARGET        0x007C7A48u  /* u32 target */
#define ISAAC_VITA_KAGE_QUAD_G_SIZE_OVERRIDE      0x007C7A4Cu  /* u8 */
#define ISAAC_VITA_KAGE_QUAD_G_CULL_ENABLE        0x007C7A4Du  /* u8 */
#define ISAAC_VITA_KAGE_QUAD_G_SNAP_ENABLE        0x007C7A4Eu  /* u8 */
#define ISAAC_VITA_KAGE_QUAD_G_PREMULTIPLY        0x007C7A4Fu  /* u8 */
#define ISAAC_VITA_KAGE_QUAD_G_DEPTH              0x007BF894u  /* f32 */
#define ISAAC_VITA_KAGE_QUAD_G_DIRTY_VECTOR       0x007E7D10u  /* begin */
#define ISAAC_VITA_KAGE_QUAD_G_DIRTY_END          0x007E7D14u  /* u32 ptr */
#define ISAAC_VITA_KAGE_QUAD_G_DIRTY_CAP          0x007E7D18u  /* u32 ptr */
#define ISAAC_VITA_KAGE_QUAD_G_RESET_OFFSET_X     0x00802FECu  /* f32 */
#define ISAAC_VITA_KAGE_QUAD_G_RESET_OFFSET_Y     0x00802FF0u  /* f32 */
/* .rdata constants the body reads (RVAs). */
#define ISAAC_VITA_KAGE_QUAD_C_ZERO               0x0076A18Cu  /* 0.0f */
#define ISAAC_VITA_KAGE_QUAD_C_HALF               0x0076A480u  /* 0.5f */
#define ISAAC_VITA_KAGE_QUAD_C_DEPTH_STEP         0x0076A214u  /* 0.01f */
#define ISAAC_VITA_KAGE_QUAD_C_ALPHA_OPAQUE       0x0076A8CCu  /* 4.0f */
#define ISAAC_VITA_KAGE_QUAD_C_FALLBACK_WIDTH     0x007AA92Cu  /* 480.0f */
#define ISAAC_VITA_KAGE_QUAD_C_FALLBACK_HEIGHT    0x007AA930u  /* 270.0f */
#define ISAAC_VITA_KAGE_QUAD_C_U32_TO_F64         0x0076C940u  /* {0.0, 2^32} */
/* 16-byte blend descriptors the select compares with the current one at
 * G_RENDERER+4 (default) and installs (translucent, premultiplied or not). */
#define ISAAC_VITA_KAGE_QUAD_C_BLEND_DEFAULT      0x0076B060u  /* {1,0,1,0} */
#define ISAAC_VITA_KAGE_QUAD_C_BLEND_PREMULTIPLIED 0x0076B3B0u /* {1,7,1,7} */
#define ISAAC_VITA_KAGE_QUAD_C_BLEND_TRANSLUCENT  0x0076B3C0u  /* {6,7,1,7} */
/* Image object layout read by the body. */
#define ISAAC_VITA_KAGE_QUAD_OFF_UV_SCALE_U       0x08u
#define ISAAC_VITA_KAGE_QUAD_OFF_UV_SCALE_V       0x0Cu
#define ISAAC_VITA_KAGE_QUAD_OFF_FLAGS            0x10u
#define ISAAC_VITA_KAGE_QUAD_OFF_FLAGS_SHADOW     0x14u
#define ISAAC_VITA_KAGE_QUAD_OFF_SNAP_WORD_A      0x24u
#define ISAAC_VITA_KAGE_QUAD_OFF_SNAP_WORD_B      0x28u
#define ISAAC_VITA_KAGE_QUAD_OFF_FORMATS          0x34u
#define ISAAC_VITA_KAGE_QUAD_OFF_FORMAT_COUNT     0x38u  /* u8 */
#define ISAAC_VITA_KAGE_QUAD_OFF_STRIDE           0x3Au  /* u16 */
#define ISAAC_VITA_KAGE_QUAD_OFF_BATCH            0x60u
#define ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_X         0x64u
#define ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_Y         0x68u
#define ISAAC_VITA_KAGE_QUAD_OFF_SNAP_SCALE       0x6Cu
#define ISAAC_VITA_KAGE_QUAD_OFF_BLEND_RESET      0x70u  /* u8: blend reset pending */
#define ISAAC_VITA_KAGE_QUAD_OFF_DIRTY            0x71u  /* u8 */
#define ISAAC_VITA_KAGE_QUAD_FLAG_DIRTY           0x4u
#define ISAAC_VITA_KAGE_QUAD_FLAG_BATCHED         0x20u
/* Batch object and its two rings {records, index, element size}; records
 * are 16 bytes {ptr, count, capacity, base}. */
#define ISAAC_VITA_KAGE_QUAD_BATCH_VERTEX_RING    0x20u
#define ISAAC_VITA_KAGE_QUAD_BATCH_INDEX_RING     0x2Cu
#define ISAAC_VITA_KAGE_QUAD_RING_RECORDS         0x0u
#define ISAAC_VITA_KAGE_QUAD_RING_INDEX           0x4u
#define ISAAC_VITA_KAGE_QUAD_RING_ELEMENT         0x8u
#define ISAAC_VITA_KAGE_QUAD_REC_PTR              0x0u
#define ISAAC_VITA_KAGE_QUAD_REC_COUNT            0x4u
#define ISAAC_VITA_KAGE_QUAD_REC_CAPACITY         0x8u
#define ISAAC_VITA_KAGE_QUAD_REC_BASE             0xCu
#define ISAAC_VITA_KAGE_QUAD_REC_BYTES            16u
#define ISAAC_VITA_KAGE_QUAD_INDEX_BYTES          2u
/* Target vtable slots the cull path calls. */
#define ISAAC_VITA_KAGE_QUAD_VT_GET_WIDTH         0x28u
#define ISAAC_VITA_KAGE_QUAD_VT_GET_HEIGHT        0x2Cu
/* Attribute formats 1..8 (frozen jump table at RVA 0x561e58): byte sizes
 * 4,8,12,16,12,16,8,4; formats 5 (position), 6 (color) and 7 (uv) are the
 * ones the body fills.  Any other format takes the translated body (its
 * "Unknown attribute format" log). */
#define ISAAC_VITA_KAGE_QUAD_FORMAT_COUNT         8u
#define ISAAC_VITA_KAGE_QUAD_FORMAT_POSITION      5u
#define ISAAC_VITA_KAGE_QUAD_FORMAT_COLOR         6u
#define ISAAC_VITA_KAGE_QUAD_FORMAT_UV            7u
/* Fail-closed bound on the attribute count (the frozen vertex layouts
 * declare at most 7). */
#define ISAAC_VITA_KAGE_QUAD_MAX_COUNT            16u
/* Stack window: the six stdcall arguments plus the return word above ESP;
 * below it the aligned 0x108-byte frame, the two saved registers, the floor
 * argument, the four rebuild pointers and the deepest callee frame
 * (sub_0055e590: 0x24 bytes) - 0x14b at most, rounded up.  The tail callees
 * check their own frames the way the translated body's calls do. */
#define ISAAC_VITA_KAGE_QUAD_FRAME_BYTES          28u
#define ISAAC_VITA_KAGE_QUAD_DEPTH_BYTES          0x160u
/* Frame slot the dirty-vector growth reads through [ebp+0xc]: the x86 stores
 * `this` at [F+0x34] before the push (005601c2). */
#define ISAAC_VITA_KAGE_QUAD_FRAME_THIS_SLOT      0x34u

#endif
