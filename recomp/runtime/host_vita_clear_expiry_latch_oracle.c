/* Differential oracle of RENDER60 S6 (host_vita_post_com.c):
 * ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH (leg 1) and
 * ISAAC_VITA_TIMEGETTIME_RECIPROCAL (leg 2), against the translated KAGE
 * Clear expiry loops.
 *
 * recomp/test_vita_clear_expiry_latch.py extracts sub_00564f50
 * (guest_0167.c) and sub_00561f40 (guest_0166.c) from the frozen corpus into
 * a bodies TU and compiles host_vita_post_com.c eight times, once per handler
 * shape, with its exported names renamed per shape (tag_indexed, tag_tgt,
 * tag_counters):
 *
 *   off       knobs OFF (the production handler)
 *   off_p     OFF + ISAAC_VITA_PHASE_PROFILE
 *   rec       TIMEGETTIME_RECIPROCAL
 *   obs       latch OBSERVE (1) + PHASE_PROFILE
 *   obs_rec   OBSERVE + PHASE_PROFILE + RECIPROCAL (measurement build A)
 *   on        latch ON (2)
 *   on_p      ON + PHASE_PROFILE
 *   on_p_rec  ON + PHASE_PROFILE + RECIPROCAL
 *
 * The bodies run on guest.c (included below) with the production import
 * route: guest_call -> guest_import_lookup -> guest_host_import_id (the
 * import-ID dispatch), or, in the GUEST_IMPORT_DIRECT flavour,
 * guest_import_call -> the validated direct table.  Both routes end in the
 * selected shape's isaac_vita_post_com_import_indexed(c, 4) -- the
 * host_vita_import_id_map.inc row 294 binding -- through oracle_tgt(), which
 * also records the call.  The guest callees are stand-ins that record a trace
 * and clobber eax/ecx/edx: glClearColor / glClear ([0x987bf9e4] /
 * [0x987c1f28], stdcall), sub_00561ff0 (thiscall, ret 4), the manager's
 * vt+0x70 (thiscall, ret 4), Image::FreeBatches vt+0x74 (thiscall, ret) and
 * sub_005ec358 = memmove (cdecl), done for real on guest memory.
 *
 * Clock model: oracle (b)'s reference clock is a function of callee
 * invocations.  k counts callee events (every stand-in
 * entry, every scripted perturbation); an epoch is the run of handler
 * entries made at one k, and i is an entry's index inside its epoch.  The
 * clock the handler reads is
 *     V(k, i) = B + G(k) + D(k, i),   D(k, 0) = 0, D non-decreasing in i,
 * in milliseconds (microseconds = 1000 V + jitter < 1000), and the reference
 * clock is W(k) = B + G(k) = V(k, 0): a clock that only advances when a
 * callee runs, i.e. the keep iterations take 0 ms.  Claims checked per
 * scenario, each byte for byte (trace with every returned eax, CPU struct,
 * guest stack, the world arena and the vector words):
 *
 *   (a) frozen clock (G = D = 0): ON == OFF == OBSERVE;
 *   (b) ON(V) == OFF(W) for every V of this form, perturbations included
 *       (the formal content of "timing-legal");
 *   (c) OBSERVE(V) == OFF(V) and RECIPROCAL(V) == OFF(V) for every V; and,
 *       without perturbations, OBSERVE's d == 0  <=>  ON(V) runs the same
 *       trajectory as OFF(V) (callee trace and final state, eax aside);
 *   (d) counting identities, per loop: with K keeps and E erases over n = K
 *       + E calls, fresh = 1 + E - [last call erased], hits = K - [last call
 *       kept]; ON reads the clock exactly at the fresh calls (checked call by
 *       call), OBSERVE at every call; the PHASE_PROFILE counters equal the
 *       oracle's own count of n, h, f, t, x, d, w and clk exactly.
 *
 * Key clauses: each scripted perturbation makes exactly one clause of the
 * latch key the only one that fails at a call that would otherwise be a hit
 * (begin word rewritten, end word grown by one element, a call from a second
 * CPU, a call from the other site, a call two elements back, a synthetic
 * arming at begin - 4 before the loop); the call must read the clock.
 * Mutants of every clause (test_vita_clear_expiry_latch.py) must fail.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <stdarg.h>
#include <windows.h>

/* The PHASE_PROFILE counter layout of host_vita_post_com.h. */
#define ISAAC_VITA_PHASE_PROFILE 1
#define ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE 2
#include "host_vita_post_com.h"
#undef ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE
#undef ISAAC_VITA_PHASE_PROFILE

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_clear_expiry_latch_oracle requires a 32-bit host
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))

/* Copied from the corpus listing comments they cite (the driver checks each
 * citation the same way as the runtime's TGT_LATCH_* defines). */
#define OR_SITE_A 0x00564FD6U /* 00564fd0  call dword ptr [0x986064c0] */
#define OR_SITE_B 0x00561F6BU /* 00561f65  call dword ptr [0x986064c0] */
#define OR_IAT_TGT 0x006064C0U /* 00564fd0  call dword ptr [0x986064c0] */
#define OR_VEC_BEGIN 0x007E7D1CU /* 00564fbd  mov esi, dword ptr [0x987e7d1c] */
#define OR_VEC_END 0x007E7D20U /* 00564fc3  cmp esi, dword ptr [0x987e7d20] */
#define OR_STAMP 0x74U /* 00564fd8  sub eax, dword ptr [ecx + 0x74] */
#define OR_LIMIT 0x1388U /* 00564fdb  cmp eax, 0x1388 */
#define OR_GL_CLEAR_COLOR 0x007BF9E4U /* 00564f7c  call dword ptr [0x987bf9e4] */
#define OR_GL_CLEAR 0x007C1F28U /* 00564f87  call dword ptr [0x987c1f28] */
#define OR_VT_PREPARE 0x70U /* 00564fba  call dword ptr [eax + 0x70] */
#define OR_VT_FREE 0x74U /* 00564fe4  call dword ptr [eax + 0x74] */
#define OR_MGR_FLAG 0x18U /* 00564f54  cmp dword ptr [esi + 0x18], 0 */
/* GUEST_IMPORT_CALL(_target, 0x006064c0U, 294U) at both sites, and
 * host_vita_import_id_map.inc row 294: ISAAC_VITA_IMPORT_POST_COM, local 4
 * (the driver pins both). */
#define OR_TGT_IMPORT_ID 294U
#define OR_TGT_LOCAL 4U

/* Stand-in entry points: RVAs outside the import table and the image's
 * translated range the bodies reference. */
#define RVA_STUB_GL_CLEAR_COLOR 0x00700010U
#define RVA_STUB_GL_CLEAR 0x00700020U
#define RVA_STUB_VT70 0x00700030U
#define RVA_STUB_FREE 0x00700040U

#define CHECK(x) do { if (!(x)) fail_check(#x, __LINE__); } while (0)

static uint32_t s_scenario_seed;
static const char *s_run_name = "setup";

static void fail_check(const char *what, int line)
{
    printf("oracle check failed at line %d (scenario seed 0x%08x, run %s): "
           "%s\n", line, (unsigned)s_scenario_seed, s_run_name, what);
    fflush(stdout);
    exit(1);
}

/* ---- the frozen import contract (host_vita_import_id_map.inc) ---------- */
typedef struct oracle_import_row {
    uint32_t id, slot_rva;
    const char *name;
    int kind;
    uint32_t local_index;
} oracle_import_row;
#define ISAAC_VITA_IMPORT_ID_ROW(id, slot_rva, name, kind, local_index) \
    { (id), (slot_rva), (name), (kind), (local_index) },
static const oracle_import_row s_contract[] = {
#include "host_vita_import_id_map.inc"
};
#undef ISAAC_VITA_IMPORT_ID_ROW
_Static_assert(sizeof s_contract / sizeof s_contract[0] ==
                   ISAAC_VITA_IMPORT_ID_COUNT,
               "import contract row count");

/* ---- stubs owned by other Vita runtime units (the hascoll oracle's) ---- */
unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
int g_isaac_vita_import_ids_ready = 1;
static unsigned s_route_generic, s_route_direct;
static int oracle_tgt(CPU *__restrict c);

/* The registration contract of host_vita_import_id.c: every row's slot and
 * name, in order. */
int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    uint32_t i;

    if (count != ISAAC_VITA_IMPORT_ID_COUNT)
        return 0;
    for (i = 0U; i < count; ++i)
        if (s_contract[i].id != i ||
            imports[i].slot_rva != s_contract[i].slot_rva ||
            strcmp(imports[i].name, s_contract[i].name) != 0)
            return 0;
    return 1;
}
/* host_vita_import_id.c dispatches a POST_COM row to
 * isaac_vita_post_com_import_indexed(c, local, ...); only the timeGetTime
 * row can be reached here (any other ID is a test failure). */
int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    if (import_id != OR_TGT_IMPORT_ID ||
        s_contract[import_id].kind != ISAAC_VITA_IMPORT_POST_COM ||
        s_contract[import_id].local_index != OR_TGT_LOCAL)
        return 0;
    ++s_route_generic;
    return oracle_tgt(c);
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
#if defined(ISAAC_VITA_IMPORT_DIRECT)
static int oracle_direct_family(CPU *__restrict c, uint32_t local_index,
                                unsigned *call_count)
{
    CHECK(local_index == OR_TGT_LOCAL);
    if (call_count)
        ++*call_count;
    ++s_route_direct;
    return oracle_tgt(c);
}
int guest_host_import_id_direct_binding(uint32_t import_id,
                                        guest_import_family_fn *fn,
                                        uint32_t *local_index)
{
    if (import_id != OR_TGT_IMPORT_ID ||
        s_contract[import_id].kind != ISAAC_VITA_IMPORT_POST_COM)
        return 0;
    *fn = oracle_direct_family;
    *local_index = s_contract[import_id].local_index;
    return 1;
}
#endif

/* ---- platform.c stand-ins seen by host_vita_post_com.c ---------------- */
static uint64_t s_now_us;
static unsigned s_clock_reads;

uint64_t isaac_vita_get_process_time(void)
{
    ++s_clock_reads;
    return s_now_us;
}

#define LOG_LINES_MAX 64
static char s_log[LOG_LINES_MAX][192];
static unsigned s_log_lines;

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    CHECK(s_log_lines < LOG_LINES_MAX);
    va_start(arguments, format);
    vsnprintf(s_log[s_log_lines], sizeof s_log[0], format, arguments);
    va_end(arguments);
    ++s_log_lines;
}

/* ---- the eight handler shapes ------------------------------------------ */
typedef int (*indexed_fn)(CPU *__restrict, uint32_t, unsigned *);
typedef const char *(*name_fn)(uint32_t);
#define DECLARE_SHAPE(tag) \
    int tag##_indexed(CPU *__restrict c, uint32_t index, unsigned *count); \
    const char *tag##_name(uint32_t index);
DECLARE_SHAPE(off)
DECLARE_SHAPE(off_p)
DECLARE_SHAPE(rec)
DECLARE_SHAPE(obs)
DECLARE_SHAPE(obs_rec)
DECLARE_SHAPE(on)
DECLARE_SHAPE(on_p)
DECLARE_SHAPE(on_p_rec)
extern uint32_t off_p_tgt, obs_tgt, obs_rec_tgt, on_p_tgt, on_p_rec_tgt;
extern IsaacVitaClearLatchCounters obs_counters, obs_rec_counters,
    on_p_counters, on_p_rec_counters;

typedef struct shape {
    const char *tag;
    indexed_fn indexed;
    name_fn name;
    int latch;                 /* 0 OFF, 1 OBSERVE, 2 ON */
    int reciprocal;
    uint32_t *tgt;             /* PHASE_PROFILE tgt counter or NULL */
    IsaacVitaClearLatchCounters *counters;
} shape;

enum { S_OFF, S_OFF_P, S_REC, S_OBS, S_OBS_REC, S_ON, S_ON_P, S_ON_P_REC,
       SHAPES };
#define SHAPE(tag, latch, rec, tgt, counters) \
    { #tag, tag##_indexed, tag##_name, latch, rec, tgt, counters }
static const shape s_shapes[SHAPES] = {
    SHAPE(off, 0, 0, NULL, NULL),
    SHAPE(off_p, 0, 0, &off_p_tgt, NULL),
    SHAPE(rec, 0, 1, NULL, NULL),
    SHAPE(obs, 1, 0, &obs_tgt, &obs_counters),
    SHAPE(obs_rec, 1, 1, &obs_rec_tgt, &obs_rec_counters),
    SHAPE(on, 2, 0, NULL, NULL),
    SHAPE(on_p, 2, 0, &on_p_tgt, &on_p_counters),
    SHAPE(on_p_rec, 2, 1, &on_p_rec_tgt, &on_p_rec_counters),
};
#undef SHAPE
static const shape *s_shape;
/* The CPU of each shape's previous site call: the handler's latch cpu is
 * always that CPU (every site call either re-arms with its own CPU or is a
 * hit, which needs the same CPU), so `foreign_cpu` counts the site calls
 * whose CPU differs from it. */
static const CPU *s_prev_site_cpu[SHAPES];
static uint32_t s_exp_foreign;

static void note_site_cpu(const CPU *c)
{
    const CPU **prev = &s_prev_site_cpu[s_shape - s_shapes];

    if (*prev && *prev != c)
        ++s_exp_foreign;
    *prev = c;
}

/* ---- deterministic randomness ------------------------------------------ */
static uint32_t mix3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1U ^ (b + 0x7F4A7C15U) * 0x85EBCA77U ^
                 (c + 0x165667B1U) * 0xC2B2AE3DU;

    h ^= h >> 15;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12;
    h *= 0x297A2D39U;
    h ^= h >> 15;
    return h;
}

static uint32_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static uint32_t pick(uint32_t n) { return n ? rnd() % n : 0U; }

/* ---- the scripted clock ------------------------------------------------- */
static uint64_t s_ms_base, s_ms_events, s_ms_drift;
static uint32_t s_k, s_epoch_k, s_epoch_i, s_entries;
static uint32_t s_clock_seed, s_event_max, s_tick_permille;
static int s_drift_on;           /* V (1) or W (0) */

static void clock_reset(uint64_t base, uint32_t seed, uint32_t event_max,
                        uint32_t tick_permille, int drift)
{
    s_ms_base = base;
    s_ms_events = s_ms_drift = 0U;
    s_k = 0U;
    s_epoch_k = UINT32_MAX;
    s_epoch_i = 0U;
    s_entries = 0U;
    s_clock_seed = seed;
    s_event_max = event_max;
    s_tick_permille = tick_permille;
    s_drift_on = drift;
}

/* A callee ran (or a perturbation happened): the clock may advance. */
static void clock_event(int forced)
{
    uint32_t inc = mix3(s_clock_seed, s_k, 0x0e7e47U) % (s_event_max + 1U);

    if (forced && inc == 0U)
        inc = 1U;
    s_ms_events += inc;
    ++s_k;
}

/* A handler entry: set the value its clock reads will see. */
static void clock_entry(void)
{
    uint64_t ms;

    if (s_epoch_k != s_k) {
        s_epoch_k = s_k;
        s_epoch_i = 0U;
        s_ms_drift = 0U;
    } else {
        ++s_epoch_i;
        if (s_drift_on &&
            mix3(s_clock_seed ^ 0x51U, s_k, s_epoch_i) % 1000U <
                s_tick_permille)
            s_ms_drift += 1U + (mix3(s_clock_seed, s_k, s_epoch_i) & 1U);
    }
    ms = s_ms_base + s_ms_events + s_ms_drift;
    s_now_us = ms * 1000U + mix3(s_clock_seed ^ 0xa5U, s_k, s_epoch_i) % 1000U;
    ++s_entries;
}

/* ---- trace --------------------------------------------------------------- */
enum { T_TGT = 1, T_SYN, T_GL_CC, T_GL_CLEAR, T_PREPARE, T_VT70, T_FREE,
       T_MOVE, T_NEST };
typedef struct trace_rec { uint32_t kind, a, b, c, d; } trace_rec;
#define TRACE_MAX 8192U
static trace_rec s_trace[TRACE_MAX], s_trace_v[TRACE_MAX], s_trace_w[TRACE_MAX];
static uint32_t s_trace_n, s_trace_v_n, s_trace_w_n;

static void trace_add(uint32_t kind, uint32_t a, uint32_t b, uint32_t c,
                      uint32_t d)
{
    CHECK(s_trace_n < TRACE_MAX);
    s_trace[s_trace_n].kind = kind;
    s_trace[s_trace_n].a = a;
    s_trace[s_trace_n].b = b;
    s_trace[s_trace_n].c = c;
    s_trace[s_trace_n].d = d;
    ++s_trace_n;
}

/* ---- the world ------------------------------------------------------------ */
#define ARENA_BYTES 0x10000U
#define IMG_COUNT 256U
#define IMG_BASE 0x1000U
#define IMG_STRIDE 0x80U
#define OUTER_STORAGE 0x9000U
#define INNER_STORAGE 0xA000U
#define STORAGE_SLOTS 512U
#define SPARE_SLOTS 8U
#define STACK_WORDS 0x1000U
#define GUARD_WORDS 0x40U
static uint8_t *s_arena;
static uint32_t s_stack[GUARD_WORDS + STACK_WORDS + GUARD_WORDS];
static uint32_t s_stack2[GUARD_WORDS + 0x400U + GUARD_WORDS];
static CPU s_cpu, s_cpu2;
static const uint32_t s_pages[] = { 0x00600000U, 0x007B0000U, 0x007C0000U,
                                    0x007E0000U };

static uint32_t A(uint32_t offset) { return (uint32_t)(uintptr_t)s_arena + offset; }
static uint32_t IMG(uint32_t i) { return A(IMG_BASE + IMG_STRIDE * i); }
static uint32_t MGR(int inner) { return A(inner ? 0x80U : 0U); }

enum { P_NONE, P_BEGIN, P_END, P_FOREIGN, P_SITE, P_ESI, P_FIRST, P_KINDS };
static const char *const k_perturb_names[P_KINDS] = {
    "none", "begin", "end", "foreign", "site", "esi", "first" };
enum { ST_MIX, ST_KEEP, ST_EXPIRE, ST_BOUNDARY, ST_ALTERNATE, ST_ONE,
       ST_AHEAD };

typedef struct scen {
    uint32_t seed;
    uint32_t n, inner_n, begin_off, inner_body;
    uint32_t bodies;          /* 1 A, 2 B, 3 A then B, 4 B then A, 5 A A */
    uint32_t stamp_mode, one_at, duplicates;
    uint64_t base_ms;
    uint32_t event_max, tick_permille;
    uint32_t perturb, perturb_at, nested_at;
    int frozen;
} scen;

static uint32_t stamp_for(const scen *s, uint32_t now0, uint32_t index)
{
    int32_t delta;
    uint32_t mode = s->stamp_mode;

    if (mode == ST_MIX) {
        const uint32_t r = pick(100U);
        mode = r < 45U ? ST_KEEP : r < 70U ? ST_EXPIRE : ST_BOUNDARY;
    }
    switch (mode) {
    case ST_KEEP: delta = 50 + (int32_t)pick(3950U); break;
    case ST_EXPIRE: delta = -10 - (int32_t)pick(100000U); break;
    case ST_BOUNDARY: delta = -3 + (int32_t)pick(16U); break;
    case ST_ALTERNATE: delta = index & 1U ? -10 - (int32_t)pick(50U)
                                          : 50 + (int32_t)pick(500U); break;
    case ST_ONE: delta = index == s->one_at ? -10 - (int32_t)pick(50U)
                                             : 50 + (int32_t)pick(500U); break;
    default: /* ST_AHEAD: newer than the clock by 1..20 ms, or boundary */
        delta = pick(3U) ? 5001 + (int32_t)pick(20U) : -2 + (int32_t)pick(8U);
        break;
    }
    return now0 - 5000U + (uint32_t)delta;
}

static void fill_vector(uint32_t storage, uint32_t begin_off, uint32_t n,
                        uint32_t first_image, uint32_t images, int duplicates)
{
    uint32_t i;

    for (i = 0U; i < STORAGE_SLOTS; ++i) {
        uint32_t image = first_image + (duplicates ? pick(images)
                                                   : (i % images));
        st32(A(storage) + 4U * i, IMG(image));
    }
    /* distinct images inside [begin, end) unless duplicates */
    if (!duplicates)
        for (i = 0U; i < n; ++i)
            st32(A(storage) + 4U * (begin_off + i), IMG(first_image + i));
}

static void build_world(const scen *s)
{
    uint32_t i, floor, ceiling;
    const uint32_t now0 = (uint32_t)s->base_ms;

    s_rng = s->seed | 1U;
    for (i = 0U; i < ARENA_BYTES / 4U; ++i)
        ((uint32_t *)s_arena)[i] = rnd();
    /* managers: vtable at 0x100 (vt+0x70 = the prepare stand-in) */
    for (i = 0U; i < 2U; ++i) {
        st32(MGR((int)i), A(0x100U));
        st32(MGR((int)i) + OR_MGR_FLAG, pick(2U) ? rnd() | 1U : 0U);
    }
    st32(A(0x100U) + OR_VT_PREPARE, VA(RVA_STUB_VT70));
    st32(A(0x200U) + OR_VT_FREE, VA(RVA_STUB_FREE));
    for (i = 0U; i < IMG_COUNT; ++i) {
        st32(IMG(i), A(0x200U));
        st32(IMG(i) + OR_STAMP, stamp_for(s, now0, i < 128U ? i : i - 128U));
    }
    /* outer vector: images 0..127, inner vector: images 128..255 */
    fill_vector(OUTER_STORAGE, s->begin_off, s->n, 0U, 128U, (int)s->duplicates);
    fill_vector(INNER_STORAGE, 1U, s->inner_n, 128U, 128U, 0);
    for (i = 0x00600000U; i < 0x00610000U; i += 0x1000U)
        memset((void *)(uintptr_t)VA(i), 0, 0x1000U);
    memset((void *)(uintptr_t)VA(0x007E7000U), 0, 0x1000U);
    st32(VA(OR_VEC_BEGIN), A(OUTER_STORAGE) + 4U * s->begin_off);
    st32(VA(OR_VEC_END), A(OUTER_STORAGE) + 4U * (s->begin_off + s->n));
    st32(VA(OR_VEC_END) + 4U, A(OUTER_STORAGE) + 4U * STORAGE_SLOTS);
    st32(VA(OR_GL_CLEAR_COLOR), VA(RVA_STUB_GL_CLEAR_COLOR));
    st32(VA(OR_GL_CLEAR), VA(RVA_STUB_GL_CLEAR));
    st32(VA(OR_IAT_TGT), VA(OR_IAT_TGT));   /* the slot token = its VA */

    for (i = 0U; i < sizeof s_stack / 4U; ++i)
        s_stack[i] = rnd();
    for (i = 0U; i < sizeof s_stack2 / 4U; ++i)
        s_stack2[i] = rnd();
    guest_cpu_init(&s_cpu);
    floor = (uint32_t)(uintptr_t)&s_stack[GUARD_WORDS];
    ceiling = floor + 4U * STACK_WORDS;
    CHECK(guest_stack_bind(&s_cpu, floor, ceiling) == 0);
    s_cpu.eax = rnd();
    s_cpu.ebx = rnd();
    s_cpu.edx = rnd();
    s_cpu.ebp = ceiling - 0x40U;
    s_cpu.esi = rnd();
    s_cpu.edi = rnd();
    s_cpu.esp = ceiling - 0x200U;
    guest_cpu_init(&s_cpu2);
    floor = (uint32_t)(uintptr_t)&s_stack2[GUARD_WORDS];
    CHECK(guest_stack_bind(&s_cpu2, floor, floor + 4U * 0x400U) == 0);
    s_cpu2.esp = floor + 4U * 0x400U - 0x100U;
}

static uint64_t state_hash(void)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    const uint32_t *w;
    uint32_t i;

#define HASH_WORDS(p, n) \
    for (w = (const uint32_t *)(p), i = 0U; i < (n); ++i) \
        h = (h ^ w[i]) * UINT64_C(0x100000001b3)
    HASH_WORDS(s_arena, ARENA_BYTES / 4U);
    HASH_WORDS(s_stack, sizeof s_stack / 4U);
    HASH_WORDS(s_stack2, sizeof s_stack2 / 4U);
    HASH_WORDS(&s_cpu, sizeof s_cpu / 4U);
    HASH_WORDS(&s_cpu2, sizeof s_cpu2 / 4U);
    HASH_WORDS((const void *)(uintptr_t)VA(0x007E7000U), 0x1000U / 4U);
    HASH_WORDS((const void *)(uintptr_t)VA(0x00606000U), 0x1000U / 4U);
#undef HASH_WORDS
    return h;
}

/* ---- per-loop bookkeeping (the oracle's own model of the latch) -------- */
typedef struct loop_state {
    uint32_t n, keeps, erases, last_erase, arm_ms;
} loop_state;
#define LOOP_DEPTH 4U
static loop_state s_loops[LOOP_DEPTH];
static uint32_t s_depth;
static uint32_t s_expect_free;          /* image an erase decision frees */
static const scen *s_scen;
static int s_perturb_done;
static uint32_t s_site_calls;           /* real site calls this run */
static uint32_t s_syn_calls;            /* scripted site calls this run */
static uint32_t s_free_calls;
/* expected counters (unperturbed runs) */
static uint32_t s_exp_fresh, s_exp_hits, s_exp_ticks, s_exp_flips,
    s_exp_ahead, s_exp_expiries, s_exp_on_ahead;
static unsigned s_perturb_fresh[P_KINDS];

static void loop_push(void)
{
    CHECK(s_depth < LOOP_DEPTH);
    memset(&s_loops[s_depth], 0, sizeof s_loops[0]);
    ++s_depth;
}

static void loop_pop(void)
{
    const loop_state *l;

    CHECK(s_depth > 0U);
    l = &s_loops[--s_depth];
    if (l->n) {
        s_exp_fresh += 1U + l->erases - l->last_erase;
        s_exp_hits += l->keeps - (l->last_erase ? 0U : 1U);
    }
}

/* One handler call on c with return word `word` and element pointer `esi`
 * made by the oracle itself (a scripted perturbation), bracketed by forced
 * clock events so it and the next real call each start an epoch. */
static void synthetic_call(CPU *c, uint32_t word, uint32_t esi)
{
    CPU saved;
    uint32_t sp;

    clock_event(1);
    memcpy(&saved, c, sizeof saved);
    sp = c->esp - 0x40U;
    st32(sp, word);
    c->esp = sp;
    c->esi = esi;
    clock_entry();
    note_site_cpu(c);
    CHECK(s_shape->indexed(c, OR_TGT_LOCAL, NULL) == 1);
    CHECK(c->esp == sp + 4U);
    trace_add(T_SYN, word, esi, c->eax, (uint32_t)(c == &s_cpu2));
    ++s_syn_calls;
    memcpy(c, &saved, sizeof saved);
    clock_event(1);
}

static int site_word(uint32_t word)
{
    return word == OR_SITE_A || word == OR_SITE_B;
}

static int oracle_tgt(CPU *__restrict c)
{
    const uint32_t word = ld32(c->esp);
    unsigned reads_before;
    loop_state *l = s_depth ? &s_loops[s_depth - 1U] : NULL;
    int perturbed = 0, eligible, erase, fresh;
    uint32_t esi, stamp, ms;

    CHECK(site_word(word) && l != NULL);
    CHECK(s_expect_free == 0U);        /* the previous erase freed its image */
    /* a scripted perturbation, once, at a call that would otherwise hit */
    if (s_scen->perturb != P_NONE && s_scen->perturb != P_FIRST &&
        !s_perturb_done && s_depth == 1U && s_site_calls >= s_scen->perturb_at &&
        l->n && !l->last_erase) {
        s_perturb_done = 1;
        perturbed = 1;
        switch (s_scen->perturb) {
        case P_BEGIN:      /* the loop never re-reads begin */
            clock_event(1);
            st32(VA(OR_VEC_BEGIN), ld32(VA(OR_VEC_BEGIN)) - 4U);
            break;
        case P_END:        /* one more element (a spare slot) */
            clock_event(1);
            st32(VA(OR_VEC_END), ld32(VA(OR_VEC_END)) + 4U);
            break;
        case P_FOREIGN:
            synthetic_call(&s_cpu2, word, c->esi - 4U);
            break;
        case P_SITE:
            synthetic_call(c, word == OR_SITE_A ? OR_SITE_B : OR_SITE_A,
                           c->esi - 4U);
            break;
        default:           /* P_ESI */
            synthetic_call(c, word, c->esi - 8U);
            break;
        }
    }
    esi = c->esi;
    clock_entry();
    note_site_cpu(c);
    reads_before = s_clock_reads;      /* after the scripted call's read */
    CHECK(s_shape->indexed(c, OR_TGT_LOCAL, NULL) == 1);
    ms = c->eax;
    if (s_scen->perturb == P_FIRST && s_site_calls == 0U) {
        perturbed = 1;      /* armed at begin - 4: only `esi != begin` */
        CHECK(l->n == 0U);
    }
    trace_add(T_TGT, word, esi, ms, s_depth);
    stamp = ld32(ld32(esi) + OR_STAMP);
    erase = (uint32_t)(ms - stamp) > OR_LIMIT;
    eligible = l->n && !l->last_erase && !perturbed;
    fresh = s_clock_reads != reads_before;
    if (s_shape->latch == 2) {
        CHECK(fresh == !eligible);     /* ON reads exactly at the fresh calls */
        CHECK(s_clock_reads - reads_before == (unsigned)fresh);
    } else {
        CHECK(s_clock_reads - reads_before == 1U);
    }
    if (perturbed)
        ++s_perturb_fresh[s_scen->perturb];
    if (eligible) {
        /* OBSERVE: ms is fresh, arm_ms the would-be latch; ON: ms is the
         * latch itself */
        if (s_shape->latch == 2) {
            CHECK(ms == l->arm_ms);
            if ((int32_t)(stamp - l->arm_ms) > 0)
                ++s_exp_on_ahead;
        } else {
            if (ms != l->arm_ms)
                ++s_exp_ticks;
            if (erase != ((uint32_t)(l->arm_ms - stamp) > OR_LIMIT))
                ++s_exp_flips;
            if ((int32_t)(stamp - l->arm_ms) > 0)
                ++s_exp_ahead;
        }
    } else {
        l->arm_ms = ms;
    }
    if (erase) {
        ++s_exp_expiries;
        ++l->erases;
        s_expect_free = ld32(esi);
    } else {
        ++l->keeps;
    }
    l->last_erase = (uint32_t)erase;
    ++l->n;
    ++s_site_calls;
    return 1;
}

/* ---- guest callee stand-ins -------------------------------------------- */
static void clobber(CPU *__restrict c, uint32_t tag)
{
    c->eax = 0x0bad0000U ^ tag ^ s_k;
    c->ecx = 0x0c1b0000U ^ tag;
    c->edx = 0x0ed10000U ^ (tag << 4) ^ s_k;
}

static void stdcall_return(CPU *__restrict c, uint32_t bytes)
{
    (void)gpop(c);
    CHECK(guest_stack_adjust(c, bytes, 0U) == 1);
}

static void st_gl_clear_color(CPU *__restrict c)
{
    clock_event(0);
    trace_add(T_GL_CC, ld32(c->esp + 4U), ld32(c->esp + 8U),
              ld32(c->esp + 12U), ld32(c->esp + 16U));
    clobber(c, 1U);
    stdcall_return(c, 16U);
}

static void st_gl_clear(CPU *__restrict c)
{
    clock_event(0);
    trace_add(T_GL_CLEAR, ld32(c->esp + 4U), 0U, 0U, 0U);
    clobber(c, 2U);
    stdcall_return(c, 4U);
}

/* sub_00561ff0: thiscall, one argument, ret 4 (guest_0166.c 0x562020) */
void sub_00561ff0(CPU *__restrict c)
{
    clock_event(0);
    trace_add(T_PREPARE, c->ecx, ld32(c->esp + 4U), 0U, 0U);
    clobber(c, 3U);
    stdcall_return(c, 4U);
}

static void st_vt70(CPU *__restrict c)
{
    clock_event(0);
    trace_add(T_VT70, c->ecx, ld32(c->esp + 4U), 0U, 0U);
    clobber(c, 4U);
    stdcall_return(c, 4U);
}

void sub_00564f50(CPU *__restrict c);
void sub_00561f40(CPU *__restrict c);

static void call_body(CPU *__restrict c, uint32_t which, uint32_t manager,
                      uint32_t ret_word)
{
    const uint32_t esp0 = c->esp;

    c->esp -= 4U;
    st32(c->esp, ret_word);
    c->ecx = manager;
    loop_push();
    if (which == 1U)
        sub_00564f50(c);
    else
        sub_00561f40(c);
    loop_pop();
    CHECK(c->fault == NULL);
    CHECK(c->esp == esp0);
}

/* Image::FreeBatches (vt+0x74): thiscall, no argument, ret. */
static void st_free_batches(CPU *__restrict c)
{
    const uint32_t image = c->ecx;

    clock_event(0);
    CHECK(s_expect_free == image);
    s_expect_free = 0U;
    trace_add(T_FREE, image, ld32(image + OR_STAMP), s_depth, 0U);
    st32(image + 0x4cU, 0U);
    st32(image + 0x5cU, 0U);
    if (s_free_calls++ == s_scen->nested_at && s_depth == 1U) {
        /* a Clear started from inside FreeBatches, on another vector */
        const uint32_t begin = ld32(VA(OR_VEC_BEGIN));
        const uint32_t end = ld32(VA(OR_VEC_END));

        st32(VA(OR_VEC_BEGIN), A(INNER_STORAGE) + 4U);
        st32(VA(OR_VEC_END), A(INNER_STORAGE) + 4U * (1U + s_scen->inner_n));
        trace_add(T_NEST, s_scen->inner_body, s_scen->inner_n, 0U, 0U);
        call_body(c, s_scen->inner_body, MGR(1), 0x0055F1A0U);
        st32(VA(OR_VEC_BEGIN), begin);
        st32(VA(OR_VEC_END), end);
    }
    clobber(c, 5U);
    (void)gpop(c);
}

/* sub_005ec358 = memmove (cdecl: the caller pops its three words) */
void sub_005ec358(CPU *__restrict c)
{
    const uint32_t dst = ld32(c->esp + 4U), src = ld32(c->esp + 8U);
    const uint32_t bytes = ld32(c->esp + 12U);

    clock_event(0);
    CHECK(bytes < 4U * STORAGE_SLOTS && (bytes & 3U) == 0U);
    trace_add(T_MOVE, dst, src, bytes, 0U);
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, bytes);
    clobber(c, 6U);
    c->eax = dst;
    (void)gpop(c);
}

static const uint32_t s_stub_rvas[] = {
    RVA_STUB_GL_CLEAR_COLOR, RVA_STUB_GL_CLEAR, RVA_STUB_VT70, RVA_STUB_FREE,
};
static const guest_fn s_stub_fns[] = {
    st_gl_clear_color, st_gl_clear, st_vt70, st_free_batches,
};

/* ---- one run ------------------------------------------------------------- */
typedef struct run_result {
    uint64_t state;
    uint32_t site_calls, entries, clock_reads, tgt;
    IsaacVitaClearLatchCounters k;
} run_result;

static void counters_snapshot(IsaacVitaClearLatchCounters *to)
{
    if (s_shape->counters)
        memcpy(to, s_shape->counters, sizeof *to);
    else
        memset(to, 0, sizeof *to);
}

static void run(const scen *s, int shape_index, int drift, run_result *r)
{
    IsaacVitaClearLatchCounters before, after;
    const uint32_t *b = (const uint32_t *)&before, *a = (const uint32_t *)&after;
    uint32_t *d = (uint32_t *)&r->k, i, tgt_before;
    unsigned generic_before = s_route_generic, direct_before = s_route_direct;
    static const uint32_t k_sequences[6][2] = {
        { 0U, 0U }, { 1U, 0U }, { 2U, 0U }, { 1U, 2U }, { 2U, 1U }, { 1U, 1U } };

    s_shape = &s_shapes[shape_index];
    s_run_name = s_shape->tag;
    s_scen = s;
    build_world(s);
    clock_reset(s->base_ms, s->seed ^ 0xc10cU, s->frozen ? 0U : s->event_max,
                s->frozen ? 0U : s->tick_permille, drift && !s->frozen);
    if (s->frozen)
        s_ms_events = 0U;
    s_trace_n = 0U;
    s_depth = 0U;
    s_expect_free = 0U;
    s_perturb_done = 0;
    s_site_calls = s_free_calls = s_syn_calls = s_exp_foreign = 0U;
    s_exp_fresh = s_exp_hits = s_exp_ticks = s_exp_flips = s_exp_ahead =
        s_exp_expiries = s_exp_on_ahead = 0U;
    counters_snapshot(&before);
    tgt_before = s_shape->tgt ? *s_shape->tgt : 0U;
    s_clock_reads = 0U;
    if (s->perturb == P_FIRST) {
        /* a synthetic arming at begin - 4 with the site, CPU, begin and end
         * of the first loop: only `esi != begin` keeps it from hitting */
        const uint32_t first = k_sequences[s->bodies][0];
        synthetic_call(&s_cpu, first == 1U ? OR_SITE_A : OR_SITE_B,
                       ld32(VA(OR_VEC_BEGIN)) - 4U);
    }
    for (i = 0U; i < 2U && k_sequences[s->bodies][i]; ++i) {
        if (i)
            clock_event(0);        /* the frame code between two Clears */
        call_body(&s_cpu, k_sequences[s->bodies][i], MGR(0),
                  0x00401000U + 0x10U * i);
    }
    CHECK(s_expect_free == 0U && s_depth == 0U);
    r->state = state_hash();
    r->site_calls = s_site_calls;
    r->entries = s_entries;
    r->clock_reads = s_clock_reads;
    r->tgt = s_shape->tgt ? *s_shape->tgt - tgt_before : 0U;
    counters_snapshot(&after);
    for (i = 0U; i < sizeof before / 4U; ++i)
        d[i] = a[i] - b[i];
#if defined(ISAAC_VITA_IMPORT_DIRECT)
    CHECK(s_route_direct - direct_before == s_site_calls);
    CHECK(s_route_generic == generic_before);
#else
    CHECK(s_route_generic - generic_before == s_site_calls);
    CHECK(s_route_direct == direct_before);
#endif
    if (s_shape->tgt)
        CHECK(r->tgt == s_entries);
    /* (d) counting identities and exact counters, unperturbed runs */
    if (s->perturb == P_NONE) {
        CHECK(s_exp_fresh + s_exp_hits == s_site_calls);
        if (s_shape->latch == 2)
            CHECK(s_clock_reads == s_exp_fresh);
        else
            CHECK(s_clock_reads == s_site_calls);
        if (s_shape->counters) {
            CHECK(r->k.site_calls == s_site_calls);
            CHECK(r->k.hits == s_exp_hits);
            CHECK(r->k.fresh == s_exp_fresh);
            CHECK(r->k.expiries == s_exp_expiries);
            CHECK(r->k.clock_reads == s_clock_reads);
            CHECK(r->k.clock_reads == r->tgt - (s_shape->latch == 2 ?
                                                r->k.hits : 0U));
            if (s_shape->latch == 1) {
                CHECK(r->k.ticks == s_exp_ticks);
                CHECK(r->k.flips == s_exp_flips);
                CHECK(r->k.stamp_ahead == s_exp_ahead);
                CHECK(r->k.flips <= r->k.ticks && r->k.ticks <= r->k.hits);
            } else {
                CHECK(r->k.ticks == 0U && r->k.flips == 0U);
                CHECK(r->k.stamp_ahead == s_exp_on_ahead);
            }
        }
    }
    if (s_shape->counters) {
        /* every run: all site calls (synthetic ones too) and the foreign
         * CPU tripwire, which only the P_FOREIGN script can raise (by 2: its
         * own call and the next real one) */
        CHECK(r->k.site_calls == s_site_calls + s_syn_calls);
        CHECK(r->k.foreign_cpu == s_exp_foreign);
        CHECK(s_exp_foreign == (s->perturb == P_FOREIGN && s_perturb_done
                                ? 2U : 0U));
        CHECK(r->k.clock_reads == s_clock_reads);
    }
}

static void keep_trace(trace_rec *to, uint32_t *n)
{
    memcpy(to, s_trace, s_trace_n * sizeof s_trace[0]);
    *n = s_trace_n;
}

static int trace_equal(const trace_rec *ref, uint32_t ref_n, int with_eax)
{
    uint32_t i;

    if (ref_n != s_trace_n)
        return 0;
    for (i = 0U; i < ref_n; ++i) {
        const trace_rec *x = &ref[i], *y = &s_trace[i];
        if (x->kind != y->kind || x->a != y->a || x->b != y->b ||
            x->d != y->d)
            return 0;
        if (x->c != y->c &&
            (with_eax || (x->kind != T_TGT && x->kind != T_SYN)))
            return 0;
    }
    return 1;
}

static void report_difference(const trace_rec *ref, uint32_t ref_n)
{
    uint32_t i, n = ref_n < s_trace_n ? ref_n : s_trace_n;

    printf("trace lengths %u/%u\n", (unsigned)ref_n, (unsigned)s_trace_n);
    for (i = 0U; i < n; ++i)
        if (memcmp(&ref[i], &s_trace[i], sizeof ref[i]) != 0) {
            printf("first difference at %u: ref {%u %08x %08x %08x %u} got "
                   "{%u %08x %08x %08x %u}\n", (unsigned)i,
                   (unsigned)ref[i].kind, (unsigned)ref[i].a,
                   (unsigned)ref[i].b, (unsigned)ref[i].c, (unsigned)ref[i].d,
                   (unsigned)s_trace[i].kind, (unsigned)s_trace[i].a,
                   (unsigned)s_trace[i].b, (unsigned)s_trace[i].c,
                   (unsigned)s_trace[i].d);
            break;
        }
}

#define SAME(ref, ref_n, ref_state, r, what) do { \
        if (!trace_equal(ref, ref_n, 1) || (r).state != (ref_state)) { \
            report_difference(ref, ref_n); \
            fail_check(what, __LINE__); \
        } \
    } while (0)

/* ---- scenarios ------------------------------------------------------------ */
static unsigned s_scenarios, s_runs, s_d_positive, s_d_zero, s_ahead_runs;
static unsigned s_nested, s_frozen, s_wrap;
static uint64_t s_on_hits, s_on_fresh, s_on_reads, s_on_tgt, s_site_total;

static void scenario(scen *s)
{
    run_result r_off_v, r_off_w, r;
    uint32_t d;
    int same;

    s_scenario_seed = s->seed;
    ++s_scenarios;
    /* reference V: OFF on the full clock */
    run(s, S_OFF, 1, &r_off_v);
    keep_trace(s_trace_v, &s_trace_v_n);
    /* reference W: OFF on the callee-only clock */
    run(s, S_OFF, 0, &r_off_w);
    keep_trace(s_trace_w, &s_trace_w_n);
    s_runs += 2U;

    /* (c) every non-latching shape and OBSERVE equal OFF on V */
    run(s, S_OFF_P, 1, &r);
    SAME(s_trace_v, s_trace_v_n, r_off_v.state, r, "off_p(V) == off(V)");
    run(s, S_REC, 1, &r);
    SAME(s_trace_v, s_trace_v_n, r_off_v.state, r, "rec(V) == off(V)");
    run(s, S_OBS_REC, 1, &r);
    SAME(s_trace_v, s_trace_v_n, r_off_v.state, r, "obs_rec(V) == off(V)");
    run(s, S_OBS, 1, &r);
    SAME(s_trace_v, s_trace_v_n, r_off_v.state, r, "obs(V) == off(V)");
    d = r.k.flips;
    if (r.k.stamp_ahead)
        ++s_ahead_runs;
    /* (b) ON on V equals OFF on W, every ON shape */
    run(s, S_ON_P, 1, &r);
    SAME(s_trace_w, s_trace_w_n, r_off_w.state, r, "on_p(V) == off(W)");
    if (s->perturb == P_NONE) {
        s_site_total += r.site_calls;
        s_on_hits += r.k.hits;
        s_on_fresh += r.k.fresh;
        s_on_reads += r.k.clock_reads;
        s_on_tgt += r.tgt;
    }
    run(s, S_ON_P_REC, 1, &r);
    SAME(s_trace_w, s_trace_w_n, r_off_w.state, r, "on_p_rec(V) == off(W)");
    run(s, S_ON, 1, &r);
    SAME(s_trace_w, s_trace_w_n, r_off_w.state, r, "on(V) == off(W)");
    s_runs += 7U;
    /* (a) frozen: V == W, so ON == OFF too */
    if (s->frozen) {
        ++s_frozen;
        SAME(s_trace_v, s_trace_v_n, r_off_v.state, r, "frozen on == off");
    }
    /* (c) d == 0 <=> ON(V) runs OFF(V)'s trajectory (r is ON on V) */
    if (s->perturb == P_NONE) {
        same = trace_equal(s_trace_v, s_trace_v_n, 0) &&
               r.state == r_off_v.state;
        if (same != (d == 0U)) {
            printf("d=%u but trajectories %s\n", (unsigned)d,
                   same ? "agree" : "differ");
            report_difference(s_trace_v, s_trace_v_n);
            fail_check("(d == 0) == same trajectory", __LINE__);
        }
        if (d)
            ++s_d_positive;
        else
            ++s_d_zero;
    }
}

static void random_scenario(scen *s, uint32_t seed, uint32_t family)
{
    static const uint32_t k_ticks[] = { 0U, 50U, 200U, 500U, 1000U };

    memset(s, 0, sizeof *s);
    s->seed = seed;
    s_rng = seed * 2654435761U | 1U;
    s->n = pick(4U) == 0U ? pick(4U) : pick(5U) == 0U ? 60U + pick(141U)
                                                        : pick(60U);
    s->begin_off = 1U + pick(16U);
    s->bodies = 1U + pick(5U);
    s->stamp_mode = ST_MIX;
    s->event_max = pick(4U);
    s->tick_permille = k_ticks[pick(5U)];
    s->base_ms = 20000U + pick(1U << 30);
    s->nested_at = UINT32_MAX;
    s->inner_body = 1U + pick(2U);
    s->inner_n = pick(40U);
    s->duplicates = pick(20U) == 0U;
    switch (family) {
    case 1: s->stamp_mode = ST_KEEP; break;
    case 2: s->stamp_mode = ST_EXPIRE; break;
    case 3: s->stamp_mode = ST_ALTERNATE; break;
    case 4:
        s->stamp_mode = ST_ONE;
        s->n = 1U + pick(80U);
        s->one_at = pick(3U) == 0U ? 0U : pick(2U) ? s->n - 1U : pick(s->n);
        break;
    case 5: s->stamp_mode = ST_BOUNDARY; s->tick_permille = 500U; break;
    case 6:   /* perturbations, on keep-heavy vectors */
        s->perturb = 1U + pick(P_KINDS - 1U);
        s->stamp_mode = pick(2U) ? ST_KEEP : ST_MIX;
        s->n = 2U + pick(80U);
        s->perturb_at = pick(3U) ? 0U : pick(s->n);
        s->event_max = pick(3U);
        break;
    case 7:   /* a Clear inside FreeBatches */
        s->stamp_mode = pick(2U) ? ST_MIX : ST_EXPIRE;
        s->n = 1U + pick(60U);
        s->nested_at = pick(4U);
        s->inner_n = 1U + pick(40U);
        ++s_nested;
        break;
    case 8:   /* stamps newer than the clock (wrap hazard, w) */
        s->stamp_mode = ST_AHEAD;
        s->tick_permille = 1000U;
        s->event_max = 3U;
        break;
    case 9: s->frozen = 1; break;
    case 10:  /* timeGetTime wraps through 0 during the scenario */
        s->base_ms = (UINT64_C(0xFFFFFFFF) - 40U + pick(60U)) +
                     ((uint64_t)pick(1024U) << 32);
        ++s_wrap;
        break;
    case 11:  /* the reciprocal at large process times */
        s->base_ms = (UINT64_C(1) << 40) + ((uint64_t)rnd() << 8);
        break;
    default: break;
    }
    if (s->n > 128U)
        s->n = 128U;          /* the outer images are 0..127 */
}

int main(int argc, char **argv)
{
    static guest_import imports[ISAAC_VITA_IMPORT_ID_COUNT];
    static const char k_selftest[] =
        "KAGE VITA TGT RECIPROCAL: selftest n=4096 mismatch=0 use=reciprocal";
    scen s;
    uint32_t i, family, rounds, seed = 0x20261026U;
    int fast = argc > 1 && strcmp(argv[1], "fast") == 0;
    unsigned observe_lines = 0U, on_lines = 0U, selftest_lines = 0U;
    const char *flavour = argc > 2 ? argv[2] : "?";

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (i = 0U; i < sizeof s_pages / sizeof s_pages[0]; ++i) {
        void *p = VirtualAlloc((LPVOID)(uintptr_t)VA(s_pages[i]), 0x10000U,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == NULL || (uintptr_t)p != VA(s_pages[i])) {
            printf("cannot commit image page %08x\n", (unsigned)VA(s_pages[i]));
            return 2;
        }
    }
    s_arena = (uint8_t *)VirtualAlloc(NULL, ARENA_BYTES,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!s_arena || (uintptr_t)s_arena + ARENA_BYTES > VA(0)) {
        printf("cannot allocate the arena below the image\n");
        return 2;
    }
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = UINT32_C(0x00840000);
    guest_register(s_stub_rvas, s_stub_fns,
                   sizeof s_stub_rvas / sizeof s_stub_rvas[0]);
    /* the production import table: all 413 rows of the frozen contract */
    for (i = 0U; i < ISAAC_VITA_IMPORT_ID_COUNT; ++i) {
        imports[i].slot_rva = s_contract[i].slot_rva;
        imports[i].name = s_contract[i].name;
    }
    CHECK(s_contract[OR_TGT_IMPORT_ID].slot_rva == OR_IAT_TGT);
    CHECK(strcmp(s_contract[OR_TGT_IMPORT_ID].name,
                 "WINMM.dll!timeGetTime") == 0);
    CHECK(s_contract[OR_TGT_IMPORT_ID].kind == ISAAC_VITA_IMPORT_POST_COM &&
          s_contract[OR_TGT_IMPORT_ID].local_index == OR_TGT_LOCAL);
    guest_register_imports(imports, ISAAC_VITA_IMPORT_ID_COUNT);
#if defined(ISAAC_VITA_IMPORT_DIRECT)
    CHECK(g_guest_import_direct[OR_TGT_IMPORT_ID].slot_va == VA(OR_IAT_TGT) &&
          g_guest_import_direct[OR_TGT_IMPORT_ID].local_index == OR_TGT_LOCAL);
#endif

    /* every shape's first call (the reciprocal self-test) off the books: a
     * non-site call, which must read the clock once and leave the latch */
    s_shape = NULL;
    for (i = 0U; i < SHAPES; ++i) {
        uint32_t sp = (uint32_t)(uintptr_t)&s_stack[GUARD_WORDS + 0x100U];
        unsigned reads;

        guest_cpu_init(&s_cpu);
        CHECK(guest_stack_bind(&s_cpu,
                               (uint32_t)(uintptr_t)&s_stack[GUARD_WORDS],
                               (uint32_t)(uintptr_t)&s_stack[GUARD_WORDS +
                                                             STACK_WORDS]) == 0);
        st32(sp, 0x0050B812U);       /* ..._GET_TIME_FIRST_RETURN_RVA */
        s_cpu.esp = sp;
        s_now_us = UINT64_C(4294967295999);
        s_clock_reads = 0U;
        /* handler index 4 of every shape is the timeGetTime row's name */
        CHECK(strcmp(s_shapes[i].name(OR_TGT_LOCAL),
                     s_contract[OR_TGT_IMPORT_ID].name) == 0);
        CHECK(s_shapes[i].indexed(&s_cpu, OR_TGT_LOCAL, NULL) == 1);
        reads = s_clock_reads;
        CHECK(s_cpu.eax == 0xFFFFFFFFU && s_cpu.esp == sp + 4U);
        CHECK(reads == (s_shapes[i].reciprocal && s_shapes[i].tgt ? 4U : 1U));
    }
    /* rec logs the plain line, obs_rec and on_p_rec (PHASE_PROFILE) the
     * same line plus the B4 timing fields */
    for (i = 0U; i < s_log_lines; ++i)
        if (strcmp(s_log[i], k_selftest) == 0 ||
            (strncmp(s_log[i], k_selftest, sizeof k_selftest - 1U) == 0 &&
             strncmp(s_log[i] + sizeof k_selftest - 1U, " div_us=", 8U) == 0 &&
             strstr(s_log[i], " reps=1000") != NULL))
            ++selftest_lines;
    CHECK(selftest_lines == 3U && s_log_lines == 3U);
    s_log_lines = 0U;

    rounds = fast ? 60U : 1500U;
    for (family = 0U; family <= 11U; ++family) {
        const uint32_t n = family == 0U ? rounds * 4U :
                           family == 6U ? rounds * 3U : rounds;
        for (i = 0U; i < n; ++i) {
            random_scenario(&s, seed++, family);
            scenario(&s);
        }
    }
    /* the banners: once per latching shape, at its first site call */
    for (i = 0U; i < s_log_lines; ++i) {
        if (strcmp(s_log[i], "KAGE VITA CLEAR LATCH: mode=1 sites=0x00564fd6,"
                             "0x00561f6b vec=0x007e7d1c stamp=+0x74 "
                             "limit=0x1388") == 0)
            ++observe_lines;
        if (strcmp(s_log[i], "KAGE VITA CLEAR LATCH: mode=2 sites=0x00564fd6,"
                             "0x00561f6b vec=0x007e7d1c stamp=+0x74 "
                             "limit=0x1388") == 0)
            ++on_lines;
    }
    CHECK(observe_lines == 2U && on_lines == 3U && s_log_lines == 5U);
    for (i = 1U; i < P_KINDS; ++i)
        CHECK(s_perturb_fresh[i] > 0U);
    CHECK(s_d_positive > 0U && s_d_zero > 0U && s_ahead_runs > 0U);
    CHECK(s_on_reads == s_on_tgt - s_on_hits);

    printf("perturbed calls (each forced a fresh read; all 9 runs per "
           "scenario):");
    for (i = 1U; i < P_KINDS; ++i)
        printf(" %s=%u", k_perturb_names[i], s_perturb_fresh[i]);
    printf("\n");
    printf("trajectory: d>0 scenarios=%u (ON(V) != OFF(V)), d==0 scenarios=%u "
           "(ON(V) == OFF(V)); w>0 scenarios=%u; nested=%u frozen=%u wrap=%u\n",
           s_d_positive, s_d_zero, s_ahead_runs, s_nested, s_frozen, s_wrap);
    printf("ON (unperturbed, on_p): site calls=%llu hits=%llu fresh=%llu "
           "clock reads=%llu tgt=%llu (reads = tgt - hits)\n",
           (unsigned long long)s_site_total, (unsigned long long)s_on_hits,
           (unsigned long long)s_on_fresh, (unsigned long long)s_on_reads,
           (unsigned long long)s_on_tgt);
    printf("Vita Clear expiry latch oracle: PASS; flavour=%s; scenarios=%u; "
           "runs=%u; claims a,b,c,d hold\n", flavour, s_scenarios, s_runs);
    return 0;
}
