/* ISAAC_VITA_KAGE_QUAD_FASTPATH (wf/cpu-render): host replay of
 * KAGE::Graphics::Image::PushQuad (sub_0055f990).
 *
 * What the translated body costs.  Every sprite, tile and text glyph is one
 * PushQuad: 491 x86 instructions rendered as C with lazy flags, a 0x108-byte
 * aligned frame the generated code spills through GSTACK_ADDR, two ring
 * reserves (sub_0055ee80, 73 instructions each), one attribute size lookup
 * per format the body does not fill (sub_00561e20), a snapped-quad rebuild
 * (sub_0055e590) and four floor imports per snapped quad, each a full
 * import crossing.  Heavy rooms issue ~2000 of these per present.
 *
 * What this file does.  After authenticating the shape the body assumes
 * from reads alone (bound stack window, at most 16 attributes all in formats
 * 1..8 whose sizes sum to the vertex stride, the argument pointers the x86
 * dereferences, for a batched image the batch/ring records the x86 indexes
 * and the ring buffers it stores through, the target's vtable/getter slots,
 * the floor import route), it performs the SAME guest memory writes above
 * the final ESP with the SAME bytes, calls the two virtual getters through
 * guest_call exactly like the translated `call eax` sites, enters the
 * translated tail callees the body reaches by `call rel32` - the blend/batch
 * select of an unbatched image (sub_005607a0), a ring reserve that grows
 * (sub_0055ee80), a dirty-vector push that grows (sub_00026fb0), the blend
 * reset (sub_00561ff0) - by direct call with the site's exact register and
 * stack state (return word pushed, arguments at the x86 slots, ESP at the
 * return word, ESP checked back to the frame; no dispatcher lookup, no
 * census note: a translated direct edge publishes nothing and the callee
 * publishes its own coverage), writes the floor import receipts the floor
 * thunk fast path writes, then performs the `ret 0x18`.  The frame words
 * below the final ESP (the body's spills, the floor argument, the rebuild
 * scratch) are reproduced only where a callee reads them ([F+0x34] = this
 * for the dirty-vector growth); nothing else reads them after the RET.
 *
 * Declines happen before the first side effect (the getters, the batch
 * select).  After the batch select nothing can decline: every state is
 * mirrored (element sizes are what the reserve computes with, whatever they
 * are), delegated to the translated callee (growth), or a fault the x86
 * takes too (null batch/records/buffer the x86 dereferences), reported
 * through guest_fault with a message instead of the x86 access violation.
 *
 * x86 facts replayed (recomp/gen_all.py pins the bytes, CFG, the size
 * table and the tail callee bodies; the oracle
 * host_vita_kage_quad_fastpath_oracle.c runs this code against the
 * translated bodies on synthetic images):
 *   frame    push ebp; mov ebp,esp; and esp,-16; sub esp,0x108; push esi;
 *            push edi  ->  F = ((ESP - 4) & ~15) - 0x110; [F+0x24..0x30] =
 *            c0..c3, [F+0x38] = this, [F+0x7c] = uv
 *   cull     if byte[cull_enable]: target=[cull_target]; W,H = target &&
 *            !byte[size_override] ? (float)((double)(int32)get() +
 *            tbl[get()>>31]) via [[target]+0x28]/[+0x2c] : 480.0f, 270.0f;
 *            minx=min(x3,min(x2,min(x1,min(x0,W)))) (minss: second operand
 *            on NaN/equal), maxx=max(x3,max(x2,max(x1,max(x0,K0)))),
 *            miny/maxy likewise with H and xorps zero; cull (eax=0, ecx=y0
 *            bits, edx=c1, ret) unless minx<W && 0<maxx && miny<H && 0<maxy
 *            with comiss CF semantics (unordered counts as below).
 *   select   0055fb3f, ([this+0x10] & 0x20) == 0: sum = ((a0+a1)+a2)+a3 of
 *            the colours' [+0xc]; bool = comiss 4.0f,sum; seta (ordered and
 *            4.0 > sum); push bool; call sub_005607a0 with eax=bool,
 *            ecx=this, edx=c1, esi=this, edi=quad, ebp=entry ESP-4, [F-4]=
 *            bool, [F-8]=0x55fb74; ret 4 -> ESP=F.  Its return values are
 *            dead (0055fb74..0055fd30 write eax/ecx/edx before reading).
 *   snap     0055fb74, after the select: if ([this+0x24]==0 || [this+0x28]
 *            ==0) && byte[snap_enable] && x0 == x2 (ucomiss ordered equal on
 *            the frame copy): for v in y3,x3,y0,x0: t=(float)((float)(s*v)+
 *            0.5f); r=(float)floor((double)t); n=(float)(r/s) with
 *            s=[this+0x6c] re-read per use; quad := (nx0,ny0),(nx3,ny0),
 *            (nx0,ny3),(nx3,ny3).
 *   rings    0055fd14: stride, batch=[this+0x60], offx/offy captured,
 *            vrecords=[batch+0x20], base = vrec.count - vrec.base; push 6;
 *            call Ring::Alloc(batch+0x2c) with eax=vrecords, ecx=ring,
 *            edx=batch, esi=base, edi=quad, [F-8]=0x55fd59; then batch
 *            re-read from [[F+0x38]+0x60]; push 4; call Ring::Alloc(batch+
 *            0x20) with eax=iptr, ecx=ring, esi=base, edi=iptr, [F-8]=
 *            0x55fd6c.  Ring::Alloc: element 0 -> 0 (record untouched);
 *            rec = records + idx*16; count + n <= cap -> ptr = rec.ptr +
 *            elem*count, rec.count += n; else grow through the allocator
 *            vtable [0x7e8170]+0x20/+0x24 and the memcpy thunk (replayed by
 *            the translated body itself).  u16 indices b,b+2,b+1,b+1,b+2,b+3
 *            at iptr+0,2,4,6,8,10 (store order 0,2,8,4,6,10); depth read
 *            after both reserves.
 *   vertices cursor = vptr; per attribute i: fmt 5 -> per vertex k:
 *            (float)(offx+Qx_k), (float)(offy+Qy_k), depth at cursor+k*
 *            stride, cursor += 12; fmt 7 -> (float)(u_k*su), (float)(v_k*sv),
 *            cursor += 8; fmt 6 -> premultiplied ? (a*r,a*g,a*b,a) computed
 *            for all four colours then stored : 16-byte copies interleaved
 *            with their loads, cursor += 16; other -> cursor += size(fmt).
 *   dirty    005601a6: depth' = (float)(depth - 0.01f) stored; if byte[this+
 *            0x71]==0 && !([this+0x10] & 4): [F+0x34] = this; end == cap ->
 *            push &[F+0x34]; push end; call sub_00026fb0(0x7e7d10) with
 *            eax=end, edx=count, esi=this, edi=(count ? 3*stride : iptr),
 *            [F-12]=0x5601f3, ret 8; else *end++ = this; then [this+0x14]
 *            rewritten, [this+0x10] |= 4.
 *   tail     005601fd: [this+0x10] re-read; bit 0x20 clear -> if byte[this+
 *            0x70] != 0: push 0; call sub_00561ff0(0x7c7a30) with eax=0,
 *            edx as above (or as the growth left it), [F-8]=0x560218, ret 4;
 *            then [this+0x60]=0, [this+0x64]=f32[0x802fec], [this+0x68]=
 *            f32[0x802ff0], [this+0x6c]=0, byte[this+0x70]=0.
 *   retire   eax = vptr; edx = count and ecx = (count ? fmt5: bits(offy+y3'),
 *            fmt7: bits(v3*sv), fmt6 premultiplied: 6, fmt6 copy: c2,
 *            other: fmt-1 of the last attribute : (u16)base + 1) unless a
 *            tail callee ran (ecx/edx as the last one left them); pop edi;
 *            pop esi; mov esp,ebp; pop ebp; ret 0x18.  EBX untouched.
 *
 * ISAAC_VITA_KAGE_QUAD_SELECT_NATIVE (default OFF) replays the select
 * sub_005607a0 itself (kq_select) instead of entering it: on device 73 % of
 * fight quads are unbatched and the translated select plus its batch
 * lookup sub_005603f0 were 3.3 % of render self time (hotprof-20261012).
 * The lookup's hit path is answered natively; a miss enters the translated
 * sub_005603f0 (SEH frame, allocation, free lists), the pair-vector growth
 * the translated sub_0008d900, SetBlend and the select's two size getters
 * go through guest_call as the listing's indirect calls do. */
#include "host_vita_kage_quad_fastpath.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_import_id.h"
#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif

#if !defined(ISAAC_VITA_KAGE_QUAD_FASTPATH) || !ISAAC_VITA_KAGE_QUAD_FASTPATH
#error The Image::PushQuad fast path must only be compiled when enabled
#endif
#if !defined(ISAAC_VITA_FLOOR_THUNK_FASTPATH)
#error The Image::PushQuad fast path writes the floor thunk fast path receipts
#endif

#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
# define KQ_MODE 2u
#else
# define KQ_MODE 1u
#endif

#define KQ_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))
/* The token guest_import_call accepts for row 313: the slot's own VA. */
#define KQ_FLOOR_SLOT_VA KQ_VA(ISAAC_VITA_KAGE_QUAD_FLOOR_IAT_RVA)

/* Translated tail callees the body reaches by `call rel32`; entered here the
 * same way (generated bodies are extern symbols of their units). */
void sub_005607a0(CPU *__restrict c);   /* blend descriptor + batch select */
void sub_0055ee80(CPU *__restrict c);   /* Ring::Alloc (growth path) */
void sub_00026fb0(CPU *__restrict c);   /* dirty-image vector growth */
void sub_00561ff0(CPU *__restrict c);   /* blend descriptor reset */

/* Oracle hook (host_vita_kage_quad_fastpath_oracle.c records each floor
 * import's argument and address to compare with the translated route);
 * production expands it to nothing. */
#if !defined(ISAAC_KAGE_QUAD_ORACLE_FLOOR_NOTE)
# define ISAAC_KAGE_QUAD_ORACLE_FLOOR_NOTE(c, value, arg) ((void)0)
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER)
# define KQ_SAMPLER_PUBLISH(token) \
    (g_kage_guest_last_indirect_target = (token))
# define KQ_SAMPLER_RESTORE(value) \
    (g_kage_guest_last_indirect_target = (value))
#else
# define KQ_SAMPLER_PUBLISH(token) ((void)(token))
# define KQ_SAMPLER_RESTORE(value) ((void)(value))
#endif

static IsaacVitaKageQuadCounters s_counters;

void isaac_vita_kage_quad_fastpath_take(IsaacVitaKageQuadCounters *out)
{
    if (out) {
        *out = s_counters;
        out->mode = KQ_MODE;
    }
    memset(&s_counters, 0, sizeof s_counters);
}

/* Byte sizes of formats 1..8 (jump table at RVA 0x561e58). */
static const uint32_t k_format_bytes[ISAAC_VITA_KAGE_QUAD_FORMAT_COUNT] = {
    4u, 8u, 12u, 16u, 12u, 16u, 8u, 4u
};

#if !defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
/* Replay-only helpers (OBSERVE never replays). */

/* minss/maxss as the generated C spells them: the comparison is made in
 * double, the second operand wins on NaN and on equality. */
static inline float kq_minss(float a, float b)
{
    double _b = (double)b;
    return (float)((double)a < _b ? (double)a : _b);
}

static inline float kq_maxss(float a, float b)
{
    double _b = (double)b;
    return (float)((double)a > _b ? (double)a : _b);
}

/* comiss CF: set when unordered or a < b (jae falls through on CF=0). */
static inline int kq_below(float a, float b)
{
    double _x = (double)a, _y = (double)b;
    if (_x != _x || _y != _y)
        return 1;
    return _x < _y;
}
#endif

/* comiss a, b; seta: CF=0 and ZF=0, i.e. ordered and a > b (unordered
 * sets ZF=PF=CF=1 and yields 0). */
static inline uint32_t kq_above(float a, float b)
{
    double _x = (double)a, _y = (double)b;
    if (_x != _x || _y != _y)
        return 0U;
    return _x > _y ? 1U : 0U;
}

/* ucomiss + lahf + test ah,0x44 + jp: the snap is taken only on ordered
 * equality (ZF=1, PF=0). */
static inline int kq_equal_ordered(float a, float b)
{
    double _x = (double)a, _y = (double)b;
    if (_x != _x || _y != _y)
        return 0;
    return _x == _y;
}

#if !defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
/* movd/cvtdq2pd/shr eax,31/addsd [tbl+eax*8]/cvtpd2ps on a getter result. */
static inline float kq_u32_to_float(uint32_t v)
{
    double d = (double)(int32_t)v;
    d = (double)(d + ldd((uint32_t)(
        (v >> 31) * 8U + KQ_VA(ISAAC_VITA_KAGE_QUAD_C_U32_TO_F64))));
    return (float)d;
}

/* One floor import on the double at `arg` with the receipts of
 * isaac_vita_floor_thunk_direct_try (host_vita_floor_thunk_direct.c): the
 * translated site pushed the return word at arg-4 and the thunk's
 * GUEST_IMPORT_JMP entered the indexed direct route, which notes the
 * argument address as a low-water candidate, counts one logical call, the
 * import coverage, the per-import census slot and the host import count,
 * publishes the slot token to the sampler for the duration and pops the
 * return word.  The result is what vita_math_floor pushes:
 * fpush(c, floor(ldd(arg))), consumed here by the body's fstp dword. */
static inline float kq_floor_import(CPU *__restrict c, double value,
                                    uint32_t arg)
{
    double r;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target = g_kage_guest_last_indirect_target;
#endif
    KQ_SAMPLER_PUBLISH(KQ_FLOOR_SLOT_VA);
    GUEST_PHASE_PROFILE_NOTE_CALL();
    guest_coverage_import(ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID);
    GUEST_PHASE_PROFILE_NOTE_IMPORT(ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID);
    ++g_host_import_calls;
    guest_stack_note_low(c, arg);
    ISAAC_KAGE_QUAD_ORACLE_FLOOR_NOTE(c, value, arg);
    r = floor(value);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    KQ_SAMPLER_RESTORE(sampler_enclosing_target);
#endif
    return (float)r;
}

/* t = (float)((float)(s * v) + 0.5f); r = (float)floor((double)t);
 * n = (float)(r / s).  The body reloads s and the 0.5f constant for every
 * component from memory nothing in between writes. */
static inline float kq_snap(CPU *__restrict c, uint32_t self, float v,
                            uint32_t arg)
{
    float s = ldf(self + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_SCALE);
    float t = (float)(s * v);
    float r;
    t = (float)(t + ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_HALF)));
    r = kq_floor_import(c, (double)t, arg);
    return (float)(r / ldf(self + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_SCALE));
}
#endif

/* 0055fb74..0055fba2 on the frame copy of the quad. */
static inline int kq_snap_predicate(uint32_t self, float x0, float x2)
{
    return (ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_WORD_A) == 0U ||
            ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_WORD_B) == 0U) &&
           ld8(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_SNAP_ENABLE)) != 0U &&
           kq_equal_ordered(x0, x2);
}

/* 0055fb3f..0055fb6b: ((a0 + a1) + a2) + a3 in single precision, then
 * comiss 4.0f, sum; seta. */
static inline uint32_t kq_translucent(const uint32_t *colour)
{
    float sum = ldf(colour[0] + 0xcU);
    sum = (float)(sum + ldf(colour[1] + 0xcU));
    sum = (float)(sum + ldf(colour[2] + 0xcU));
    sum = (float)(sum + ldf(colour[3] + 0xcU));
    return kq_above(ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_ALPHA_OPAQUE)), sum);
}

#if !defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
static inline void kq_getter_call(CPU *__restrict c, uint32_t frame,
                                  uint32_t return_word, uint32_t target,
                                  uint32_t fn)
{
    /* The translated `call eax`: push the return RVA, ESP at it, then
     * guest_call with the registers the body has at the site. */
    st32(frame - 4U, return_word);
    c->esp = frame - 4U;
    c->eax = fn;
    c->ecx = target;
    guest_call(c, fn);
    if (c->esp != frame)
        guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                    "Image::PushQuad replay: getter returned with a foreign "
                    "ESP");
}

/* A translated `call rel32` as the body performs it (GPUSH of the return
 * word; GUEST_GPR_FLUSH; callee; GUEST_GPR_RELOAD): the arguments already
 * sit at [frame-4] (and [frame-8]), the return word goes below them, ESP
 * points at it and the callee is entered directly.  The seam's
 * GUEST_GPR_FLUSH published the caller's GPRs, so `c` is authoritative on
 * both sides of the call; the callee's `ret N` must bring ESP back to the
 * frame (the callee bodies are pinned by hash).  guest_fault never returns
 * (it unwinds to the run scope). */
static inline void kq_direct_call(CPU *__restrict c, uint32_t frame,
                                  uint32_t return_word, uint32_t args_bytes,
                                  void (*fn)(CPU *__restrict))
{
    st32(frame - args_bytes - 4U, return_word);
    c->esp = frame - args_bytes - 4U;
    fn(c);
    if (c->esp != frame)
        guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                    "Image::PushQuad replay: translated callee returned with "
                    "a foreign ESP");
}
#endif

/* Ring::Alloc as seen from a call site, before any side effect: the decline
 * reason the x86's fault maps to, or -1 when the reserve is replayable
 * (natively, or by the translated body when it grows).  Check order = the
 * order the x86 faults in: element size 0 returns 0 before the records are
 * touched (0055ee8c), the records are indexed next (0055eea6), the body
 * then stores through the returned pointer (`stores`: always for the index
 * ring, only with attributes for the vertex ring).  `fresh`/`grows` collect
 * the OBSERVE snapshot. */
static int kq_ring_probe(uint32_t ring, uint32_t n, int stores,
                         uint32_t *fresh, uint32_t *grows)
{
    uint32_t element = ld32(ring + ISAAC_VITA_KAGE_QUAD_RING_ELEMENT);
    uint32_t records, rec, ptr, count, cap;

    if (element == 0U)
        return stores ? ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER : -1;
    records = ld32(ring + ISAAC_VITA_KAGE_QUAD_RING_RECORDS);
    if (records == 0U)
        return ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BATCH;
    rec = records + ld32(ring + ISAAC_VITA_KAGE_QUAD_RING_INDEX) *
                    ISAAC_VITA_KAGE_QUAD_REC_BYTES;
    ptr = ld32(rec + ISAAC_VITA_KAGE_QUAD_REC_PTR);
    count = ld32(rec + ISAAC_VITA_KAGE_QUAD_REC_COUNT);
    cap = ld32(rec + ISAAC_VITA_KAGE_QUAD_REC_CAPACITY);
    *fresh += (uint32_t)(ptr == 0U && cap == 0U);
    if ((uint32_t)(count + n) <= cap)
        return (ptr == 0U && stores) ?
            ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER : -1;
    ++*grows;
    return -1;
}

#if !defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
/* Register state of a `call sub_0055ee80` site (ecx = the ring). */
typedef struct kq_site {
    uint32_t eax, edx, esi, edi, ebp, return_word;
} kq_site;

/* One reserve: natively when the record has room (0055ef17: ptr = rec.ptr
 * + element*count, rec.count += n, u32 wrap as the x86), through the
 * translated Ring::Alloc with the site's exact state when it must grow
 * (allocator vtable calls, memcpy thunk, free - all the translated body's
 * own).  The states the x86 faults on fault here with a message. */
static uint32_t kq_ring_reserve(CPU *__restrict c, uint32_t frame,
                                uint32_t ring, uint32_t n, int stores,
                                const kq_site *site, int *delegated)
{
    uint32_t element = ld32(ring + ISAAC_VITA_KAGE_QUAD_RING_ELEMENT);
    uint32_t records, rec, ptr, count, cap;

    if (element == 0U) {
        /* 0055ee8c: Ring::Alloc returns 0 without touching the record. */
        if (stores)
            guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                        "Image::PushQuad replay: ring element size 0 (the "
                        "x86 stores through a null pointer)");
        return 0U;
    }
    records = ld32(ring + ISAAC_VITA_KAGE_QUAD_RING_RECORDS);
    if (records == 0U)
        guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                    "Image::PushQuad replay: ring records null (the x86 "
                    "faults in Ring::Alloc)");
    rec = records + ld32(ring + ISAAC_VITA_KAGE_QUAD_RING_INDEX) *
                    ISAAC_VITA_KAGE_QUAD_REC_BYTES;
    ptr = ld32(rec + ISAAC_VITA_KAGE_QUAD_REC_PTR);
    count = ld32(rec + ISAAC_VITA_KAGE_QUAD_REC_COUNT);
    cap = ld32(rec + ISAAC_VITA_KAGE_QUAD_REC_CAPACITY);
    if (ptr == 0U && cap == 0U)
        ++s_counters.fresh_records;
    if ((uint32_t)(count + n) <= cap) {
        if (ptr == 0U && stores)
            guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                        "Image::PushQuad replay: ring record without a "
                        "buffer (the x86 stores through a null pointer)");
        st32(rec + ISAAC_VITA_KAGE_QUAD_REC_COUNT, (uint32_t)(count + n));
        return (uint32_t)(element * count) + ptr;
    }
    ++s_counters.ring_growths;
    *delegated = 1;
    c->eax = site->eax;
    c->ecx = ring;
    c->edx = site->edx;
    c->esi = site->esi;
    c->edi = site->edi;
    c->ebp = site->ebp;
    st32(frame - 4U, n);
    kq_direct_call(c, frame, site->return_word, 4U, sub_0055ee80);
    return c->eax;
}
#endif

#if defined(ISAAC_VITA_KAGE_QUAD_SELECT_NATIVE)
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
#error ISAAC_VITA_KAGE_QUAD_SELECT_NATIVE replays the select; OBSERVE never replays
#endif
void sub_005603f0(CPU *__restrict c);   /* batch find-or-create */
void sub_0008d900(CPU *__restrict c);   /* (batch, image) pair-vector growth */

/* Oracle hook (records a natively answered lookup the way its stand-in
 * records the translated one); production expands it to nothing. */
#if !defined(ISAAC_KAGE_QUAD_ORACLE_LOOKUP_HIT)
# define ISAAC_KAGE_QUAD_ORACLE_LOOKUP_HIT(c, batch, edx) ((void)0)
#endif

/* The hit path of sub_005603f0 (00560432..0056048b -> 0056077b), reads
 * only.  The slot's batch is a hit when its 16-byte blend descriptor equals
 * the current one at Renderer+4, its group word equals [0x7c7a44] and
 * sub_00567620(batch+0x14, group+0x1c) finds the two id vectors equal:
 * equal counts ((int32)(end - begin) >> 2 each, sar), then the elements in
 * order (a differing dword always holds a differing byte, so the byte tail
 * at 00567666 cannot turn a mismatch into a match).  1 = hit, with the
 * batch and the EDX sub_00567620 leaves (the first vector's cursor after
 * the loop: begin + 4 * count); 0 = the translated body decides (a miss, or
 * a count this replay does not bound). */
static int kq_lookup_hit(uint32_t self, uint32_t slot, uint32_t *batch_out,
                         uint32_t *edx_out)
{
    const uint32_t current = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 4U;
    uint32_t batch = ld32(self + slot), group, a, b, k;
    int32_t na, nb;

    if (batch == 0U)
        return 0;
    for (k = 0U; k < 16U; k += 4U)
        if (ld32(batch + k) != ld32(current + k))
            return 0;
    group = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_BATCH_GROUP));
    if (ld32(batch + ISAAC_VITA_KAGE_QUAD_BATCH_GROUP) != group)
        return 0;
    a = ld32(batch + ISAAC_VITA_KAGE_QUAD_BATCH_IDS);
    b = ld32(group + ISAAC_VITA_KAGE_QUAD_GROUP_IDS);
    na = (int32_t)(ld32(batch + ISAAC_VITA_KAGE_QUAD_BATCH_IDS + 4U) - a) >> 2;
    nb = (int32_t)(ld32(group + ISAAC_VITA_KAGE_QUAD_GROUP_IDS + 4U) - b) >> 2;
    if (na != nb || (uint32_t)na > ISAAC_VITA_KAGE_QUAD_LOOKUP_MAX_IDS)
        return 0;
    for (k = 0U; k < (uint32_t)na; ++k)
        if (ld32(a + 4U * k) != ld32(b + 4U * k))
            return 0;
    *batch_out = batch;
    *edx_out = a + 4U * (uint32_t)na;
    return 1;
}

/* One `push arg; mov ecx, edi; call 0x5603f0` site of the select (G = the
 * select's ESP).  A hit is answered with sub_005603f0's effects on what its
 * caller reads: EAX = the batch, EDX as sub_00567620 left it, ESI/EDI/EBX/
 * EBP restored by its pops, ESP back at G (ret 4); ECX - the security
 * cookie it pops - is overwritten before any read after every site
 * (005608b7, 005608d4, and 00560932/00560962 after 00560954) and is left
 * as `mov ecx, edi` set it.  The hit path writes nothing above ESP but
 * fs:[0], which it restores.  Everything else enters the translated body. */
static void kq_lookup(CPU *__restrict c, uint32_t g, uint32_t arg,
                      uint32_t return_word)
{
    uint32_t batch, edx;

    st32(g - 4U, arg);
    c->ecx = c->edi;
    if (kq_lookup_hit(c->edi, (arg & 0xffU) ?
                          ISAAC_VITA_KAGE_QUAD_OFF_SLOT_CUSTOM :
                          ISAAC_VITA_KAGE_QUAD_OFF_SLOT_DEFAULT,
                      &batch, &edx)) {
        st32(g - 8U, return_word);
        c->esp = g - 8U;
        ISAAC_KAGE_QUAD_ORACLE_LOOKUP_HIT(c, batch, edx);
        c->eax = batch;
        c->edx = edx;
        c->esp = g;
        return;
    }
    kq_direct_call(c, g, return_word, 4U, sub_005603f0);
}

/* sub_005607a0 (005607a0..005609e5) entered as the PushQuad site 0055fb6f
 * enters it: EAX = translucent, ECX = ESI = this, EDX = c1, EDI = quad,
 * EBP = entry ESP-4, ESP = frame after `ret 4`.  Every register the x86
 * hands a callee is set as the listing leaves it, every callee is entered
 * as the listing enters it (guest_call for `call [eax+0x38]`/`call eax`,
 * direct for `call rel32`), every write above the final ESP is the
 * listing's, in its order.  The select's own frame (saved registers, the
 * two descriptor copies, the flag bytes, the width spill) is read back by
 * nothing but the select - callees are entered below it and do not write
 * above their arguments - so those values live in locals; the one frame
 * word a callee reads is the argument dword pushed from [G+0xc] (the bool
 * byte over whatever the stack held), reproduced byte for byte. */
static void kq_select(CPU *__restrict c, uint32_t frame, uint32_t translucent)
{
    /* push ebp; mov ebp,esp; and esp,-16; sub esp,0x28; push esi; push edi */
    const uint32_t ebp = frame - 12U;
    const uint32_t g = ((ebp & ~15U) - 0x28U) - 8U;
    const uint32_t current = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 4U;
    const uint32_t standard = KQ_VA(ISAAC_VITA_KAGE_QUAD_C_BLEND_DEFAULT);
    const uint32_t self = c->ecx;
    const uint32_t saved_esi = c->esi, saved_edi = c->edi;
    const uint32_t saved_ebp = c->ebp;
    uint32_t eax = translucent, edx = g + 0x20U, esi = 0xcU;
    uint32_t group, flag = translucent, target, override_byte, i;
    int is_default = 1;
    float w, h, sw, sh;

    /* 005607cb */
    st8(g + 0xcU, (uint8_t)translucent);
    /* 005607e0: current descriptor == {1,0,1,0}; eax/edx/esi as the loop
     * leaves them. */
    for (i = 0U; i < 16U; i += 4U) {
        eax = ld32(current + i);
        if (eax != ld32(standard + i)) {
            is_default = 0;
            break;
        }
        edx += 4U;
        esi -= 4U;
    }
    /* 005607fd..00560863: the group object for [this+0x20] in 0..2. */
    group = ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_GROUP);
    if (group != 3U) {
        const uint32_t g0 = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_GROUP_0);
        const uint32_t g1 = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_GROUP_1);
        const uint32_t g2 = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_GROUP_2);
        uint32_t held = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_BATCH_GROUP));
        int known = 1;
        edx = held;
        if (held == g0)
            eax = 0U;
        else if (held == g1)
            eax = 1U;
        else if (held == g2)
            eax = 2U;
        else
            known = 0;
        if (!known || eax != group) {
            if (group == 0U) {
                st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_BATCH_GROUP), g0);
            } else if (group == 1U) {
                st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_BATCH_GROUP), g1);
            } else {
                eax = group == 2U ? g2 : held;
                st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_BATCH_GROUP), eax);
            }
        }
    }
    /* 0056086d */
    st8(self + ISAAC_VITA_KAGE_QUAD_OFF_BLEND_RESET, 0U);
    c->eax = eax;
    c->edx = edx;
    c->esi = esi;
    c->edi = self;
    c->ebp = ebp;
    if (!translucent && is_default) {
        /* 0056094b: no rebind on this path; al = [G+0xc] = 0 below. */
        kq_lookup(c, g, ld32(g + 0xcU),
                  ISAAC_VITA_KAGE_QUAD_RET_LOOKUP_DEFAULT);
        st32(c->edi + ISAAC_VITA_KAGE_QUAD_OFF_BATCH, c->eax);
    } else {
        uint32_t batch;
        if (translucent) {
            if (is_default) {
                /* 00560883..005608a8: SetBlend(descriptor by value). */
                uint32_t descriptor =
                    ld8(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREMULTIPLY)) != 0U ?
                    KQ_VA(ISAAC_VITA_KAGE_QUAD_C_BLEND_PREMULTIPLIED) :
                    KQ_VA(ISAAC_VITA_KAGE_QUAD_C_BLEND_TRANSLUCENT);
                stx(g - 0x10U, ldx(descriptor));
                st32(g - 0x14U, ISAAC_VITA_KAGE_QUAD_RET_SET_BLEND);
                c->esp = g - 0x14U;
                c->ecx = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER);
                c->eax = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER));
                guest_call(c, ld32(c->eax + ISAAC_VITA_KAGE_QUAD_VT_SET_BLEND));
                if (c->esp != g)
                    guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                                "Image::PushQuad replay: SetBlend returned "
                                "with a foreign ESP");
                st8(c->edi + ISAAC_VITA_KAGE_QUAD_OFF_BLEND_RESET, 1U);
            }
            kq_lookup(c, g, ld32(g + 0xcU),
                      ISAAC_VITA_KAGE_QUAD_RET_LOOKUP_TRANSLUCENT);
        } else {
            /* 005608c8: push 1; mov byte [esp+0xe], 1 (= [G+0xa]). */
            flag = 1U;
            kq_lookup(c, g, 1U, ISAAC_VITA_KAGE_QUAD_RET_LOOKUP_CUSTOM);
        }
        /* 005608d6: mov ecx, eax; the (batch, image) rebind. */
        batch = c->eax;
        c->ecx = batch;
        st32(c->edi + ISAAC_VITA_KAGE_QUAD_OFF_BATCH, batch);
        if (ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREV_BATCH)) != batch) {
            uint32_t prev = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREV_IMAGE));
            uint32_t end;
            if (prev != 0U
#if defined(ISAAC_VITA_KAGE_QUAD_SELECT_VANILLA_SEAM)
                    /* The generated 005608ea seam (gen_all
                     * VITA_VANILLA_KAGE_INVALIDATE_*, CMake passes this
                     * when ISAAC_VITA_VANILLA_KAGE_BUNDLE is ON): keep the
                     * current image's just-selected batch reusable. */
                    && prev != c->edi
#endif
               )
                st32(prev + ISAAC_VITA_KAGE_QUAD_OFF_SLOT_CUSTOM, 0U);
            end = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PAIRS_END));
            st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREV_IMAGE), c->edi);
            st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREV_BATCH), batch);
            if (end == ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PAIRS_CAP))) {
                /* 00560939: push 0x7e7d34; push eax; mov ecx, 0x7e7d28;
                 * call 0x8d900 (ret 8). */
                c->eax = end;
                c->ecx = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PAIRS);
                st32(g - 4U, KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREV_BATCH));
                st32(g - 8U, end);
                kq_direct_call(c, g, ISAAC_VITA_KAGE_QUAD_RET_PAIRS_GROW, 8U,
                               sub_0008d900);
            } else {
                st32(end, batch);
                st32(end + 4U, c->edi);
                st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PAIRS_END), end + 8U);
                c->eax = end;
            }
        }
    }
    /* 0056091a: the low byte is all that is stored; EAX is rewritten by
     * the getter load below. */
    st8(c->edi + ISAAC_VITA_KAGE_QUAD_OFF_DIRTY, (uint8_t)flag);
    /* 0056091d..005609db: GetWidth/GetHeight of the bound target (re-read
     * before the second call) or of the Renderer; scale = minss(w/W, h/H)
     * against the target's own size or 480x270. */
    target = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_CULL_TARGET));
    override_byte = ld8(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_SIZE_OVERRIDE));
    c->esi = target;
    if (target != 0U)
        kq_getter_call(c, g, ISAAC_VITA_KAGE_QUAD_RET_SELECT_WIDTH, target,
                       ld32(ld32(target) + ISAAC_VITA_KAGE_QUAD_VT_GET_WIDTH));
    else
        kq_getter_call(c, g, ISAAC_VITA_KAGE_QUAD_RET_SELECT_WIDTH,
                       KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER),
                       ld32(ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER)) +
                            ISAAC_VITA_KAGE_QUAD_VT_RENDERER_WIDTH));
    w = (float)(int32_t)c->eax;
    target = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_CULL_TARGET));
    if (target != 0U)
        kq_getter_call(c, g, ISAAC_VITA_KAGE_QUAD_RET_SELECT_HEIGHT, target,
                       ld32(ld32(target) + ISAAC_VITA_KAGE_QUAD_VT_GET_HEIGHT));
    else
        kq_getter_call(c, g, ISAAC_VITA_KAGE_QUAD_RET_SELECT_HEIGHT,
                       KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER),
                       ld32(ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER)) +
                            ISAAC_VITA_KAGE_QUAD_VT_RENDERER_HEIGHT));
    h = (float)(int32_t)c->eax;
    if (c->esi != 0U && override_byte == 0U) {
        sw = w;
        sh = h;
    } else {
        sw = ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_FALLBACK_WIDTH));
        sh = ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_FALLBACK_HEIGHT));
    }
    stf(c->edi + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_SCALE,
        kq_minss((float)(w / sw), (float)(h / sh)));
    /* 005609e0: pop edi; pop esi; mov esp,ebp; pop ebp; ret 4. */
    c->edi = saved_edi;
    c->esi = saved_esi;
    c->ebp = saved_ebp;
    c->esp = frame;
}
#endif

int isaac_vita_kage_quad_try(CPU *__restrict c)
{
    uint32_t esp, capacity, offset, frame;
    uint32_t self, uv, quad, colour[4];
    uint32_t flags, count, formats, stride, sum, i;
    uint32_t batch = 0, target = 0, vtable = 0;
    uint32_t observe_fresh = 0, observe_grows = 0;
    uint8_t format[ISAAC_VITA_KAGE_QUAD_MAX_COUNT];
    int cull, use_getters, unbatched, need_uv = 0, need_colour = 0, reason;

    ++s_counters.calls;

    /* ---- decline checks: reads only ------------------------------------ */
    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME;
    if (c == NULL || !guest_stack_fast_bound(c))
        goto decline;
    esp = c->esp;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = esp - c->stack_floor;
    if (capacity < ISAAC_VITA_KAGE_QUAD_FRAME_BYTES ||
            offset > capacity - ISAAC_VITA_KAGE_QUAD_FRAME_BYTES ||
            offset < ISAAC_VITA_KAGE_QUAD_DEPTH_BYTES)
        goto decline;
    frame = ((esp - 4U) & ~15U) - 0x110U;

    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT;
    self = c->ecx;
    quad = ld32(esp + 8U);
    if (self == 0U || quad == 0U)
        goto decline;
    flags = ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS);
    unbatched = (flags & ISAAC_VITA_KAGE_QUAD_FLAG_BATCHED) == 0U;
    count = ld8(self + ISAAC_VITA_KAGE_QUAD_OFF_FORMAT_COUNT);
    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_COUNT_BOUND;
    if (count > ISAAC_VITA_KAGE_QUAD_MAX_COUNT)
        goto decline;
    formats = ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_FORMATS);
    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT;
    if (count != 0U && formats == 0U)
        goto decline;
    sum = 0U;
    for (i = 0U; i < count; ++i) {
        uint32_t f = ld8(formats + i);
        reason = ISAAC_VITA_KAGE_QUAD_DECLINE_FORMAT;
        if (f - 1U >= ISAAC_VITA_KAGE_QUAD_FORMAT_COUNT)
            goto decline;
        format[i] = (uint8_t)f;
        sum += k_format_bytes[f - 1U];
        need_uv |= f == ISAAC_VITA_KAGE_QUAD_FORMAT_UV;
        need_colour |= f == ISAAC_VITA_KAGE_QUAD_FORMAT_COLOR;
    }
    uv = ld32(esp + 4U);
    colour[0] = ld32(esp + 12U);
    colour[1] = ld32(esp + 16U);
    colour[2] = ld32(esp + 20U);
    colour[3] = ld32(esp + 24U);
    /* The alpha sum of the batch select reads all four colours (0055fb3f). */
    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT;
    if ((need_uv && uv == 0U) ||
            ((need_colour || unbatched) &&
             (colour[0] == 0U || colour[1] == 0U ||
              colour[2] == 0U || colour[3] == 0U)))
        goto decline;
    stride = ld16(self + ISAAC_VITA_KAGE_QUAD_OFF_STRIDE);
    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_STRIDE;
    if (count != 0U && sum != stride)
        goto decline;

    if (!unbatched) {
        /* A batched image reserves in the batch it already holds: the
         * states the x86 faults on (0055fd30/0055fd49 and Ring::Alloc) are
         * visible now.  An unbatched image gets its batch from the select
         * and is checked after it (faults, not declines). */
        batch = ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_BATCH);
        reason = ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BATCH;
        if (batch == 0U ||
                ld32(batch + ISAAC_VITA_KAGE_QUAD_BATCH_VERTEX_RING +
                     ISAAC_VITA_KAGE_QUAD_RING_RECORDS) == 0U)
            goto decline;
        reason = kq_ring_probe(batch + ISAAC_VITA_KAGE_QUAD_BATCH_INDEX_RING,
                               6U, 1, &observe_fresh, &observe_grows);
        if (reason >= 0)
            goto decline;
        reason = kq_ring_probe(batch + ISAAC_VITA_KAGE_QUAD_BATCH_VERTEX_RING,
                               4U, count != 0U, &observe_fresh,
                               &observe_grows);
        if (reason >= 0)
            goto decline;
    }

    cull = ld8(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_CULL_ENABLE)) != 0U;
    use_getters = 0;
    if (cull) {
        target = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_CULL_TARGET));
        if (target != 0U &&
                ld8(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_SIZE_OVERRIDE)) == 0U) {
            use_getters = 1;
            vtable = ld32(target);
            reason = ISAAC_VITA_KAGE_QUAD_DECLINE_GETTER;
            if (vtable == 0U ||
                    ld32(vtable + ISAAC_VITA_KAGE_QUAD_VT_GET_WIDTH) == 0U ||
                    ld32(vtable + ISAAC_VITA_KAGE_QUAD_VT_GET_HEIGHT) == 0U)
                goto decline;
        }
    }

    /* The snap predicate is evaluated where the x86 evaluates it (after the
     * getters and the batch select, on the frame copy), so the floor route
     * is authenticated for every handled quad: nothing may decline once
     * the getters have run. */
    reason = ISAAC_VITA_KAGE_QUAD_DECLINE_FLOOR;
    if (!g_isaac_vita_import_ids_ready ||
            ld32(KQ_FLOOR_SLOT_VA) != KQ_FLOOR_SLOT_VA)
        goto decline;

    ++s_counters.handled;
    if (cull) {
        if (use_getters)
            ++s_counters.getters;
        else
            ++s_counters.constant_cull;
    }
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
    /* OBSERVE: the candidates before the cull, from the entry state.  The
     * batch select's memory effects are not visible before the call: f/r
     * are the batched images' snapshot; d predicts the select's
     * [this+0x71] write - 1 for a translucent quad and for an opaque quad
     * while a non-default blend descriptor is current (0056091a/00560957),
     * 0 otherwise. */
    s_counters.snapped += (uint32_t)kq_snap_predicate(
        self, ldf(quad), ldf(quad + 0x10U));
    s_counters.fresh_records += observe_fresh;
    s_counters.ring_growths += observe_grows;
    {
        uint32_t dirty_byte = ld8(self + ISAAC_VITA_KAGE_QUAD_OFF_DIRTY);
        if (unbatched) {
            uint32_t translucent = kq_translucent(colour);
            uint32_t current = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 4U;
            uint32_t standard = KQ_VA(ISAAC_VITA_KAGE_QUAD_C_BLEND_DEFAULT);
            int blend_default =
                ld32(current) == ld32(standard) &&
                ld32(current + 4U) == ld32(standard + 4U) &&
                ld32(current + 8U) == ld32(standard + 8U) &&
                ld32(current + 12U) == ld32(standard + 12U);
            ++s_counters.unbatched;
            s_counters.translucent += translucent;
            dirty_byte = translucent | (uint32_t)!blend_default;
        }
        if (dirty_byte == 0U &&
                (flags & ISAAC_VITA_KAGE_QUAD_FLAG_DIRTY) == 0U &&
                ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_END)) ==
                ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_CAP)))
            ++s_counters.dirty_growths;
    }
    (void)frame;
    return 0;
#else
    /* ---- replay ---------------------------------------------------------- */
    {
    float w, h, depth, offx, offy, q[8];
    uint32_t iptr, vptr, cursor, base, retire_ecx, vrecords, vrec;
    uint16_t base16;
    kq_site site;
    int delegated = 0, tail_registers = 0;
    /* EBX is never written by the body (or by this replay); ESI/EDI/EBP are
     * pushed at entry and popped at the RET. */
    uint32_t esi0 = c->esi, edi0 = c->edi, ebp0 = c->ebp;

    (void)observe_fresh;
    (void)observe_grows;
    if (cull) {
        float x0, y0, x1, y1, x2, y2, x3, y3, minx, maxx, miny, maxy;
        if (use_getters) {
            /* Registers at 0055f9f3: ecx=target, edx=c1, esi=this,
             * edi=quad, ebp=entry ESP-4, ebx untouched. */
            c->edx = colour[1];
            c->esi = self;
            c->edi = quad;
            c->ebp = esp - 4U;
            kq_getter_call(c, frame, ISAAC_VITA_KAGE_QUAD_RET_GET_WIDTH,
                           target,
                           ld32(vtable + ISAAC_VITA_KAGE_QUAD_VT_GET_WIDTH));
            w = kq_u32_to_float(c->eax);
            /* 0055fa0d: the vtable and slot are re-read after the call. */
            kq_getter_call(c, frame, ISAAC_VITA_KAGE_QUAD_RET_GET_HEIGHT,
                           target,
                           ld32(ld32(target) +
                                ISAAC_VITA_KAGE_QUAD_VT_GET_HEIGHT));
            h = kq_u32_to_float(c->eax);
        } else {
            w = ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_FALLBACK_WIDTH));
            h = ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_FALLBACK_HEIGHT));
        }
        x0 = ldf(quad);
        y0 = ldf(quad + 0x4U);
        x1 = ldf(quad + 0x8U);
        y1 = ldf(quad + 0xcU);
        x2 = ldf(quad + 0x10U);
        y2 = ldf(quad + 0x14U);
        x3 = ldf(quad + 0x18U);
        y3 = ldf(quad + 0x1cU);
        minx = kq_minss(x3, kq_minss(x2, kq_minss(x1, kq_minss(x0, w))));
        maxx = kq_maxss(x3, kq_maxss(x2, kq_maxss(x1, kq_maxss(
            x0, ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_ZERO))))));
        miny = kq_minss(y3, kq_minss(y2, kq_minss(y1, kq_minss(y0, h))));
        maxy = kq_maxss(y3, kq_maxss(y2, kq_maxss(y1, kq_maxss(y0, 0.0f))));
        if (!kq_below(minx, w) || !kq_below(0.0f, maxx) ||
                !kq_below(miny, h) || !kq_below(0.0f, maxy)) {
            /* 0055fb26: xor eax,eax; pop edi; pop esi; mov esp,ebp;
             * pop ebp; ret 0x18.  ecx = y0 bits (0055fa61), edx = c1. */
            ++s_counters.culled;
            c->eax = 0U;
            c->ecx = ld32(quad + 0x4U);
            c->edx = colour[1];
            c->esi = esi0;
            c->edi = edi0;
            c->ebp = ebp0;
            c->esp = esp + ISAAC_VITA_KAGE_QUAD_FRAME_BYTES;
            return 1;
        }
    }

    if (unbatched) {
        /* 0055fb3f..0055fb6f: the blend/batch select, entered exactly like
         * the body's `call 0x5607a0` (eax = bool, ecx = this, edx = c1,
         * esi = this, edi = quad, ebp = entry ESP-4, [F-4] = bool).  Its
         * return values are dead; its memory effects ([this+0x60],
         * [this+0x6c], [this+0x70], [this+0x71], the Renderer state, fresh
         * batches) are read below where the x86 reads them.  The xmm/x87
         * scratch is volatile at the call and read by nothing. */
        uint32_t translucent = kq_translucent(colour);
        ++s_counters.unbatched;
        s_counters.translucent += translucent;
        c->eax = translucent;
        c->ecx = self;
        c->edx = colour[1];
        c->esi = self;
        c->edi = quad;
        c->ebp = esp - 4U;
#if defined(ISAAC_VITA_KAGE_QUAD_SELECT_NATIVE)
        kq_select(c, frame, translucent);
#else
        st32(frame - 4U, translucent);
        kq_direct_call(c, frame, ISAAC_VITA_KAGE_QUAD_RET_BATCH_SELECT, 4U,
                       sub_005607a0);
#endif
    }

    /* 0055fb78: the quad is copied into the frame; the snap rebuilds it. */
    for (i = 0U; i < 8U; ++i)
        q[i] = ldf(quad + i * 4U);
    if (kq_snap_predicate(self, q[0], q[4])) {
        /* Order of the four floor calls: y3, x3, y0, x0; each pushes the
         * double at F-8 and its return word at F-12. */
        float ny3, nx3, ny0, nx0;
        ++s_counters.snapped;
        ny3 = kq_snap(c, self, q[7], frame - 8U);
        nx3 = kq_snap(c, self, q[6], frame - 8U);
        ny0 = kq_snap(c, self, q[1], frame - 8U);
        nx0 = kq_snap(c, self, q[0], frame - 8U);
        q[0] = nx0; q[1] = ny0;
        q[2] = nx3; q[3] = ny0;
        q[4] = nx0; q[5] = ny3;
        q[6] = nx3; q[7] = ny3;
    }

    /* 0055fd14: the batch and its vertex records as the x86 reads them
     * (a null here is the access violation of 0055fd30 / 0055fd49). */
    batch = ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_BATCH);
    if (batch == 0U)
        guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                    "Image::PushQuad replay: null batch (the x86 faults at "
                    "0055fd30)");
    offx = ldf(self + ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_X);
    offy = ldf(self + ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_Y);
    vrecords = ld32(batch + ISAAC_VITA_KAGE_QUAD_BATCH_VERTEX_RING +
                    ISAAC_VITA_KAGE_QUAD_RING_RECORDS);
    if (vrecords == 0U)
        guest_fault(c, KQ_VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA),
                    "Image::PushQuad replay: null vertex ring records (the "
                    "x86 faults at 0055fd49)");
    vrec = vrecords + ld32(batch + ISAAC_VITA_KAGE_QUAD_BATCH_VERTEX_RING +
                           ISAAC_VITA_KAGE_QUAD_RING_INDEX) *
                      ISAAC_VITA_KAGE_QUAD_REC_BYTES;
    base = ld32(vrec + ISAAC_VITA_KAGE_QUAD_REC_COUNT) -
           ld32(vrec + ISAAC_VITA_KAGE_QUAD_REC_BASE);
    /* 0055fd54: eax = vrecords, ecx = batch+0x2c, edx = batch, esi = base,
     * edi = quad. */
    site.eax = vrecords;
    site.edx = batch;
    site.esi = base;
    site.edi = quad;
    site.ebp = esp - 4U;
    site.return_word = ISAAC_VITA_KAGE_QUAD_RET_RING_INDEX;
    iptr = kq_ring_reserve(c, frame,
                           batch + ISAAC_VITA_KAGE_QUAD_BATCH_INDEX_RING, 6U,
                           1, &site, &delegated);
    /* 0055fd67: eax = iptr, ecx = [[F+0x38]+0x60]+0x20, edx = batch unless
     * the first reserve grew (then as Ring::Alloc left it), esi = base,
     * edi = iptr. */
    site.eax = iptr;
    site.edx = delegated ? c->edx : batch;
    site.edi = iptr;
    site.return_word = ISAAC_VITA_KAGE_QUAD_RET_RING_VERTEX;
    vptr = kq_ring_reserve(c, frame,
                           ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_BATCH) +
                           ISAAC_VITA_KAGE_QUAD_BATCH_VERTEX_RING, 4U,
                           count != 0U, &site, &delegated);
    /* 0055fd6c: the depth cursor is read after both reserves. */
    depth = ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DEPTH));
    base16 = (uint16_t)base;
    st16(iptr, base16);
    st16(iptr + 2U, (uint16_t)(base16 + 2U));
    st16(iptr + 8U, (uint16_t)(base16 + 2U));
    st16(iptr + 4U, (uint16_t)(base16 + 1U));
    st16(iptr + 6U, (uint16_t)(base16 + 1U));
    st16(iptr + 10U, (uint16_t)(base16 + 3U));
    retire_ecx = (uint32_t)base16 + 1U;

    cursor = vptr;
    for (i = 0U; i < count; ++i) {
        uint32_t f = format[i];
        if (f == ISAAC_VITA_KAGE_QUAD_FORMAT_POSITION) {
            uint32_t k;
            float y = 0.0f;
            for (k = 0U; k < 4U; ++k) {
                uint32_t at = cursor + k * stride;
                float x = (float)(offx + q[2U * k]);
                y = (float)(offy + q[2U * k + 1U]);
                stf(at, x);
                stf(at + 4U, y);
                stf(at + 8U, depth);
            }
            memcpy(&retire_ecx, &y, 4U);
            cursor += 12U;
        } else if (f == ISAAC_VITA_KAGE_QUAD_FORMAT_UV) {
            float su = ldf(self + ISAAC_VITA_KAGE_QUAD_OFF_UV_SCALE_U);
            float sv = ldf(self + ISAAC_VITA_KAGE_QUAD_OFF_UV_SCALE_V);
            uint32_t k;
            float v = 0.0f;
            for (k = 0U; k < 4U; ++k) {
                uint32_t at = cursor + k * stride;
                float u = (float)(ldf(uv + 8U * k) * su);
                v = (float)(ldf(uv + 8U * k + 4U) * sv);
                stf(at, u);
                stf(at + 4U, v);
            }
            memcpy(&retire_ecx, &v, 4U);
            cursor += 8U;
        } else if (f == ISAAC_VITA_KAGE_QUAD_FORMAT_COLOR) {
            uint32_t k;
            if (ld8(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_PREMULTIPLY)) != 0U) {
                xmm_t out[4];
                for (k = 0U; k < 4U; ++k) {
                    float a = ldf(colour[k] + 0xcU);
                    out[k].f[0] = (float)(a * ldf(colour[k]));
                    out[k].f[1] = (float)(a * ldf(colour[k] + 4U));
                    out[k].f[2] = (float)(a * ldf(colour[k] + 8U));
                    out[k].f[3] = a;
                }
                for (k = 0U; k < 4U; ++k)
                    stx(cursor + k * stride, out[k]);
                retire_ecx = ISAAC_VITA_KAGE_QUAD_FORMAT_COLOR;
            } else {
                for (k = 0U; k < 4U; ++k)
                    stx(cursor + k * stride, ldx(colour[k]));
                retire_ecx = colour[2];
            }
            cursor += 16U;
        } else {
            cursor += k_format_bytes[f - 1U];
            retire_ecx = f - 1U;
        }
    }

    /* 005601a6 */
    depth = (float)(depth - ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_C_DEPTH_STEP)));
    stf(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DEPTH), depth);
    if (ld8(self + ISAAC_VITA_KAGE_QUAD_OFF_DIRTY) == 0U) {
        /* 005601c2: [F+0x34] = this before the flag test; the growth reads
         * the value through its second argument. */
        st32(frame + ISAAC_VITA_KAGE_QUAD_FRAME_THIS_SLOT, self);
        if ((ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS) &
             ISAAC_VITA_KAGE_QUAD_FLAG_DIRTY) == 0U) {
            uint32_t end = ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_END));
            uint32_t shadow;
            if (end != ld32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_CAP))) {
                st32(end, self);
                st32(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_END), end + 4U);
            } else {
                /* 005601e3: lea ecx,[esp+0x34]; push ecx; push eax(end);
                 * mov ecx,0x7e7d10; call 0x26fb0 with edx = count,
                 * esi = this, edi = count ? 3*stride : iptr. */
                ++s_counters.dirty_growths;
                c->eax = end;
                c->ecx = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_VECTOR);
                c->edx = count;
                c->esi = self;
                c->edi = count != 0U ? stride * 3U : iptr;
                c->ebp = esp - 4U;
                st32(frame - 4U,
                     frame + ISAAC_VITA_KAGE_QUAD_FRAME_THIS_SLOT);
                st32(frame - 8U, end);
                kq_direct_call(c, frame, ISAAC_VITA_KAGE_QUAD_RET_DIRTY_GROW,
                               8U, sub_00026fb0);
                tail_registers = 1;
            }
            /* 005601f3 */
            shadow = ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS_SHADOW);
            st32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS,
                 ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS) |
                 ISAAC_VITA_KAGE_QUAD_FLAG_DIRTY);
            st32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS_SHADOW, shadow);
        }
    }
    /* 005601fd: the batched flag is re-read; clear -> the unbatched tail. */
    if ((ld32(self + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS) &
         ISAAC_VITA_KAGE_QUAD_FLAG_BATCHED) == 0U) {
        if (ld8(self + ISAAC_VITA_KAGE_QUAD_OFF_BLEND_RESET) != 0U) {
            /* 0056020d: push eax(0); mov ecx,0x7c7a30; call 0x561ff0 with
             * edx = count (or as the dirty growth left it), esi = this,
             * edi = count ? 3*stride : iptr. */
            c->eax = 0U;
            c->ecx = KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER);
            if (!tail_registers)
                c->edx = count;
            c->esi = self;
            c->edi = count != 0U ? stride * 3U : iptr;
            c->ebp = esp - 4U;
            st32(frame - 4U, 0U);
            kq_direct_call(c, frame, ISAAC_VITA_KAGE_QUAD_RET_BLEND_RESET, 4U,
                           sub_00561ff0);
            tail_registers = 1;
        }
        /* 00560218..00560240 */
        st32(self + ISAAC_VITA_KAGE_QUAD_OFF_BATCH, 0U);
        stf(self + ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_X,
            ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RESET_OFFSET_X)));
        stf(self + ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_Y,
            ldf(KQ_VA(ISAAC_VITA_KAGE_QUAD_G_RESET_OFFSET_Y)));
        st32(self + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_SCALE, 0U);
        st8(self + ISAAC_VITA_KAGE_QUAD_OFF_BLEND_RESET, 0U);
    }

    /* 00560244: eax = vptr; pop edi; pop esi; mov esp,ebp; pop ebp;
     * ret 0x18.  edx = the loop counter (count), ecx per the last
     * attribute (count 0: (u16)base + 1) - unless a tail callee ran, after
     * which nothing writes ecx/edx before the RET. */
    c->eax = vptr;
    if (!tail_registers) {
        c->ecx = retire_ecx;
        c->edx = count;
    }
    c->esi = esi0;
    c->edi = edi0;
    c->ebp = ebp0;
    c->esp = esp + ISAAC_VITA_KAGE_QUAD_FRAME_BYTES;
    return 1;
    }
#endif

decline:
    ++s_counters.declined[reason];
    return 0;
}
