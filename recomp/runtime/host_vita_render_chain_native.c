/* ISAAC_VITA_RENDER_CHAIN_NATIVE (default OFF): two links of the sprite
 * render chain replayed on the host (GNU ld --wrap=sub_0025af60
 * --wrap=sub_0055f610).
 *
 * Why.  Per sprite, AnimationLayer::RenderFrame (sub_00003e50) calls
 * Image::Render through the vtable (+0xc = sub_0055f610), which re-dispatches
 * through the vtable again (+0x8 = sub_0055f630, the UV copy) into
 * Image::PushQuad (sub_0055f990, already native behind the KAGE quad fast
 * path); for colour-offset sprites it then calls the vertex attribute filler
 * sub_0025af60 (RenderFrame's "final consumer", ~140 call sites), which makes
 * eight virtual size-getter calls per quad.  Every translated push/pop is an
 * out-of-line stack validator call, every virtual call a guest_call.  In the
 * sampler (decomp-profile-install-20261016, fight) 55f630 + 25af60 are 2.6 %
 * of render self time, 4.8 % in the water room (hotprof-20261012 w5-8).
 *
 * What is replayed, instruction by instruction from the frozen listing (the
 * translated bodies of guest_0061.c / guest_0166.c, of a corpus
 * generated 2026-10-12, are the reference the oracle
 * runs against):
 *
 * sub_0025af60(this=ecx; dest, image, counter) ret 0xc, E = ESP at entry:
 *   push ebp; ebp=E-4; SEH frame [E-8]=-1, [E-0xc]=0x985f229d (handler),
 *   [E-0x10]=fs:[0]; sub esp,0x10; push ebx,esi,edi; push cookie
 *   ([0x7aa3b4]^ebp); fs:[0]=E-0x10; edi=this; esi=dest+0x28; ebx=4;
 *   [ebp-4]=0; xmm0/xmm1 f0 loads of 0x7fedf0 / 0x76a614; ecx=image;
 *   [ebp-0x10..-0x1c] = f32 [0x7fedf0], [0x7fedf4], [0x7fedf8], [0x7fd648].
 *   4 x: [esi-4..esi+0x14] = this+0x10..0x28 (7 words, in order);
 *        size(+0x30) -> [esi+0x18], size(+0x34) -> [esi+0x1c] where
 *        size = image ? (float)((double)(int32)eax + f64[0x76c940 +
 *        (eax>>31)*8]) via `call [vtbl+0x30/0x34]`, ecx re-read from
 *        [ebp+0xc] and xmm1 f0 re-loaded after each call : xmm1 (movaps);
 *        [esi+0x20..0x2c] = the four frame floats (re-read; the last load
 *        zeroes xmm0 first); esi += 0x58.
 *   [ebp-4]=1; counter=[ebp+0x10]; if counter: call [[counter]+0xc]
 *   (DecrementReference), if al: eax=[0x7fd62c] (release observer), if eax:
 *   push &image ([ebp+0xc]); call eax; add esp,4.  Epilogue: fs:[0]=[ebp-0xc];
 *   pop ecx (cookie), edi, esi, ebx; esp=ebp; pop ebp; ret 0xc.
 *   The size getters: when [vtbl+0x30/0x34] is one of the nine pinned
 *   `mov eax,[ecx+d32]; ret` / `movzx eax,word [ecx+d32]; ret` leaves that
 *   the ImageBase vtables hold (recomp/test_vita_render_chain_native.py pins
 *   their bytes and that no --wrap or seam replaces them) the leaf is
 *   answered here: return word stored at [ESP-4] as the call would, EAX =
 *   the field, nothing else changes (the translated leaf touches nothing
 *   else).  Any other target goes through guest_call exactly as the
 *   translated `call [eax+0x30]`.
 *
 * sub_0055f610(this=ecx; a, b, c) ret 0xc (Image::Render):
 *   push ebp; ebp=E-4; eax=c; edx=[this]; push c x4, b, a; call [edx+8].
 *   When [edx+8] is sub_0055f630 its 31 instructions run here instead of
 *   through guest_call (E' = E-0x20, [E']=0x55f625): push ebp; ebp=E'-4;
 *   esp=E'-0x28; push esi; esi=ecx; 36 bytes of *a copied to [E'-0x28]
 *   (16 + word + 16, the listing's load/store order); if word != 1: push
 *   esi, ecx; ecx=E'-0x28; call sub_0055ebd0 (direct, translated); push
 *   [ebp+0x1c..+0xc] (5 words), lea eax,[ebp-0x24]; push eax; ecx=esi; call
 *   sub_0055f990 (direct: the translated body with its KAGE seam); pop esi;
 *   esp=ebp; pop ebp; ret 0x18.  Any other [edx+8] goes through guest_call.
 *   Then pop ebp; ret 0xc.
 *
 * Every register the translated body leaves (EAX..EDI, EBP, ESP, xmm0,
 * xmm1) and every stack word it writes, live or dead, is reproduced.
 * Translated callees are entered with the registers published (`c` is
 * authoritative across the call, as after GUEST_GPR_FLUSH), and everything
 * read after a call is re-read from `c` and guest memory, so a callee that
 * moves ESP or clobbers a callee-saved register sees the same continuation.
 * The one up-front decline: the guest stack cannot hold the frames this
 * replay writes (the translated body then raises the same stack fault). */
#include "host_vita_render_chain_native.h"

#include <stdint.h>
#include <string.h>

#include "guest.h"

#if !defined(ISAAC_VITA_RENDER_CHAIN_NATIVE) || !ISAAC_VITA_RENDER_CHAIN_NATIVE
#error host_vita_render_chain_native.c must only be compiled when enabled
#endif
#if !defined(GUEST_FLAGS_LOCAL) || !GUEST_FLAGS_LOCAL
#error the replay leaves c->fl untouched: GUEST_FLAGS_LOCAL=1 corpus only
#endif

#define RC_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))

/* Frozen PE (sha256 31846486...), copied from the listing above. */
#define RC_FILL_SEH_HANDLER   0x005F229DU  /* push 0x9f229d @25af65 */
#define RC_COOKIE             0x007AA3B4U  /* mov eax,[0xbaa3b4] @25af77 */
#define RC_FILL_C0            0x007FEDF0U  /* @25af9c */
#define RC_FILL_DEFAULT_SIZE  0x0076A614U  /* @25afa4 */
#define RC_FILL_C1            0x007FEDF4U  /* @25afb4 */
#define RC_FILL_C2            0x007FEDF8U  /* @25afc1 */
#define RC_FILL_C3            0x007FD648U  /* @25afce */
#define RC_U32_BIAS_TABLE     0x0076C940U  /* addsd [eax*8+0xb6c940] @25b028 */
#define RC_RELEASE_OBSERVER   0x007FD62CU  /* mov eax,[0xbfd62c] @25b0c2 */
#define RC_RET_FILL_W         0x0025B012U
#define RC_RET_FILL_H         0x0025B048U
#define RC_RET_FILL_RELEASE   0x0025B0BEU
#define RC_RET_FILL_OBSERVER  0x0025B0D1U
#define RC_RENDER_UV_RVA      0x0055F630U  /* [vtable+8] replayed here */
#define RC_RET_RENDER         0x0055F625U
#define RC_RET_UV_TRANSFORM   0x0055F660U
#define RC_RET_PUSHQUAD       0x0055F67AU

/* The +0x30/+0x34 slots of the nine ImageBase vtables that hold
 * Image::Render at +0xc (frozen .rdata; bytes pinned by the test). */
typedef struct rc_leaf { uint32_t rva; uint32_t disp; uint32_t word; } rc_leaf;
static const rc_leaf s_leaves[] = {
    { 0x005A0BB0U, 0x108U, 0U }, { 0x005A0BC0U, 0x10CU, 0U },
    { 0x005A0DF0U, 0x084U, 1U }, { 0x005A0E00U, 0x086U, 1U },
    { 0x005A1A70U, 0x084U, 0U }, { 0x005A1A80U, 0x088U, 0U },
    { 0x005A1F10U, 0x0C8U, 0U }, { 0x005A1F20U, 0x0CCU, 0U },
    { 0x005B6270U, 0x08CU, 0U },
};

void __real_sub_0025af60(CPU *__restrict c);
void __real_sub_0055f610(CPU *__restrict c);
void sub_0055ebd0(CPU *__restrict c);
void sub_0055f990(CPU *__restrict c);

static IsaacVitaRenderChainCounters s_counters;

void isaac_vita_render_chain_take(IsaacVitaRenderChainCounters *out)
{
    if (out)
        *out = s_counters;
    memset(&s_counters, 0, sizeof s_counters);
}

static inline uint32_t rc_fs_base(CPU *__restrict c)
{
    /* = guest_fs_base_generated (GUEST_FS_BASE_INLINE): same TIB words */
    uint32_t base = g_guest_fs_base_cached;
    return base ? base : guest_fs_base(c);
}

/* [low, high) of the guest stack this replay touches lies inside the bound
 * stack (the translated body checks each push/pop; one range check here). */
static inline int rc_stack_ok(const CPU *c, uint32_t low, uint32_t high)
{
    return low <= high && low >= c->stack_floor && high <= c->stack_ceiling;
}

static inline void rc_push(CPU *__restrict c, uint32_t value)
{
    c->esp -= 4U;
    st32(c->esp, value);
}

static inline uint32_t rc_pop(CPU *__restrict c)
{
    uint32_t value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

/* `call [vtbl+slot]` with ecx = image at the fill's call site. */
static void rc_size_call(CPU *__restrict c, uint32_t target, uint32_t ret)
{
    uint32_t i;

    for (i = 0U; i < sizeof s_leaves / sizeof s_leaves[0]; ++i) {
        if (target != RC_VA(s_leaves[i].rva))
            continue;
        st32(c->esp - 4U, ret);   /* the call's return word, popped by ret */
        c->eax = s_leaves[i].word ? (uint32_t)ld16(c->ecx + s_leaves[i].disp)
                                  : ld32(c->ecx + s_leaves[i].disp);
        ++s_counters.getters_native;
        return;
    }
    ++s_counters.getters_called;
    rc_push(c, ret);
    guest_call(c, target);
}

/* movd/cvtdq2pd/shr/addsd/cvtpd2ps on eax, as translated (0025b01d). */
static inline void rc_size_to_float(CPU *__restrict c)
{
    int32_t lo, hi;

    c->x[0] = (xmm_t){0};
    c->x[0].u32[0] = c->eax;
    lo = c->x[0].i32[0];
    hi = c->x[0].i32[1];
    c->x[0].d[0] = (double)lo;
    c->x[0].d[1] = (double)hi;
    c->eax >>= 31;
    c->x[0].d[0] = (double)(c->x[0].d[0] +
                            ldd(c->eax * 8U + RC_VA(RC_U32_BIAS_TABLE)));
    {
        double a = c->x[0].d[0], b = c->x[0].d[1];
        c->x[0] = (xmm_t){0};
        c->x[0].f[0] = (float)a;
        c->x[0].f[1] = (float)b;
    }
}

void __wrap_sub_0025af60(CPU *__restrict c)
{
    uint32_t entry = c->esp;
    uint32_t k;

    ++s_counters.fill_calls;
    if (!rc_stack_ok(c, entry - 0x40U, entry + 0x10U)) {
        ++s_counters.fill_declined;
        __real_sub_0025af60(c);
        return;
    }
    rc_push(c, c->ebp);
    c->ebp = c->esp;
    rc_push(c, 0xFFFFFFFFU);
    rc_push(c, RC_VA(RC_FILL_SEH_HANDLER));
    c->eax = ld32(rc_fs_base(c));
    rc_push(c, c->eax);
    c->esp -= 0x10U;
    rc_push(c, c->ebx);
    rc_push(c, c->esi);
    rc_push(c, c->edi);
    c->eax = ld32(RC_VA(RC_COOKIE)) ^ c->ebp;
    rc_push(c, c->eax);
    c->eax = c->ebp - 0xCU;
    st32(rc_fs_base(c), c->eax);
    c->edi = c->ecx;
    c->esi = ld32(c->ebp + 8U);
    c->ebx = 4U;
    st32(c->ebp - 4U, 0U);
    c->esi += 0x28U;
    c->x[0].f[0] = ldf(RC_VA(RC_FILL_C0));
    c->x[1].f[0] = ldf(RC_VA(RC_FILL_DEFAULT_SIZE));
    c->ecx = ld32(c->ebp + 0xCU);
    stf(c->ebp - 0x10U, c->x[0].f[0]);
    c->x[0].f[0] = ldf(RC_VA(RC_FILL_C1));
    stf(c->ebp - 0x14U, c->x[0].f[0]);
    c->x[0].f[0] = ldf(RC_VA(RC_FILL_C2));
    stf(c->ebp - 0x18U, c->x[0].f[0]);
    c->x[0].f[0] = ldf(RC_VA(RC_FILL_C3));
    stf(c->ebp - 0x1CU, c->x[0].f[0]);
    do {
        for (k = 0U; k < 7U; ++k) {
            c->eax = ld32(c->edi + 0x10U + 4U * k);
            st32(c->esi - 4U + 4U * k, c->eax);
        }
        for (k = 0U; k < 2U; ++k) {
            if (c->ecx != 0U) {
                c->eax = ld32(c->ecx);
                rc_size_call(c, ld32(c->eax + (k ? 0x34U : 0x30U)),
                             k ? RC_RET_FILL_H : RC_RET_FILL_W);
                c->ecx = ld32(c->ebp + 0xCU);
                c->x[1].f[0] = ldf(RC_VA(RC_FILL_DEFAULT_SIZE));
                rc_size_to_float(c);
            } else {
                c->x[0] = c->x[1];
            }
            stf(c->esi + (k ? 0x1CU : 0x18U), c->x[0].f[0]);
        }
        c->x[0].f[0] = ldf(c->ebp - 0x10U);
        stf(c->esi + 0x20U, c->x[0].f[0]);
        c->x[0].f[0] = ldf(c->ebp - 0x14U);
        stf(c->esi + 0x24U, c->x[0].f[0]);
        c->x[0].f[0] = ldf(c->ebp - 0x18U);
        stf(c->esi + 0x28U, c->x[0].f[0]);
        c->x[0] = (xmm_t){0};
        c->x[0].f[0] = ldf(c->ebp - 0x1CU);
        stf(c->esi + 0x2CU, c->x[0].f[0]);
        c->esi += 0x58U;
        c->ebx -= 1U;
    } while (c->ebx != 0U);
    st32(c->ebp - 4U, 1U);
    c->ecx = ld32(c->ebp + 0x10U);
    if (c->ecx != 0U) {
        ++s_counters.releases;
        c->eax = ld32(ld32(c->ecx) + 0xCU);
        rc_push(c, RC_RET_FILL_RELEASE);
        guest_call(c, c->eax);
        if ((uint8_t)c->eax != 0U) {
            c->eax = ld32(RC_VA(RC_RELEASE_OBSERVER));
            if (c->eax != 0U) {
                c->ecx = c->ebp + 0xCU;
                rc_push(c, c->ecx);
                rc_push(c, RC_RET_FILL_OBSERVER);
                guest_call(c, c->eax);
                c->esp += 4U;
            }
        }
    }
    c->ecx = ld32(c->ebp - 0xCU);
    st32(rc_fs_base(c), c->ecx);
    c->ecx = rc_pop(c);
    c->edi = rc_pop(c);
    c->esi = rc_pop(c);
    c->ebx = rc_pop(c);
    c->esp = c->ebp;
    c->ebp = rc_pop(c);
    (void)rc_pop(c);
    c->esp += 0xCU;
}

/* sub_0055f630 entered at `call [edx+8]` (ESP at the pushed 0x55f625). */
static void rc_render_uv(CPU *__restrict c)
{
    uint32_t a;
    uint32_t word;
    xmm_t first;

    rc_push(c, c->ebp);
    c->ebp = c->esp;
    c->esp -= 0x24U;
    c->eax = ld32(c->ebp + 8U);
    rc_push(c, c->esi);
    c->esi = c->ecx;
    a = c->eax;
    memcpy(&first, (const void *)(uintptr_t)a, 16U);
    memcpy((void *)(uintptr_t)(c->ebp - 0x24U), &first, 16U);
    memcpy(&c->x[0], (const void *)(uintptr_t)(a + 0x10U), 16U);
    word = ld32(a + 0x20U);
    c->eax = word;
    st32(c->ebp - 4U, word);
    memcpy((void *)(uintptr_t)(c->ebp - 0x14U), &c->x[0], 16U);
    if (word != 1U) {
        rc_push(c, c->esi);
        rc_push(c, c->ecx);
        c->ecx = c->ebp - 0x24U;
        rc_push(c, RC_RET_UV_TRANSFORM);
        sub_0055ebd0(c);
    }
    rc_push(c, ld32(c->ebp + 0x1CU));
    c->eax = c->ebp - 0x24U;
    c->ecx = c->esi;
    rc_push(c, ld32(c->ebp + 0x18U));
    rc_push(c, ld32(c->ebp + 0x14U));
    rc_push(c, ld32(c->ebp + 0x10U));
    rc_push(c, ld32(c->ebp + 0xCU));
    rc_push(c, c->eax);
    rc_push(c, RC_RET_PUSHQUAD);
    sub_0055f990(c);
    c->esi = rc_pop(c);
    c->esp = c->ebp;
    c->ebp = rc_pop(c);
    (void)rc_pop(c);
    c->esp += 0x18U;
}

void __wrap_sub_0055f610(CPU *__restrict c)
{
    uint32_t entry = c->esp;
    uint32_t target;

    ++s_counters.render_calls;
    /* deepest write: [E-0x20] return word, then 0x55f630's frame down to
     * [E-0x20-0x38] (its call into sub_0055ebd0) */
    if (!rc_stack_ok(c, entry - 0x60U, entry + 0x10U)) {
        ++s_counters.render_declined;
        __real_sub_0055f610(c);
        return;
    }
    rc_push(c, c->ebp);
    c->ebp = c->esp;
    c->eax = ld32(c->ebp + 0x10U);
    c->edx = ld32(c->ecx);
    rc_push(c, c->eax);
    rc_push(c, c->eax);
    rc_push(c, c->eax);
    rc_push(c, c->eax);
    rc_push(c, ld32(c->ebp + 0xCU));
    rc_push(c, ld32(c->ebp + 8U));
    target = ld32(c->edx + 8U);
    rc_push(c, RC_RET_RENDER);
    if (target == RC_VA(RC_RENDER_UV_RVA)) {
        ++s_counters.render_inlined;
        rc_render_uv(c);
    } else {
        guest_call(c, target);
    }
    c->ebp = rc_pop(c);
    (void)rc_pop(c);
    c->esp += 0xCU;
}
