/* Differential oracle of host_vita_sfx_floor_preload.c.
 *
 * test_vita_sfx_floor_preload.py extracts the translated SFXManager::Play
 * (sub_00488de0) from the frozen corpus, renames it real_sub_00488de0 and
 * stops it at L_00488ea4 -- the point where both of Play's paths join after
 * the lazy load (0x488e8a..0x488ea1) or past it (entry already loaded) -- by
 * calling oracle_play_ea4() there.  That prefix is the reference: it decides,
 * exactly as the game does, whether a Play of `id` would load the sound now,
 * calls SoundEffect::Load (the stub below, shared with the native replay)
 * with its ECX and return word, logs, and increments [SFXManager+0xc].
 *
 * Part A runs the native bounds/state test (sfxp_entry) against Play on
 * random vector spans across the int32 range (the 0x94f2095 multiply),
 * negative/edge ids and every entry state.  Part B builds whole worlds -- a
 * Manager with its SFXManager vector, a Level object, Load's audio-running
 * byte, the heap reading, the eager/terminal gates, Load faults and ESP
 * breaches -- runs the wrapper (the Level::Init stub as __real_) and, on a
 * copy, the reference: the same stub, then Play for every id of the native
 * list in the same order, rolling back the ones an independent model of the
 * headroom rule declines.  It compares every byte of the world (stack
 * excluded), every architectural CPU field, the sequence of Load calls (ECX,
 * return word, ESP), the verdict counters and the log lines, and prints the
 * branch coverage.
 *
 * With ISAAC_VITA_SFX_FIGHT_PRELOAD the native list grows by s_fight and the
 * reference grows with it: after floor lists that completed, the fight ids a
 * translated Play would load now (each tried and rolled back) give the bytes
 * the whole-list guard needs; below RESERVE_MB + those bytes of headroom, or
 * the pool's margin + those bytes, nothing loads; otherwise Play runs for
 * every fight id like the floor ids.  The extension's verdict counters, its
 * one log line and every field of that line (headroom, need, sound count,
 * skipped-sound mask, loads, skips) are compared, and Part B adds worlds for
 * the guard's exact fit / one byte short / below reserve on both rules, an
 * empty list on a heap below every reserve, a tail of two small sounds whose
 * per-sound rule declines after the whole-list guard passed, and Load
 * faults/breaches inside the fight list.  The kinds and the pool come from
 * their own draws, so these legs have their own pinned results.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <stdarg.h>
#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_sfx_floor_preload_oracle requires a 32-bit host
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))

/* ---- stubs owned by other Vita runtime units (the G FUEL oracle's) ---- */
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

/* ---- the platform the module asks for --------------------------------- */
static int s_materialization_ready = 1;
static int s_heap_terminal;
static uint64_t s_clock_us;
int isaac_vita_audio_materialization_ready(void) { return s_materialization_ready; }
int isaac_vita_guest_heap_terminal(void) { return s_heap_terminal; }
uint64_t isaac_sfxp_oracle_now_us(void) { return s_clock_us += 7U; }

/* The heap reading lives in the compared world (a Load stub "allocates"). */
/* The heap reading and the OpenAL pool's free bytes (UINT32_MAX = the
 * whole-session newlib fallback) live in the compared world. */
static struct { uint32_t used; uint32_t pool; uint32_t pad[2]; } s_heap;
uint32_t isaac_sfxp_oracle_heap_used(void) { return s_heap.used; }
uint32_t isaac_sfxp_oracle_pool_room(void) { return s_heap.pool; }

static unsigned s_log_armed, s_log_sound, s_log_declined, s_log_level,
                s_log_stopped, s_log_other;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
/* The extension's own lines ("... fight level=N ..."), by kind, and the
 * text of the last one (its fields are checked against the reference). */
static unsigned s_log_fight_heap, s_log_fight_pool, s_log_fight_walked,
                s_log_fight_stopped;
static char s_fight_line[512];
static const char k_fight_prefix[] = "KAGE VITA SFX PRELOAD: fight level=";
#endif
void isaac_vita_log(const char *format, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    /* first: a declined fight line also holds " declined hr_kb=" */
    if (strncmp(line, k_fight_prefix, sizeof k_fight_prefix - 1U) == 0) {
        if (strstr(line, " declined hr_kb=") != NULL)
            ++s_log_fight_heap;
        else if (strstr(line, " declined pool_kb=") != NULL)
            ++s_log_fight_pool;
        else if (strstr(line, " stopped fault=") != NULL)
            ++s_log_fight_stopped;
        else if (strstr(line, " loaded=") != NULL)
            ++s_log_fight_walked;
        else
            ++s_log_other;
        memcpy(s_fight_line, line, sizeof s_fight_line);
        return;
    }
#endif
    if (strncmp(line, "KAGE VITA SFX PRELOAD: armed ", 29) == 0)
        ++s_log_armed;
    else if (strstr(line, " declined hr_kb=") != NULL ||
             strstr(line, " declined pool_kb=") != NULL)
        ++s_log_declined;
    else if (strncmp(line, "KAGE VITA SFX PRELOAD: sound=", 29) == 0)
        ++s_log_sound;
    else if (strstr(line, " stopped fault=") != NULL)
        ++s_log_stopped;
    else if (strncmp(line, "KAGE VITA SFX PRELOAD: level=", 29) == 0)
        ++s_log_level;
    else
        ++s_log_other;
}

/* ---- the module, with the Level::Init stub as __real_ ------------------ */
void oracle_level_init(CPU *__restrict c);
#define __real_sub_00311e60 oracle_level_init
#include "host_vita_sfx_floor_preload.c"
#undef __real_sub_00311e60

/* ---- the reference ------------------------------------------------------ */
void real_sub_00488de0(CPU *__restrict c);

static unsigned s_failures;
static const char *s_case = "-";
static void fail(const char *what)
{
    ++s_failures;
    if (s_failures <= 40)
        printf("FAIL %s: %s\n", s_case, what);
}

/* Play's join point L_00488ea4 (truncation). */
static unsigned s_ea4_hits;
static uint32_t s_ea4_esi;
void oracle_play_ea4(CPU *__restrict c)
{
    ++s_ea4_hits;
    s_ea4_esi = c->esi;
}

/* Play's logger calls (cdecl: the caller pops the arguments). */
static unsigned s_logger_calls;
static uint32_t s_logger_fmt, s_logger_id;
void sub_0055e330(CPU *__restrict c)
{
    ++s_logger_calls;
    s_logger_fmt = ld32(c->esp + 8U);
    s_logger_id = ld32(c->esp + 12U);
    c->eax = 0x1066e330U;
    (void)gpop(c);
}

/* Callees past the truncation point and the magic-static initialiser: the
 * worlds keep the static initialised, so none of them may run. */
#define ORACLE_UNREACHABLE(name)                                           \
    void name(CPU *__restrict c)                                           \
    {                                                                      \
        (void)c;                                                           \
        fail(#name " reached");                                            \
    }
ORACLE_UNREACHABLE(sub_003a2950)
ORACLE_UNREACHABLE(sub_0056e9a0)
ORACLE_UNREACHABLE(sub_0056e9d0)
ORACLE_UNREACHABLE(sub_0056eb70)
ORACLE_UNREACHABLE(sub_0056f1c0)
ORACLE_UNREACHABLE(sub_005eb079)
ORACLE_UNREACHABLE(sub_005eb225)
ORACLE_UNREACHABLE(sub_005eb26f)

/* One string per fault kind, shared by the stubs and the expectations. */
static const char k_load_fault[] = "oracle Load fault";
static const char k_level_fault[] = "oracle Level::Init fault";

/* ---- SoundEffect::Load stand-in (shared by both sides) ----------------- */
static uint32_t entries_base(void);
#define MAX_LOADS 64u
typedef struct load_call { uint32_t ecx, ret, esp; } load_call;
static load_call s_loads[MAX_LOADS];
static unsigned s_load_count;
static uint32_t s_fault_entry, s_breach_entry;

static uint32_t mix(uint32_t v)
{
    v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU;
    return v ^ (v >> 16);
}

void sub_00488070(CPU *__restrict c)
{
    const uint32_t entry = c->ecx;
    const uint32_t samples = ld32(entry + 0x190U);
    const uint32_t n = samples < 6U ? samples : 6U;
    /* hashed by slot, not address: the result lines must not depend on
     * where the linker put s_entries */
    const uint32_t h = mix(entry - entries_base());
    uint32_t i;

    if (s_load_count < MAX_LOADS) {
        s_loads[s_load_count].ecx = entry;
        s_loads[s_load_count].ret = ld32(c->esp);
        s_loads[s_load_count].esp = c->esp;
    }
    ++s_load_count;
    (void)gpop(c);                                  /* plain ret */
    /* 004880b4..0048819b: one result byte per sample (edi = esi+0x20,
     * stride 0x38, [edi+0x18]), then 004881e3 mov byte [esi+0x198], 1 */
    for (i = 0; i < n; ++i)
        st8(entry + 0x20U + i * 0x38U + 0x18U, (uint8_t)(mix(h + i) & 1U));
    st32(entry + 0x20U, h);                         /* a sample handle */
    st8(entry + 0x198U, 1U);
    s_heap.used += 4096U + (h & 0x3fffU);           /* PCM + objects */
    if (s_heap.pool != UINT32_MAX) {                /* alBufferData copy */
        const uint32_t pcm = 2048U + ((h >> 14) & 0x7fffU);
        s_heap.pool = s_heap.pool > pcm ? s_heap.pool - pcm : 0U;
    }
    /* caller-saved state a real body leaves behind; ebx/esi/edi/ebp kept */
    c->eax = h; c->ecx = h ^ 0x5a5a5a5aU; c->edx = ~h;
    c->x[0].u64[0] = ((uint64_t)h << 32) | i; c->x[0].u64[1] = h;
    c->x[7].u64[1] = mix(h ^ 7U);
    c->st[(c->st_top + 7) & 7] = (double)h;
    c->fsw = (uint16_t)(h & 0x4700U);
    c->fl.f_a = h; c->fl.f_b = ~h; c->fl.f_r = h + 1U; c->fl.f_op = 3U;
    c->fl.f_sz = 4U; c->fl.f_cf = (uint8_t)(h & 1U); c->fl.f_zf = 1U;
    c->df = 0U;
    c->last_error = 0x00000002U;
    if (entry == s_fault_entry)
        c->fault = k_load_fault;
    if (entry == s_breach_entry)
        c->esp -= 4U;                               /* violates `ret` */
}

/* ---- Level::Init stand-in (__real_ of the wrapper) --------------------- */
static uint32_t s_level_seed;
static int s_level_fault;
static int s_level_writes;          /* the body rewrites stage/type */
static uint32_t s_level_stage_after, s_level_type_after;
static unsigned s_level_calls;
static uint32_t s_level_this;

void oracle_level_init(CPU *__restrict c)
{
    const uint32_t h = mix(s_level_seed);
    ++s_level_calls;
    s_level_this = c->ecx;
    (void)gpop(c);                                  /* 00312ea9 ret */
    if (s_level_writes) {
        st32(c->ecx + 0U, s_level_stage_after);
        st32(c->ecx + 4U, s_level_type_after);
    }
    c->eax = h; c->ecx = mix(h); c->edx = mix(h + 1U);
    c->x[1].u64[0] = mix(h + 2U); c->x[3].u64[1] = mix(h + 3U);
    c->st[c->st_top & 7] = 1.25 * (double)(h & 0xffffU);
    c->fsw = (uint16_t)(h & 0x0100U);
    c->fl.f_a = mix(h + 4U); c->fl.f_b = h; c->fl.f_r = 0U; c->fl.f_op = 1U;
    c->fl.f_sz = 4U; c->fl.f_sf = (uint8_t)(h & 1U);
    c->df = (uint8_t)((h >> 3) & 1U);
    c->idf = (uint8_t)((h >> 4) & 1U);
    c->last_error = h & 0xffU;
    if (s_level_fault)
        c->fault = k_level_fault;
}

/* ---- the world -------------------------------------------------------- */
#define ORACLE_PAGE 0x10000u
#define MANAGER_BYTES 0x29e00u
#define SFX_OFFSET 0x29de0u
#define ENTRY_BYTES 0x1b8u
#define MAX_ENTRIES 640u
#define STACK_WORDS 4096u
static const uint32_t s_pages[] = {
    0x007a0000u,   /* 7aa3b4 the security cookie Play's prologue reads */
    0x007e0000u,   /* 7e7d44 Load's audio-running byte */
    0x007f0000u,   /* 7fd680 the Manager pointer */
    0x00800000u,   /* 806e40 Play's magic-static guard */
};
static uint8_t s_manager[MANAGER_BYTES];
static uint8_t s_entries[MAX_ENTRIES * ENTRY_BYTES];
static uint32_t entries_base(void) { return (uint32_t)(uintptr_t)s_entries; }
static uint8_t s_level[64];
static uint32_t s_stack[STACK_WORDS];
static uint32_t s_stack_snap[STACK_WORDS];
static uint32_t s_tls_block[16];

typedef struct region { void *at; size_t bytes; } region;
static region s_regions[16];
static unsigned s_region_count;
static uint8_t *s_snap[16], *s_ref[16], *s_tmp[16];

static void add_region(void *at, size_t bytes)
{
    s_regions[s_region_count].at = at;
    s_regions[s_region_count].bytes = bytes;
    s_snap[s_region_count] = (uint8_t *)malloc(bytes);
    s_ref[s_region_count] = (uint8_t *)malloc(bytes);
    s_tmp[s_region_count] = (uint8_t *)malloc(bytes);
    ++s_region_count;
}
static void world_save(uint8_t **to)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        memcpy(to[i], s_regions[i].at, s_regions[i].bytes);
}
static void world_load(uint8_t **from)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        memcpy(s_regions[i].at, from[i], s_regions[i].bytes);
}
static int world_equal(uint8_t **with)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        if (memcmp(s_regions[i].at, with[i], s_regions[i].bytes) != 0) {
            size_t b;
            for (b = 0; b < s_regions[i].bytes &&
                        ((uint8_t *)s_regions[i].at)[b] == with[i][b]; ++b)
                ;
            printf("  region %u differs at +0x%x (%08x)\n", i, (unsigned)b,
                   (unsigned)(uintptr_t)((uint8_t *)s_regions[i].at + b));
            return 0;
        }
    return 1;
}

static uint32_t s_lcg = 0x5f1a2026u;
static uint32_t lcg(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg >> 8;
}
static uint32_t A(const void *p) { return (uint32_t)(uintptr_t)p; }
static uint32_t sfx_addr(void) { return A(s_manager) + SFX_OFFSET; }

/* A CPU's stack token names the CPU itself (guest.h:247): a copy must
 * take the token over or every push of the copy fails closed. */
static void cpu_copy(CPU *to, const CPU *from)
{
    *to = *from;
    to->stack_owner = to;
}

static void cpu_fresh(CPU *c)
{
    const uint32_t floor = A(&s_stack[0]);
    guest_cpu_init(c);
    if (guest_stack_bind(c, floor, floor + 4u * STACK_WORDS)) {
        printf("stack bind failed\n");
        exit(2);
    }
    c->eax = lcg(); c->ebx = lcg(); c->ecx = lcg(); c->edx = lcg();
    c->esi = lcg(); c->edi = lcg();
    c->ebp = floor + 4u * STACK_WORDS - 0x40u;
    c->esp = floor + 4u * STACK_WORDS - 0x800u;
    c->x[0].u64[0] = ((uint64_t)lcg() << 32) | lcg();
    c->x[5].u64[1] = ((uint64_t)lcg() << 32) | lcg();
    c->st[1] = (double)lcg();
    c->fl.f_a = lcg(); c->fl.f_b = lcg(); c->fl.f_r = lcg();
    c->fl.f_op = (uint8_t)(lcg() & 7U); c->fl.f_sz = 4U;
    c->last_error = lcg() & 0xffffU;
}

/* Every architectural field, field by field (no padding compared). */
static int cpu_arch_equal(const CPU *a, const CPU *b)
{
    int i;
    for (i = 0; i < 8; ++i)
        if (a->r[i] != b->r[i] || memcmp(&a->x[i], &b->x[i], sizeof a->x[i]) ||
                memcmp(&a->st[i], &b->st[i], sizeof a->st[i]))
            return 0;
    return a->st_top == b->st_top && a->fsw == b->fsw &&
           a->fl.f_a == b->fl.f_a && a->fl.f_b == b->fl.f_b &&
           a->fl.f_r == b->fl.f_r && a->fl.f_op == b->fl.f_op &&
           a->fl.f_sz == b->fl.f_sz && a->fl.f_cf == b->fl.f_cf &&
           a->fl.f_of == b->fl.f_of && a->fl.f_zf == b->fl.f_zf &&
           a->fl.f_sf == b->fl.f_sf && a->fl.f_pf == b->fl.f_pf &&
           a->df == b->df && a->idf == b->idf &&
           a->last_error == b->last_error && a->fault == b->fault;
}

/* One translated Play(id) on the current world.  Returns the class:
 * 0 range (no log, no join), 1 no samples ("has no samples" log),
 * 2 loaded (join without Load), 3 load (Load, warn log, join). */
enum { PLAY_RANGE, PLAY_NO_SAMPLES, PLAY_LOADED, PLAY_LOAD, PLAY_ODD };
static uint32_t s_play_entry;
static int play_reference(int32_t id)
{
    CPU c;
    uint32_t fs0, i;
    unsigned ea4 = s_ea4_hits, logs = s_logger_calls, loads = s_load_count;
    int cls;

    cpu_fresh(&c);
    c.ecx = sfx_addr();
    /* thiscall Play(id, a2..a6), ret 0x18: [esp] return word of the
     * 0x38a4b0 call site, then six argument words */
    for (i = 6; i >= 1; --i)
        gpush(&c, i == 1 ? (uint32_t)id : lcg());
    gpush(&c, 0x0038a4b5U);
    fs0 = ld32(guest_fs_base(&c));
    real_sub_00488de0(&c);
    /* the truncated prefix leaves Play's SEH frame linked */
    st32(guest_fs_base(&c), fs0);
    if (c.fault && s_load_count == loads)
        fail("reference Play faulted");
    s_play_entry = s_ea4_esi;
    if (s_load_count != loads) {
        cls = (s_load_count == loads + 1U && s_ea4_hits == ea4 + 1U &&
               s_logger_calls == logs + 1U && s_logger_fmt == 0x98757a48U &&
               s_logger_id == (uint32_t)id) ? PLAY_LOAD : PLAY_ODD;
        s_play_entry = s_loads[loads < MAX_LOADS ? loads : MAX_LOADS - 1U].ecx;
    } else if (s_ea4_hits != ea4) {
        cls = s_logger_calls == logs ? PLAY_LOADED : PLAY_ODD;
    } else if (s_logger_calls != logs) {
        cls = (s_logger_calls == logs + 1U && s_logger_fmt == 0x98757a6cU &&
               s_logger_id == (uint32_t)id) ? PLAY_NO_SAMPLES : PLAY_ODD;
    } else {
        cls = PLAY_RANGE;
    }
    if (cls == PLAY_ODD)
        fail("reference Play took an unexpected path");
    return cls;
}

/* ---- coverage ------------------------------------------------------------ */
static unsigned cov_a[5], cov_a_neg, cov_a_boundary, cov_a_cases;
static unsigned cov_level[SFXP_LEVEL_KINDS], cov_water, cov_dry, cov_cases;
static unsigned cov_skip[SFXP_SKIP_KINDS], cov_loads, cov_faults, cov_breaches;
static unsigned cov_hr_exact, cov_hr_short, cov_hr_reserve, cov_rewrite;
static unsigned cov_pool_exact, cov_pool_short, cov_pool_reserve,
                cov_pool_fallback;
static unsigned cov_null_manager, cov_null_begin, cov_reversed;

/* ---- Part A: the bounds/state test against Play -------------------------- */
static void set_entry(uint32_t entry, uint32_t samples, uint8_t loaded)
{
    st32(entry + 0x190U, samples);
    st8(entry + 0x198U, loaded);
}

static void part_a_one(uint32_t span, int32_t id, uint32_t begin)
{
    const uint32_t sfx = sfx_addr();
    sfxp_skip why = SFXP_SKIP_KINDS;
    uint32_t entry;
    int cls;

    ++cov_a_cases;
    s_load_count = 0;
    world_save(s_tmp);
    st32(sfx, begin);
    st32(sfx + 4U, begin + span);
    cls = play_reference(id);
    world_load(s_tmp);
    st32(sfx, begin);
    st32(sfx + 4U, begin + span);
    entry = sfxp_entry(sfx, id, &why);
    ++cov_a[cls < 5 ? cls : 4];
    if (id < 0)
        ++cov_a_neg;
    switch (cls) {
    case PLAY_RANGE:
        if (entry || why != SFXP_SKIP_RANGE) fail("A: range verdict differs");
        break;
    case PLAY_NO_SAMPLES:
        if (entry || why != SFXP_SKIP_NO_SAMPLES) fail("A: no-samples verdict differs");
        break;
    case PLAY_LOADED:
        if (entry || why != SFXP_SKIP_LOADED) fail("A: loaded verdict differs");
        break;
    case PLAY_LOAD:
        if (entry != s_play_entry) fail("A: entry address differs from Load's ECX");
        break;
    default:
        break;
    }
    world_load(s_tmp);
}

static void part_a(void)
{
    static const uint32_t edge_spans[] = {
        0u, 1u, 0x1b7u, 0x1b8u, 0x1b9u, 0x370u, 2u * 0x1b8u + 0x1b7u,
        MAX_ENTRIES * 0x1b8u, 0x7fffffffu, 0x7ffffe48u, 0x80000000u,
        0xfffffe48u, 0xffffffffu, 0x80000001u, 0x7fffff00u, 0x00123456u };
    unsigned k, j;
    char name[96];

    s_case = name;
    for (k = 0; k < 1200u + sizeof edge_spans / sizeof edge_spans[0]; ++k) {
        uint32_t span = k < sizeof edge_spans / sizeof edge_spans[0]
            ? edge_spans[k]
            : (k & 1u) ? lcg() * 0x101u : (lcg() & 0x3ffffu);
        /* Play's count: (span/0x1b8) rounded toward zero, signed span */
        int64_t q = (int64_t)(int32_t)span / 0x1b8;
        int32_t count = (int32_t)q;
        /* entry(count - 1) = the last slot, so ids count-1-j are mapped */
        uint32_t last = A(s_entries) + (MAX_ENTRIES - 1u) * ENTRY_BYTES;
        uint32_t begin = count > 0 ? last - (uint32_t)(count - 1) * ENTRY_BYTES
                                   : A(s_entries);
        int32_t ids[12];
        unsigned n = 0;
        snprintf(name, sizeof name, "A span=%08x", span);
        ids[n++] = count; ids[n++] = count + 1; ids[n++] = -1;
        ids[n++] = (int32_t)0x80000000u; ids[n++] = 0x7fffffff;
        ids[n++] = count + (int32_t)(lcg() & 0xffffu);
        if (count > 0) {
            ids[n++] = count - 1;
            for (j = 0; j < 5u; ++j) {
                uint32_t back = lcg() % MAX_ENTRIES;
                if ((int64_t)count - 1 - (int64_t)back >= 0)
                    ids[n++] = count - 1 - (int32_t)back;
            }
        }
        for (j = 0; j < n; ++j) {
            /* every mapped slot gets a random state */
            if (ids[j] >= 0 && ids[j] < count &&
                    (uint32_t)(count - 1 - ids[j]) < MAX_ENTRIES) {
                static const uint32_t samples[] = { 0u, 1u, 2u, 5u, 0x80000000u,
                                                    0xffffffffu };
                static const uint8_t loaded[] = { 0u, 0u, 1u, 0x80u, 0xffu };
                uint32_t e = begin + (uint32_t)ids[j] * ENTRY_BYTES;
                set_entry(e, samples[lcg() % 6u], loaded[lcg() % 5u]);
                if (ids[j] == count - 1)
                    ++cov_a_boundary;
            }
            part_a_one(span, ids[j], begin);
        }
    }
}

/* ---- Part B: whole worlds -------------------------------------------------- */
typedef struct bcase {
    int eager, audio_off, terminal, null_manager, null_begin, reversed;
    int level_fault, rewrite;
    uint32_t stage, type, stage_after, type_after;
    uint32_t entries;             /* vector length */
    uint32_t heap_used;
    uint32_t pool;                /* OpenAL pool free bytes */
    int pool_exact;               /* as `exact`, for the pool rule */
    int fault_at, breach_at;      /* index in the native list, -1 none */
    int exact;                    /* 0 none, 1 exact fit, 2 one byte short,
                                   * 3 below the reserve; first loadable id */
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    /* the extension's whole-list guard, after floor lists that load
     * nothing: 1 exact fit, 2 one byte short, 3 below the reserve/margin */
    int fight_exact, fight_pool_exact;
    int fight_none;               /* every fight id already loaded */
    int fight_tail;               /* exact fit, only the two smallest
                                   * fight ids loadable: per-sound declines */
    int fight_stop;               /* 1 a Load faults, 2 breaches ESP, inside
                                   * the fight list */
#endif
} bcase;

/* The native order: [water,] generic -- the floor lists, s_floor_n ids --
 * then, with the extension, the fight list. */
static unsigned s_list_n, s_floor_n;
static int32_t s_list[48];
static uint16_t s_list_kib[48];

static void build_list(int water)
{
    size_t i;
    s_list_n = 0;
    if (water)
        for (i = 0; i < sizeof s_water / sizeof s_water[0]; ++i) {
            s_list_kib[s_list_n] = s_water[i].kib;
            s_list[s_list_n++] = s_water[i].id;
        }
    for (i = 0; i < sizeof s_generic / sizeof s_generic[0]; ++i) {
        s_list_kib[s_list_n] = s_generic[i].kib;
        s_list[s_list_n++] = s_generic[i].id;
    }
    s_floor_n = s_list_n;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    for (i = 0; i < sizeof s_fight / sizeof s_fight[0]; ++i) {
        s_list_kib[s_list_n] = s_fight[i].kib;
        s_list[s_list_n++] = s_fight[i].id;
    }
#endif
}

static void world_build(const bcase *k)
{
    const uint32_t sfx = sfx_addr();
    uint32_t i;

    memset(s_manager, 0, sizeof s_manager);
    memset(s_entries, 0, sizeof s_entries);
    for (i = 0; i < MAX_ENTRIES; ++i) {
        uint32_t e = A(s_entries) + i * ENTRY_BYTES;
        uint32_t r = lcg();
        st32(e + 0x190U, (r & 7u) < 2u ? 0u : (r & 7u) == 7u ? 0xffffffffu
                                                             : (r >> 3) % 5u + 1u);
        st8(e + 0x198U, (r >> 8) % 4u == 0u ? (uint8_t)(1u + (r >> 12) % 255u) : 0u);
    }
    st32(sfx, k->null_begin ? 0u : A(s_entries));
    st32(sfx + 4U, (k->null_begin ? 0u : A(s_entries)) +
                   (k->reversed ? (uint32_t)-(int32_t)ENTRY_BYTES
                                : k->entries * ENTRY_BYTES + (lcg() % ENTRY_BYTES)));
    st32(sfx + 8U, A(s_entries) + MAX_ENTRIES * ENTRY_BYTES);  /* capacity */
    st32(sfx + 0xcU, lcg() & 0xfffU);
    st32(VA(0x007fd680u), k->null_manager ? 0u : A(s_manager));
    st8(VA(0x007e7d44u), k->audio_off ? 0u : (uint8_t)(1u + lcg() % 255u));
    memset(s_level, 0xee, sizeof s_level);
    st32(A(s_level) + 0U, k->stage);
    st32(A(s_level) + 4U, k->type);
    s_heap.used = k->heap_used;
    s_heap.pool = k->pool;
}

static void level_stub_params(const bcase *k, uint32_t seed)
{
    s_level_seed = seed;
    s_level_fault = k->level_fault;
    s_level_writes = k->rewrite;
    s_level_stage_after = k->stage_after;
    s_level_type_after = k->type_after;
}

/* The reference for one case.  Leaves the expected world in s_ref, the
 * expected CPU in *expect and the expected verdicts in the out counters. */
static uint32_t s_exp_skip[SFXP_SKIP_KINDS], s_exp_loads, s_exp_faults,
                s_exp_breaches, s_exp_level_kind, s_exp_water;
static int s_exp_floor_stop;              /* the floor lists stopped */
static load_call s_exp_calls[MAX_LOADS];

/* Play for s_list[from..to) on the reference world, rolling back the ones
 * the headroom/pool model declines.  Returns 0 where the replay must stop
 * (a Load that faults or breaches ESP), 1 at the end of the range. */
static int reference_walk(unsigned from, unsigned to, CPU *expect)
{
    const uint32_t sfx = sfx_addr();
    unsigned i;

    for (i = from; i < to; ++i) {
        const uint32_t used = s_heap.used;
        const uint32_t pool = s_heap.pool;
        const uint32_t room = used < 0x05100000U ? 0x05100000U - used : 0U;
        const uint32_t reserve = (uint32_t)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB * 1048576U;
        const uint32_t need = (uint32_t)s_list_kib[i] * 1024U;
        unsigned before_loads = s_load_count;
        int cls;

        world_save(s_tmp);
        cls = play_reference(s_list[i]);
        if (cls == PLAY_RANGE) { ++s_exp_skip[SFXP_SKIP_RANGE]; continue; }
        if (cls == PLAY_NO_SAMPLES) { ++s_exp_skip[SFXP_SKIP_NO_SAMPLES]; continue; }
        if (cls == PLAY_LOADED) { ++s_exp_skip[SFXP_SKIP_LOADED]; continue; }
        if (room < reserve || room - reserve < need) {
            /* the model declines: the first Play loads it later */
            world_load(s_tmp);
            s_load_count = before_loads;
            ++s_exp_skip[SFXP_SKIP_HEADROOM];
            continue;
        }
        if (pool < 0x00200000U || pool - 0x00200000U < need) {
            world_load(s_tmp);
            s_load_count = before_loads;
            ++s_exp_skip[SFXP_SKIP_POOL];
            continue;
        }
        if (s_exp_loads < MAX_LOADS)
            s_exp_calls[s_exp_loads] = s_loads[before_loads < MAX_LOADS ? before_loads : 0];
        ++s_exp_loads;
        if (s_play_entry == s_fault_entry || s_play_entry == s_breach_entry) {
            /* stop after Load's effects, before 00488ea1 inc [ebx+0xc] */
            st32(sfx + 0xcU, ld32(sfx + 0xcU) - 1U);
            if (s_play_entry == s_fault_entry) {
                ++s_exp_faults;
                expect->fault = k_load_fault;
            } else {
                ++s_exp_breaches;
            }
            return 0;
        }
    }
    return 1;
}

#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
/* The extension's expected outcome. */
enum { FX_NONE, FX_HEAP, FX_POOL, FX_WALKED, FX_STOPPED, FX_KINDS };
static unsigned s_exp_fight_kind;
static uint32_t s_exp_fight_need, s_exp_fight_loadable, s_exp_fight_mask,
                s_exp_fight_room, s_exp_fight_pool, s_exp_fight_loads,
                s_exp_fight_faults, s_exp_fight_breaches,
                s_exp_fight_skip[SFXP_SKIP_KINDS];

/* Which fight ids a Play would load now, decided by the translated Play
 * itself on the current world (each one rolled back): the bytes they need
 * (their kib from the list), how many, and a bit per list index. */
static void fight_need_reference(uint32_t *need, uint32_t *loadable,
                                 uint32_t *mask)
{
    const unsigned loads = s_load_count;
    unsigned i;

    *need = *loadable = *mask = 0U;
    world_save(s_tmp);
    for (i = s_floor_n; i < s_list_n; ++i) {
        const int cls = play_reference(s_list[i]);
        world_load(s_tmp);
        s_load_count = loads;
        if (cls == PLAY_LOAD) {
            *need += (uint32_t)s_list_kib[i] * 1024U;
            ++*loadable;
            *mask |= 1U << (i - s_floor_n);
        }
    }
}

/* After floor lists that completed: decline the whole list on the heap or
 * pool rule (nothing loadable: no guard), else walk it like the others. */
static void reference_fight(CPU *expect)
{
    const uint32_t used = s_heap.used;
    const uint32_t pool = s_heap.pool;
    const uint32_t room = used < 0x05100000U ? 0x05100000U - used : 0U;
    const uint32_t reserve = (uint32_t)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB * 1048576U;
    const uint32_t loads0 = s_exp_loads, faults0 = s_exp_faults,
                   breaches0 = s_exp_breaches;
    uint32_t skip0[SFXP_SKIP_KINDS], need, loadable, mask;
    unsigned i;

    fight_need_reference(&need, &loadable, &mask);
    s_exp_fight_need = need;
    s_exp_fight_loadable = loadable;
    s_exp_fight_mask = mask;
    s_exp_fight_room = room;
    s_exp_fight_pool = pool;
    if (loadable != 0U && (room < reserve || room - reserve < need)) {
        s_exp_fight_kind = FX_HEAP;
        return;
    }
    if (loadable != 0U && (pool < 0x00200000U || pool - 0x00200000U < need)) {
        s_exp_fight_kind = FX_POOL;
        return;
    }
    memcpy(skip0, s_exp_skip, sizeof skip0);
    s_exp_fight_kind = reference_walk(s_floor_n, s_list_n, expect)
        ? FX_WALKED : FX_STOPPED;
    s_exp_fight_loads = s_exp_loads - loads0;
    s_exp_fight_faults = s_exp_faults - faults0;
    s_exp_fight_breaches = s_exp_breaches - breaches0;
    for (i = 0; i < SFXP_SKIP_KINDS; ++i)
        s_exp_fight_skip[i] = s_exp_skip[i] - skip0[i];
}
#endif

static void reference_case(const bcase *k, const CPU *entry_cpu, CPU *expect,
                           uint32_t seed)
{
    uint32_t stage, type;
    int water;

    memset(s_exp_skip, 0, sizeof s_exp_skip);
    s_exp_loads = s_exp_faults = s_exp_breaches = 0;
    s_exp_floor_stop = 0;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    s_exp_fight_kind = FX_NONE;
    s_exp_fight_loads = s_exp_fight_faults = s_exp_fight_breaches = 0U;
    memset(s_exp_fight_skip, 0, sizeof s_exp_fight_skip);
#endif
    cpu_copy(expect, entry_cpu);
    level_stub_params(k, seed);
    oracle_level_init(expect);                 /* the same __real_ */
    if (expect->fault) { s_exp_level_kind = SFXP_LEVEL_FAULT; return; }
    if (k->eager) { s_exp_level_kind = SFXP_LEVEL_EAGER; return; }
    if (k->audio_off) { s_exp_level_kind = SFXP_LEVEL_AUDIO_OFF; return; }
    if (k->terminal) { s_exp_level_kind = SFXP_LEVEL_TERMINAL; return; }
    if (k->null_manager || k->null_begin || k->reversed) {
        s_exp_level_kind = SFXP_LEVEL_NO_MANAGER;
        return;
    }
    s_exp_level_kind = SFXP_LEVEL_REPLAYED;
    stage = ld32(A(s_level));
    type = ld32(A(s_level) + 4U);
    water = type == 4U && stage >= 1U && stage <= 2U;
    s_exp_water = (uint32_t)water;
    build_list(water);
    if (!reference_walk(0U, s_floor_n, expect)) {
        s_exp_floor_stop = 1;
        return;
    }
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    reference_fight(expect);
#endif
}

#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
/* Fight-list worlds, after the floor-list setup of run_case_b (its fault and
 * breach targets, exact and pool edges already placed). */
static void fight_setup(const bcase *k)
{
    const uint32_t reserve = (uint32_t)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB * 1048576U;
    unsigned j;

    if (k->fight_exact || k->fight_pool_exact || k->fight_none || k->fight_tail)
        /* the floor lists load nothing: the extension sees this heap */
        for (j = 0; j < s_floor_n; ++j)
            set_entry(A(s_entries) + (uint32_t)s_list[j] * ENTRY_BYTES, 1u, 1u);
    if (k->fight_none || k->fight_tail)
        for (j = s_floor_n; j < s_list_n; ++j)
            set_entry(A(s_entries) + (uint32_t)s_list[j] * ENTRY_BYTES, 1u, 1u);
    if (k->fight_tail) {
        /* the two smallest fight sounds, in list order: the Load stub may
         * use more heap than the first one's kib, so the second can meet a
         * per-sound decline after the whole-list guard passed */
        unsigned lo = s_floor_n, lo2 = s_floor_n + 1U;
        if (s_list_kib[lo2] < s_list_kib[lo]) { lo = lo2; lo2 = s_floor_n; }
        for (j = s_floor_n + 2U; j < s_list_n; ++j)
            if (s_list_kib[j] < s_list_kib[lo]) { lo2 = lo; lo = j; }
            else if (s_list_kib[j] < s_list_kib[lo2]) lo2 = j;
        set_entry(A(s_entries) + (uint32_t)s_list[lo] * ENTRY_BYTES, 1u, 0u);
        set_entry(A(s_entries) + (uint32_t)s_list[lo2] * ENTRY_BYTES, 1u, 0u);
    }
    if (k->fight_exact || k->fight_tail || k->fight_pool_exact) {
        uint32_t need, loadable, mask;
        fight_need_reference(&need, &loadable, &mask);
        if (k->fight_exact || k->fight_tail)
            s_heap.used = k->fight_exact == 2 ? 0x05100000U - reserve - need + 1U
                        : k->fight_exact == 3 ? 0x05100000U - reserve + 1U
                        : 0x05100000U - reserve - need;
        if (k->fight_pool_exact)
            s_heap.pool = k->fight_pool_exact == 1 ? 0x00200000U + need
                        : k->fight_pool_exact == 2 ? 0x00200000U + need - 1U
                        : 0x00200000U - 1U;
    }
    if (k->fight_stop) {
        const unsigned at = s_floor_n + lcg() % (s_list_n - s_floor_n);
        const uint32_t e = A(s_entries) + (uint32_t)s_list[at] * ENTRY_BYTES;
        if (k->fight_stop == 1) {
            s_fault_entry = e;
            set_entry(e, 2u, 0u);
        } else {
            s_breach_entry = e;
            set_entry(e, 3u, 0u);
        }
    }
}

static unsigned cov_fx[FX_KINDS], cov_fx_none, cov_fx_faults, cov_fx_breaches,
                cov_fx_exact, cov_fx_short, cov_fx_reserve, cov_fx_pool_exact,
                cov_fx_pool_short, cov_fx_pool_margin, cov_fx_walk_declines,
                cov_fx_fallback, cov_fx_loads, cov_fx_after_floor_decline;

/* The extension's verdict counters, its one line and that line's fields. */
static void check_fight(const sfxp_fight_counters *fb, unsigned lh, unsigned lp,
                        unsigned lw, unsigned lst)
{
    const uint32_t reserve = (uint32_t)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB * 1048576U;
    const unsigned kind = s_exp_fight_kind;
    unsigned v[12];
    int n;

    if (s_fight_counters.levels - fb->levels != (unsigned)(kind != FX_NONE) ||
            s_fight_counters.declined_heap - fb->declined_heap !=
                (unsigned)(kind == FX_HEAP) ||
            s_fight_counters.declined_pool - fb->declined_pool !=
                (unsigned)(kind == FX_POOL) ||
            s_fight_counters.stopped - fb->stopped != (unsigned)(kind == FX_STOPPED))
        fail("fight verdict counters");
    if (s_log_fight_heap - lh != (unsigned)(kind == FX_HEAP) ||
            s_log_fight_pool - lp != (unsigned)(kind == FX_POOL) ||
            s_log_fight_walked - lw != (unsigned)(kind == FX_WALKED) ||
            s_log_fight_stopped - lst != (unsigned)(kind == FX_STOPPED)) {
        fail("fight log lines");
        return;
    }
    switch (kind) {
    case FX_HEAP:
        n = sscanf(s_fight_line, "KAGE VITA SFX PRELOAD: fight level=%u declined "
                   "hr_kb=%u need_kb=%u sounds=%u mask=0x%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4]);
        if (n != 5 || v[0] != s_counters.levels || v[1] != s_exp_fight_room >> 10 ||
                v[2] != (s_exp_fight_need + reserve) >> 10 ||
                v[3] != s_exp_fight_loadable || v[4] != s_exp_fight_mask)
            fail("fight heap decline line fields");
        break;
    case FX_POOL:
        n = sscanf(s_fight_line, "KAGE VITA SFX PRELOAD: fight level=%u declined "
                   "pool_kb=%u need_kb=%u sounds=%u mask=0x%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4]);
        if (n != 5 || v[0] != s_counters.levels || v[1] != s_exp_fight_pool >> 10 ||
                v[2] != (s_exp_fight_need + 0x00200000U) >> 10 ||
                v[3] != s_exp_fight_loadable || v[4] != s_exp_fight_mask)
            fail("fight pool decline line fields");
        break;
    case FX_WALKED: {
        const uint32_t room1 = s_heap.used < 0x05100000U
            ? 0x05100000U - s_heap.used : 0U;
        n = sscanf(s_fight_line, "KAGE VITA SFX PRELOAD: fight level=%u loaded=%u "
                   "skip(l,r,s,h,p)=%u,%u,%u,%u,%u us=%*u need_kb=%u hr_kb=%u..%u "
                   "pool_kb=%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &v[7], &v[8], &v[9], &v[10]);
        if (n != 11 || v[0] != s_counters.levels || v[1] != s_exp_fight_loads ||
                v[2] != s_exp_fight_skip[0] || v[3] != s_exp_fight_skip[1] ||
                v[4] != s_exp_fight_skip[2] || v[5] != s_exp_fight_skip[3] ||
                v[6] != s_exp_fight_skip[4] || v[7] != s_exp_fight_need >> 10 ||
                v[8] != s_exp_fight_room >> 10 || v[9] != room1 >> 10 ||
                v[10] != (s_exp_fight_pool == UINT32_MAX ? 0U
                                                         : s_exp_fight_pool >> 10))
            fail("fight walk line fields");
        break;
    }
    case FX_STOPPED:
        n = sscanf(s_fight_line, "KAGE VITA SFX PRELOAD: fight level=%u stopped "
                   "fault=%u breach=%u loaded=%u", &v[0], &v[1], &v[2], &v[3]);
        if (n != 4 || v[0] != s_counters.levels || v[1] != s_exp_fight_faults ||
                v[2] != s_exp_fight_breaches || v[3] != s_exp_fight_loads - 1U)
            fail("fight stop line fields");
        break;
    default:
        break;
    }
}

static void cover_fight(const bcase *k)
{
    const unsigned kind = s_exp_fight_kind;
    ++cov_fx[kind];
    if (kind == FX_WALKED && s_exp_fight_loadable == 0U) ++cov_fx_none;
    cov_fx_faults += s_exp_fight_faults;
    cov_fx_breaches += s_exp_fight_breaches;
    cov_fx_loads += s_exp_fight_loads;
    cov_fx_walk_declines += s_exp_fight_skip[SFXP_SKIP_HEADROOM] +
                            s_exp_fight_skip[SFXP_SKIP_POOL];
    if (k->fight_exact == 1 && kind == FX_WALKED && s_exp_fight_loads) ++cov_fx_exact;
    if (k->fight_exact == 2 && kind == FX_HEAP) ++cov_fx_short;
    if (k->fight_exact == 3 && kind == FX_HEAP) ++cov_fx_reserve;
    if (k->fight_pool_exact == 1 && kind == FX_WALKED && s_exp_fight_loads)
        ++cov_fx_pool_exact;
    if (k->fight_pool_exact == 2 && kind == FX_POOL) ++cov_fx_pool_short;
    if (k->fight_pool_exact == 3 && kind == FX_POOL) ++cov_fx_pool_margin;
    if (k->pool == UINT32_MAX && kind == FX_WALKED && s_exp_fight_loads)
        ++cov_fx_fallback;
    /* a floor sound declined, the extension still asked (and decided) */
    if (kind != FX_NONE && (s_exp_skip[SFXP_SKIP_HEADROOM] +
                            s_exp_skip[SFXP_SKIP_POOL] >
                            s_exp_fight_skip[SFXP_SKIP_HEADROOM] +
                            s_exp_fight_skip[SFXP_SKIP_POOL]))
        ++cov_fx_after_floor_decline;
}
#endif

static void run_case_b(const bcase *k, const char *name)
{
    CPU entry, native, expect;
    const uint32_t seed = lcg();
    unsigned load_before, i;
    sfxp_counters before;
    unsigned la = s_log_sound, ld = s_log_declined, ll = s_log_level,
             ls = s_log_stopped;
    uint32_t entry_esp;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    const sfxp_fight_counters fight_before = s_fight_counters;
    const unsigned lfh = s_log_fight_heap, lfp = s_log_fight_pool,
                   lfw = s_log_fight_walked, lfs = s_log_fight_stopped;
#endif

    s_case = name;
    ++cov_cases;
    s_materialization_ready = !k->eager;
    s_heap_terminal = k->terminal;
    world_build(k);
    /* fault/breach targets: the k-th id of the native list, as an entry */
    s_fault_entry = s_breach_entry = 0xffffffffu;
    {
        int water = (k->rewrite ? k->type_after : k->type) == 4U &&
                    (k->rewrite ? k->stage_after : k->stage) >= 1U &&
                    (k->rewrite ? k->stage_after : k->stage) <= 2U;
        build_list(water);
        if (k->fault_at >= 0 && (unsigned)k->fault_at < s_list_n) {
            s_fault_entry = A(s_entries) + (uint32_t)s_list[k->fault_at] * ENTRY_BYTES;
            set_entry(s_fault_entry, 2u, 0u);
        }
        if (k->breach_at >= 0 && (unsigned)k->breach_at < s_list_n) {
            s_breach_entry = A(s_entries) + (uint32_t)s_list[k->breach_at] * ENTRY_BYTES;
            set_entry(s_breach_entry, 3u, 0u);
        }
        if (k->exact) {
            /* the first id of the list becomes loadable; size the heap so
             * the rule is exactly met / one byte short / under the reserve */
            const uint32_t reserve = (uint32_t)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB * 1048576U;
            const uint32_t need = (uint32_t)s_list_kib[0] * 1024U;
            set_entry(A(s_entries) + (uint32_t)s_list[0] * ENTRY_BYTES, 1u, 0u);
            s_heap.used = k->exact == 1 ? 0x05100000U - reserve - need
                        : k->exact == 2 ? 0x05100000U - reserve - need + 1U
                        : 0x05100000U - reserve + 1U;
        }
        if (k->pool_exact) {
            /* the same three edges for the pool rule (2 MiB margin) */
            const uint32_t need = (uint32_t)s_list_kib[0] * 1024U;
            set_entry(A(s_entries) + (uint32_t)s_list[0] * ENTRY_BYTES, 1u, 0u);
            s_heap.pool = k->pool_exact == 1 ? 0x00200000U + need
                        : k->pool_exact == 2 ? 0x00200000U + need - 1U
                        : 0x00200000U - 1U;
        }
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
        fight_setup(k);
#endif
    }
    cpu_fresh(&entry);
    entry.ecx = A(s_level);
    gpush(&entry, 0x0031e0a5U);       /* a caller's return word */
    entry_esp = entry.esp;

    world_save(s_snap);
    /* reference */
    load_before = s_load_count;
    reference_case(k, &entry, &expect, seed);
    world_save(s_ref);
    /* native */
    world_load(s_snap);
    s_load_count = load_before;
    cpu_copy(&native, &entry);
    memcpy(s_stack_snap, s_stack, sizeof s_stack);
    before = s_counters;
    level_stub_params(k, seed);
    __wrap_sub_00311e60(&native);

    if (s_level_this != A(s_level))
        fail("Level::Init did not receive the entry ECX");
    if (!world_equal(s_ref))
        fail("world differs from the reference");
    if (expect.fault == NULL || native.fault == NULL) {
        if (!cpu_arch_equal(&native, &expect)) {
            fail("CPU state differs from Level::Init's return state");
            printf("  exp eax=%08x ecx=%08x edx=%08x esp=%08x fl.a=%08x le=%u\n"
                   "  nat eax=%08x ecx=%08x edx=%08x esp=%08x fl.a=%08x le=%u\n",
                   expect.eax, expect.ecx, expect.edx, expect.esp, expect.fl.f_a,
                   expect.last_error, native.eax, native.ecx, native.edx,
                   native.esp, native.fl.f_a, native.last_error);
        }
    } else if (native.fault != expect.fault) {
        fail("fault not left for the guarded driver");
    }
    if (native.esp != entry_esp + 4U && !native.fault && !expect.fault)
        fail("ESP is not Level::Init's return ESP");
    /* the caller's frame, from Level::Init's return ESP up, is untouched */
    {
        const uint32_t from = (entry_esp + 4U - A(&s_stack[0])) / 4U;
        if (memcmp(&s_stack[from], &s_stack_snap[from],
                   (STACK_WORDS - from) * 4U) != 0)
            fail("the replay wrote into the caller's frame");
    }
    /* the Load calls: same ECX and return word, at the wrapper's own ESP */
    if (s_load_count - load_before != s_exp_loads)
        fail("number of Load calls differs");
    for (i = 0; i < s_exp_loads && load_before + i < MAX_LOADS; ++i) {
        const load_call *n = &s_loads[load_before + i];
        if (n->ecx != s_exp_calls[i].ecx || n->ret != 0x00488e91U ||
                s_exp_calls[i].ret != 0x00488e91U || n->esp != entry_esp)
            fail("Load call context differs");
    }
    /* verdict counters */
    if (s_counters.levels != before.levels + 1U)
        fail("levels counter");
    for (i = 0; i < SFXP_LEVEL_KINDS; ++i)
        if (s_counters.level[i] - before.level[i] != (unsigned)(i == s_exp_level_kind))
            fail("level verdict counter");
    if (s_exp_level_kind == SFXP_LEVEL_REPLAYED) {
        for (i = 0; i < SFXP_SKIP_KINDS; ++i)
            if (s_counters.skip[i] - before.skip[i] != s_exp_skip[i])
                fail("skip counter");
        if (s_counters.water_levels - before.water_levels != s_exp_water)
            fail("water gate");
    }
    if (s_counters.load_faults - before.load_faults != s_exp_faults ||
            s_counters.esp_breaches - before.esp_breaches != s_exp_breaches)
        fail("fault/breach counters");
    if (s_counters.loaded - before.loaded != s_exp_loads - s_exp_faults - s_exp_breaches)
        fail("loaded counter");
    /* log lines: one per load, per headroom decline, one per replayed
     * level (or a stop line) */
    if (s_log_sound - la != s_exp_loads - s_exp_faults - s_exp_breaches ||
            s_log_declined - ld != (s_exp_level_kind == SFXP_LEVEL_REPLAYED
                                    ? s_exp_skip[SFXP_SKIP_HEADROOM] +
                                      s_exp_skip[SFXP_SKIP_POOL] : 0u) ||
            s_log_level - ll != (unsigned)(s_exp_level_kind == SFXP_LEVEL_REPLAYED &&
                                           !s_exp_floor_stop) ||
            s_log_stopped - ls != (unsigned)s_exp_floor_stop)
        fail("log lines");
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    check_fight(&fight_before, lfh, lfp, lfw, lfs);
    cover_fight(k);
#endif

    /* coverage */
    ++cov_level[s_exp_level_kind];
    if (s_exp_level_kind == SFXP_LEVEL_REPLAYED) {
        if (s_exp_water) ++cov_water; else ++cov_dry;
        for (i = 0; i < SFXP_SKIP_KINDS; ++i)
            cov_skip[i] += s_exp_skip[i];
        if (k->rewrite) ++cov_rewrite;
        if (k->exact == 1 && s_exp_loads) ++cov_hr_exact;
        if (k->exact == 2 && s_exp_skip[SFXP_SKIP_HEADROOM]) ++cov_hr_short;
        if (k->exact == 3 && s_exp_skip[SFXP_SKIP_HEADROOM]) ++cov_hr_reserve;
        if (k->pool_exact == 1 && s_exp_loads) ++cov_pool_exact;
        if (k->pool_exact == 2 && s_exp_skip[SFXP_SKIP_POOL]) ++cov_pool_short;
        if (k->pool_exact == 3 && s_exp_skip[SFXP_SKIP_POOL]) ++cov_pool_reserve;
        if (k->pool == UINT32_MAX && s_exp_loads) ++cov_pool_fallback;
    }
    if (k->null_manager) ++cov_null_manager;
    if (k->null_begin) ++cov_null_begin;
    if (k->reversed) ++cov_reversed;
    cov_loads += s_exp_loads;
    cov_faults += s_exp_faults;
    cov_breaches += s_exp_breaches;
    s_load_count = 0;
}

static void part_b(void)
{
    static const uint32_t stages[][2] = {
        {1u, 4u}, {2u, 4u}, {1u, 0u}, {2u, 1u}, {3u, 4u}, {0u, 4u},
        {1u, 5u}, {4u, 4u}, {8u, 0u}, {2u, 4u}, {1u, 4u}, {13u, 2u} };
    char name[96];
    unsigned n;

    for (n = 0; n < 3000u; ++n) {
        bcase k;
        uint32_t r = lcg();
        const uint32_t *st = stages[r % (sizeof stages / sizeof stages[0])];
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
        /* own draws: 17..23 are the fight kinds, 24..31 plain worlds */
        const uint32_t kind = lcg() % 32u;
#else
        const uint32_t kind = (r >> 12) % 24u;
#endif
        /* the pool from its own draw: lcg() returns 24 bits, so the earlier
         * (r >> 24) % 5u was always 0 and the floor legs met pools 1..4 only
         * through their pool edges (the floor pins were re-pinned
         * with this draw, the fight legs drew it this way already) */
        const uint32_t pool_kind = lcg() % 5u;
        memset(&k, 0, sizeof k);
        k.stage = st[0];
        k.type = st[1];
        k.fault_at = k.breach_at = -1;
        k.entries = (r >> 4) % 4u == 0u ? (lcg() % 500u) : MAX_ENTRIES - (lcg() % 100u);
        /* heap: plenty, tight, past the reserve, past the contract */
        switch ((r >> 8) % 6u) {
        case 0: k.heap_used = 0x05100000U - 0x00a00000U - (lcg() & 0x7ffffU); break;
        case 1: k.heap_used = 0x05100000U - 0x00800000U - (lcg() % 0x00400000U); break;
        case 2: k.heap_used = 0x05100000U + (lcg() & 0xffffU); break;
        default: k.heap_used = 0x03000000U + (lcg() & 0xfffffU); break;
        }
        /* OpenAL pool: newlib fallback, roomy, tight, below the margin */
        switch (pool_kind) {
        case 0: k.pool = UINT32_MAX; break;
        case 1: k.pool = 0x00600000U + (lcg() & 0xfffffU); break;
        case 2: k.pool = 0x00200000U + (lcg() % 0x00500000U); break;
        case 3: k.pool = lcg() % 0x00200000U; break;
        default: k.pool = 0x00a00000U; break;
        }
        switch (kind) {
        case 0: k.eager = 1; break;
        case 1: k.audio_off = 1; break;
        case 2: k.terminal = 1; break;
        case 3: k.null_manager = 1; break;
        case 4: k.null_begin = 1; break;
        case 5: k.reversed = 1; break;
        case 6: k.level_fault = 1; break;
        case 7: case 8: k.fault_at = (int)(lcg() % 18u); k.heap_used = 0x03000000U;
            k.pool = UINT32_MAX; break;
        case 9: case 10: k.breach_at = (int)(lcg() % 18u); k.heap_used = 0x03000000U;
            k.pool = UINT32_MAX; break;
        case 11: k.exact = 1; k.pool = UINT32_MAX; break;
        case 12: k.exact = 2; k.pool = UINT32_MAX; break;
        case 13: k.exact = 3; k.pool = UINT32_MAX; break;
        case 14: k.pool_exact = 1; k.heap_used = 0x03000000U; break;
        case 15: k.pool_exact = 2; k.heap_used = 0x03000000U; break;
        case 16: k.pool_exact = 3; k.heap_used = 0x03000000U; break;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
        case 17: case 18: case 19: k.fight_exact = (int)kind - 16; k.pool = UINT32_MAX;
            break;
        case 20: k.fight_pool_exact = 1 + (int)(lcg() % 3u);
            k.heap_used = 0x03000000U; break;
        /* nothing to load on a heap below every reserve: no decline */
        case 21: k.fight_none = 1; k.heap_used = 0x05100000U - 0x00100000U;
            k.pool = 0x00100000U; break;
        case 22: k.fight_tail = 1; k.pool = UINT32_MAX; break;
        case 23: k.fight_stop = 1 + (int)(lcg() % 2u); k.heap_used = 0x03000000U;
            k.pool = UINT32_MAX; break;
#endif
        default: break;
        }
        if (k.exact || k.pool_exact || k.fault_at >= 0 || k.breach_at >= 0)
            k.entries = MAX_ENTRIES;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
        if (k.fight_exact || k.fight_pool_exact || k.fight_none || k.fight_tail ||
                k.fight_stop)
            k.entries = MAX_ENTRIES;
#endif
        if ((r >> 20) % 5u == 0u) {
            const uint32_t *after = stages[lcg() % (sizeof stages / sizeof stages[0])];
            k.rewrite = 1;
            k.stage_after = after[0];
            k.type_after = after[1];
        }
        snprintf(name, sizeof name, "B#%u stage=%u.%u%s", n, k.stage, k.type,
                 k.rewrite ? " (rewritten)" : "");
        run_case_b(&k, name);
    }
}

static int commit_page(uint32_t base)
{
    void *r = VirtualAlloc((LPVOID)(uintptr_t)base, ORACLE_PAGE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return r != NULL && (uintptr_t)r == base;
}

int main(void)
{
    unsigned i;
    CPU probe;
    uint32_t tls;

    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        if (!commit_page(VA(s_pages[i]))) {
            fprintf(stderr, "could not commit %08x (/LARGEADDRESSAWARE)\n",
                    VA(s_pages[i]));
            return 2;
        }
    /* Play's magic static (0x488e0a..0x488e1d): initialised in every world,
     * so the guard word is not above the thread's epoch. */
    cpu_fresh(&probe);
    tls = ld32(guest_fs_base(&probe) + 0x2cU);
    st32(tls, A(s_tls_block));
    s_tls_block[3] = 5u;
    st32(VA(0x00806e40u), 1u);
    st32(VA(0x007aa3b4u), 0x2f8e7d1bu);

    add_region(s_manager, sizeof s_manager);
    add_region(s_entries, sizeof s_entries);
    add_region(s_level, sizeof s_level);
    add_region(&s_heap, sizeof s_heap);
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        add_region((void *)(uintptr_t)VA(s_pages[i]), ORACLE_PAGE);

    part_a();
    part_b();

    printf("coverage A: cases=%u range=%u (neg %u) no_samples=%u loaded=%u "
           "load=%u boundary=%u\n", cov_a_cases, cov_a[PLAY_RANGE], cov_a_neg,
           cov_a[PLAY_NO_SAMPLES], cov_a[PLAY_LOADED], cov_a[PLAY_LOAD],
           cov_a_boundary);
    printf("coverage B: cases=%u replayed=%u fault=%u eager=%u audio_off=%u "
           "terminal=%u no_manager=%u (null %u, begin %u, reversed %u) "
           "water=%u dry=%u rewritten=%u\n", cov_cases,
           cov_level[SFXP_LEVEL_REPLAYED], cov_level[SFXP_LEVEL_FAULT],
           cov_level[SFXP_LEVEL_EAGER], cov_level[SFXP_LEVEL_AUDIO_OFF],
           cov_level[SFXP_LEVEL_TERMINAL], cov_level[SFXP_LEVEL_NO_MANAGER],
           cov_null_manager, cov_null_begin, cov_reversed, cov_water, cov_dry,
           cov_rewrite);
    printf("coverage B ids: loads=%u skip(l,r,s,h,p)=%u,%u,%u,%u,%u "
           "load_faults=%u esp_breaches=%u headroom(exact,short,reserve)="
           "%u,%u,%u pool(exact,short,reserve,fallback)=%u,%u,%u,%u\n",
           cov_loads, cov_skip[0], cov_skip[1], cov_skip[2], cov_skip[3],
           cov_skip[4], cov_faults, cov_breaches, cov_hr_exact, cov_hr_short,
           cov_hr_reserve, cov_pool_exact, cov_pool_short, cov_pool_reserve,
           cov_pool_fallback);
    printf("log lines: armed=%u sound=%u declined=%u level=%u stopped=%u other=%u\n",
           s_log_armed, s_log_sound, s_log_declined, s_log_level, s_log_stopped,
           s_log_other);
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    printf("coverage F: walked=%u (nothing loadable %u) heap=%u pool=%u "
           "stopped=%u (fault %u, breach %u) loads=%u walk_declines=%u "
           "after_floor_decline=%u heap(exact,short,reserve)=%u,%u,%u "
           "pool(exact,short,margin,fallback)=%u,%u,%u,%u\n",
           cov_fx[FX_WALKED], cov_fx_none, cov_fx[FX_HEAP], cov_fx[FX_POOL],
           cov_fx[FX_STOPPED], cov_fx_faults, cov_fx_breaches, cov_fx_loads,
           cov_fx_walk_declines, cov_fx_after_floor_decline, cov_fx_exact,
           cov_fx_short, cov_fx_reserve, cov_fx_pool_exact, cov_fx_pool_short,
           cov_fx_pool_margin, cov_fx_fallback);
    printf("fight log lines: heap=%u pool=%u walked=%u stopped=%u\n",
           s_log_fight_heap, s_log_fight_pool, s_log_fight_walked,
           s_log_fight_stopped);
    {
        const unsigned need_f[] = {
            cov_fx[FX_WALKED], cov_fx_none, cov_fx[FX_HEAP], cov_fx[FX_POOL],
            cov_fx[FX_STOPPED], cov_fx_faults, cov_fx_breaches, cov_fx_loads,
            cov_fx_walk_declines, cov_fx_after_floor_decline, cov_fx_exact,
            cov_fx_short, cov_fx_reserve, cov_fx_pool_exact, cov_fx_pool_short,
            cov_fx_pool_margin, cov_fx_fallback };
        for (i = 0; i < sizeof need_f / sizeof need_f[0]; ++i)
            if (!need_f[i]) {
                s_case = "coverage";
                fail("a fight branch was never exercised");
                printf("  fight coverage slot %u is zero\n", i);
            }
    }
#endif
    {
        const unsigned need[] = {
            cov_a[PLAY_RANGE], cov_a_neg, cov_a[PLAY_NO_SAMPLES],
            cov_a[PLAY_LOADED], cov_a[PLAY_LOAD], cov_a_boundary,
            cov_level[SFXP_LEVEL_REPLAYED], cov_level[SFXP_LEVEL_FAULT],
            cov_level[SFXP_LEVEL_EAGER], cov_level[SFXP_LEVEL_AUDIO_OFF],
            cov_level[SFXP_LEVEL_TERMINAL], cov_null_manager, cov_null_begin,
            cov_reversed, cov_water, cov_dry, cov_rewrite, cov_skip[0],
            cov_skip[1], cov_skip[2], cov_skip[3], cov_skip[4], cov_faults,
            cov_breaches, cov_hr_exact, cov_hr_short, cov_hr_reserve,
            cov_pool_exact, cov_pool_short, cov_pool_reserve,
            cov_pool_fallback };
        for (i = 0; i < sizeof need / sizeof need[0]; ++i)
            if (!need[i]) {
                s_case = "coverage";
                fail("a branch was never exercised");
                printf("  coverage slot %u is zero\n", i);
            }
    }
    if (s_log_armed != 1u || s_log_other != 0u)
        fail("banner / unexpected log lines");
    if (s_failures) {
        printf("Vita SFX floor preload oracle: FAIL; failures=%u\n", s_failures);
        return 1;
    }
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    printf("Vita SFX floor preload oracle: PASS; reserve_mb=%u; fight=%u; A=%u; "
           "B=%u; loads=%u; declines(h,p)=%u,%u; fight(w,h,p,s)=%u,%u,%u,%u\n",
           (unsigned)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB,
           (unsigned)(sizeof s_fight / sizeof s_fight[0]), cov_a_cases,
           cov_cases, cov_loads, cov_skip[SFXP_SKIP_HEADROOM],
           cov_skip[SFXP_SKIP_POOL], cov_fx[FX_WALKED], cov_fx[FX_HEAP],
           cov_fx[FX_POOL], cov_fx[FX_STOPPED]);
#else
    printf("Vita SFX floor preload oracle: PASS; reserve_mb=%u; A=%u; B=%u; "
           "loads=%u; declines(h,p)=%u,%u\n",
           (unsigned)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB, cov_a_cases,
           cov_cases, cov_loads, cov_skip[SFXP_SKIP_HEADROOM],
           cov_skip[SFXP_SKIP_POOL]);
#endif
    return 0;
}
