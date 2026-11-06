/* Native SEED_G_FUEL queries under HasCollectible
 * (ISAAC_VITA_GFUEL_SEED_NATIVE, default OFF).
 *
 * Entity_Player::HasCollectible (0x33af30) calls sub_0039cb10 once directly
 * (0x33b1d8) and once through sub_0039cb80 (0x33b1a8) on every call.  Counted
 * on the shipped PC build of the same image (hcount.dll, 2026-10-12): 690
 * HasCollectible calls per game tick in an empty room, 955 in a fight, 1,091
 * in a fight with seven items; the set below was empty on every query.
 *
 * sub_0039cb10 (thiscall Entity_Player *): lower_bound(SEED_G_FUEL = 79 in
 *   enums.lua SeedEffect) in Game's std::set<SeedEffect> at Game+0x1bb58,
 *   through the generic tree helper sub_000206b0; when the key is present,
 *   the byte at index 0x8f of the player's byte vector [this+0x1ecc,
 *   this+0x1ed0) if that vector is longer than 0x8f, else 0.
 * sub_0039cb80 (thiscall): that byte through the frozen 8-entry jump table
 *   at 0x39cc68 (0 and 1 -> -1, 2..7 -> 0..5); above 7 the body's RNG path.
 *
 * Link-time seam: GNU ld --wrap sends every edge from another unit and the
 * guest_table.c dispatch slot here.  The one edge inside the owner unit
 * (0x39cb8b, sub_0039cb80 -> sub_0039cb10) only runs inside the translated
 * sub_0039cb80, i.e. after a decline.  The replay leaves exactly the guest
 * stack words, registers and ESP the translated bodies leave (listings of
 * guest_0106.c and guest_0004.c, generated-int3-off) and declines, before
 * any write, into __real_ on a null link, a walk longer than the bound, a
 * guest stack that cannot hold the frames, or (sub_0039cb80) a byte above 7.
 * Neither body flushes its lazy flags on a return path, so the flags are
 * left as the bodies leave them: untouched.
 *
 * Counters (ph120.gf, host_vita_gfuel_seed_native.h): entries, native
 * answers and declines by reason for each wrapper.  The guest sampler cannot
 * tell the replay from the translated chain: the replay writes the same
 * stack words, including the 0x39cb3c return word of the sub_000206b0 call,
 * so stale copies of it are attributed to sub_000206b0 either way.  With
 * ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE (profiling builds, which also pass
 * --wrap for sub_000206b0, sub_0033af30, sub_002fe5e0, sub_0038d4c0 and
 * sub_0039ba90): every entry into the translated sub_000206b0 from another
 * unit is counted by its return word (0x39cb3c = the translated lookup under
 * sub_0039cb10, others tabled by call site, ph120.gl); HasCollectible's
 * entries from other units are counted and tabled by call site (ph120.gh),
 * split into top-level and nested ones (ph120.gn); every HasCollectible
 * body is counted and tabled by its caller through its first call
 * (ph120.gn b, ph120.gb); both G FUEL wrappers, those sub_000206b0 entries,
 * top-level HasCollectible and GetTrinketMultiplier outside HasCollectible
 * are timed on a 1-in-64 sample without the sampler (ph120.gu, ph120.gn).
 * None of it touches guest state. */
#include "guest.h"
#include "host_vita_gfuel_seed_native.h"

void __real_sub_0039cb10(CPU *__restrict c);
void __real_sub_0039cb80(CPU *__restrict c);
void __wrap_sub_0039cb10(CPU *__restrict c);
void __wrap_sub_0039cb80(CPU *__restrict c);

#define GFUEL_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (uint32_t)(rva)))
/* 0039cb20 mov ecx, [0x987fd65c]: the Game pointer. */
#define GFUEL_G_GAME 0x007FD65CU
/* 0039cb2a add ecx, 0x1bb58: the set object {head, size}. */
#define GFUEL_SEEDS_SET 0x0001BB58U
/* 0039cb19 mov dword ptr [ebp - 4], 0x4f */
#define GFUEL_KEY 0x4FU
/* 0039cb4f/0039cb55 end/begin, 0039cb5d cmp eax, 0x8f, 0039cb64 movzx. */
#define GFUEL_VEC_BEGIN 0x1ECCU
#define GFUEL_VEC_END 0x1ED0U
#define GFUEL_INDEX 0x8FU
/* MSVC _Tree_node as sub_000206b0 reads it: _Left +0, _Parent +4,
 * _Right +8, _Isnil +0xd, key +0x10. */
#define GFUEL_NODE_PARENT 0x4U
#define GFUEL_NODE_RIGHT 0x8U
#define GFUEL_NODE_ISNIL 0xDU
#define GFUEL_NODE_KEY 0x10U
/* Return words of 0039cb37 call 0x206b0 and 0039cb8b call 0x39cb10. */
#define GFUEL_RET_LOOKUP 0x0039CB3CU
#define GFUEL_RET_QUERY 0x0039CB90U
/* shortcut: a walk longer than this declines to the translated body; any
 * well-formed set of the 80 SeedEffect keys is at most ~13 deep. */
#define GFUEL_MAX_WALK 64U
/* Deepest word written below the entry ESP: sub_0039cb10 e-0x30;
 * sub_0039cb80 (frame <= 0x1b) + return word + 0x30 = e-0x57. */
#define GFUEL_QUERY_BYTES 0x30U
#define GFUEL_MODE_BYTES 0x58U

typedef struct {
    uint32_t result;              /* EAX of sub_0039cb10 */
    uint32_t ecx;                 /* ECX it returns with */
    uint32_t node, dir, bound;    /* sub_000206b0's 12-byte result */
    int walked;                   /* 000206d0 push esi executed */
    int present;                  /* the key was found (0039cb4b taken) */
} gfuel_query;

#define GFUEL_OK ISAAC_VITA_GFUEL_DECLINE_REASONS

static IsaacVitaGfuelSeedCounters s_counters;

#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE)
# define GFUEL_LOOKUP_LINKED 1U
uint64_t sceKernelGetProcessTimeWide(void);
void __real_sub_000206b0(CPU *__restrict c);
void __wrap_sub_000206b0(CPU *__restrict c);
void __real_sub_0033af30(CPU *__restrict c);
void __wrap_sub_0033af30(CPU *__restrict c);
void __real_sub_002fe5e0(CPU *__restrict c);
void __wrap_sub_002fe5e0(CPU *__restrict c);
void __real_sub_0038d4c0(CPU *__restrict c);
void __wrap_sub_0038d4c0(CPU *__restrict c);
void __real_sub_0039ba90(CPU *__restrict c);
void __wrap_sub_0039ba90(CPU *__restrict c);

# define GFUEL_RET_HAS_ITEM 0x0033AF53U    /* 0033af4e call 0x2fe5e0, every body */
# define GFUEL_RET_HAS_GTM_A 0x0033B006U   /* 0033b001 call 0x38d4c0 (ids 0x147/0x148) */
# define GFUEL_RET_HAS_GTM_B 0x0033B0CBU   /* 0033b0c6 call 0x38d4c0 (id 9) */
# define GFUEL_RET_HAS_JUDAS 0x0033B12EU   /* 0033b129 call 0x39ba90 (id 0x3b) */

/* shortcut: a span of 100 us or more (preemption, an interrupt) only counts
 * in `over`; raise it if a timed body ever legitimately takes that long. */
# define GFUEL_OUTLIER_US 100U
# define GFUEL_SITE_SLOTS 1024U
# define GFUEL_SITE_PROBES 16U

typedef struct { uint32_t ret, n; } gfuel_slot;
static gfuel_slot s_lookup_slots[GFUEL_SITE_SLOTS];
static gfuel_slot s_has_slots[GFUEL_SITE_SLOTS];
static gfuel_slot s_body_slots[GFUEL_SITE_SLOTS];
/* A timed span is open: nested wrappers neither time nor calibrate.  Every
 * take clears it, in case a guest longjmp left a span open. */
static int s_timing;
/* Open wrap-entered HasCollectible calls plus open calls a HasCollectible
 * (however entered) made to sub_0038d4c0 or sub_0039ba90: nonzero = inside a
 * HasCollectible.  Of its sixteen callees only those two reach it again
 * (39ba90 at 0x39baa7, 38d4c0 at 0x38d6bd/0x38d6cf/0x38d777); the others are
 * leaves or call only sub_000206b0 or, on error paths, KAGE::LogMessage
 * (their listings read, LogMessage's not), and its two self-calls stay
 * inside the unit.  Cleared by every take like
 * s_timing; never decremented below zero. */
static uint32_t s_has_depth;

static void gfuel_has_leave(void)
{
    if (s_has_depth)
        --s_has_depth;
}

static void gfuel_timer_add(unsigned kind, uint64_t us)
{
    IsaacVitaGfuelTimer *t = &s_counters.timer[kind];
    ++t->n;
    if (us >= GFUEL_OUTLIER_US)
        ++t->over;
    else
        t->us += (uint32_t)us;
}

/* Entry ordinal & 63: 0 = time the call, 32 = time an empty bracket. */
# define GFUEL_TIMED(kind, ordinal, call)                                   \
    do {                                                                    \
        const uint32_t gfuel_phase = (ordinal) & 63U;                       \
        if (gfuel_phase == 0U && !s_timing) {                               \
            uint64_t gfuel_t0;                                              \
            s_timing = 1;                                                   \
            gfuel_t0 = sceKernelGetProcessTimeWide();                       \
            call;                                                           \
            gfuel_timer_add((kind), sceKernelGetProcessTimeWide() - gfuel_t0); \
            s_timing = 0;                                                   \
        } else {                                                            \
            if (gfuel_phase == 32U && !s_timing) {                          \
                const uint64_t gfuel_t0 = sceKernelGetProcessTimeWide();    \
                gfuel_timer_add(ISAAC_VITA_GFUEL_TIMER_EMPTY,               \
                                sceKernelGetProcessTimeWide() - gfuel_t0);  \
            }                                                               \
            call;                                                           \
        }                                                                   \
    } while (0)

static void gfuel_site_count(gfuel_slot *t, uint32_t *overflow, uint32_t ret)
{
    uint32_t i = (ret * 2654435761U) >> 22, k;
    for (k = 0; k < GFUEL_SITE_PROBES; ++k, i = (i + 1U) & (GFUEL_SITE_SLOTS - 1U)) {
        if (t[i].n == 0U) {
            t[i].ret = ret;
            t[i].n = 1U;
            return;
        }
        if (t[i].ret == ret) {
            ++t[i].n;
            return;
        }
    }
    ++*overflow;
}

/* distinct and the top sites by entries (ties keep slot order). */
static void gfuel_sites_take(const gfuel_slot *t, IsaacVitaGfuelSites *s)
{
    uint32_t i, j;
    for (i = 0; i < GFUEL_SITE_SLOTS; ++i) {
        if (t[i].n == 0U)
            continue;
        ++s->distinct;
        for (j = ISAAC_VITA_GFUEL_TOP_SITES; j > 0U && s->top[j - 1U].n < t[i].n; --j)
            if (j < ISAAC_VITA_GFUEL_TOP_SITES)
                s->top[j] = s->top[j - 1U];
        if (j < ISAAC_VITA_GFUEL_TOP_SITES) {
            s->top[j].ret = t[i].ret;
            s->top[j].n = t[i].n;
        }
    }
}

/* Every generated call site pushes its return RVA and publishes ESP before
 * entering the callee, so [esp] names the caller. */
void __wrap_sub_000206b0(CPU *__restrict c)
{
    const uint32_t ret = ld32(c->esp);
    uint32_t ordinal;
    if (ret == GFUEL_RET_LOOKUP) {
        ++s_counters.lookup_translated;
        __real_sub_000206b0(c);
        return;
    }
    ordinal = s_counters.lookup_other++;
    gfuel_site_count(s_lookup_slots, &s_counters.lookup_sites.overflow, ret);
    GFUEL_TIMED(ISAAC_VITA_GFUEL_TIMER_LOOKUP, ordinal, __real_sub_000206b0(c));
}

/* Entity_Player::HasCollectible (thiscall, ret 8): [esp] the return word,
 * [esp+4] the id (0033af3a mov ebx, [ebp+8]), [esp+8] ignoreModifiers
 * (0033afe3 cmp byte ptr [ebp+0xc], 0).  Reads only. */
void __wrap_sub_0033af30(CPU *__restrict c)
{
    const uint32_t e = c->esp;
    /* top-level ordinal: earlier entries minus the nested ones */
    const uint32_t ordinal = s_counters.has_calls++ - s_counters.has_nested;
    s_counters.has_ignore += ld8(e + 8U) != 0U;
    s_counters.has_negative += (int32_t)ld32(e + 4U) < 0;
    gfuel_site_count(s_has_slots, &s_counters.has_sites.overflow, ld32(e));
    if (s_has_depth) {
        /* its time is inside the enclosing span already */
        ++s_counters.has_nested;
        ++s_has_depth;
        ISAAC_VITA_GFUEL_HAS_BODY(c);
        gfuel_has_leave();
        return;
    }
    s_has_depth = 1U;
    GFUEL_TIMED(ISAAC_VITA_GFUEL_TIMER_HAS, ordinal, ISAAC_VITA_GFUEL_HAS_BODY(c));
    gfuel_has_leave();
}

/* ItemConfig::GetCollectible (thiscall, ret 4).  Its call at 0x33af4e is the
 * first call of every HasCollectible body, after `push ebp; mov ebp, esp`, so
 * [ebp+4] is that body's own return word: the table sees the callers inside
 * HasCollectible's unit (its two self-calls included) that --wrap cannot. */
void __wrap_sub_002fe5e0(CPU *__restrict c)
{
    if (ld32(c->esp) == GFUEL_RET_HAS_ITEM) {
        ++s_counters.has_bodies;
        gfuel_site_count(s_body_slots, &s_counters.body_sites.overflow,
                         ld32(c->ebp + 4U));
    }
    __real_sub_002fe5e0(c);
}

/* Entity_Player::GetTrinketMultiplier (thiscall, ret 4): calls
 * HasCollectible for Mom's Purse 0x8b, Belly Button 0x1ca and Mom's Box
 * 0x1b7 in every body, and HasCollectible calls it for ids 0x147/0x148/9. */
void __wrap_sub_0038d4c0(CPU *__restrict c)
{
    const uint32_t ret = ld32(c->esp);
    ++s_counters.gtm_calls;
    if (ret == GFUEL_RET_HAS_GTM_A || ret == GFUEL_RET_HAS_GTM_B) {
        ++s_counters.gtm_from_has;
        ++s_has_depth;
        __real_sub_0038d4c0(c);
        gfuel_has_leave();
        return;
    }
    if (s_has_depth) {
        __real_sub_0038d4c0(c);
        return;
    }
    {
        const uint32_t ordinal = s_counters.gtm_top++;
        GFUEL_TIMED(ISAAC_VITA_GFUEL_TIMER_GTM, ordinal, __real_sub_0038d4c0(c));
    }
}

/* sub_0039ba90 (Judas / Black Judas: HasCollectible(Birthright 0x26b)),
 * called by HasCollectible for id 0x3b; only its depth is kept. */
void __wrap_sub_0039ba90(CPU *__restrict c)
{
    if (ld32(c->esp) != GFUEL_RET_HAS_JUDAS) {
        __real_sub_0039ba90(c);
        return;
    }
    ++s_has_depth;
    __real_sub_0039ba90(c);
    gfuel_has_leave();
}
#else
# define GFUEL_LOOKUP_LINKED 0U
# define GFUEL_TIMED(kind, ordinal, call) call
#endif

void isaac_vita_gfuel_seed_take(IsaacVitaGfuelSeedCounters *out)
{
#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE)
    if (out) {
        gfuel_sites_take(s_lookup_slots, &s_counters.lookup_sites);
        gfuel_sites_take(s_has_slots, &s_counters.has_sites);
        gfuel_sites_take(s_body_slots, &s_counters.body_sites);
    }
    memset(s_lookup_slots, 0, sizeof s_lookup_slots);
    memset(s_has_slots, 0, sizeof s_has_slots);
    memset(s_body_slots, 0, sizeof s_body_slots);
    s_timing = 0;
    s_has_depth = 0U;
#endif
    if (out) {
        *out = s_counters;
        out->lookup_linked = GFUEL_LOOKUP_LINKED;
    }
    memset(&s_counters, 0, sizeof s_counters);
}

/* The 0x39cc68 table: targets 39cb9e, 39cb9e, 39cba7, 39cbaf, 39cbba,
 * 39cbc5, 39cbd0, 39cbdb load -1, -1, 0, 1, 2, 3, 4, 5. */
static const uint32_t k_gfuel_mode[8] = {
    0xFFFFFFFFU, 0xFFFFFFFFU, 0U, 1U, 2U, 3U, 4U, 5U
};

static int gfuel_stack_ok(const CPU *c, uint32_t esp, uint32_t below)
{
    uint32_t capacity, offset;
    if (!guest_stack_fast_bound(c))
        return 0;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = esp - c->stack_floor;
    return capacity >= below + 4U && offset >= below &&
           offset <= capacity - 4U;
}

/* Reads only: the walk of sub_000206b0 and the tail of sub_0039cb10.
 * GFUEL_OK, or the decline reason. */
static int gfuel_read(uint32_t self, gfuel_query *q)
{
    uint32_t game = ld32(GFUEL_VA(GFUEL_G_GAME));
    uint32_t head, node, steps = 0U;

    if (game == 0U)
        return ISAAC_VITA_GFUEL_DECLINE_NULL;
    head = ld32(game + GFUEL_SEEDS_SET);               /* 0039cb35 */
    if (head == 0U)
        return ISAAC_VITA_GFUEL_DECLINE_NULL;
    node = ld32(head + GFUEL_NODE_PARENT);             /* 000206b8 */
    if (node == 0U)
        return ISAAC_VITA_GFUEL_DECLINE_NULL;
    q->node = node;                                    /* 000206bb..c4 */
    q->dir = 0U;
    q->bound = head;
    q->ecx = head;                                     /* 000206b3 */
    q->walked = 0;
    q->present = 0;
    if (!ld8(node + GFUEL_NODE_ISNIL)) {               /* 000206c7 */
        q->walked = 1;
        do {                                           /* 000206d3..f2 */
            if (++steps > GFUEL_MAX_WALK)
                return ISAAC_VITA_GFUEL_DECLINE_WALK;
            q->node = node;
            if ((int32_t)ld32(node + GFUEL_NODE_KEY) >= (int32_t)GFUEL_KEY) {
                q->bound = node;
                q->dir = 1U;
                node = ld32(node);
            } else {
                q->dir = 0U;
                node = ld32(node + GFUEL_NODE_RIGHT);
            }
            if (node == 0U)
                return ISAAC_VITA_GFUEL_DECLINE_NULL;
        } while (!ld8(node + GFUEL_NODE_ISNIL));
        q->ecx = q->dir;
    }
    q->result = 0U;
    if (!ld8(q->bound + GFUEL_NODE_ISNIL) &&                       /* 0039cb3f */
            (int32_t)ld32(q->bound + GFUEL_NODE_KEY) <= (int32_t)GFUEL_KEY &&
            q->bound != head) {                                    /* 0039cb4b */
        uint32_t begin, end;
        if (self == 0U)
            return ISAAC_VITA_GFUEL_DECLINE_NULL;
        q->present = 1;
        end = ld32(self + GFUEL_VEC_END);
        begin = ld32(self + GFUEL_VEC_BEGIN);
        q->ecx = begin;
        if ((int32_t)(end - begin) > (int32_t)GFUEL_INDEX)        /* 0039cb5d */
            q->result = ld8(begin + GFUEL_INDEX);
    }
    return GFUEL_OK;
}

static void gfuel_count_handled(const gfuel_query *q)
{
    s_counters.walked += (uint32_t)q->walked;
    s_counters.present += (uint32_t)q->present;
}

/* The words sub_0039cb10 and sub_000206b0 leave below entry ESP e. */
static void gfuel_write(uint32_t e, uint32_t ebp, uint32_t esi, uint32_t edi,
                        uint32_t self, const gfuel_query *q)
{
    st32(e - 0x04U, ebp);                 /* 0039cb10 push ebp */
    st32(e - 0x08U, GFUEL_KEY);           /* 0039cb19 [ebp-4] */
    st32(e - 0x0CU, q->bound);            /* [ebp-8]  result +8 */
    st32(e - 0x10U, q->dir);              /* [ebp-0xc] result +4 */
    st32(e - 0x14U, q->node);             /* [ebp-0x10] result +0 */
    st32(e - 0x18U, esi);                 /* 0039cb16 push esi */
    st32(e - 0x1CU, edi);                 /* 0039cb29 push edi */
    st32(e - 0x20U, e - 0x08U);           /* 0039cb30 push eax (&key) */
    st32(e - 0x24U, e - 0x14U);           /* 0039cb34 push eax (&result) */
    st32(e - 0x28U, GFUEL_RET_LOOKUP);    /* 0039cb37 call 0x206b0 */
    st32(e - 0x2CU, e - 0x04U);           /* 000206b0 push ebp */
    if (q->walked)
        st32(e - 0x30U, self);            /* 000206d0 push esi (= this) */
}

static void gfuel_query_call(CPU *__restrict c)
{
    gfuel_query q;
    const uint32_t e = c->esp;
    const int verdict = gfuel_stack_ok(c, e, GFUEL_QUERY_BYTES)
        ? gfuel_read(c->ecx, &q) : ISAAC_VITA_GFUEL_DECLINE_STACK;

    ++s_counters.query_calls;
    if (verdict != GFUEL_OK) {
        ++s_counters.declined[verdict];
        __real_sub_0039cb10(c);
        return;
    }
    ++s_counters.query_handled;
    gfuel_count_handled(&q);
    gfuel_write(e, c->ebp, c->esi, c->edi, c->ecx, &q);
    c->eax = q.result;
    c->ecx = q.ecx;
    c->edx = e - 0x14U;                   /* 000206b5 mov edx, [ebp+8] */
    c->esp = e + 4U;
}

static void gfuel_mode_call(CPU *__restrict c)
{
    gfuel_query q;
    const uint32_t e = c->esp;
    uint32_t frame, inner;
    int verdict = ISAAC_VITA_GFUEL_DECLINE_STACK;

    if (gfuel_stack_ok(c, e, GFUEL_MODE_BYTES)) {
        verdict = gfuel_read(c->ecx, &q);
        if (verdict == GFUEL_OK && q.result > 7U)          /* 0039cb95 ja */
            verdict = ISAAC_VITA_GFUEL_DECLINE_BYTE;
    }
    ++s_counters.mode_calls;
    if (verdict != GFUEL_OK) {
        ++s_counters.declined[verdict];
        __real_sub_0039cb80(c);
        return;
    }
    ++s_counters.mode_handled;
    gfuel_count_handled(&q);
    frame = ((e - 4U) & ~7U) - 0x10U;     /* 0039cb83 and esp,-8; sub 0x10 */
    inner = frame - 12U;                  /* esi, edi, return word */
    st32(e - 4U, c->ebp);                 /* 0039cb80 push ebp */
    st32(frame - 4U, c->esi);             /* 0039cb89 push esi */
    st32(frame - 8U, c->edi);             /* 0039cb8a push edi */
    st32(inner, GFUEL_RET_QUERY);         /* 0039cb8b call 0x39cb10 */
    gfuel_write(inner, e - 4U, c->esi, c->edi, c->ecx, &q);
    c->eax = k_gfuel_mode[q.result];
    c->ecx = q.ecx;
    c->edx = inner - 0x14U;
    c->esp = e + 4U;
}

void __wrap_sub_0039cb10(CPU *__restrict c)
{
    GFUEL_TIMED(ISAAC_VITA_GFUEL_TIMER_QUERY, s_counters.query_calls,
                gfuel_query_call(c));
}

void __wrap_sub_0039cb80(CPU *__restrict c)
{
    GFUEL_TIMED(ISAAC_VITA_GFUEL_TIMER_MODE, s_counters.mode_calls,
                gfuel_mode_call(c));
}
