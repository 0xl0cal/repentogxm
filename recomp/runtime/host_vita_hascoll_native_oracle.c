/* Differential oracle of host_vita_hascoll_native.c.
 *
 * test_vita_hascoll_native.py extracts the translated sub_0033af30 and
 * every callee its paths reach from the frozen corpus (sub_0033af30 renamed
 * real_sub_0033af30 together with its two self-calls: the view inside its
 * own unit) and compiles host_vita_hascoll_native.c separately with
 * __real_sub_0033af30 = oracle_real_hc and the branch counters on.  Each
 * case builds one world - Game, ItemConfig, two Entity_Player objects with
 * temporary effects, a collectible map, a byte vector, slots and counts, the
 * G FUEL seed set, a register file and an entry ESP over random stack
 * words - runs the translated body, then the wrapper on the same state, and
 * compares the whole CPU struct and every stack word (guard words below the
 * floor included).  Neither side may write anything else: the world lives
 * in one MEM_WRITE_WATCH arena (and the image pages are write-watched too),
 * and any written page after either run is a failure.
 *
 * GetTrinketMultiplier (sub_0038d4c0, 228 instructions) and the logger
 * (sub_0055e330) are deterministic stand-ins shared by both runs; the
 * stand-in can call HasCollectible back from "another unit" like 0x38d6bd,
 * and so does the translated sub_0039ba90 (0x39baa7): those edges reach the
 * translated body in the reference run and the wrapper in the native run,
 * exactly as --wrap links them.
 *
 * Fault probes (null links, a corrupted jump table): the translated body
 * would dereference the null, so __real_ is suppressed; the wrapper must
 * decline with the expected reason, leave the CPU and the world untouched,
 * and write no stack word outside [entry ESP - HC_ORACLE_STACK_BYTES, entry
 * ESP).
 *
 * Flavours: HC_ORACLE_CHAINED links the G FUEL profiling TU
 * (ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE + ISAAC_VITA_HASCOLLECTIBLE_NATIVE)
 * whose __wrap_sub_0033af30 must hand every entry to
 * isaac_vita_hascoll_native_call.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <stdarg.h>
#include <windows.h>

#include "host_vita_hascoll_native.h"
#if defined(HC_ORACLE_CHAINED)
/* isaac_vita_gfuel_seed_take() fills the profiling TU's counter layout; the
 * ph120.gn/gb members exist only under these defines, so a chained oracle
 * compiled without them would hand a short struct to the take. */
# if !defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE) && \
     !defined(ISAAC_VITA_PHASE_PROFILE)
#  error HC_ORACLE_CHAINED links the G FUEL profiling TU: define ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE
# endif
# include "host_vita_gfuel_seed_native.h"
#endif

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_hascoll_native_oracle requires a 32-bit host
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif
#if !defined(HC_COV_MARKERS) || !defined(HC_ORACLE_STACK_BYTES)
# error test_vita_hascoll_native.py passes HC_COV_MARKERS and HC_ORACLE_STACK_BYTES
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))

/* ---- stubs owned by other Vita runtime units (the kage-quad oracle's) - */
unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
int g_isaac_vita_import_ids_ready = 1;
int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    (void)imports;
    (void)count;
    return 1;
}
int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    (void)c;
    (void)import_id;
    return 0;
}
const char *guest_host_import_id_error(void)
{
    return "oracle import-ID error";
}
int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

#if defined(HC_ORACLE_CHAINED)
static uint64_t s_clock;
uint64_t sceKernelGetProcessTimeWide(void)
{
    return ++s_clock;
}
#endif

/* ---- the translated bodies and the native TU -------------------------- */
void real_sub_0033af30(CPU *__restrict c);
void __wrap_sub_0033af30(CPU *__restrict c);
unsigned g_hc_cov[HC_COV_MARKERS];

static int s_native_mode;          /* which side an other-unit edge reaches */
static int s_suppress_real;        /* fault probe: the body would fault */
static unsigned s_real_calls, s_real_suppressed, s_other_native;

/* __real_sub_0033af30 of the native TU */
void oracle_real_hc(CPU *__restrict c)
{
    ++s_real_calls;
    if (s_suppress_real) {
        ++s_real_suppressed;
        return;
    }
    real_sub_0033af30(c);
}

/* A HasCollectible edge from another generated unit (sub_0039ba90 in the
 * bodies TU, the GetTrinketMultiplier stand-in): --wrap sends it to the
 * wrapper. */
void oracle_hc_other_unit(CPU *__restrict c)
{
    if (s_native_mode) {
        ++s_other_native;
        __wrap_sub_0033af30(c);
    } else {
        real_sub_0033af30(c);
    }
}

/* KAGE::LogMessage (cdecl; the caller pops its two words). */
static unsigned s_logger_calls;
void sub_0055e330(CPU *__restrict c)
{
    ++s_logger_calls;
    c->eax = 0x0bad1065u;
    c->esp += 4u;
}

/* GetTrinketMultiplier stand-in (thiscall, ret 4): optionally calls
 * HasCollectible(0x8b, 0) back from 0x38d6bd, returns ([this+0x1f40] % 3)
 * plus that answer, clobbers ECX/EDX like a body. */
static int s_gtm_nest;
static unsigned s_gtm_calls;
void sub_0038d4c0(CPU *__restrict c)
{
    const uint32_t e = c->esp, self = c->ecx, arg = ld32(e + 4u);
    uint32_t nested = 0u;
    ++s_gtm_calls;
    if (s_gtm_nest) {
        c->esp = e - 0x20u;
        st32(c->esp + 8u, 0u);
        st32(c->esp + 4u, 0x8bu);
        st32(c->esp, 0x0038d6c2u);
        c->ecx = self;
        oracle_hc_other_unit(c);
        nested = c->eax & 0xffu;
    }
    c->eax = (uint32_t)ld8(self + 0x1f40u) % 3u + nested;
    c->ecx = arg ^ 0x5a5a0000u;
    c->edx = self;
    c->esp = e + 8u;
}

/* ---- the world -------------------------------------------------------- */
#define STACK_WORDS 4096u
#define GUARD_WORDS 512u
#define ABOVE_WORDS 16u
#define GAME_BYTES 0x284000u
#define CONFIG_BYTES 0x2a000u
#define PLAYER_BYTES 0x2000u
#define ITEMS 0x300u
#define ITEM_BYTES 0xc0u
#define EFFECTS 16u
#define BIG_EFFECTS 4100u
#define EFFCFGS 64u
#define MAP_NODES 80u
#define NEG_NODES 8u
#define SEED_NODES 80u
#define VEC_BYTES 0x400u
#define COUNT_WORDS 0x400u
#define TRINKETS 16u

typedef struct node {            /* MSVC _Tree_node<int> / <pair<int,int>> */
    uint32_t left, parent, right;
    uint8_t color, isnil, pad[2];
    int32_t key;
    int32_t value;
} node;

typedef struct world {
    uint8_t game[GAME_BYTES];
    uint8_t config[CONFIG_BYTES];
    uint8_t player[2][PLAYER_BYTES];
    uint8_t items[ITEMS][ITEM_BYTES];
    uint32_t item_ptrs[ITEMS + 16u];
    uint32_t effects[2][EFFECTS * 3u];
    uint32_t big_effects[BIG_EFFECTS * 3u];
    uint32_t effcfg[EFFCFGS][4];
    node map[2][MAP_NODES + 1u];
    node neg[2][NEG_NODES + 1u];
    node seeds[SEED_NODES + 1u];
    uint8_t vec[2][VEC_BYTES];
    int32_t counts[2][COUNT_WORDS];
    uint32_t level[16], room[32];
    uint32_t players_vec[4];
    uint32_t trinket_ptrs[TRINKETS], trinket_objs[TRINKETS];
} world;

static world *W;                  /* the write-watched arena */
static uint32_t s_stack[GUARD_WORDS + STACK_WORDS + ABOVE_WORDS];
static uint32_t s_stack_snap[GUARD_WORDS + STACK_WORDS + ABOVE_WORDS];
static uint32_t s_stack_ref[GUARD_WORDS + STACK_WORDS + ABOVE_WORDS];
static CPU s_cpu, s_cpu_snap, s_cpu_ref;

#define ORACLE_PAGE 0x10000u
static const uint32_t s_pages[] = {
    0x002f0000u,   /* 2ff0bc/2ff0c4 item predicate tables */
    0x00390000u,   /* 39cc68 G FUEL mode table */
    0x00600000u,   /* 608ad0/608b00/608b54 RNG shift words */
    0x00740000u,   /* 74d554 RNG id table */
    0x007f0000u,   /* 7fd65c Game, 7fd680 ItemConfig */
};

static uint32_t s_lcg = 0x20261024u;
static uint32_t lcg(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg >> 8;
}
static uint32_t pick(uint32_t n) { return lcg() % n; }
static int chance(uint32_t percent) { return pick(100u) < percent; }

static uint32_t A(const void *p) { return (uint32_t)(uintptr_t)p; }
static uint32_t P(unsigned i) { return A(W->player[i]); }

static unsigned s_failures, s_fail_fast;

static void failf(const char *name, const char *fmt, ...)
{
    va_list ap;
    ++s_failures;
    if (s_failures <= 40u) {
        printf("FAIL %s: ", name);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
    if (s_fail_fast) {
        printf("Vita HasCollectible oracle: FAIL (fail-fast)\n");
        exit(1);
    }
}

/* ---- write watch -------------------------------------------------------- */
/* GetWriteWatch reports (and with the reset flag clears) at most the array's
 * worth of pages per call, so loop until a call comes back short: otherwise a
 * world builder that wrote more than 64 pages leaves the rest armed and the
 * next check blames the body under test. */
static unsigned written_pages(void *base, SIZE_T bytes)
{
    PVOID addrs[64];
    unsigned total = 0u;
    for (;;) {
        ULONG_PTR count = 64;
        ULONG gran;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, bytes, addrs, &count,
                          &gran) != 0) {
            printf("GetWriteWatch failed\n");
            exit(2);
        }
        total += (unsigned)count;
        if (count < 64)
            return total;
    }
}

static unsigned world_written(void)
{
    unsigned n = written_pages(W, sizeof *W), i;
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        n += written_pages((void *)(uintptr_t)VA(s_pages[i]), ORACLE_PAGE);
    return n;
}

/* ---- trees ------------------------------------------------------------ */
typedef struct tree {
    node *n;
    unsigned cap, used;
    uint32_t holder;              /* {head, size} */
} tree;

static void tree_init(tree *t, node *pool, unsigned cap, uint32_t holder)
{
    t->n = pool;
    t->cap = cap;
    t->used = 0;
    t->holder = holder;
    memset(pool, 0, sizeof(node) * (cap + 1u));
    pool[0].isnil = 1;
    pool[0].left = pool[0].right = pool[0].parent = A(&pool[0]);
    pool[0].key = (int32_t)0xdeadbeef;
    pool[0].value = (int32_t)lcg();
    st32(holder, A(&pool[0]));
    st32(holder + 4u, 0u);
}

static void tree_insert(tree *t, int32_t key, int32_t value)
{
    node *head = &t->n[0], *n;
    uint32_t at;
    if (t->used >= t->cap)
        return;
    n = &t->n[++t->used];
    n->left = n->right = n->parent = A(head);
    n->color = (uint8_t)(lcg() & 1u);
    n->isnil = 0;
    n->key = key;
    n->value = value;
    st32(t->holder + 4u, t->used);
    if (head->parent == A(head)) {
        head->parent = head->left = head->right = A(n);
        return;
    }
    at = head->parent;
    for (;;) {
        node *p = (node *)(uintptr_t)at;
        uint32_t *link = key < p->key ? &p->left : &p->right;
        if (*link == A(head)) {
            n->parent = at;
            *link = A(n);
            return;
        }
        at = *link;
    }
}

/* ---- one world -------------------------------------------------------- */
enum { MODE_CLEAN, MODE_WILD };

typedef struct hcase {
    char name[64];
    uint32_t id, ign_word, esp_offset;
    int expect;                   /* -1 any, HC_HANDLED, or a decline reason */
    int suppress;                 /* fault probe */
} hcase;
#define HC_HANDLED 100

static const uint32_t k_special[] = { 0x131u, 0x101u, 0xe7u, 0xb6u, 0x14au,
                                      0x99u, 0x76u, 0x216u, 0xbfu, 0x1e8u,
                                      0x8bu, 0x26bu };
static const uint32_t k_declined_ids[] = { 0x147u, 0x148u, 0x139u, 9u, 0x3bu };
static const uint32_t k_pred_ids[] = { 0x163u, 0x1fcu, 0x2dcu, 0x164u, 0x2ddu,
                                       0x1cu, 0x1du, 0x1eu, 0x20u, 0x37u,
                                       0x38u, 0xe4u, 0xe5u, 0xe6u, 0x162u };

static uint32_t rand_id(void)
{
    return pick(ITEMS + 0x20u);
}

static uint32_t item_tags(void)
{
    uint32_t t = lcg() & lcg();
    if (chance(50)) t |= 0x8000u; else t &= ~0x8000u;
    if (chance(50)) t |= 0x40000000u; else t &= ~0x40000000u;
    return t;
}

static void build_items(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < ITEMS; ++i) {
        uint8_t *it = W->items[i];
        uint32_t b;
        for (b = 0; b < ITEM_BYTES; b += 4u)
            st32(A(it) + b, lcg());
        static const uint32_t k_types[] = { 1u, 3u, 4u };
        st32(A(it), chance(70) ? k_types[pick(3u)] : pick(6u));
        st32(A(it) + 4u, chance(40) ? k_pred_ids[pick(sizeof k_pred_ids / 4u)]
                                    : pick(0x330u));
        st32(A(it) + 0xb8u, item_tags());
        st32(A(it) + 0xbcu, lcg());
        W->item_ptrs[i] = chance(12) ? 0u : A(it);
    }
    st32(A(W->config) + 0x29e84u, A(W->item_ptrs));
    st32(A(W->config) + 0x29e88u, A(W->item_ptrs) + 4u * n);
}

static void build_effects(unsigned p, uint32_t id, int big)
{
    const uint32_t self = P(p) + 0x1450u;
    uint32_t *list = big ? W->big_effects : W->effects[p];
    uint32_t n = big ? BIG_EFFECTS : pick(EFFECTS + 1u), i;
    for (i = 0; i < EFFCFGS; ++i) {
        W->effcfg[i][0] = chance(75) ? (pick(2u) ? 1u : 3u + pick(2u)) : pick(7u);
        W->effcfg[i][1] = chance(30) ? id :
                          chance(40) ? (pick(2u) ? 0x1e8u : 0x76u) : rand_id();
        W->effcfg[i][2] = lcg();
        W->effcfg[i][3] = lcg();
    }
    for (i = 0; i < n; ++i) {
        list[3u * i] = A(W->effcfg[big ? EFFCFGS - 1u : pick(EFFCFGS)]);
        list[3u * i + 1u] = lcg();
        list[3u * i + 2u] = lcg();
    }
    if (big)
        W->effcfg[EFFCFGS - 1u][0] = 2u;       /* never a hit */
    st8(self + 0x11u, chance(15) ? (uint8_t)(1u + pick(255u)) : 0u);
    st32(self + 4u, A(list));
    st32(self + 8u, A(list) + 12u * n);
}

static void build_map(unsigned p, uint32_t id, int chain)
{
    tree t;
    unsigned n, i;
    tree_init(&t, W->map[p], MAP_NODES, P(p) + 0x1e7cu);
    if (chain) {
        for (i = 0; i < 70u; ++i)
            tree_insert(&t, (int32_t)i, (int32_t)pick(3u));
        st32(P(p) + 0x1e80u, 70u);
        return;
    }
    n = chance(40) ? 0u : pick(12u);
    for (i = 0; i < n; ++i) {
        const int32_t key = chance(30) ? (int32_t)id + (int32_t)pick(3u) - 1 :
                            (int32_t)rand_id();
        tree_insert(&t, key, (int32_t)pick(4u) - 1);
    }
    /* the size word only gates the lookup; keep it independent sometimes */
    st32(P(p) + 0x1e80u, chance(25) ? 0u : chance(20) ? 1u + pick(3u) : t.used);
}

static void build_player(unsigned p, uint32_t id, int mode)
{
    const uint32_t pl = P(p);
    uint32_t i, size, begin, gfuel_byte;
    tree neg;
    for (i = 0; i < PLAYER_BYTES; i += 4u)
        st32(pl + i, lcg());
    st32(pl + 0x2cu, chance(50) ? 0u : chance(50) ? 2u + pick(5u) : 0xffffffffu);
    st32(pl + 0x28u, pick(3u));
    st32(pl + 0x314u, 0u);
    st32(pl + 0x130cu, chance(40) ? 0u : (uint32_t)(int32_t)
         (int)(pick(2u) ? (pick(2u) ? 0xau : 0x1fu) : (pick(2u) ? 3u : 0xcu)));
    st8(pl + 0x168u, chance(30) ? (uint8_t)(1u + pick(3u)) : 0u);
    st32(pl + 0x1d84u, 0u);
    st8(pl + 0x1f1cu, chance(30) ? 1u : 0u);
    st8(pl + 0x1f88u, chance(10) ? 1u : 0u);
    st8(pl + 0x1f18u, (uint8_t)lcg());
    st32(pl + 0x1eb0u, chance(50) ? 0u : chance(50) ? id : rand_id());
    st32(pl + 0x12e0u, chance(40) ? id : rand_id());
    st32(pl + 0x12d8u, chance(40) ? id : rand_id());
    st8(pl + 0x1f40u, (uint8_t)lcg());
    /* four slots, and a fifth word past them that only a wrong count reads */
    for (i = 0; i < 5u; ++i)
        st32(pl + 0x14c4u + 0x1cu * i, chance(i < 4u ? 10 : 25) ? id :
                                       chance(10) ? 0xbfu : rand_id());
    /* the byte vector at +0x1ecc/+0x1ed0 (sub_00535fa0 and the G FUEL byte) */
    for (i = 0; i < VEC_BYTES; ++i)
        W->vec[p][i] = chance(85) ? 0u : (uint8_t)lcg();
    if (chance(20) && id < VEC_BYTES - 0x40u)
        W->vec[p][0x40u + id] = (uint8_t)(1u + pick(3u));
    gfuel_byte = mode == MODE_CLEAN ? pick(8u) : chance(15) ? 8u + pick(250u) : pick(8u);
    W->vec[p][0x40u + 0x8fu] = (uint8_t)gfuel_byte;
    begin = A(W->vec[p]) + 0x40u;
    switch (pick(7u)) {
    case 0: size = 0u; break;
    case 1: size = 0x8fu; break;
    case 2: size = 0x90u; break;
    case 3: size = 0x10u + pick(0x80u); break;
    case 4: size = (uint32_t)-0x10; break;
    default: size = 0x100u + pick(0x2c0u); break;
    }
    st32(pl + 0x1eccu, begin);
    st32(pl + 0x1ed0u, begin + size);
    /* negative-id map at +0x1ec4 (sub_00535e00) */
    tree_init(&neg, W->neg[p], NEG_NODES, pl + 0x1ec4u);
    for (i = pick(NEG_NODES); i; --i)
        tree_insert(&neg, -(int32_t)(1u + pick(8u)), (int32_t)pick(3u) - 1);
    for (i = 0; i < COUNT_WORDS; ++i)
        W->counts[p][i] = chance(80) ? 0 : (int32_t)pick(4u) - 1;
    if (id < COUNT_WORDS && chance(25))
        W->counts[p][id] = (int32_t)pick(3u) - 1;
    st32(pl + 0x15f0u, A(W->counts[p]));
    build_effects(p, id, 0);
    build_map(p, id, 0);
}

static void build_game(int mode)
{
    const uint32_t g = A(W->game);
    tree seeds;
    unsigned i, n;
    st32(g + 0x1bb04u, lcg());
    st32(g + 0x1bb08u, 1u + pick(0x7fffffffu));
    st32(g + 0x1a30dcu, lcg());
    st32(g + 0x1c3204u, chance(50) ? 0u : lcg());
    st8(g + 0x1c31a1u, (uint8_t)(chance(50) ? 0u : 1u));
    W->room[0x58u / 4u] = 1u + pick(0x7fffffffu);
    W->level[1] = A(W->room);
    st32(g + 0x18190u, A(W->level));
    W->players_vec[0] = P(1);
    W->players_vec[1] = P(0);
    st32(g + 0x1ba50u, A(W->players_vec));
    st32(g + 0x1ba54u, A(W->players_vec) + 8u);
    for (i = 0; i < TRINKETS; ++i) {
        W->trinket_objs[i] = A(W->items[pick(ITEMS)]);
        W->trinket_ptrs[i] = A(&W->trinket_objs[i]);
    }
    st32(g + 0x283c20u, A(W->trinket_ptrs));
    st32(g + 0x283c24u, A(W->trinket_ptrs) + 4u * TRINKETS);
    tree_init(&seeds, W->seeds, SEED_NODES, g + 0x1bb58u);
    n = pick(8u);
    for (i = 0; i < n; ++i) {
        static const int32_t pool[] = { 10, 12, 30, 41, 55, 63, 75, 78, 80, 81,
                                        100, 1, 0, -5, 0x7fffffff };
        tree_insert(&seeds, pool[pick(sizeof pool / 4u)], 0);
    }
    if (chance(45))
        tree_insert(&seeds, 79, 0);
    (void)mode;
    st8(A(W->config) + 0x146u, (uint8_t)(chance(70) ? 0u : 1u));
    st32(A(W->config) + 8u, chance(50) ? 2u : pick(5u));
}

static void full_items(void)
{
    st32(A(W->config) + 0x29e88u, A(W->item_ptrs) + 4u * ITEMS);
}

/* Under [player+0x1f88] every body derefs its item: the call's own and the
 * nested 0xbf (0x33b356), 0x8b (stand-in) and 0x26b (sub_0039ba90) ones. */
static void gate_items(uint32_t id)
{
    full_items();
    if ((int32_t)id >= 0 && id < ITEMS)
        W->item_ptrs[id] = A(W->items[id]);
    W->item_ptrs[0xbfu] = A(W->items[0xbfu]);
    W->item_ptrs[0x8bu] = A(W->items[0x8bu]);
    W->item_ptrs[0x26bu] = A(W->items[0x26bu]);
}

/* A world for one call: player 0 is `this`, player 1 the twin / co-op
 * owner (never itself twinned: [player1+0x168] = 0). */
static void build_world(int mode, uint32_t id)
{
    build_game(mode);
    build_items(chance(10) ? pick(ITEMS) : ITEMS);
    build_player(0, id, mode);
    build_player(1, id, mode);
    st8(P(1) + 0x168u, 0u);
    st32(P(1) + 0x2cu, 0u);
    if (mode == MODE_WILD) {
        if (chance(6)) {
            st32(P(0) + 0x2cu, 1u);                       /* co-op baby */
            switch (pick(3u)) {
            case 0:                                       /* owner link */
                st32(P(0) + 0x314u, P(1));
                st32(P(1) + 0x28u, 1u);
                break;
            case 1:                                       /* players[0] */
                W->players_vec[0] = P(1);
                break;
            default:                                      /* no player */
                W->players_vec[0] = 0u;
                break;
            }
        }
        if (chance(25))
            st32(P(0) + 0x1d84u, P(1));                   /* the twin */
    }
    /* never a null item under the 0x1f88 gate in a random world (that is a
     * fault probe); player 1 (twin / co-op owner) runs without the gate */
    st8(P(1) + 0x1f88u, 0u);
    if (ld8(P(0) + 0x1f88u)) {
        if ((int32_t)id >= 0 && id < ITEMS)
            gate_items(id);
        else
            st8(P(0) + 0x1f88u, 0u);
    }
}

static int declined_id(uint32_t id)
{
    unsigned i;
    for (i = 0; i < sizeof k_declined_ids / 4u; ++i)
        if (id == k_declined_ids[i])
            return 1;
    return 0;
}

/* ---- run and compare --------------------------------------------------- */
static unsigned s_cases, s_handled, s_declined, s_probes;
static unsigned s_ref_true;
static unsigned s_max_depth, s_cov_handled[HC_COV_MARKERS];
static unsigned s_reason_cases[ISAAC_VITA_HASCOLL_DECLINE_REASONS];
static IsaacVitaHasCollCounters s_total;

static void set_cpu(const hcase *k)
{
    const uint32_t floor = A(&s_stack[GUARD_WORDS]);
    uint32_t i, e;
    for (i = 0; i < GUARD_WORDS + STACK_WORDS + ABOVE_WORDS; ++i)
        s_stack[i] = lcg() ^ (lcg() << 8);
    guest_cpu_init(&s_cpu);
    if (guest_stack_bind(&s_cpu, floor, floor + 4u * STACK_WORDS)) {
        printf("stack bind failed\n");
        exit(2);
    }
    s_cpu.eax = lcg() ^ (lcg() << 8);
    s_cpu.ebx = lcg();
    s_cpu.ecx = P(0);
    s_cpu.edx = lcg() ^ 0x77000000u;
    s_cpu.ebp = floor + 4u * STACK_WORDS - 0x40u;
    s_cpu.esi = lcg() ^ 0x51510000u;
    s_cpu.edi = lcg() ^ 0xa7a70000u;
    e = floor + k->esp_offset;
    s_cpu.esp = e;
    s_cpu.x[0].u64[0] = ((uint64_t)lcg() << 32) | lcg();
    s_cpu.f_a = lcg();
    s_cpu.f_b = lcg();
    s_cpu.f_r = lcg();
    s_cpu.f_op = FLAG_SUB;
    s_cpu.f_sz = 4;
    if (k->esp_offset + 12u <= 4u * (STACK_WORDS + ABOVE_WORDS)) {
        st32(e, 0x00401000u + 5u * pick(64u));   /* some caller's return word */
        st32(e + 4u, k->id);
        st32(e + 8u, k->ign_word);
    }
}

static void stack_snapshot(uint32_t *to) { memcpy(to, s_stack, sizeof s_stack); }

static int stack_equal(const uint32_t *a, const uint32_t *b, const char *name,
                       const char *what)
{
    uint32_t i;
    for (i = 0; i < GUARD_WORDS + STACK_WORDS + ABOVE_WORDS; ++i)
        if (a[i] != b[i]) {
            failf(name, "%s: stack word %08x differs (ref %08x, native %08x)",
                  what, A(&s_stack[i]), b[i], a[i]);
            return 0;
        }
    return 1;
}

static void print_cpu(const char *tag, const CPU *c)
{
    printf("  %s eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
           "esi=%08x edi=%08x\n", tag, c->eax, c->ecx, c->edx, c->ebx,
           c->esp, c->ebp, c->esi, c->edi);
}

static int verdict_of(const IsaacVitaHasCollCounters *t)
{
    int r, v = -1;
    for (r = 0; r < ISAAC_VITA_HASCOLL_DECLINE_REASONS; ++r)
        if (t->declined[r])
            v = r;
    return v;
}

/* The world is already built: run the translated body and the wrapper. */
static void run_case(const hcase *k)
{
    IsaacVitaHasCollCounters t;
    unsigned cov_before[HC_COV_MARKERS], i, real_before, other_before;
    int outer;
    const uint32_t e_index = GUARD_WORDS + k->esp_offset / 4u;
#if defined(HC_ORACLE_CHAINED)
    IsaacVitaGfuelSeedCounters gf;
#endif

    ++s_cases;
    set_cpu(k);
    stack_snapshot(s_stack_snap);
    s_cpu_snap = s_cpu;
    (void)world_written();                /* reset the watch */

    if (!k->suppress) {
        s_native_mode = 0;
        real_sub_0033af30(&s_cpu);
        if (s_cpu.fault) {
            failf(k->name, "the translated body faulted: %s", s_cpu.fault);
            return;
        }
        if (world_written()) {
            failf(k->name, "the translated body wrote the heap or the image");
            return;
        }
        s_ref_true += (s_cpu.eax & 0xffu) != 0u;
    }
    stack_snapshot(s_stack_ref);
    s_cpu_ref = s_cpu;

    memcpy(s_stack, s_stack_snap, sizeof s_stack);
    s_cpu = s_cpu_snap;
    memcpy(cov_before, g_hc_cov, sizeof cov_before);
    isaac_vita_hascoll_native_take(NULL);
#if defined(HC_ORACLE_CHAINED)
    isaac_vita_gfuel_seed_take(NULL);
#endif
    real_before = s_real_calls;
    other_before = s_other_native;
    s_native_mode = 1;
    s_suppress_real = k->suppress;
    __wrap_sub_0033af30(&s_cpu);
    s_suppress_real = 0;
    s_native_mode = 0;
    isaac_vita_hascoll_native_take(&t);

    /* the counters: every entry (this one and the ones the translated body
     * made from "other units") is handled or declined once */
    {
        uint32_t declined = 0u, r;
        for (r = 0; r < ISAAC_VITA_HASCOLL_DECLINE_REASONS; ++r) {
            declined += t.declined[r];
            s_total.declined[r] += t.declined[r];
        }
        s_total.calls += t.calls;
        s_total.handled += t.handled;
        s_total.bodies += t.bodies;
        s_total.yes += t.yes;
        if (t.calls != 1u + (s_other_native - other_before) ||
                t.handled + declined != t.calls)
            failf(k->name, "counters: n=%u h=%u d=%u other=%u", t.calls,
                  t.handled, declined, s_other_native - other_before);
    }
    /* the outer verdict: __real_ ran iff the outer call declined (nested
     * entries that decline add their own __real_ calls) */
    outer = s_real_calls == real_before ? HC_HANDLED : -2;
    if (outer == HC_HANDLED) {
        if (t.calls != 1u || t.handled != 1u)
            failf(k->name, "handled without the handled counter");
        if (t.bodies < 1u || t.bodies > 2u)
            failf(k->name, "bodies=%u", t.bodies);
    } else {
        /* the first decline counted is the outer one when nothing nested */
        if (t.calls == 1u)
            outer = verdict_of(&t);
        else
            outer = -3;                   /* nested: reason of the outer unknown */
    }
#if defined(HC_ORACLE_CHAINED)
    isaac_vita_gfuel_seed_take(&gf);
    if (gf.has_calls != t.calls)
        failf(k->name, "chained: G FUEL wrapper saw %u entries, native %u",
              gf.has_calls, t.calls);
#endif
    if (k->expect != -1) {
        if (k->expect == HC_HANDLED && outer != HC_HANDLED)
            failf(k->name, "declined a replayable state (reason %d)", outer);
        else if (k->expect != HC_HANDLED && outer != k->expect &&
                 !(outer == -3 && s_real_calls != real_before))
            failf(k->name, "verdict %d, expected %d", outer, k->expect);
    }
    if (outer == HC_HANDLED) {
        ++s_handled;
        for (i = 0; i < HC_COV_MARKERS; ++i)
            s_cov_handled[i] += g_hc_cov[i] - cov_before[i];
    } else {
        ++s_declined;
        if (outer >= 0 && outer < ISAAC_VITA_HASCOLL_DECLINE_REASONS)
            ++s_reason_cases[outer];
    }
    if (world_written()) {
        failf(k->name, "the wrapper wrote the heap or the image");
        return;
    }

    if (k->suppress) {
        ++s_probes;
        if (s_real_suppressed == 0u || outer == HC_HANDLED)
            failf(k->name, "fault probe was not declined");
        if (memcmp(&s_cpu, &s_cpu_snap, sizeof s_cpu) != 0)
            failf(k->name, "fault probe touched the CPU");
        /* stack words: only the decline prefix below ESP, within the bound */
        for (i = 0; i < GUARD_WORDS + STACK_WORDS + ABOVE_WORDS; ++i) {
            const uint32_t at = A(&s_stack[i]);
            if (s_stack[i] != s_stack_snap[i] &&
                    (i >= e_index || at < s_cpu_snap.esp - HC_ORACLE_STACK_BYTES))
                failf(k->name, "fault probe wrote stack word %08x (entry %08x)",
                      at, s_cpu_snap.esp);
        }
        s_real_suppressed = 0u;
        return;
    }
    if (memcmp(&s_cpu, &s_cpu_ref, sizeof s_cpu) != 0) {
        failf(k->name, "CPU state differs (verdict %d)", outer);
        if (s_failures <= 40u) {
            print_cpu("ref", &s_cpu_ref);
            print_cpu("nat", &s_cpu);
        }
    }
    stack_equal(s_stack, s_stack_ref, k->name, "final");
    if (outer == HC_HANDLED) {
        /* deepest word the bodies wrote (the stack starts random) */
        for (i = 0; i < e_index; ++i)
            if (s_stack_ref[i] != s_stack_snap[i])
                break;
        if (i < e_index) {
            const uint32_t depth = s_cpu_snap.esp - A(&s_stack[i]);
            if (depth > s_max_depth)
                s_max_depth = depth;
            if (depth > HC_ORACLE_STACK_BYTES)
                failf(k->name, "wrote e-0x%x, below the declared bound", depth);
        }
    }
}

static void init_case(hcase *k, const char *name, uint32_t id)
{
    memset(k, 0, sizeof *k);
    snprintf(k->name, sizeof k->name, "%s", name);
    k->id = id;
    k->ign_word = 0u;
    k->esp_offset = 4u * (STACK_WORDS / 2u) + 4u * pick(2u);
    k->expect = -1;
}

/* ---- frozen bytes ------------------------------------------------------ */
static int hexbyte(const char *s)
{
    int v = 0, i;
    for (i = 0; i < 2; ++i) {
        char ch = s[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= ch - '0';
        else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
        else return -1;
    }
    return v;
}

/* argv[1]: the frozen PE bytes, in this order: 2ff0bc (8, two relocated
 * dwords), 2ff0c4 (200), 39cc68 (32, eight relocated dwords), 608ad0 (12),
 * 608b00 (12), 608b54 (12), 74d554 (24). */
#define FROZEN_BYTES (8u + 200u + 32u + 12u + 12u + 12u + 24u)
static uint32_t s_pred_table[2];

static int install_frozen(const char *hex)
{
    static uint8_t b[FROZEN_BYTES];
    unsigned i, o = 0;
    uint32_t w;
    if (strlen(hex) != 2u * FROZEN_BYTES)
        return 0;
    for (i = 0; i < FROZEN_BYTES; ++i) {
        int v = hexbyte(hex + 2u * i);
        if (v < 0)
            return 0;
        b[i] = (uint8_t)v;
    }
    for (i = 0; i < 2u; ++i, o += 4u) {
        memcpy(&w, b + o, 4u);
        s_pred_table[i] = w - 0x00400000u + GUEST_IMAGE_BASE;
        st32(VA(0x002ff0bcu) + 4u * i, s_pred_table[i]);
    }
    for (i = 0; i < 200u; ++i, ++o)
        st8(VA(0x002ff0c4u) + i, b[o]);
    for (i = 0; i < 8u; ++i, o += 4u) {
        memcpy(&w, b + o, 4u);
        st32(VA(0x0039cc68u) + 4u * i, w - 0x00400000u + GUEST_IMAGE_BASE);
    }
    for (i = 0; i < 12u; ++i, ++o)
        st8(VA(0x00608ad0u) + i, b[o]);
    for (i = 0; i < 12u; ++i, ++o)
        st8(VA(0x00608b00u) + i, b[o]);
    for (i = 0; i < 12u; ++i, ++o)
        st8(VA(0x00608b54u) + i, b[o]);
    for (i = 0; i < 24u; ++i, ++o)
        st8(VA(0x0074d554u) + i, b[o]);
    return o == FROZEN_BYTES;
}

static int commit_page(uint32_t base)
{
    void *r = VirtualAlloc((LPVOID)(uintptr_t)base, ORACLE_PAGE,
                           MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                           PAGE_READWRITE);
    return r != NULL && (uintptr_t)r == base;
}

/* An id whose item reaches the predicate's table jump (type 1, item id in
 * 0x1d..0xe4 with frozen byte b) */
static uint32_t pred_id_with_byte(uint8_t want)
{
    uint32_t v;
    for (v = 0x1du; v <= 0x1du + 0xc7u; ++v)
        if (ld8(VA(0x002ff0c4u) + v - 0x1du) == want)
            return v;
    return 0u;
}

/* A world where every early exit is closed: the path runs to the end. */
static void close_exits(uint32_t id)
{
    unsigned p, i;
    for (p = 0; p < 2u; ++p) {
        const uint32_t pl = P(p);
        st8(pl + 0x1f88u, 0u);
        st8(pl + 0x168u, 0u);
        st8(pl + 0x1f1cu, 0u);
        st32(pl + 0x1e80u, 0u);
        st32(pl + 0x1eb0u, 0u);
        st32(pl + 0x1ed0u, ld32(pl + 0x1eccu));
        for (i = 0; i < 4u; ++i)
            st32(pl + 0x14c4u + 0x1cu * i, 0x7fff0000u + i);
        st32(pl + 0x12e0u, 0x7ffe0000u);
        st32(pl + 0x12d8u, 0x7ffe0001u);
        st8(pl + 0x1450u + 0x11u, 0u);
        st32(pl + 0x1450u + 8u, ld32(pl + 0x1450u + 4u));
    }
    (void)id;
}

int main(int argc, char **argv)
{
    hcase k;
    unsigned i, round;

    if (argc < 2) {
        fprintf(stderr, "usage: oracle <hex of frozen bytes> [fast]\n");
        return 2;
    }
    /* a mutant that faults must exit, not wait on an error dialog */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    s_fail_fast = argc > 2 && strcmp(argv[2], "fast") == 0;
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        if (!commit_page(VA(s_pages[i]))) {
            printf("cannot commit image page %08x\n", VA(s_pages[i]));
            return 2;
        }
    W = (world *)VirtualAlloc(NULL, sizeof *W, MEM_RESERVE | MEM_COMMIT |
                              MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!W) {
        printf("cannot allocate the world\n");
        return 2;
    }
    if (!install_frozen(argv[1])) {
        printf("bad frozen bytes\n");
        return 2;
    }
    st32(VA(0x007fd65cu), A(W->game));
    st32(VA(0x007fd680u), A(W->config));

    /* 1. clean random worlds: no co-op, twin, special id or RNG byte, every
     *    pointer valid - the wrapper must answer every one natively */
    for (round = 0; round < 24000u; ++round) {
        uint32_t id;
        if (chance(30))
            id = k_special[pick(sizeof k_special / 4u)];
        else if (chance(15))
            id = k_pred_ids[pick(sizeof k_pred_ids / 4u)];
        else
            do id = rand_id(); while (declined_id(id));
        build_world(MODE_CLEAN, id);
        if (chance(20))
            close_exits(id);
        init_case(&k, "clean", id);
        k.ign_word = chance(12) ? 1u : chance(5) ? 0x100u :
                     chance(3) ? 0xffffff01u : 0u;
        k.esp_offset = HC_ORACLE_STACK_BYTES + 4u * pick(STACK_WORDS - 0x80u);
        k.expect = HC_HANDLED;
        run_case(&k);
    }
    /* 2. wild random worlds: co-op, twin, the declined ids, negative ids,
     *    RNG bytes; any verdict, same final state */
    for (round = 0; round < 24000u; ++round) {
        uint32_t id;
        if (chance(20))
            id = k_declined_ids[pick(sizeof k_declined_ids / 4u)];
        else if (chance(8))
            id = (uint32_t)-(int32_t)(1u + pick(TRINKETS + 4u));
        else if (chance(30))
            id = k_special[pick(sizeof k_special / 4u)];
        else
            id = rand_id();
        build_world(MODE_WILD, id);
        if ((int32_t)id < 0 && ~id < TRINKETS && chance(20)) {
            /* GetCollectible's negative arm: [[trinkets + ~id*4]], an item,
             * so the 0x1f88 gate reads a valid object */
            st8(P(0) + 0x1f88u, 1u);
            gate_items(id);
        }
        s_gtm_nest = chance(40);
        init_case(&k, "wild", id);
        k.ign_word = chance(12) ? 1u : 0u;
        k.esp_offset = 0x400u + 4u * pick(STACK_WORDS - 0x180u);
        run_case(&k);
    }
    s_gtm_nest = 0;

    /* 3. targeted declines the translated body runs */
    for (round = 0; round < 64u; ++round) {
        build_world(MODE_CLEAN, 0x2au);
        st32(P(0) + 0x2cu, 1u);
        st32(P(0) + 0x314u, P(1));
        st32(P(1) + 0x28u, 1u);
        init_case(&k, "decline-coop", 0x2au);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_COOP;
        run_case(&k);

        build_world(MODE_CLEAN, k_declined_ids[round % 5u]);
        st8(P(0) + 0x1f88u, 0u);          /* its ret0 comes before the id test */
        init_case(&k, "decline-id", k_declined_ids[round % 5u]);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_ID;
        run_case(&k);

        build_world(MODE_CLEAN, (uint32_t)-3);
        st8(P(0) + 0x1f88u, 0u);
        init_case(&k, "decline-negative", (uint32_t)-3);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_ID;
        run_case(&k);

        build_world(MODE_CLEAN, 0x40u + round);
        close_exits(0x40u + round);
        {
            tree seeds;
            tree_init(&seeds, W->seeds, SEED_NODES, A(W->game) + 0x1bb58u);
            tree_insert(&seeds, 79, 0);
        }
        st32(P(0) + 0x1ed0u, ld32(P(0) + 0x1eccu) + 0x100u);
        st8(ld32(P(0) + 0x1eccu) + 0x8fu, (uint8_t)(8u + round));
        init_case(&k, "decline-gfuel-byte", 0x40u + round);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
        run_case(&k);

        build_world(MODE_CLEAN, 0x41u);
        close_exits(0x41u);
        {
            tree seeds;
            tree_init(&seeds, W->seeds, SEED_NODES, A(W->game) + 0x1bb58u);
            for (i = 0; i < 70u; ++i)
                tree_insert(&seeds, (int32_t)i, 0);
        }
        init_case(&k, "decline-gfuel-walk", 0x41u);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
        run_case(&k);

        build_world(MODE_CLEAN, 0x42u);
        close_exits(0x42u);
        full_items();
        st8(P(0) + 0x168u, 1u);
        W->item_ptrs[0x42u] = A(W->items[0x42u]);
        st32(A(W->items[0x42u]) + 0xb8u, 0x40000000u);
        st32(P(0) + 0x1d84u, P(1));
        init_case(&k, "decline-twin", 0x42u);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_TWIN;
        run_case(&k);

        build_world(MODE_CLEAN, 0x200u);
        close_exits(0x200u);
        build_map(0, 0x200u, 1);
        init_case(&k, "decline-map-walk", 0x200u);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_WALK;
        run_case(&k);

        build_world(MODE_CLEAN, 0x201u);
        close_exits(0x201u);
        build_effects(0, 0x201u, 1);
        st8(P(0) + 0x1450u + 0x11u, 0u);
        init_case(&k, "decline-effect-walk", 0x201u);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_WALK;
        run_case(&k);

        /* the ESP bound: offset >= bound is replayed, below it declined */
        build_world(MODE_CLEAN, 0x43u);
        init_case(&k, "stack-short", 0x43u);
        k.esp_offset = HC_ORACLE_STACK_BYTES - 4u * (1u + round % 4u);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_STACK;
        run_case(&k);
        build_world(MODE_CLEAN, 0x43u);
        init_case(&k, "stack-exact", 0x43u);
        k.esp_offset = HC_ORACLE_STACK_BYTES;
        k.expect = HC_HANDLED;
        run_case(&k);
        build_world(MODE_CLEAN, 0x44u);
        init_case(&k, "stack-top", 0x44u);
        k.esp_offset = 4u * STACK_WORDS - 4u;
        k.expect = HC_HANDLED;
        run_case(&k);
        build_world(MODE_CLEAN, 0x44u);
        init_case(&k, "stack-ceiling", 0x44u);
        k.esp_offset = 4u * STACK_WORDS;
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_STACK;
        run_case(&k);
    }

    /* 4. fault probes: the translated body would dereference a null */
    for (round = 0; round < 16u; ++round) {
        const uint32_t id = 0x50u + round;
        build_world(MODE_CLEAN, id);
        st8(P(0) + 0x1f88u, 1u);
        full_items();
        W->item_ptrs[id] = 0u;
        init_case(&k, "probe-null-item-gate", id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_NULL;
        k.suppress = 1;
        run_case(&k);

        build_world(MODE_CLEAN, id);
        close_exits(id);
        st32(P(0) + 0x15f0u, 0u);
        init_case(&k, "probe-null-counts", id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_NULL;
        k.suppress = 1;
        run_case(&k);

        build_world(MODE_CLEAN, id);
        close_exits(id);
        build_effects(0, id, 0);
        st8(P(0) + 0x1450u + 0x11u, 0u);
        W->effects[0][0] = 0u;
        st32(P(0) + 0x1450u + 8u, A(W->effects[0]) + 12u);
        init_case(&k, "probe-null-effect", id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_NULL;
        k.suppress = 1;
        run_case(&k);

        build_world(MODE_CLEAN, id);
        close_exits(id);
        st32(P(0) + 0x1e80u, 1u);
        st32(P(0) + 0x1e7cu, 0u);
        init_case(&k, "probe-null-map-head", id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_NULL;
        k.suppress = 1;
        run_case(&k);

        build_world(MODE_CLEAN, id);
        close_exits(id);
        build_map(0, id, 0);
        {
            tree t;
            tree_init(&t, W->map[0], MAP_NODES, P(0) + 0x1e7cu);
            tree_insert(&t, (int32_t)id - 1, 1);
            W->map[0][1].left = W->map[0][1].right = 0u;
        }
        st32(P(0) + 0x1e80u, 1u);
        init_case(&k, "probe-null-map-child", id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_NULL;
        k.suppress = 1;
        run_case(&k);

        build_world(MODE_CLEAN, id);
        close_exits(id);
        W->seeds[0].parent = 0u;
        init_case(&k, "probe-null-seed-root", id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
        k.suppress = 1;
        run_case(&k);

        build_world(MODE_CLEAN, id);
        init_case(&k, "probe-null-game", id);
        st32(VA(0x007fd65cu), 0u);
        close_exits(id);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_GFUEL;
        k.suppress = 1;
        run_case(&k);
        st32(VA(0x007fd65cu), A(W->game));

        build_world(MODE_CLEAN, id);
        init_case(&k, "probe-null-config", id);
        st32(VA(0x007fd680u), 0u);
        k.expect = ISAAC_VITA_HASCOLL_DECLINE_NULL;
        k.suppress = 1;
        run_case(&k);
        st32(VA(0x007fd680u), A(W->config));

        /* an item-predicate target outside the frozen pair */
        {
            const uint8_t b = (uint8_t)(round & 1u);
            const uint32_t v = pred_id_with_byte(b);
            build_world(MODE_CLEAN, id);
            close_exits(id);
            full_items();
            W->item_ptrs[id] = A(W->items[id]);
            st32(A(W->items[id]), 1u);
            st32(A(W->items[id]) + 4u, v);
            st32(VA(0x002ff0bcu) + 4u * b, VA(0x002ff0a0u));
            init_case(&k, "probe-table-target", id);
            k.expect = ISAAC_VITA_HASCOLL_DECLINE_TABLE;
            k.suppress = 1;
            run_case(&k);
            st32(VA(0x002ff0bcu) + 4u * b, s_pred_table[b]);
            (void)world_written();
        }
    }

    {
        IsaacVitaHasCollCounters zero;
        unsigned hit = 0u, low = 0xffffffffu, r;
        isaac_vita_hascoll_native_take(&zero);
        if (zero.calls != 0u) {
            ++s_failures;
            printf("FAIL take: counters did not reset\n");
        }
        for (i = 0; i < HC_COV_MARKERS; ++i) {
            if (s_cov_handled[i])
                ++hit;
            else
                printf("FAIL coverage: marker %u never hit on a handled call\n", i),
                ++s_failures;
            if (s_cov_handled[i] < low)
                low = s_cov_handled[i];
        }
        printf("coverage (handled calls): %u/%u markers, rarest %u:", hit,
               (unsigned)HC_COV_MARKERS, low);
        for (i = 0; i < HC_COV_MARKERS; ++i)
            printf(" %u", s_cov_handled[i]);
        printf("\n");
        printf("verdicts: d(s,c,i,g,t,n,w,x) cases=");
        for (r = 0; r < ISAAC_VITA_HASCOLL_DECLINE_REASONS; ++r) {
            printf(r ? ",%u" : "%u", s_reason_cases[r]);
            if (!s_reason_cases[r]) {
                ++s_failures;
                printf(" <- FAIL never declined for this reason");
            }
        }
        printf("\n");
        printf("counters: n=%u h=%u b=%u y=%u d=%u,%u,%u,%u,%u,%u,%u,%u\n",
               s_total.calls, s_total.handled, s_total.bodies, s_total.yes,
               s_total.declined[0], s_total.declined[1], s_total.declined[2],
               s_total.declined[3], s_total.declined[4], s_total.declined[5],
               s_total.declined[6], s_total.declined[7]);
        printf("deepest handled write: e-0x%x (bound 0x%x)\n", s_max_depth,
               (unsigned)HC_ORACLE_STACK_BYTES);
    }
    if (s_failures) {
        printf("Vita HasCollectible oracle: FAIL; failures=%u\n", s_failures);
        return 1;
    }
    printf("Vita HasCollectible oracle: PASS; cases=%u; handled=%u; "
           "declined=%u; fault-probes=%u; true=%u; gtm=%u; logger=%u\n",
           s_cases, s_handled, s_declined, s_probes, s_ref_true, s_gtm_calls,
           s_logger_calls);
    return 0;
}
