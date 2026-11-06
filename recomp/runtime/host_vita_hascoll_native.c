/* Native Entity_Player::HasCollectible (ISAAC_VITA_HASCOLLECTIBLE_NATIVE,
 * default OFF).
 *
 * sub_0033af30 (thiscall Entity_Player *, ret 8; args id, ignoreModifiers;
 * name from REPENTOGON's libzhl signature) is the
 * hottest single update function measured on the console: 413 calls per game
 * tick from other units at 3.126 us each inclusive (profile build
 * int4-hotprof-decomp-profile-20261015), and almost every call runs a second
 * body through its own HasCollectible(0xbf) call at 0x33b356.
 *
 * Every instruction below is copied from the listing of the camera-regen
 * corpus (recipe 8ba2f580, output set 740dddbb, the production corpus):
 * guest_0092.c (sub_0033af30), guest_0080.c (sub_002fe5e0, sub_002ff070),
 * guest_0140.c (sub_0048b570), guest_0161.c (sub_00535fa0), guest_0044.c
 * (sub_0019fce0, sub_0019ff20), guest_0034.c (sub_00144990), guest_0004.c
 * (sub_000206b0), guest_0106.c (sub_0039cb10, sub_0039cb80); the G FUEL part
 * is the replay of host_vita_gfuel_seed_native.c (oracle-proven against the
 * same bodies), copied.  Each constant cites the instruction it comes from;
 * test_vita_hascoll_native.py checks every cited instruction against the
 * corpus listing and the bodies against the frozen PE.
 *
 * What runs natively: the whole body and every callee of these paths
 *   - ignoreModifiers set (straight to the [player+0x15f0] count);
 *   - [player+0x1f88] set (the item's 0x8000 tag);
 *   - ids 0x131, 0x101, 0xe7, 0xb6 ([player+0x1f18] bits), 0x14a and 0x99
 *     (the G FUEL mode), 0x76 (temporary effects), 0x216;
 *   - [player+0x168] and [player+0x1f1c] tag tests (sub_0019fce0);
 *   - the [player+0x1e7c] map lookup (sub_00144990 -> sub_000206b0);
 *   - the vector, item predicate, effect list and slot queries;
 *   - the HasCollectible(0xbf) call (the same native body, recursively);
 *   - the final [player+0x15f0][id] > 0.
 * Declines (into the translated body, see host_vita_hascoll_native.h): a
 * short guest stack, a co-op baby, a negative id or 0x147/0x148/0x139/9/0x3b
 * (GetTrinketMultiplier, RNG and Judas paths), a G FUEL byte above 7 (the
 * RNG paths of sub_0039cb80 and 0x33b1e2), the HasCollectible call on the
 * [player+0x1d84] player, a null pointer the body would dereference, a walk
 * or list over the bound, an item-predicate jump target that is not one of
 * the two the frozen table holds.
 *
 * Exactness.  The replay leaves exactly what the translated bodies leave:
 * EAX/ECX/EDX, ESP = entry + 12, EBX/ESI/EDI/EBP (popped, i.e. unchanged),
 * and every guest stack word the bodies write below the entry ESP, in
 * program order (pushes, return words, locals, the two lookup result
 * structs).  No body on these paths writes anything else.  The lazy flags
 * are local to each generated body (GUEST_FLAGS_LOCAL, flags are dead across
 * CALL/RET) and are not touched.
 *
 * A decline may come after some stack words were written (a null link deep
 * in a callee, the twin call).  The words written up to that point are the
 * ones the translated body writes on the same path before it gets there,
 * with the same values (the replay follows the same path), and the
 * translated body re-runs from the untouched CPU and rewrites all of them,
 * so the final state is the translated body's.  No guest-visible state other
 * than those dead words is written before the decline point.
 *
 * Seams.  GNU ld --wrap=sub_0033af30 sends every edge from another unit and
 * the guest_table.c dispatch slot here (2678 of the 2798 direct call sites;
 * the 120 inside guest_0092.c, of which two are the body's own calls, keep
 * the translated body: measured as ~11 bodies per tick on the console,
 * 837 G FUEL queries = 2 x 413 + 11).  No guest function tail-jumps into
 * 0x33af30, so no root holds an inlined copy (checked on the corpus by the
 * test).  In profiling builds with ISAAC_VITA_GFUEL_SEED_NATIVE the G FUEL
 * TU owns __wrap_sub_0033af30 (its counters and 1-in-64 timer) and calls
 * isaac_vita_hascoll_native_call instead of __real_ (CMake defines
 * ISAAC_VITA_HASCOLL_NATIVE_CHAINED here). */
#include "guest.h"
#include "host_vita_hascoll_native.h"

#if !defined(ISAAC_VITA_HASCOLLECTIBLE_NATIVE) || !ISAAC_VITA_HASCOLLECTIBLE_NATIVE
#error host_vita_hascoll_native.c must only be compiled when enabled
#endif
#if !defined(GUEST_FLAGS_LOCAL) || !GUEST_FLAGS_LOCAL
#error the replay leaves c->fl untouched: GUEST_FLAGS_LOCAL=1 corpus only
#endif

void __real_sub_0033af30(CPU *__restrict c);
#if !defined(ISAAC_VITA_HASCOLL_NATIVE_CHAINED)
void __wrap_sub_0033af30(CPU *__restrict c);
#endif

#if defined(HC_ORACLE_COVERAGE)
/* host oracle only: one counter per branch below */
extern unsigned g_hc_cov[];
# define HC_COV(n) (++g_hc_cov[(n)])
#else
# define HC_COV(n) ((void)0)
#endif

#define HC_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (uint32_t)(rva)))

/* The callees below have one to three call sites each.  Forced inline, with
 * the hc_regs/hc_call/hc_gfuel pointers __restrict (they point at this
 * call's C frame, never at guest memory), the register replay stays in ARM
 * registers instead of round-tripping through memory around every guest
 * store: 803 -> 704 executed instructions and 518 -> 392 data accesses per
 * call on the lean state of hascoll_arm_cost.py (clang Thumb-2). */
#if defined(__GNUC__)
# define HC_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
# define HC_INLINE static __forceinline
#else
# define HC_INLINE static inline
#endif

/* ---- globals ------------------------------------------------------------ */
#define HC_G_GAME 0x007FD65CU       /* 0033b047  mov esi, dword ptr [0x987fd65c] */
#define HC_G_CONFIG 0x007FD680U     /* 0033af41  mov ecx, dword ptr [0x987fd680] */
#define HC_CONFIG_ITEMS 0x00029E84U /* 0033af48  lea ecx, [ecx + 0x29e84] */

/* ---- Entity_Player fields ------------------------------------------------ */
#define HC_P_VARIANT 0x2CU          /* 0033af53  cmp dword ptr [edi + 0x2c], 1 */
#define HC_P_TAGGATE 0x1F88U        /* 0033afad  cmp byte ptr [edi + 0x1f88], 0 */
#define HC_P_FLAGS 0x1F18U          /* 0033b145  test byte ptr [edi + 0x1f18], 0x10 */
#define HC_P_TWINGATE 0x168U        /* 0033b254  cmp byte ptr [edi + 0x168], 0 */
#define HC_P_TWIN 0x1D84U           /* 0033b277  mov ecx, dword ptr [edi + 0x1d84] */
#define HC_P_TAGGATE2 0x1F1CU       /* 0033b295  cmp byte ptr [edi + 0x1f1c], 0 */
#define HC_P_MAP 0x1E7CU            /* 0033b2d9  lea ecx, [edi + 0x1e7c] */
#define HC_P_MAP_SIZE 0x1E80U       /* 0033b2c7  cmp dword ptr [edi + 0x1e80], 0 */
#define HC_P_LAST 0x1EB0U           /* 0033b2fa  mov eax, dword ptr [edi + 0x1eb0] */
#define HC_P_BYTES 0x1EC0U          /* 0033b309  lea ecx, [edi + 0x1ec0] */
#define HC_P_EFFECTS 0x1450U        /* 0033b32a  lea ecx, [edi + 0x1450] */
#define HC_P_BF_ID 0x12E0U          /* 0033b35f  cmp ebx, dword ptr [edi + 0x12e0] */
#define HC_P_1E8_ID 0x12D8U         /* 0033b37b  cmp ebx, dword ptr [edi + 0x12d8] */
#define HC_P_COUNTS 0x15F0U         /* 0033b38e  mov eax, dword ptr [edi + 0x15f0] */
#define HC_P_SLOTS 0x14C4U          /* 0019ff28  add ecx, 0x14c4 */

/* ---- ids and masks ------------------------------------------------------- */
#define HC_TAG_8000 0x8000U         /* 0033afbc  and eax, 0x8000 */
#define HC_TAG_TWIN 0x40000000U     /* 0033b267  push 0x40000000 */
#define HC_TAG_GATE2 0x8000U        /* 0033b2a4  push 0x8000 */
#define HC_ID_147 0x147U            /* 0033afed  cmp ebx, 0x147 */
#define HC_ID_148 0x148U            /* 0033aff5  cmp ebx, 0x148 */
#define HC_ID_139 0x139U            /* 0033b04d  cmp ebx, 0x139 */
#define HC_ID_9 0x9U                /* 0033b0bd  cmp ebx, 9 */
#define HC_ID_3B 0x3BU              /* 0033b122  cmp ebx, 0x3b */
#define HC_ID_131 0x131U            /* 0033b13d  cmp ebx, 0x131 */
#define HC_BIT_131 0x10U            /* 0033b145  test byte ptr [edi + 0x1f18], 0x10 */
#define HC_ID_101 0x101U            /* 0033b159  cmp ebx, 0x101 */
#define HC_BIT_101 0x8U             /* 0033b161  test byte ptr [edi + 0x1f18], 8 */
#define HC_ID_E7 0xE7U              /* 0033b175  cmp ebx, 0xe7 */
#define HC_BIT_E7 0x20U             /* 0033b17d  test byte ptr [edi + 0x1f18], 0x20 */
#define HC_ID_B6 0xB6U              /* 0033b191  cmp ebx, 0xb6 */
#define HC_BIT_B6 0x40U             /* 0033b199  test byte ptr [edi + 0x1f18], 0x40 */
#define HC_ID_14A 0x14AU            /* 0033b1ad  cmp ebx, 0x14a */
#define HC_ID_99 0x99U              /* 0033b1c5  cmp ebx, 0x99 */
#define HC_GFUEL_MAX 0x8U           /* 0033b1dd  cmp eax, 8 */
#define HC_ID_76 0x76U              /* 0033b23b  cmp ebx, 0x76 */
#define HC_ID_216 0x216U            /* 0033b2b4  cmp ebx, 0x216 */
#define HC_ID_BF 0xBFU              /* 0033b345  cmp ebx, 0xbf */
#define HC_ID_1E8 0x1E8U            /* 0033b367  push 0x1e8 */
#define HC_MAP_VALUE 0x14U          /* 0033b2f0  cmp dword ptr [eax + 0x14], 0 */

/* ---- return words (call site + 5) ---------------------------------------- */
#define HC_RET_ITEM 0x0033AF53U     /* 0033af4e  call 0x2fe5e0 */
#define HC_RET_MODE 0x0033B1ADU     /* 0033b1a8  call 0x39cb80 */
#define HC_RET_QUERY 0x0033B1DDU    /* 0033b1d8  call 0x39cb10 */
#define HC_RET_EFF76 0x0033B24CU    /* 0033b247  call 0x48b570 */
#define HC_RET_TAGTWIN 0x0033B273U  /* 0033b26e  call 0x19fce0 */
#define HC_RET_TAGGATE2 0x0033B2B0U /* 0033b2ab  call 0x19fce0 */
#define HC_RET_MAP 0x0033B2E4U      /* 0033b2df  call 0x144990 */
#define HC_RET_BYTES 0x0033B314U    /* 0033b30f  call 0x535fa0 */
#define HC_RET_PRED 0x0033B325U     /* 0033b320  call 0x2ff070 */
#define HC_RET_EFFPRED 0x0033B335U  /* 0033b330  call 0x48b570 */
#define HC_RET_SLOTS 0x0033B341U    /* 0033b33c  call 0x19ff20 */
#define HC_RET_SELF 0x0033B35BU     /* 0033b356  call 0x33af30 */
#define HC_RET_EFF1E8 0x0033B377U   /* 0033b372  call 0x48b570 */
#define HC_RET_LOOKUP 0x001449A7U   /* 001449a2  call 0x206b0 */

/* ---- callee constants ---------------------------------------------------- */
#define HC_EFF_OFF 0x11U            /* 0048b573  cmp byte ptr [ecx + 0x11], 0 */
#define HC_EFF_BEGIN 0x4U           /* 0048b57b  mov eax, dword ptr [ecx + 4] */
#define HC_EFF_END 0x8U             /* 0048b57e  mov ecx, dword ptr [ecx + 8] */
#define HC_EFF_STRIDE 0xCU          /* 0048b5a0  add eax, 0xc */
#define HC_ITEM_ID 0x4U             /* 0048b59b  cmp dword ptr [esi + 4], edi */
#define HC_TYPE_1 0x1U              /* 0048b58c  cmp edx, 1 */
#define HC_TYPE_3 0x3U              /* 0048b591  cmp edx, 3 */
#define HC_TYPE_4 0x4U              /* 0048b596  cmp edx, 4 */
#define HC_SLOT_STRIDE 0x1CU        /* 0019ff35  add ecx, 0x1c */
#define HC_SLOT_COUNT 0x4U          /* 0019ff38  cmp eax, 4 */
#define HC_TAGS_LO 0xB8U            /* 0019fcec  and edx, dword ptr [ecx + 0xb8] */
#define HC_TAGS_HI 0xBCU            /* 0019fce6  mov eax, dword ptr [ecx + 0xbc] */
#define HC_BYTES_BEGIN 0xCU         /* 00535fae  mov esi, dword ptr [ecx + 0xc] */
#define HC_BYTES_END 0x10U          /* 00535fab  mov eax, dword ptr [ecx + 0x10] */
#define HC_PRED_HIGH 0x163U         /* 002ff084  cmp eax, 0x163 */
#define HC_PRED_1FC 0x1FCU          /* 002ff0a5  cmp eax, 0x1fc */
#define HC_PRED_2DC 0x2DCU          /* 002ff0ac  cmp eax, 0x2dc */
#define HC_PRED_BASE 0x1DU          /* 002ff08d  sub eax, 0x1d */
#define HC_PRED_SPAN 0xC7U          /* 002ff090  cmp eax, 0xc7 */
#define HC_PRED_BYTES 0x002FF0C4U   /* 002ff097  movzx eax, byte ptr [eax - 0x67d00f3c] */
#define HC_PRED_TARGETS 0x002FF0BCU /* 002ff09e  jmp dword ptr [eax*4 - 0x67d00f44] */
#define HC_PRED_YES 0x002FF0B6U     /* 002ff0b6  mov al, 1 */
#define HC_PRED_NO 0x002FF0B3U      /* 002ff0b3  xor al, al */
/* MSVC _Tree_node as sub_000206b0 reads it: _Left +0, root/_Parent +4,
 * _Right +8, _Isnil +0xd, key +0x10. */
#define HC_NODE_ROOT 0x4U           /* 000206b8  mov eax, dword ptr [ecx + 4] */
#define HC_NODE_RIGHT 0x8U          /* 000206da  mov eax, dword ptr [eax + 8] */
#define HC_NODE_ISNIL 0xDU          /* 000206c7  cmp byte ptr [eax + 0xd], 0 */
#define HC_NODE_KEY 0x10U           /* 000206d5  cmp dword ptr [eax + 0x10], esi */

/* ---- G FUEL (host_vita_gfuel_seed_native.c, copied) ----------------------- */
#define HC_GF_SEEDS_SET 0x0001BB58U /* 0039cb2a  add ecx, 0x1bb58 */
#define HC_GF_KEY 0x4FU             /* 0039cb19  mov dword ptr [ebp - 4], 0x4f */
#define HC_GF_VEC_BEGIN 0x1ECCU     /* 0039cb55  mov ecx, dword ptr [esi + 0x1ecc] */
#define HC_GF_VEC_END 0x1ED0U       /* 0039cb4f  mov eax, dword ptr [esi + 0x1ed0] */
#define HC_GF_INDEX 0x8FU           /* 0039cb64  movzx eax, byte ptr [ecx + 0x8f] */
#define HC_GF_RET_LOOKUP 0x0039CB3CU /* 0039cb37  call 0x206b0 */
#define HC_GF_RET_QUERY 0x0039CB90U  /* 0039cb8b  call 0x39cb10 */
#define HC_GF_MODE_MAX 0x7U         /* 0039cb92  cmp esi, 7 */

/* shortcuts (bounds, not constants of the game): a walk or a list longer
 * than these declines to the translated body. */
#define HC_MAX_WALK 64U
#define HC_MAX_EFFECTS 4096U
/* Deepest word written below the entry ESP: outer frame <= 0x30, the
 * HasCollectible(0xbf) call 0xc and its frame <= 0x30, then sub_0039cb80
 * under it (return word + frame <= 0x1b + sub_0039cb10 + sub_000206b0)
 * <= 0x5c: 0xc8.  The oracle measures the deepest write. */
#define HC_STACK_BYTES 0xD0U

enum { HC_OK = ISAAC_VITA_HASCOLL_DECLINE_REASONS };

typedef struct {
    uint32_t eax, ecx, edx;
} hc_regs;

typedef struct {
    uint32_t result;              /* EAX of sub_0039cb10 */
    uint32_t ecx;                 /* ECX it returns with */
    uint32_t node, dir, bound;    /* sub_000206b0's 12-byte result */
    int walked;                   /* 000206d0 push esi executed */
} hc_gfuel;

/* What every body of one call shares (no body writes any of it). */
typedef struct {
    uint32_t player;              /* ECX at entry, EDI in the bodies */
    uint32_t game;                /* [0x7fd65c] */
    uint32_t items;               /* [0x7fd680] + 0x29e84, ECX of GetCollectible */
    int gfuel_read;               /* gfuel holds sub_0039cb10's reads */
    hc_gfuel gfuel;
    uint32_t bodies;
} hc_call;

static IsaacVitaHasCollCounters s_counters;

void isaac_vita_hascoll_native_take(IsaacVitaHasCollCounters *out)
{
    if (out)
        *out = s_counters;
    memset(&s_counters, 0, sizeof s_counters);
}

/* The 0x39cc68 table: targets 39cb9e, 39cb9e, 39cba7, 39cbaf, 39cbba,
 * 39cbc5, 39cbd0, 39cbdb load -1, -1, 0, 1, 2, 3, 4, 5
 * (host_vita_gfuel_seed_native.c k_gfuel_mode; the test pins the table). */
static const uint32_t k_hc_gfuel_mode[8] = {
    0xFFFFFFFFU, 0xFFFFFFFFU, 0U, 1U, 2U, 3U, 4U, 5U
};

static int hc_stack_ok(const CPU *c, uint32_t esp)
{
    uint32_t capacity, offset;
    if (!guest_stack_fast_bound(c))
        return 0;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = esp - c->stack_floor;
    return capacity >= HC_STACK_BYTES + 4U && offset >= HC_STACK_BYTES &&
           offset <= capacity - 4U;
}

/* sub_0039cb10's reads (gfuel_read of host_vita_gfuel_seed_native.c with the
 * player known non-null): HC_OK or a decline reason. */
HC_INLINE int hc_gfuel_read(const hc_call *k, hc_gfuel *q)
{
    uint32_t head, node, steps = 0U;

    if (k->game == 0U)
        return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
    head = ld32(k->game + HC_GF_SEEDS_SET);            /* 0039cb35 */
    if (head == 0U)
        return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
    node = ld32(head + HC_NODE_ROOT);                  /* 000206b8 */
    if (node == 0U)
        return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
    q->node = node;                                    /* 000206bb..c4 */
    q->dir = 0U;
    q->bound = head;
    q->ecx = head;                                     /* 000206b3 */
    q->walked = 0;
    if (!ld8(node + HC_NODE_ISNIL)) {                  /* 000206c7 */
        q->walked = 1;
        do {                                           /* 000206d3..f2 */
            if (++steps > HC_MAX_WALK)
                return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
            q->node = node;
            if ((int32_t)ld32(node + HC_NODE_KEY) >= (int32_t)HC_GF_KEY) {
                q->bound = node;
                q->dir = 1U;
                node = ld32(node);
            } else {
                q->dir = 0U;
                node = ld32(node + HC_NODE_RIGHT);
            }
            if (node == 0U)
                return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
        } while (!ld8(node + HC_NODE_ISNIL));
        q->ecx = q->dir;
    }
    q->result = 0U;
    if (!ld8(q->bound + HC_NODE_ISNIL) &&                          /* 0039cb3f */
            (int32_t)ld32(q->bound + HC_NODE_KEY) <= (int32_t)HC_GF_KEY &&
            q->bound != head) {                                    /* 0039cb4b */
        const uint32_t end = ld32(k->player + HC_GF_VEC_END);
        const uint32_t begin = ld32(k->player + HC_GF_VEC_BEGIN);
        q->ecx = begin;
        if ((int32_t)(end - begin) > (int32_t)HC_GF_INDEX)        /* 0039cb5d */
            q->result = ld8(begin + HC_GF_INDEX);
    }
    if (q->result > HC_GF_MODE_MAX)                    /* 0039cb95 ja: RNG path */
        return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
    return HC_OK;
}

/* The words sub_0039cb10 and sub_000206b0 leave below their entry ESP e
 * (gfuel_write of host_vita_gfuel_seed_native.c). */
HC_INLINE void hc_gfuel_write(uint32_t e, uint32_t ebp, uint32_t esi, uint32_t edi,
                           uint32_t self, const hc_gfuel *__restrict q)
{
    st32(e - 0x04U, ebp);                 /* 0039cb10 push ebp */
    st32(e - 0x08U, HC_GF_KEY);           /* 0039cb19 [ebp-4] */
    st32(e - 0x0CU, q->bound);            /* [ebp-8]  result +8 */
    st32(e - 0x10U, q->dir);              /* [ebp-0xc] result +4 */
    st32(e - 0x14U, q->node);             /* [ebp-0x10] result +0 */
    st32(e - 0x18U, esi);                 /* 0039cb16 push esi */
    st32(e - 0x1CU, edi);                 /* 0039cb29 push edi */
    st32(e - 0x20U, e - 0x08U);           /* 0039cb30 push eax (&key) */
    st32(e - 0x24U, e - 0x14U);           /* 0039cb34 push eax (&result) */
    st32(e - 0x28U, HC_GF_RET_LOOKUP);    /* 0039cb37 call 0x206b0 */
    st32(e - 0x2CU, e - 0x04U);           /* 000206b0 push ebp */
    if (q->walked)
        st32(e - 0x30U, self);            /* 000206d0 push esi (= this) */
}

/* sub_0039cb80 entered at e (its return word already written): the mode. */
HC_INLINE void hc_gfuel_mode(uint32_t e, uint32_t ebp, uint32_t esi, uint32_t edi,
                          uint32_t self, const hc_gfuel *__restrict q, hc_regs *__restrict r)
{
    const uint32_t frame = ((e - 4U) & ~7U) - 0x10U;  /* 0039cb83 and esp,-8; sub 0x10 */
    const uint32_t inner = frame - 12U;               /* esi, edi, return word */
    st32(e - 4U, ebp);                    /* 0039cb80 push ebp */
    st32(frame - 4U, esi);                /* 0039cb89 push esi */
    st32(frame - 8U, edi);                /* 0039cb8a push edi */
    st32(inner, HC_GF_RET_QUERY);         /* 0039cb8b call 0x39cb10 */
    hc_gfuel_write(inner, e - 4U, esi, edi, self, q);
    r->eax = k_hc_gfuel_mode[q->result];
    r->ecx = q->ecx;
    r->edx = inner - 0x14U;               /* 000206b5 mov edx, [ebp+8] */
}

/* sub_0039cb10 entered at e: the byte. */
HC_INLINE void hc_gfuel_query(uint32_t e, uint32_t ebp, uint32_t esi, uint32_t edi,
                           uint32_t self, const hc_gfuel *__restrict q, hc_regs *__restrict r)
{
    hc_gfuel_write(e, ebp, esi, edi, self, q);
    r->eax = q->result;
    r->ecx = q->ecx;
    r->edx = e - 0x14U;                   /* 000206b5 mov edx, [ebp+8] */
}

/* ItemConfig::GetCollectible sub_002fe5e0 (thiscall, ret 4) entered at e with
 * ECX = items and a non-negative id: both arms of 002fe5ed reach 002fe617. */
HC_INLINE uint32_t hc_get_collectible(uint32_t e, uint32_t ebp, uint32_t items,
                                   uint32_t id, hc_regs *__restrict r)
{
    uint32_t begin, count;
    st32(e - 4U, ebp);                                      /* 002fe5e0 push ebp */
    r->edx = id;                                            /* 002fe5e8 mov edx, [ebp + 8] */
    begin = ld32(items);                                    /* 002fe617 mov eax, [ecx] */
    count = (uint32_t)((int32_t)(ld32(items + 4U) - begin) >> 2); /* 002fe619..1e */
    r->ecx = count;
    if ((int32_t)id >= (int32_t)count) {                    /* 002fe621 cmp; jge */
        HC_COV(0);
        r->eax = 0U;                                        /* 002fe62c xor eax, eax */
    } else {
        HC_COV(1);
        r->eax = ld32(begin + id * 4U);                     /* 002fe625 */
    }
    return r->eax;
}

/* sub_0048b570 (thiscall, ret 4) entered at e with ECX = self: a temporary
 * effect of type 1/3/4 whose item id is `key`. */
HC_INLINE int hc_effects(uint32_t e, uint32_t ebp, uint32_t esi, uint32_t edi,
                      uint32_t self, uint32_t key, hc_regs *__restrict r)
{
    uint32_t a, end, steps = 0U;
    st32(e - 4U, ebp);                    /* 0048b570 push ebp */
    st32(e - 8U, esi);                    /* 0048b577 push esi */
    st32(e - 0xCU, edi);                  /* 0048b578 push edi */
    if (ld8(self + HC_EFF_OFF)) {         /* 0048b573, 0048b579 jne 0x48b5a7 */
        HC_COV(2);
        r->ecx = self;
        r->eax &= ~0xFFU;                 /* 0048b5a8 xor al, al */
        return HC_OK;
    }
    a = ld32(self + HC_EFF_BEGIN);        /* 0048b57b */
    end = ld32(self + HC_EFF_END);        /* 0048b57e */
    r->ecx = end;
    if (a != end) {                       /* 0048b583 je 0x48b5a7 */
        do {
            uint32_t item, type;
            if (++steps > HC_MAX_EFFECTS)
                return ISAAC_VITA_HASCOLL_DECLINE_WALK;
            item = ld32(a);               /* 0048b588 mov esi, [eax] */
            if (item == 0U)
                return ISAAC_VITA_HASCOLL_DECLINE_NULL;
            type = ld32(item);            /* 0048b58a mov edx, [esi] */
            r->edx = type;
            if ((type == HC_TYPE_1 || type == HC_TYPE_3 || type == HC_TYPE_4) &&
                    ld32(item + HC_ITEM_ID) == key) {   /* 0048b59b je 0x48b5af */
                HC_COV(3);
                r->eax = (a & ~0xFFU) | 1U;             /* 0048b5b0 mov al, 1 */
                return HC_OK;
            }
            a += HC_EFF_STRIDE;           /* 0048b5a0 */
        } while (a != end);
        HC_COV(4);
    } else {
        HC_COV(5);
    }
    r->eax = a & ~0xFFU;                  /* 0048b5a8 xor al, al */
    return HC_OK;
}

/* sub_0019fce0 (thiscall, ret 8) entered at e with ECX = item:
 * (lo & [item+0xb8]) | (hi & [item+0xbc]) != 0. */
HC_INLINE void hc_tags(uint32_t e, uint32_t ebp, uint32_t item, uint32_t lo,
                    uint32_t hi, hc_regs *__restrict r)
{
    uint32_t a;
    st32(e - 4U, ebp);                                  /* 0019fce0 push ebp */
    a = ld32(item + HC_TAGS_HI) & hi;                   /* 0019fce6, 0019fcf2 */
    r->edx = (lo & ld32(item + HC_TAGS_LO)) | a;        /* 0019fce3, 0019fcec, 0019fcf5 */
    r->ecx = item;
    if (r->edx != 0U) {
        HC_COV(6);
        r->eax = (a & ~0xFFU) | 1U;                     /* 0019fcf9 mov al, 1 */
    } else {
        HC_COV(7);
        r->eax = a & ~0xFFU;                            /* 0019fcff xor al, al */
    }
}

/* sub_00144990 (thiscall, ret 8; ECX = map, args result*, key*) with its
 * sub_000206b0 lower bound, entered at e: *result = the node, or the head. */
HC_INLINE int hc_map_find(uint32_t e, uint32_t ebp, uint32_t esi, uint32_t edi,
                       uint32_t map, uint32_t result, uint32_t keyp, hc_regs *__restrict r)
{
    const uint32_t f = e - 4U;            /* sub_00144990's EBP */
    const uint32_t res = f - 0xCU;        /* 0014499a lea eax, [ebp - 0xc] */
    const uint32_t e2 = e - 0x24U;        /* sub_000206b0's entry ESP */
    uint32_t head, node, bound, dir, ecx, key, steps = 0U;
    int walked = 0;

    st32(e - 0x04U, ebp);                 /* 00144990 push ebp */
    st32(e - 0x14U, esi);                 /* 00144996 push esi (after sub esp, 0xc) */
    st32(e - 0x18U, edi);                 /* 0014499d push edi */
    st32(e - 0x1CU, keyp);                /* 0014499e push esi (= [ebp + 0xc]) */
    st32(e - 0x20U, res);                 /* 0014499f push eax */
    st32(e2, HC_RET_LOOKUP);              /* 001449a2 call 0x206b0 */
    st32(e2 - 4U, f);                     /* 000206b0 push ebp */
    head = ld32(map);                     /* 000206b3 mov ecx, [ecx] */
    if (head == 0U)
        return ISAAC_VITA_HASCOLL_DECLINE_NULL;
    node = ld32(head + HC_NODE_ROOT);     /* 000206b8 */
    if (node == 0U)
        return ISAAC_VITA_HASCOLL_DECLINE_NULL;
    bound = head;
    dir = 0U;
    ecx = head;
    if (!ld8(node + HC_NODE_ISNIL)) {     /* 000206c7 */
        uint32_t at = node;
        walked = 1;
        st32(e2 - 8U, keyp);              /* 000206d0 push esi (= key*) */
        key = ld32(keyp);                 /* 000206d1 mov esi, [ecx] */
        do {                              /* 000206d3..f2 */
            if (++steps > HC_MAX_WALK)
                return ISAAC_VITA_HASCOLL_DECLINE_WALK;
            node = at;
            if ((int32_t)ld32(at + HC_NODE_KEY) >= (int32_t)key) {
                bound = at;
                dir = 1U;
                at = ld32(at);
            } else {
                dir = 0U;
                at = ld32(at + HC_NODE_RIGHT);
            }
            if (at == 0U)
                return ISAAC_VITA_HASCOLL_DECLINE_NULL;
        } while (!ld8(at + HC_NODE_ISNIL));
        ecx = dir;
    }
    st32(res, node);                      /* 000206bb / 000206d3 */
    st32(res + 4U, dir);                  /* 000206bd / 000206eb */
    st32(res + 8U, bound);                /* 000206c4 / 000206e1 */
    (void)walked;
    /* back in sub_00144990: 001449a7 mov edx, [ebp - 4] */
    r->edx = bound;
    if (!ld8(bound + HC_NODE_ISNIL) &&                       /* 001449aa */
            (int32_t)ld32(keyp) >= (int32_t)ld32(bound + HC_NODE_KEY)) {
        HC_COV(8);
        st32(result, bound);              /* 001449bc mov [eax], edx */
        r->ecx = ecx;
    } else {
        HC_COV(9);
        r->ecx = ld32(map);               /* 001449c7 mov ecx, [edi] */
        st32(result, r->ecx);             /* 001449cb mov [eax], ecx */
    }
    r->eax = result;                      /* 001449b7 / 001449c4 mov eax, [ebp + 8] */
    return HC_OK;
}

/* sub_002ff070 (thiscall, no args): the item predicate; HC_OK or TABLE. */
HC_INLINE int hc_item_predicate(uint32_t item, hc_regs *__restrict r)
{
    uint32_t v = ld32(item);              /* 002ff070 mov eax, [ecx] */
    r->eax = v;
    if (v != HC_TYPE_1 && v != HC_TYPE_3 && v != HC_TYPE_4) {
        HC_COV(10);
        goto no;
    }
    v = ld32(item + HC_ITEM_ID);          /* 002ff081 mov eax, [ecx + 4] */
    r->eax = v;
    if ((int32_t)v > (int32_t)HC_PRED_HIGH) {          /* 002ff089 jg */
        if (v == HC_PRED_1FC || v == HC_PRED_2DC) {
            HC_COV(11);
            goto yes;
        }
        HC_COV(12);
        goto no;
    }
    if (v == HC_PRED_HIGH) {                           /* 002ff08b je */
        HC_COV(13);
        goto yes;
    }
    v -= HC_PRED_BASE;                                 /* 002ff08d */
    r->eax = v;
    if (v > HC_PRED_SPAN) {                            /* 002ff095 ja */
        HC_COV(14);
        goto no;
    }
    {
        const uint32_t b = ld8(v + HC_VA(HC_PRED_BYTES));          /* 002ff097 */
        const uint32_t target = ld32(b * 4U + HC_VA(HC_PRED_TARGETS)); /* 002ff09e */
        r->eax = b;
        if (target == HC_VA(HC_PRED_YES)) {
            HC_COV(15);
            goto yes;
        }
        if (target != HC_VA(HC_PRED_NO))
            return ISAAC_VITA_HASCOLL_DECLINE_TABLE;     /* guest_call */
        HC_COV(16);
    }
no:
    r->eax &= ~0xFFU;                     /* 002ff0b3 xor al, al */
    return HC_OK;
yes:
    r->eax = (r->eax & ~0xFFU) | 1U;      /* 002ff0b6 mov al, 1 */
    return HC_OK;
}

/* One body of sub_0033af30 entered at e ([e] = return word, [e+4] = id,
 * [e+8] = ignoreModifiers) with ECX = k->player and the callee-saved
 * registers ebp_in/ebx_in/esi_in/edi_in; r carries EAX/ECX/EDX in and out.
 * HC_OK or a decline reason. */
static int hc_body(hc_call *__restrict k, uint32_t e, uint32_t id, uint32_t ignore,
                   uint32_t ebp_in, uint32_t ebx_in, uint32_t esi_in,
                   uint32_t edi_in, hc_regs *__restrict r)
{
    const uint32_t player = k->player;
    const uint32_t ebp = e - 4U;          /* 0033af31 mov ebp, esp */
    const uint32_t a = (e - 4U) & ~7U;    /* 0033af33 and esp, 0xfffffff8 */
    const uint32_t s = a - 0x28U;         /* sub esp, 0x1c; push ebx, esi, edi */
    uint32_t esi = esi_in, item;
    int v;

    ++k->bodies;
    st32(e - 4U, ebp_in);                 /* 0033af30 push ebp */
    st32(a - 0x20U, ebx_in);              /* 0033af39 push ebx */
    st32(a - 0x24U, esi_in);              /* 0033af3d push esi */
    st32(a - 0x28U, edi_in);              /* 0033af3e push edi */
    if ((int32_t)id < 0)                  /* sub_002fe5e0's 002fe5f3 path, 0033afca */
        return ISAAC_VITA_HASCOLL_DECLINE_ID;
    st32(s - 4U, id);                     /* 0033af47 push ebx */
    st32(s - 8U, HC_RET_ITEM);            /* 0033af4e call 0x2fe5e0 */
    item = hc_get_collectible(s - 8U, ebp, k->items, id, r);
    if (ld32(player + HC_P_VARIANT) == 1U)                 /* 0033af53 */
        return ISAAC_VITA_HASCOLL_DECLINE_COOP;
    st32(s + 0x10U, item);                /* 0033af57 mov [esp + 0x10], eax */
    if (ld8(player + HC_P_TAGGATE)) {     /* 0033afad */
        if (item == 0U)
            return ISAAC_VITA_HASCOLL_DECLINE_NULL;
        r->eax = ld32(item + HC_TAGS_LO) & HC_TAG_8000;    /* 0033afb6..c1 */
        if (r->eax == 0U) {
            HC_COV(17);
            goto ret0;                    /* 0033afc4 je 0x33b2bc */
        }
        HC_COV(18);
    }
    if (ignore) {                         /* 0033afe3 cmp byte ptr [ebp + 0xc], 0 */
        HC_COV(19);
        goto counts;
    }
    if (id == HC_ID_147 || id == HC_ID_148)                /* 0033afed, 0033aff5 */
        return ISAAC_VITA_HASCOLL_DECLINE_ID;
    esi = k->game;                        /* 0033b047 mov esi, [0x987fd65c] */
    if (id == HC_ID_139 || id == HC_ID_9 || id == HC_ID_3B) /* 0033b04d, b0bd, b122 */
        return ISAAC_VITA_HASCOLL_DECLINE_ID;
    if (id == HC_ID_131) {                                 /* 0033b13d */
        if (ld8(player + HC_P_FLAGS) & HC_BIT_131) {
            HC_COV(20);
            goto ret1;
        }
    } else if (id == HC_ID_101) {                          /* 0033b159 */
        if (ld8(player + HC_P_FLAGS) & HC_BIT_101) {
            HC_COV(21);
            goto ret1;
        }
    } else if (id == HC_ID_E7) {                           /* 0033b175 */
        if (ld8(player + HC_P_FLAGS) & HC_BIT_E7) {
            HC_COV(22);
            goto ret1;
        }
    } else if (id == HC_ID_B6) {                           /* 0033b191 */
        if (ld8(player + HC_P_FLAGS) & HC_BIT_B6) {
            HC_COV(23);
            goto ret1;
        }
    }
    /* 0033b1a6 mov ecx, edi; call 0x39cb80 */
    if (!k->gfuel_read) {
        v = hc_gfuel_read(k, &k->gfuel);
        if (v != HC_OK)
            return v;
        k->gfuel_read = 1;
    }
    st32(s - 4U, HC_RET_MODE);
    hc_gfuel_mode(s - 4U, ebp, esi, player, player, &k->gfuel, r);
    if (id == HC_ID_14A) {                                 /* 0033b1ad */
        if (r->eax == 1U) {
            HC_COV(24);
            goto out;                     /* 0033b1ba mov al, al */
        }
        HC_COV(25);
    } else if (id == HC_ID_99 && r->eax == 2U) {           /* 0033b1c5 */
        HC_COV(26);
        goto ret1;
    }
    /* 0033b1d6 mov ecx, edi; call 0x39cb10 */
    st32(s - 4U, HC_RET_QUERY);
    hc_gfuel_query(s - 4U, ebp, esi, player, player, &k->gfuel, r);
    if ((int32_t)r->eax > (int32_t)HC_GFUEL_MAX)           /* 0033b1dd: RNG path */
        return ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
    if (id == HC_ID_76) {                                  /* 0033b23b */
        st32(s - 4U, id);                 /* 0033b240 push ebx */
        st32(s - 8U, HC_RET_EFF76);       /* 0033b247 call 0x48b570 */
        v = hc_effects(s - 8U, ebp, esi, player, player + HC_P_EFFECTS, id, r);
        if (v != HC_OK)
            return v;
        if (r->eax & 0xFFU) {
            HC_COV(27);
            goto ret1;
        }
    }
    r->eax = item;                        /* 0033b25b mov eax, [esp + 0x10] */
    if (ld8(player + HC_P_TWINGATE) && item != 0U) {       /* 0033b254, 0033b261 */
        st32(s - 4U, 0U);                 /* 0033b265 push 0 */
        st32(s - 8U, HC_TAG_TWIN);        /* 0033b267 push 0x40000000 */
        st32(s - 0xCU, HC_RET_TAGTWIN);   /* 0033b26e call 0x19fce0 */
        hc_tags(s - 0xCU, ebp, item, HC_TAG_TWIN, 0U, r);
        if (r->eax & 0xFFU) {
            r->ecx = ld32(player + HC_P_TWIN);             /* 0033b277 */
            if (r->ecx != 0U)
                return ISAAC_VITA_HASCOLL_DECLINE_TWIN;    /* 0033b284 call 0x33af30 */
            HC_COV(28);
        }
        r->eax = item;                    /* 0033b291 mov eax, [esp + 0x10] */
    }
    if (ld8(player + HC_P_TAGGATE2) && item != 0U) {       /* 0033b295, 0033b29e */
        st32(s - 4U, 0U);                 /* 0033b2a2 push 0 */
        st32(s - 8U, HC_TAG_GATE2);       /* 0033b2a4 push 0x8000 */
        st32(s - 0xCU, HC_RET_TAGGATE2);  /* 0033b2ab call 0x19fce0 */
        hc_tags(s - 0xCU, ebp, item, HC_TAG_GATE2, 0U, r);
        if (!(r->eax & 0xFFU) && id != HC_ID_216) {        /* 0033b2b2, 0033b2b4 */
            HC_COV(29);
            goto ret0;
        }
        HC_COV(30);
    }
    if (ld32(player + HC_P_MAP_SIZE) != 0U) {              /* 0033b2c7 */
        uint32_t node;
        st32(s - 4U, e + 4U);             /* 0033b2d0 lea eax, [ebp + 8]; push eax */
        st32(s - 8U, s + 0x14U);          /* 0033b2d4 lea eax, [esp + 0x18]; push eax */
        st32(s - 0xCU, HC_RET_MAP);       /* 0033b2df call 0x144990 */
        v = hc_map_find(s - 0xCU, ebp, esi, player, player + HC_P_MAP,
                        s + 0x14U, e + 4U, r);
        if (v != HC_OK)
            return v;
        node = ld32(s + 0x14U);           /* 0033b2e4 mov eax, [esp + 0x14] */
        r->eax = node;
        if (node != ld32(player + HC_P_MAP)) {             /* 0033b2e8 */
            if ((int32_t)ld32(node + HC_MAP_VALUE) > 0) {  /* 0033b2f0 */
                HC_COV(31);
                goto ret1;
            }
            HC_COV(32);
        } else {
            HC_COV(33);
        }
    }
    r->eax = ld32(player + HC_P_LAST);    /* 0033b2fa */
    if (r->eax != 0U && r->eax == id) {
        HC_COV(34);
        goto ret1;
    }
    {
        /* 0033b308 push ebx; lea ecx, [edi + 0x1ec0]; call 0x535fa0 */
        const uint32_t self = player + HC_P_BYTES;
        const uint32_t eb = s - 8U;
        uint32_t begin, size;
        st32(s - 4U, id);
        st32(eb, HC_RET_BYTES);
        st32(eb - 4U, ebp);               /* 00535fa0 push ebp */
        st32(eb - 8U, esi);               /* 00535fa6 push esi */
        r->edx = id;                      /* 00535fa3 mov edx, [ebp + 8] */
        size = ld32(self + HC_BYTES_END);                  /* 00535fab */
        begin = ld32(self + HC_BYTES_BEGIN);               /* 00535fae */
        size -= begin;
        r->ecx = self;
        r->eax = (int32_t)id >= (int32_t)size ? 0U : ld8(begin + id); /* 00535fb3..b7 */
        if ((int32_t)r->eax > 0) {        /* 0033b314 test eax, eax; jg */
            HC_COV(35);
            goto ret1;
        }
    }
    r->ecx = item;                        /* 0033b318 mov ecx, [esp + 0x10] */
    if (item != 0U) {
        st32(s - 4U, HC_RET_PRED);        /* 0033b320 call 0x2ff070 */
        v = hc_item_predicate(item, r);
        if (v != HC_OK)
            return v;
        if (r->eax & 0xFFU) {
            st32(s - 4U, id);             /* 0033b329 push ebx */
            st32(s - 8U, HC_RET_EFFPRED); /* 0033b330 call 0x48b570 */
            v = hc_effects(s - 8U, ebp, esi, player, player + HC_P_EFFECTS, id, r);
            if (v != HC_OK)
                return v;
            if (r->eax & 0xFFU) {
                HC_COV(36);
                goto ret1;
            }
        }
    }
    {
        /* 0033b339 push ebx; mov ecx, edi; call 0x19ff20 */
        const uint32_t es = s - 8U;
        uint32_t slot = player + HC_P_SLOTS, n = 0U;
        st32(s - 4U, id);
        st32(es, HC_RET_SLOTS);
        st32(es - 4U, ebp);               /* 0019ff20 push ebp */
        r->edx = id;                      /* 0019ff23 mov edx, [ebp + 8] */
        for (;;) {                        /* 0019ff30..3b */
            if (ld32(slot) == id)
                break;
            ++n;
            slot += HC_SLOT_STRIDE;
            if (n >= HC_SLOT_COUNT) {
                n = 0xFFFFFFFFU;          /* 0019ff3d or eax, 0xffffffff */
                break;
            }
        }
        r->eax = n;
        r->ecx = slot;
        if ((int32_t)n >= 0) {            /* 0033b343 jns */
            HC_COV(37);
            goto ret1;
        }
    }
    if (id != HC_ID_BF) {                 /* 0033b345 */
        st32(s - 4U, 0U);                 /* 0033b34d push 0 */
        st32(s - 8U, HC_ID_BF);           /* 0033b34f push 0xbf */
        st32(s - 0xCU, HC_RET_SELF);      /* 0033b356 call 0x33af30 */
        v = hc_body(k, s - 0xCU, HC_ID_BF, 0U, ebp, id, esi, player, r);
        if (v != HC_OK)
            return v;
        if ((r->eax & 0xFFU) && id == ld32(player + HC_P_BF_ID)) { /* 0033b35f */
            HC_COV(38);
            goto ret1;
        }
    }
    st32(s - 4U, HC_ID_1E8);              /* 0033b367 push 0x1e8 */
    st32(s - 8U, HC_RET_EFF1E8);          /* 0033b372 call 0x48b570 */
    v = hc_effects(s - 8U, ebp, esi, player, player + HC_P_EFFECTS, HC_ID_1E8, r);
    if (v != HC_OK)
        return v;
    if ((r->eax & 0xFFU) && id == ld32(player + HC_P_1E8_ID)) {   /* 0033b37b */
        HC_COV(39);
        goto ret1;
    }
    HC_COV(40);
counts:
    {
        /* 0033b38e mov eax, [edi + 0x15f0]; cmp [eax + ebx*4], 0; setg al */
        const uint32_t counts = ld32(player + HC_P_COUNTS);
        if (counts == 0U)
            return ISAAC_VITA_HASCOLL_DECLINE_NULL;
        r->eax = (counts & ~0xFFU) |
                 ((int32_t)ld32(counts + id * 4U) > 0 ? 1U : 0U);
        goto out;
    }
ret1:
    r->eax = (r->eax & ~0xFFU) | 1U;      /* 0033b383 mov al, 1 */
    goto out;
ret0:
    r->eax &= ~0xFFU;                     /* 0033b2bc xor al, al */
out:
    return HC_OK;
}

static void hc_decline(CPU *__restrict c, int verdict)
{
    ++s_counters.declined[verdict];
    __real_sub_0033af30(c);
}

void isaac_vita_hascoll_native_call(CPU *__restrict c)
{
    hc_call k;
    hc_regs r;
    const uint32_t e = c->esp;
    uint32_t config;
    int verdict;

    ++s_counters.calls;
    if (!hc_stack_ok(c, e)) {
        hc_decline(c, ISAAC_VITA_HASCOLL_DECLINE_STACK);
        return;
    }
    config = ld32(HC_VA(HC_G_CONFIG));
    if (c->ecx == 0U || config == 0U) {
        hc_decline(c, ISAAC_VITA_HASCOLL_DECLINE_NULL);
        return;
    }
    k.player = c->ecx;
    k.game = ld32(HC_VA(HC_G_GAME));
    k.items = config + HC_CONFIG_ITEMS;
    k.gfuel_read = 0;
    k.bodies = 0U;
    r.eax = c->eax;
    r.ecx = c->ecx;
    r.edx = c->edx;
    verdict = hc_body(&k, e, ld32(e + 4U), ld8(e + 8U), c->ebp, c->ebx,
                      c->esi, c->edi, &r);
    if (verdict != HC_OK) {
        hc_decline(c, verdict);
        return;
    }
    ++s_counters.handled;
    s_counters.bodies += k.bodies;
    s_counters.yes += (r.eax & 0xFFU) != 0U;
    c->eax = r.eax;
    c->ecx = r.ecx;
    c->edx = r.edx;
    c->esp = e + 12U;                     /* 0033b38b ret 8 */
}

#if !defined(ISAAC_VITA_HASCOLL_NATIVE_CHAINED)
void __wrap_sub_0033af30(CPU *__restrict c)
{
    isaac_vita_hascoll_native_call(c);
}
#endif
