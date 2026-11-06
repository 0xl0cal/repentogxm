/* Differential oracle of host_vita_gfuel_seed_native.c.
 *
 * test_vita_gfuel_seed_native.py extracts the translated sub_0039cb10,
 * sub_0039cb80 and sub_000206b0 from the frozen corpus and links them here
 * with sub_0039cb10/sub_0039cb80 renamed real_* (the --wrap view: the edge
 * 0x39cb8b inside the owner unit reaches the translated body).  Every case
 * builds one world - the Game pointer, a tree at Game+0x1bb58 (empty, a
 * random BST, a degenerate chain, hostile shapes), the player's byte vector,
 * a register file and an entry ESP - snapshots it, runs the translated body
 * and the wrapper on identical copies, and compares the whole CPU struct and
 * every byte of the world (stack with guard words, Game, nodes, player,
 * vector, image pages).  Null links are decline probes: the wrapper must
 * hand the untouched state to __real_ (whose translated body would fault).
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <stdarg.h>
#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_gfuel_seed_native_oracle requires a 32-bit host
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
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

/* ---- the translated bodies and the wrapper ---------------------------- */
void real_sub_0039cb10(CPU *__restrict c);
void real_sub_0039cb80(CPU *__restrict c);
void sub_000206b0(CPU *__restrict c);

void oracle_real_cb10(CPU *__restrict c);
void oracle_real_cb80(CPU *__restrict c);
#define __real_sub_0039cb10 oracle_real_cb10
#define __real_sub_0039cb80 oracle_real_cb80
#include "host_vita_gfuel_seed_native.c"
#undef __real_sub_0039cb10
#undef __real_sub_0039cb80

#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE)
/* The profiling seams' clock: +1 per read, so an empty bracket spans 1 and a
 * timed span 1 + whatever the stub body adds. */
static uint64_t s_clock;
uint64_t sceKernelGetProcessTimeWide(void)
{
    return ++s_clock;
}

/* Stand-ins for the wrapped callees.  Each wrapper must hand its stand-in
 * the CPU it was given (s_entry, checked on entry).  HasCollectible "runs"
 * s_has_delay clock steps, plays a body's first call (0033af4e call
 * 0x2fe5e0, after push ebp; mov ebp, esp) and, at its first depth, the call
 * s_nest selects; GetTrinketMultiplier runs s_gtm_delay steps and, under
 * NEST_GTM, calls HasCollectible back (0x38d6bd); sub_0039ba90 always does
 * (0x39baa7). */
enum { NEST_NONE, NEST_GTM, NEST_GTM_ELSEWHERE, NEST_JUDAS };
static uint32_t s_has_delay, s_gtm_delay, s_nest, s_has_stub_depth;
static uint32_t s_has_stub_calls, s_item_stub_calls, s_gtm_stub_calls, s_judas_stub_calls;
static CPU s_entry;
static int s_entry_bad;

static void stub_entered(const CPU *c)
{
    if (memcmp(c, &s_entry, sizeof *c) != 0)
        s_entry_bad = 1;
}

/* A call made from inside a stand-in: `ret` pushed 0x40 below its ESP. */
static void stub_call(CPU *c, uint32_t ret, void (*callee)(CPU *__restrict))
{
    const uint32_t e = c->esp;
    c->esp = e - 0x40u;
    st32(c->esp, ret);
    s_entry = *c;
    callee(c);
    c->esp = e;
}

void __real_sub_002fe5e0(CPU *__restrict c)
{
    ++s_item_stub_calls;
    stub_entered(c);
}

void __real_sub_0039ba90(CPU *__restrict c)
{
    ++s_judas_stub_calls;
    stub_entered(c);
    stub_call(c, 0x0039baacu, __wrap_sub_0033af30);
}

void __real_sub_0038d4c0(CPU *__restrict c)
{
    ++s_gtm_stub_calls;
    stub_entered(c);
    if (s_nest == NEST_GTM)
        stub_call(c, 0x0038d6c2u, __wrap_sub_0033af30);
    s_clock += s_gtm_delay;
}

void __real_sub_0033af30(CPU *__restrict c)
{
    const uint32_t e = c->esp, ebp = c->ebp;
    ++s_has_stub_calls;
    stub_entered(c);
    ++s_has_stub_depth;
    st32(e - 4u, ebp);
    c->ebp = e - 4u;
    c->esp = e - 0x30u;
    stub_call(c, GFUEL_RET_HAS_ITEM, __wrap_sub_002fe5e0);
    if (s_has_stub_depth == 1u) {
        if (s_nest == NEST_GTM)
            stub_call(c, s_gtm_stub_calls & 1u ? GFUEL_RET_HAS_GTM_A
                                               : GFUEL_RET_HAS_GTM_B,
                      __wrap_sub_0038d4c0);
        else if (s_nest == NEST_GTM_ELSEWHERE)
            stub_call(c, 0x00440000u, __wrap_sub_0038d4c0);
        else if (s_nest == NEST_JUDAS)
            stub_call(c, GFUEL_RET_HAS_JUDAS, __wrap_sub_0039ba90);
    }
    --s_has_stub_depth;
    c->esp = e;
    c->ebp = ebp;
    s_clock += s_has_delay;
}
#endif

/* Logger of the 39cb80 RNG path (cdecl, two words the caller pops). */
static unsigned s_logger_calls;
void sub_0055e330(CPU *__restrict c)
{
    ++s_logger_calls;
    c->eax = 0x0bad1065u;
    (void)gpop(c);
}

static unsigned s_declines, s_real_suppressed;
static int s_suppress_real;      /* decline probes: the body would fault */
/* Runs of the translated sub_0039cb10 (directly, or at 0x39cb8b inside the
 * translated sub_0039cb80): each enters sub_000206b0 once with return word
 * 0x39cb3c, which the PROFILE flavour's counter must report. */
static unsigned s_real_cb10_runs;

void oracle_real_cb10(CPU *__restrict c)
{
    ++s_declines;
    if (s_suppress_real) {
        ++s_real_suppressed;
        return;
    }
    ++s_real_cb10_runs;
    real_sub_0039cb10(c);
}

void oracle_real_cb80(CPU *__restrict c)
{
    ++s_declines;
    if (s_suppress_real) {
        ++s_real_suppressed;
        return;
    }
    ++s_real_cb10_runs;
    real_sub_0039cb80(c);
}

/* ---- the world ------------------------------------------------------- */
#define STACK_WORDS 1024u
#define GUARD_WORDS 64u
#define ABOVE_WORDS 16u
#define MAX_NODES 96u
#define GAME_BYTES 0x1bb80u
#define PLAYER_BYTES 0x1f00u
#define VECTOR_BYTES 0x400u

typedef struct node {            /* MSVC _Tree_node<int> */
    uint32_t left, parent, right;
    uint8_t color, isnil, pad[2];
    int32_t key;
} node;

static uint32_t s_stack[GUARD_WORDS + STACK_WORDS + ABOVE_WORDS];
static uint8_t s_game[GAME_BYTES];
static node s_nodes[MAX_NODES + 1u];       /* [0] is the head */
static uint8_t s_player[PLAYER_BYTES];
static uint8_t s_vector[VECTOR_BYTES];
static CPU s_cpu;

#define ORACLE_PAGE 0x10000u
static const uint32_t s_pages[] = {
    0x00390000u,   /* 39cc68 jump table */
    0x00600000u,   /* 608b54/608b5c words of the 39cb80 RNG path */
    0x007f0000u,   /* 7fd65c Game pointer */
};

typedef struct region { void *at; size_t bytes; } region;
static region s_regions[8];
static unsigned s_region_count;
static uint8_t *s_snap[8], *s_ref[8];
static CPU s_cpu_snap, s_cpu_ref;

static void add_region(void *at, size_t bytes)
{
    s_regions[s_region_count].at = at;
    s_regions[s_region_count].bytes = bytes;
    s_snap[s_region_count] = (uint8_t *)malloc(bytes);
    s_ref[s_region_count] = (uint8_t *)malloc(bytes);
    ++s_region_count;
}

static void snapshot(uint8_t **to, CPU *cpu_to)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        memcpy(to[i], s_regions[i].at, s_regions[i].bytes);
    *cpu_to = s_cpu;
}

static void restore(uint8_t **from, const CPU *cpu_from)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        memcpy(s_regions[i].at, from[i], s_regions[i].bytes);
    s_cpu = *cpu_from;
}

static uint32_t s_lcg = 0x2026101au;
static uint32_t lcg(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg >> 8;
}

static uint32_t A(const void *p) { return (uint32_t)(uintptr_t)p; }

/* ---- tree shapes ------------------------------------------------------ */
static unsigned s_used;

static void tree_reset(void)
{
    memset(s_nodes, 0, sizeof s_nodes);
    s_nodes[0].isnil = 1;
    s_nodes[0].left = s_nodes[0].right = s_nodes[0].parent = A(&s_nodes[0]);
    s_nodes[0].key = (int32_t)0xdeadbeef;
    s_used = 0;
    st32(A(s_game) + 0x1bb58u, A(&s_nodes[0]));
    st32(A(s_game) + 0x1bb5cu, 0u);
}

static uint32_t new_node(int32_t key)
{
    node *n = &s_nodes[++s_used];
    n->left = n->right = A(&s_nodes[0]);
    n->parent = A(&s_nodes[0]);
    n->color = (uint8_t)(lcg() & 1u);
    n->isnil = 0;
    n->key = key;
    st32(A(s_game) + 0x1bb5cu, s_used);
    return A(n);
}

/* Plain BST insert (shape random, order of keys as given). */
static void bst_insert(int32_t key)
{
    node *head = &s_nodes[0];
    uint32_t at, n;
    if (s_used >= MAX_NODES)
        return;
    if (head->parent == A(head)) {
        n = new_node(key);
        head->parent = head->left = head->right = n;
        return;
    }
    at = head->parent;
    for (;;) {
        node *p = (node *)(uintptr_t)at;
        uint32_t *link = key < p->key ? &p->left : &p->right;
        if (*link == A(head)) {
            n = new_node(key);
            ((node *)(uintptr_t)n)->parent = at;
            *link = n;
            return;
        }
        at = *link;
    }
}

/* ---- one case ---------------------------------------------------------- */
typedef struct gcase {
    const char *name;
    int which;                   /* 0 = sub_0039cb10, 1 = sub_0039cb80 */
    uint32_t esp_offset;         /* bytes above the floor */
    uint32_t vec_bytes;          /* end - begin (two's complement) */
    uint8_t byte;                /* vector[0x8f] */
    int self_null, game_null, head_null, root_null, child_null;
    int expect_decline;
} gcase;

static unsigned s_cases, s_failures, s_handled, s_suppressed_cases;
static unsigned s_present, s_walk_cases;

static void set_vector(const gcase *k)
{
    uint32_t begin = A(s_vector) + 0x100u;
    memset(s_vector, 0x5a, sizeof s_vector);
    s_vector[0x100u + 0x8fu] = k->byte;
    st32(A(s_player) + 0x1eccu, begin);
    st32(A(s_player) + 0x1ed0u, begin + k->vec_bytes);
}

static void set_cpu(const gcase *k)
{
    uint32_t floor = A(&s_stack[GUARD_WORDS]);
    guest_cpu_init(&s_cpu);
    if (guest_stack_bind(&s_cpu, floor, floor + 4u * STACK_WORDS)) {
        printf("stack bind failed\n");
        exit(2);
    }
    s_cpu.eax = lcg() | 1u;
    s_cpu.ebx = lcg();
    s_cpu.ecx = k->self_null ? 0u : A(s_player);
    s_cpu.edx = lcg();
    s_cpu.ebp = floor + 4u * STACK_WORDS - 0x40u;
    s_cpu.esi = lcg() ^ 0x5151u;
    s_cpu.edi = lcg() ^ 0xa7a7u;
    s_cpu.esp = floor + k->esp_offset;
    s_cpu.x[0].u64[0] = ((uint64_t)lcg() << 32) | lcg();
    st32(s_cpu.esp, 0x0033b1ddu);      /* the caller's return word */
}

static void fail(const gcase *k, const char *what)
{
    ++s_failures;
    printf("FAIL %s: %s\n", k->name, what);
}

static void run_case(const gcase *k)
{
    unsigned declines_before, suppressed_before, i;
    int declined;
    IsaacVitaGfuelSeedCounters counters_before;

    ++s_cases;
    set_vector(k);
    set_cpu(k);
    if (k->game_null)
        st32(VA(0x007fd65cu), 0u);
    else
        st32(VA(0x007fd65cu), A(s_game));
    if (k->head_null)
        st32(A(s_game) + 0x1bb58u, 0u);
    if (k->root_null)
        s_nodes[0].parent = 0u;
    if (k->child_null && s_used)
        s_nodes[1].left = s_nodes[1].right = 0u;
    if (s_used)
        ++s_walk_cases;
    snapshot(s_snap, &s_cpu_snap);

    /* reference: the translated body, unless it would fault */
    s_suppress_real = k->expect_decline == 2;
    if (!s_suppress_real) {
        unsigned d = s_declines;
        ++s_real_cb10_runs;
        if (k->which == 0)
            real_sub_0039cb10(&s_cpu);
        else
            real_sub_0039cb80(&s_cpu);
        s_declines = d;
    }
    snapshot(s_ref, &s_cpu_ref);

    restore(s_snap, &s_cpu_snap);
    declines_before = s_declines;
    suppressed_before = s_real_suppressed;
    counters_before = s_counters;
    if (k->which == 0)
        __wrap_sub_0039cb10(&s_cpu);
    else
        __wrap_sub_0039cb80(&s_cpu);
    declined = s_declines != declines_before;
    s_suppress_real = 0;

    if (k->expect_decline && !declined)
        fail(k, "wrapper did not decline");
    if (!k->expect_decline && declined)
        fail(k, "wrapper declined a replayable state");
    {
        /* exactly one verdict counter moves, and it is the right one */
        const uint32_t calls = k->which ? s_counters.mode_calls - counters_before.mode_calls
                                        : s_counters.query_calls - counters_before.query_calls;
        const uint32_t handled = k->which
            ? s_counters.mode_handled - counters_before.mode_handled
            : s_counters.query_handled - counters_before.query_handled;
        uint32_t moved = 0U, r, reason = ISAAC_VITA_GFUEL_DECLINE_REASONS;
        for (r = 0; r < ISAAC_VITA_GFUEL_DECLINE_REASONS; ++r)
            if (s_counters.declined[r] != counters_before.declined[r]) {
                moved += s_counters.declined[r] - counters_before.declined[r];
                reason = r;
            }
        if (calls != 1U || handled + moved != 1U || (declined != 0) != (moved == 1U))
            fail(k, "verdict counters disagree with the verdict");
        /* d(b) belongs to sub_0039cb80 alone, and never to a null-link
         * probe */
        if (reason == ISAAC_VITA_GFUEL_DECLINE_BYTE &&
                (k->which == 0 || k->expect_decline == 2))
            fail(k, "decline wrongly counted as d(b)");
    }
    if (declined)
        ;
    else
        ++s_handled;
    if (k->expect_decline == 2) {
        ++s_suppressed_cases;
        if (s_real_suppressed != suppressed_before + 1u)
            fail(k, "decline probe did not reach __real_ once");
        /* a decline leaves everything untouched */
        if (memcmp(&s_cpu, &s_cpu_snap, sizeof s_cpu) != 0)
            fail(k, "decline touched the CPU");
        for (i = 0; i < s_region_count; ++i)
            if (memcmp(s_regions[i].at, s_snap[i], s_regions[i].bytes) != 0)
                fail(k, "decline touched memory");
        return;
    }
    if (memcmp(&s_cpu, &s_cpu_ref, sizeof s_cpu) != 0) {
        fail(k, "CPU state differs");
        printf("  ref eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
               "esi=%08x edi=%08x\n  nat eax=%08x ecx=%08x edx=%08x ebx=%08x "
               "esp=%08x ebp=%08x esi=%08x edi=%08x\n",
               s_cpu_ref.eax, s_cpu_ref.ecx, s_cpu_ref.edx, s_cpu_ref.ebx,
               s_cpu_ref.esp, s_cpu_ref.ebp, s_cpu_ref.esi, s_cpu_ref.edi,
               s_cpu.eax, s_cpu.ecx, s_cpu.edx, s_cpu.ebx, s_cpu.esp,
               s_cpu.ebp, s_cpu.esi, s_cpu.edi);
    }
    for (i = 0; i < s_region_count; ++i) {
        if (memcmp(s_regions[i].at, s_ref[i], s_regions[i].bytes) != 0) {
            size_t b;
            for (b = 0; b < s_regions[i].bytes &&
                        ((uint8_t *)s_regions[i].at)[b] == s_ref[i][b]; ++b)
                ;
            printf("FAIL %s: region %u differs at +0x%x (%08x)\n", k->name, i,
                   (unsigned)b, A((uint8_t *)s_regions[i].at + b));
            ++s_failures;
        }
    }
    if (k->which == 0 && s_cpu_ref.eax != 0u)
        ++s_present;
}

static void init_case(gcase *k, const char *name, int which)
{
    memset(k, 0, sizeof *k);
    k->name = name;
    k->which = which;
    k->esp_offset = 4u * (STACK_WORDS / 2u);
    k->vec_bytes = 0x200u;
    k->byte = 0x03u;
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

/* argv[1]: 88 hex digits: the 32-byte jump table at 0x39cc68 as the PE
 * stores it (image base 0x400000; relocated here like the loader's HIGHLOW
 * fixups) and the 12 bytes at 0x608b54. */
static int install_frozen(const char *hex)
{
    uint8_t b[44];
    unsigned i;
    if (strlen(hex) != 88u)
        return 0;
    for (i = 0; i < 44u; ++i) {
        int v = hexbyte(hex + 2u * i);
        if (v < 0)
            return 0;
        b[i] = (uint8_t)v;
    }
    for (i = 0; i < 8u; ++i) {
        uint32_t w;
        memcpy(&w, b + 4u * i, 4u);
        st32(VA(0x0039cc68u) + 4u * i, w - 0x00400000u + GUEST_IMAGE_BASE);
    }
    for (i = 0; i < 12u; ++i)
        st8(VA(0x00608b54u) + i, b[32u + i]);
    return 1;
}

#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE)
static void profile_fail(const char *what)
{
    ++s_failures;
    printf("FAIL profile: %s\n", what);
}

/* One HasCollectible entry from the call site that pushed `ret`. */
static void has_call(uint32_t ret, uint32_t id, uint32_t flag_word,
                     uint32_t delay)
{
    st32(s_cpu.esp, ret);
    st32(s_cpu.esp + 4u, id);
    st32(s_cpu.esp + 8u, flag_word);
    s_has_delay = delay;
    s_entry = s_cpu;
    __wrap_sub_0033af30(&s_cpu);
}

/* An entry into a wrapped callee from the call site that pushed `ret`. */
static void wrapped_call(uint32_t ret, void (*wrapper)(CPU *__restrict))
{
    st32(s_cpu.esp, ret);
    s_entry = s_cpu;
    wrapper(&s_cpu);
}

static int world_matches_ref(void)
{
    unsigned i;
    if (memcmp(&s_cpu, &s_cpu_ref, sizeof s_cpu) != 0)
        return 0;
    for (i = 0; i < s_region_count; ++i)
        if (memcmp(s_regions[i].at, s_ref[i], s_regions[i].bytes) != 0)
            return 0;
    return 1;
}

/* The seams that only count and time: HasCollectible and the other
 * sub_000206b0 callers (ph120.gu/gl/gh). */
static void profile_selftest(void)
{
    IsaacVitaGfuelSeedCounters t;
    gcase k;
    uint32_t i, j, r, calls = 0u, ignore = 0u, negative = 0u, timed;

    init_case(&k, "profile", 0);
    set_cpu(&k);
    isaac_vita_gfuel_seed_take(NULL);
    /* twelve heavy call sites (50..61 entries) among 88 light ones (1..3);
     * ordinal 64 is a timed entry spanning exactly the 100-step outlier bound */
    for (i = 0u; i < 100u; ++i) {
        const uint32_t n = i < 12u ? 50u + i : 1u + i % 3u;
        for (j = 0u; j < n; ++j, ++calls) {
            /* 0x100: only the low byte of ignoreModifiers is the bool */
            const uint32_t flag = calls % 5u == 0u ? 1u :
                                  calls % 5u == 1u ? 0x100u : 0u;
            /* 0x7fffffff: the largest id that is not negative */
            const uint32_t id = calls % 7u == 0u ? 0xffffffffu :
                                calls % 7u == 1u ? 0x7fffffffu : 0x200u;
            ignore += flag == 1u;
            negative += id == 0xffffffffu;
            has_call(0x00400000u + 5u * i, id, flag, calls == 64u ? 99u : 7u);
        }
    }
    isaac_vita_gfuel_seed_take(&t);
    timed = (calls + 63u) / 64u;
    if (t.has_calls != calls || t.has_ignore != ignore ||
            t.has_negative != negative || s_has_stub_calls != calls ||
            s_entry_bad)
        profile_fail("HasCollectible entry counts");
    /* not nested: every entry is a top-level one and one body */
    if (t.has_nested != 0u || t.has_bodies != calls ||
            s_item_stub_calls != calls || t.gtm_calls != 0u ||
            t.body_sites.distinct != 100u ||
            t.body_sites.top[0].ret != t.has_sites.top[0].ret ||
            t.body_sites.top[0].n != t.has_sites.top[0].n)
        profile_fail("HasCollectible bodies");
    if (t.has_sites.distinct != 100u || t.has_sites.overflow != 0u)
        profile_fail("HasCollectible site table");
    for (r = 0u; r < ISAAC_VITA_GFUEL_TOP_SITES; ++r)
        if (t.has_sites.top[r].ret != 0x00400000u + 5u * (11u - r) ||
                t.has_sites.top[r].n != 61u - r)
            profile_fail("HasCollectible top sites");
    if (t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].n != timed ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].over != 1u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].us != 8u * (timed - 1u) ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_EMPTY].n != (calls + 31u) / 64u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_EMPTY].us !=
                t.timer[ISAAC_VITA_GFUEL_TIMER_EMPTY].n ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_QUERY].n != 0u ||
            t.lookup_other != 0u || t.query_calls != 0u)
        profile_fail("HasCollectible timer");
    printf("profile has: h(n,i,g)=%u,%u,%u us(h)=%u/%u/%u z=%u/%u n=%u "
           "top=%x:%u,%x:%u\n", t.has_calls, t.has_ignore, t.has_negative,
           t.timer[3].n, t.timer[3].us, t.timer[3].over, t.timer[4].n,
           t.timer[4].us, t.has_sites.distinct, t.has_sites.top[0].ret,
           t.has_sites.top[0].n, t.has_sites.top[9].ret, t.has_sites.top[9].n);

    /* more call sites than slots: the excess is counted, never lost */
    for (i = 0u; i < 3000u; ++i)
        has_call(0x00500000u + 8u * i, 1u, 0u, 0u);
    isaac_vita_gfuel_seed_take(&t);
    if (t.has_calls != 3000u || t.has_sites.distinct > 1024u ||
            t.has_sites.distinct <= 512u ||
            t.has_sites.distinct + t.has_sites.overflow != 3000u ||
            t.has_sites.top[0].n != 1u)
        profile_fail("site table overflow");
    printf("profile overflow: n=%u x=%u\n", t.has_sites.distinct,
           t.has_sites.overflow);

    /* Nesting A: 130 top-level entries from 0x00410000 whose body calls
     * GetTrinketMultiplier (0x33b006 / 0x33b0cb alternating), which calls
     * HasCollectible back (0x38d6c2), then 10 that do not.  Only the 140
     * top-level entries are timed (ordinals 0, 64, 128, each 1 + 3 + 5 + 3
     * steps with the nested body inside) or calibrated (32, 96). */
    s_nest = NEST_GTM;
    s_gtm_delay = 5u;
    s_item_stub_calls = s_has_stub_calls = s_gtm_stub_calls = 0u;
    for (i = 0u; i < 140u; ++i) {
        if (i == 130u)
            s_nest = NEST_NONE;
        has_call(0x00410000u, 0x200u, 0u, 3u);
    }
    isaac_vita_gfuel_seed_take(&t);
    if (t.has_calls != 270u || t.has_nested != 130u || t.has_bodies != 270u ||
            s_item_stub_calls != 270u || s_has_stub_calls != 270u ||
            t.gtm_calls != 130u || t.gtm_from_has != 130u || t.gtm_top != 0u ||
            s_gtm_stub_calls != 130u ||
            t.has_sites.distinct != 2u || t.has_sites.top[0].n != 140u ||
            t.has_sites.top[1].ret != 0x0038d6c2u || t.has_sites.top[1].n != 130u ||
            t.body_sites.distinct != 2u ||
            t.body_sites.top[0].ret != 0x00410000u || t.body_sites.top[0].n != 140u ||
            t.body_sites.top[1].ret != 0x0038d6c2u || t.body_sites.top[1].n != 130u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].n != 3u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].us != 36u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_EMPTY].n != 2u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_GTM].n != 0u || s_entry_bad)
        profile_fail("HasCollectible nested through GetTrinketMultiplier");
    printf("profile nest A: h(n,x,b)=%u,%u,%u t(n,h,p)=%u,%u,%u us(h)=%u/%u "
           "z=%u gb=%x:%u,%x:%u\n", t.has_calls, t.has_nested, t.has_bodies,
           t.gtm_calls, t.gtm_from_has, t.gtm_top, t.timer[3].n, t.timer[3].us,
           t.timer[4].n, t.body_sites.top[0].ret, t.body_sites.top[0].n,
           t.body_sites.top[1].ret, t.body_sites.top[1].n);

    /* Nesting B: 70 GetTrinketMultiplier entries outside HasCollectible
     * (timed at ordinals 0, 64: 1 + 5 steps; calibrated at 32); 5
     * HasCollectible entries whose body reaches it from elsewhere (counted,
     * not timed; the first top-level entry is timed: 1 + 3 + 5); 4 whose body
     * goes through sub_0039ba90 (0x33b12e), whose HasCollectible is nested;
     * 2 sub_0039ba90 entries from elsewhere, whose HasCollectible is not; 3
     * GetCollectible calls from other sites (no body). */
    s_item_stub_calls = s_has_stub_calls = s_gtm_stub_calls = 0u;
    for (i = 0u; i < 70u; ++i)
        wrapped_call(0x00420000u, __wrap_sub_0038d4c0);
    s_nest = NEST_GTM_ELSEWHERE;
    for (i = 0u; i < 5u; ++i)
        has_call(0x00430000u, 0x200u, 0u, 3u);
    s_nest = NEST_JUDAS;
    for (i = 0u; i < 4u; ++i)
        has_call(0x00450000u, 0x3bu, 0u, 3u);
    s_nest = NEST_NONE;
    for (i = 0u; i < 2u; ++i)
        wrapped_call(0x00470000u, __wrap_sub_0039ba90);
    for (i = 0u; i < 3u; ++i)
        wrapped_call(0x00460000u, __wrap_sub_002fe5e0);
    isaac_vita_gfuel_seed_take(&t);
    if (t.has_calls != 15u || t.has_nested != 4u || t.has_bodies != 15u ||
            s_item_stub_calls != 18u || s_judas_stub_calls != 6u ||
            t.gtm_calls != 75u || t.gtm_from_has != 0u || t.gtm_top != 70u ||
            s_gtm_stub_calls != 75u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_GTM].n != 2u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_GTM].us != 12u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].n != 1u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].us != 9u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_EMPTY].n != 1u ||
            t.body_sites.distinct != 3u ||
            t.body_sites.top[0].ret != 0x0039baacu || t.body_sites.top[0].n != 6u ||
            t.body_sites.top[1].ret != 0x00430000u || t.body_sites.top[1].n != 5u ||
            s_entry_bad)
        profile_fail("GetTrinketMultiplier and sub_0039ba90 depth");
    printf("profile nest B: h(n,x,b)=%u,%u,%u t(n,h,p)=%u,%u,%u us(t)=%u/%u "
           "us(h)=%u/%u z=%u gb=%x:%u,%x:%u\n", t.has_calls, t.has_nested,
           t.has_bodies, t.gtm_calls, t.gtm_from_has, t.gtm_top, t.timer[5].n,
           t.timer[5].us, t.timer[3].n, t.timer[3].us, t.timer[4].n,
           t.body_sites.top[0].ret, t.body_sites.top[0].n,
           t.body_sites.top[1].ret, t.body_sites.top[1].n);

    /* Nesting C: bodies entered from inside HasCollectible's own unit (no
     * wrapper; the stand-in is called directly): 3 whose GetTrinketMultiplier
     * and 2 whose sub_0039ba90 call HasCollectible back.  Only those two
     * wrappers know the inner entries are nested. */
    s_item_stub_calls = s_has_stub_calls = s_gtm_stub_calls = 0u;
    s_judas_stub_calls = 0u;
    s_nest = NEST_GTM;
    for (i = 0u; i < 3u; ++i)
        wrapped_call(0x00480000u, __real_sub_0033af30);
    s_nest = NEST_JUDAS;
    for (i = 0u; i < 2u; ++i)
        wrapped_call(0x00490000u, __real_sub_0033af30);
    s_nest = NEST_NONE;
    isaac_vita_gfuel_seed_take(&t);
    if (t.has_calls != 5u || t.has_nested != 5u || t.has_bodies != 10u ||
            t.gtm_calls != 3u || t.gtm_from_has != 3u || t.gtm_top != 0u ||
            s_judas_stub_calls != 2u || s_has_stub_calls != 10u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].n != 0u ||
            t.body_sites.distinct != 4u || s_entry_bad)
        profile_fail("nesting under a body entered inside its unit");
    printf("profile nest C: h(n,x,b)=%u,%u,%u t(n,h,p)=%u,%u,%u us(h)=%u/%u "
           "n=%u\n", t.has_calls, t.has_nested, t.has_bodies, t.gtm_calls,
           t.gtm_from_has, t.gtm_top, t.timer[3].n, t.timer[3].us,
           t.body_sites.distinct);

    /* sub_000206b0 from other call sites: the translated body runs exactly
     * as it would unwrapped (empty set, key 79 at player+0x20) */
    tree_reset();
    set_cpu(&k);
    s_cpu.ecx = A(s_game) + 0x1bb58u;
    st32(A(s_player) + 0x20u, 79u);
    for (i = 0u; i < 5u; ++i) {
        const uint32_t ret = i < 3u ? 0x00123456u :
                             i == 3u ? 0x00654321u : GFUEL_RET_LOOKUP;
        st32(s_cpu.esp, ret);
        st32(s_cpu.esp + 4u, A(s_player) + 0x40u);
        st32(s_cpu.esp + 8u, A(s_player) + 0x20u);
        snapshot(s_snap, &s_cpu_snap);
        __real_sub_000206b0(&s_cpu);
        snapshot(s_ref, &s_cpu_ref);
        restore(s_snap, &s_cpu_snap);
        __wrap_sub_000206b0(&s_cpu);
        if (!world_matches_ref())
            profile_fail("sub_000206b0 wrapper changed the result");
        restore(s_snap, &s_cpu_snap);
    }
    isaac_vita_gfuel_seed_take(&t);
    if (t.lookup_other != 4u || t.lookup_translated != 1u ||
            t.lookup_sites.distinct != 2u || t.lookup_sites.overflow != 0u ||
            t.lookup_sites.top[0].ret != 0x00123456u ||
            t.lookup_sites.top[0].n != 3u ||
            t.lookup_sites.top[1].ret != 0x00654321u ||
            t.lookup_sites.top[1].n != 1u || t.lookup_sites.top[2].n != 0u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_LOOKUP].n != 1u ||
            t.timer[ISAAC_VITA_GFUEL_TIMER_LOOKUP].us != 1u)
        profile_fail("sub_000206b0 call sites");
    printf("profile lookup: o=%u t=%u top=%x:%u,%x:%u us(o)=%u/%u/%u\n",
           t.lookup_other, t.lookup_translated, t.lookup_sites.top[0].ret,
           t.lookup_sites.top[0].n, t.lookup_sites.top[1].ret,
           t.lookup_sites.top[1].n, t.timer[2].n, t.timer[2].us,
           t.timer[2].over);
}
#endif

static int commit_page(uint32_t base)
{
    void *r = VirtualAlloc((LPVOID)(uintptr_t)base, ORACLE_PAGE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return r != NULL && (uintptr_t)r == base;
}

int main(int argc, char **argv)
{
    static const int32_t pool[] = { 10, 12, 13, 16, 23, 29, 30, 33, 41, 42,
                                    54, 55, 56, 63, 74, 75, 76, 77, 78, 79,
                                    80, 1, 0, -5, 0x7fffffff, (int32_t)0x80000000 };
    static const uint32_t vec_sizes[] = { 0x200u, 0x90u, 0x8fu, 0u, 1u,
                                          0xffffff00u, 0x80000000u, 0x7fffffffu };
    static char names[4096][48];
    unsigned name_index = 0, i, which, round;
    gcase k;

    if (argc != 2) {
        fprintf(stderr, "usage: oracle <88 hex digits of frozen bytes>\n");
        return 2;
    }
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        if (!commit_page(VA(s_pages[i]))) {
            fprintf(stderr, "could not commit %08x (/LARGEADDRESSAWARE)\n",
                    VA(s_pages[i]));
            return 2;
        }
    if (!install_frozen(argv[1])) {
        fprintf(stderr, "bad frozen-bytes argument\n");
        return 2;
    }
    memset(s_stack, 0xcd, sizeof s_stack);
    add_region(s_stack, sizeof s_stack);
    add_region(s_game, sizeof s_game);
    add_region(s_nodes, sizeof s_nodes);
    add_region(s_player, sizeof s_player);
    add_region(s_vector, sizeof s_vector);
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        add_region((void *)(uintptr_t)VA(s_pages[i]), ORACLE_PAGE);

#define NAME(...) (snprintf(names[name_index], sizeof names[0], __VA_ARGS__), \
                   names[name_index++ % 4096u])

    for (which = 0; which < 2u; ++which) {
        const char *fn = which ? "cb80" : "cb10";
        /* the measured world: no special seed */
        tree_reset();
        init_case(&k, NAME("%s-empty-set", fn), (int)which);
        run_case(&k);
        /* G FUEL present: every byte class, every vector length class */
        for (i = 0; i < 12u; ++i) {
            static const uint8_t bytes[12] = { 0, 1, 2, 3, 6, 7, 8, 9, 0x7f,
                                               0x80, 0xfe, 0xff };
            tree_reset();
            bst_insert(42); bst_insert(79); bst_insert(10); bst_insert(77);
            init_case(&k, NAME("%s-gfuel-byte-%02x", fn, bytes[i]), (int)which);
            k.byte = bytes[i];
            k.expect_decline = which && bytes[i] > 7u;
            run_case(&k);
        }
        for (i = 0; i < sizeof vec_sizes / sizeof vec_sizes[0]; ++i) {
            tree_reset();
            bst_insert(79);
            init_case(&k, NAME("%s-gfuel-vec-%08x", fn, vec_sizes[i]), (int)which);
            k.vec_bytes = vec_sizes[i];
            k.byte = 5u;
            run_case(&k);
        }
        /* lower_bound lands next to 79: 80 (absent), 78 (absent), INT_MIN */
        tree_reset();
        bst_insert(80); bst_insert(78); bst_insert(1);
        init_case(&k, NAME("%s-neighbours-only", fn), (int)which);
        run_case(&k);
        tree_reset();
        bst_insert((int32_t)0x80000000); bst_insert(0x7fffffff);
        init_case(&k, NAME("%s-extreme-keys", fn), (int)which);
        run_case(&k);
        /* hostile: the head is not marked nil and holds key 79, one node of
         * key 50 whose right link is a separate nil node, so lower_bound
         * stays at the head; only 0039cb4b cmp eax, edi rejects it */
        tree_reset();
        bst_insert(50);
        s_nodes[0].isnil = 0;
        s_nodes[0].key = 79;
        s_nodes[MAX_NODES].isnil = 1;
        s_nodes[MAX_NODES].key = (int32_t)0xdeadbeef;
        s_nodes[1].right = A(&s_nodes[MAX_NODES]);
        init_case(&k, NAME("%s-head-not-nil", fn), (int)which);
        run_case(&k);
        /* walk bound: a 64-chain is replayed, a 65-chain declines */
        tree_reset();
        for (i = 0; i < 64u; ++i)
            bst_insert((int32_t)i);
        init_case(&k, NAME("%s-chain-64", fn), (int)which);
        k.byte = 4u;
        run_case(&k);
        tree_reset();
        for (i = 0; i < 70u; ++i)
            bst_insert((int32_t)(200 - (int)i));
        init_case(&k, NAME("%s-chain-70-descending", fn), (int)which);
        k.expect_decline = 1;
        run_case(&k);
        /* stack: exactly enough, one word short (declines), at the ceiling */
        tree_reset();
        bst_insert(79);
        init_case(&k, NAME("%s-stack-exact", fn), (int)which);
        k.esp_offset = which ? 0x58u : 0x30u;
        run_case(&k);
        init_case(&k, NAME("%s-stack-short", fn), (int)which);
        k.esp_offset = which ? 0x54u : 0x2cu;
        k.expect_decline = 1;
        run_case(&k);
        init_case(&k, NAME("%s-stack-ceiling", fn), (int)which);
        k.esp_offset = 4u * STACK_WORDS - 4u;
        run_case(&k);
        init_case(&k, NAME("%s-stack-above-ceiling", fn), (int)which);
        k.esp_offset = 4u * STACK_WORDS;
        k.expect_decline = 1;
        run_case(&k);
        /* ESP alignment classes for 0039cb83 and esp,-8 */
        for (i = 0; i < 8u; ++i) {
            tree_reset();
            bst_insert(79); bst_insert(30);
            init_case(&k, NAME("%s-esp-mod8-%u", fn, i), (int)which);
            k.esp_offset = 4u * (STACK_WORDS / 2u) + i;
            run_case(&k);
        }
        /* decline probes (the translated body would fault) */
        tree_reset();
        init_case(&k, NAME("%s-null-game", fn), (int)which);
        k.game_null = 1; k.expect_decline = 2;
        run_case(&k);
        init_case(&k, NAME("%s-null-head", fn), (int)which);
        k.head_null = 1; k.expect_decline = 2;
        run_case(&k);
        init_case(&k, NAME("%s-null-root", fn), (int)which);
        k.root_null = 1; k.expect_decline = 2;
        run_case(&k);
        tree_reset();
        bst_insert(50);
        init_case(&k, NAME("%s-null-child", fn), (int)which);
        k.child_null = 1; k.expect_decline = 2;
        run_case(&k);
        tree_reset();
        bst_insert(79);
        init_case(&k, NAME("%s-null-player-gfuel", fn), (int)which);
        k.self_null = 1; k.expect_decline = 2;
        run_case(&k);
        /* the null player is never read without G FUEL */
        tree_reset();
        bst_insert(12);
        init_case(&k, NAME("%s-null-player-no-gfuel", fn), (int)which);
        k.self_null = 1;
        run_case(&k);
    }

    /* random worlds */
    for (round = 0; round < 4000u; ++round) {
        unsigned n = lcg() % 24u, j;
        tree_reset();
        for (j = 0; j < n; ++j)
            bst_insert(pool[lcg() % (sizeof pool / sizeof pool[0])]);
        init_case(&k, "random", (int)(lcg() & 1u));
        k.vec_bytes = vec_sizes[lcg() % (sizeof vec_sizes / sizeof vec_sizes[0])];
        k.byte = (uint8_t)(lcg() % 12u);
        k.esp_offset = 0x60u + 4u * (lcg() % (STACK_WORDS - 40u)) + (lcg() & 4u);
        {
            /* expected decline: only 39cb80 with G FUEL, a long vector and a
             * byte above 7 */
            int present = 0;
            for (j = 1; j <= s_used; ++j)
                if (s_nodes[j].key == 79)
                    present = 1;
            k.expect_decline = k.which && present &&
                               (int32_t)k.vec_bytes > 0x8f && k.byte > 7u;
        }
        run_case(&k);
    }

    {
        /* the window totals the phase profiler prints (ph120.gf) */
        IsaacVitaGfuelSeedCounters t, zero;
        uint32_t r, declined_total = 0U;
        isaac_vita_gfuel_seed_take(&t);
        isaac_vita_gfuel_seed_take(&zero);
        for (r = 0; r < ISAAC_VITA_GFUEL_DECLINE_REASONS; ++r)
            declined_total += t.declined[r];
        if (t.query_calls + t.mode_calls != s_cases ||
                t.query_handled + t.mode_handled != s_handled ||
                declined_total != s_declines ||
                t.lookup_linked != GFUEL_LOOKUP_LINKED ||
                zero.query_calls != 0U || zero.declined[0] != 0U) {
            ++s_failures;
            printf("FAIL counters: q=%u,%u m=%u,%u declined=%u\n", t.query_calls,
                   t.query_handled, t.mode_calls, t.mode_handled, declined_total);
        }
#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE)
        /* every run of the translated sub_0039cb10 entered sub_000206b0 once
         * with return word 0x39cb3c; nothing else calls it here */
        printf("lookup counter: t=%u o=%u (translated sub_0039cb10 runs %u)\n",
               t.lookup_translated, t.lookup_other, s_real_cb10_runs);
        if (t.lookup_translated != s_real_cb10_runs || t.lookup_other != 0U) {
            ++s_failures;
            printf("FAIL lookup counter\n");
        }
        /* entry ordinals 0, 64, ... of each wrapper are timed (span 1 on the
         * stub clock: no clock read nests), 32, 96, ... time an empty
         * bracket */
        {
            const IsaacVitaGfuelTimer *q = &t.timer[ISAAC_VITA_GFUEL_TIMER_QUERY];
            const IsaacVitaGfuelTimer *m = &t.timer[ISAAC_VITA_GFUEL_TIMER_MODE];
            const IsaacVitaGfuelTimer *z = &t.timer[ISAAC_VITA_GFUEL_TIMER_EMPTY];
            printf("wrapper timers: q=%u/%u/%u m=%u/%u/%u z=%u/%u/%u\n", q->n,
                   q->us, q->over, m->n, m->us, m->over, z->n, z->us, z->over);
            if (q->n != (t.query_calls + 63U) / 64U || q->us != q->n || q->over ||
                    m->n != (t.mode_calls + 63U) / 64U || m->us != m->n || m->over ||
                    z->n != (t.query_calls + 31U) / 64U + (t.mode_calls + 31U) / 64U ||
                    z->us != z->n || z->over ||
                    t.timer[ISAAC_VITA_GFUEL_TIMER_LOOKUP].n != 0U ||
                    t.timer[ISAAC_VITA_GFUEL_TIMER_HAS].n != 0U || t.has_calls) {
                ++s_failures;
                printf("FAIL wrapper timers\n");
            }
        }
        profile_selftest();
#else
        if (t.lookup_translated != 0U || t.lookup_other != 0U) {
            ++s_failures;
            printf("FAIL lookup counter moved without its --wrap\n");
        }
#endif
        if (!s_failures)
            printf("counters: q(n,h)=%u,%u m(n,h)=%u,%u w=%u p=%u "
                   "d(s,n,w,b)=%u,%u,%u,%u\n", t.query_calls, t.query_handled,
                   t.mode_calls, t.mode_handled, t.walked, t.present,
                   t.declined[0], t.declined[1], t.declined[2], t.declined[3]);
    }
    if (s_failures) {
        printf("Vita G FUEL seed oracle: FAIL; failures=%u\n", s_failures);
        return 1;
    }
    printf("Vita G FUEL seed oracle: PASS; cases=%u; handled=%u; declines=%u; "
           "decline-probes=%u; present=%u; walks=%u; logger=%u\n", s_cases,
           s_handled, s_declines, s_suppressed_cases, s_present, s_walk_cases,
           s_logger_calls);
    return 0;
}
