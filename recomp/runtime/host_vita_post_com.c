/* Target-local policy for the four imports immediately following COM setup. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_post_com.h"
#include "host_vita_import_id.h"
#include "vita_host_services.h"
#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL) || \
    defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
#include "platform.h"
#endif
#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL)
#include "host_vita_tgt_reciprocal.h"
#endif

typedef void (*vita_post_com_handler)(CPU *__restrict c);

typedef struct vita_post_com_import_entry {
    const char *name;
    vita_post_com_handler handler;
} vita_post_com_import_entry;

static uint32_t s_execution_state =
    ISAAC_VITA_POST_COM_EXECUTION_STATE_INITIAL;

static uint32_t vita_post_com_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_post_com_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

static void vita_post_com_stdcall_return(CPU *__restrict c,
                                         uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void vita_post_com_SteamAPI_Init(CPU *__restrict c)
{
    /* No Steam client exists on Vita.  False selects the game's established
     * offline path without manufacturing interfaces or callbacks. */
    c->eax = 0U;
    vita_post_com_cdecl_return(c);
}

static void vita_post_com_SetThreadExecutionState(CPU *__restrict c)
{
    uint32_t requested = vita_post_com_arg(c, 0U);
    uint32_t previous;

    /* Suspend/display policy belongs to the Vita frontend.  Preserve only
     * Win32's observable previous-state result, exactly as host_win32 does. */
    previous = s_execution_state;
    if (requested & 0x80000000U)
        s_execution_state = requested;
    c->eax = previous;
    vita_post_com_stdcall_return(c, 4U);
}

static void vita_post_com_timeGetDevCaps(CPU *__restrict c)
{
    uint32_t caps = vita_post_com_arg(c, 0U);
    uint32_t size = vita_post_com_arg(c, 1U);

    if (!caps || size < ISAAC_VITA_POST_COM_CAPS_SIZE) {
        c->eax = ISAAC_VITA_POST_COM_TIMERR_NOCANDO;
    } else {
        st32(caps, ISAAC_VITA_POST_COM_PERIOD_MIN);
        st32(caps + 4U, ISAAC_VITA_POST_COM_PERIOD_MAX);
        c->eax = ISAAC_VITA_POST_COM_TIMERR_NOERROR;
    }
    vita_post_com_stdcall_return(c, 8U);
}

static void vita_post_com_timeBeginPeriod(CPU *__restrict c)
{
    uint32_t period = vita_post_com_arg(c, 0U);

    c->eax = period >= ISAAC_VITA_POST_COM_PERIOD_MIN &&
             period <= ISAAC_VITA_POST_COM_PERIOD_MAX
                 ? ISAAC_VITA_POST_COM_TIMERR_NOERROR
                 : ISAAC_VITA_POST_COM_TIMERR_NOCANDO;
    vita_post_com_stdcall_return(c, 4U);
}

static void vita_post_com_timeEndPeriod(CPU *__restrict c)
{
    uint32_t period = vita_post_com_arg(c, 0U);

    c->eax = period >= ISAAC_VITA_POST_COM_PERIOD_MIN &&
             period <= ISAAC_VITA_POST_COM_PERIOD_MAX
                 ? ISAAC_VITA_POST_COM_TIMERR_NOERROR
                 : ISAAC_VITA_POST_COM_TIMERR_NOCANDO;
    vita_post_com_stdcall_return(c, 4U);
}

#if defined(ISAAC_VITA_PHASE_PROFILE)
/* ph120.c tgt: every guest timeGetTime import call.  The KAGE clear/present
 * hook (sub_00564f50) loops over its pending-release list calling it once per
 * element per pass, a volume no other record captured. */
uint32_t g_isaac_vita_post_com_time_get_time_calls;
#endif

#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
IsaacVitaClearLatchCounters g_isaac_vita_clear_latch_counters;
#endif

#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL)
/* ISAAC_VITA_TIMEGETTIME_RECIPROCAL: the OFF conversion
 * `(uint32_t)(t / UINT64_C(1000))` is a __aeabi_uldivmod call on the
 * Cortex-A9 (no UDIV); isaac_vita_tgt_ms_from_us computes the same integer
 * with four 32x32 multiplies (proof in host_vita_tgt_reciprocal.h).  The
 * first handler call reads its clock, then runs a self-test of 4096 values
 * against the library division and logs `KAGE VITA TGT RECIPROCAL:
 * selftest n=.. mismatch=.. use=..`; any mismatch makes this and every later
 * call use the division, so a broken reciprocal cannot reach the guest. */
#define TGT_RECIPROCAL_UNTESTED 0U
#define TGT_RECIPROCAL_USE 1U
#define TGT_RECIPROCAL_DIVIDE 2U
#define TGT_RECIPROCAL_SELFTEST_VALUES 4096U
#define TGT_RECIPROCAL_TIMING_REPS 1000U
static uint32_t s_tgt_reciprocal_state;
/* volatile: the self-test reference must be the library division the OFF
 * build calls, not a constant division the compiler could rewrite. */
static volatile uint64_t s_tgt_reciprocal_divisor = UINT64_C(1000);
#if defined(ISAAC_VITA_PHASE_PROFILE)
static volatile uint32_t s_tgt_reciprocal_sink;
#endif

static const uint64_t s_tgt_reciprocal_edges[] = {
    UINT64_C(0), UINT64_C(1), UINT64_C(7), UINT64_C(8), UINT64_C(999),
    UINT64_C(1000), UINT64_C(1001), UINT64_C(7999), UINT64_C(8000),
    UINT64_C(124999), UINT64_C(125000), UINT64_C(0xFFFFFFFF),
    UINT64_C(0x100000000), UINT64_C(4294967295999), UINT64_C(4294967296000),
    UINT64_C(0x1FFFFFFFFFFFFF), UINT64_C(0x20000000000000),
    UINT64_C(0x1FFFFFFFFFFFFFFF), UINT64_C(0x2000000000000000),
    UINT64_C(0x7FFFFFFFFFFFFFFF), UINT64_C(0x8000000000000000),
    UINT64_C(18446744073709550615), UINT64_C(18446744073709550616),
    UINT64_C(18446744073709550617), UINT64_C(0xFFFFFFFFFFFFFFFF),
};

static uint64_t vita_post_com_tgt_selftest_value(uint32_t index,
                                                 uint64_t *lcg)
{
    if (index < sizeof s_tgt_reciprocal_edges /
                    sizeof s_tgt_reciprocal_edges[0])
        return s_tgt_reciprocal_edges[index];
    /* MMIX LCG; the shift spreads the samples over every magnitude. */
    *lcg = *lcg * UINT64_C(6364136223846793005) +
           UINT64_C(1442695040888963407);
    return *lcg >> (index & 63U);
}

static GUEST_STACK_COLD_NOINLINE void vita_post_com_tgt_selftest(void)
{
    const uint64_t divisor = s_tgt_reciprocal_divisor;
    uint64_t lcg = UINT64_C(0x20261025);
    uint32_t index, mismatches = 0U;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    uint64_t start, divided, multiplied;
    uint32_t sink = 0U;
#endif

    for (index = 0U; index < TGT_RECIPROCAL_SELFTEST_VALUES; ++index) {
        const uint64_t t = vita_post_com_tgt_selftest_value(index, &lcg);

        if (isaac_vita_tgt_ms_from_us(t) != (uint32_t)(t / divisor))
            ++mismatches;
    }
    s_tgt_reciprocal_state =
        mismatches ? TGT_RECIPROCAL_DIVIDE : TGT_RECIPROCAL_USE;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    /* B4: the per-call cost of both conversions on this CPU (microseconds
     * for TGT_RECIPROCAL_TIMING_REPS calls each, profile builds only). */
    start = isaac_vita_get_process_time();
    for (index = 0U; index < TGT_RECIPROCAL_TIMING_REPS; ++index)
        sink += (uint32_t)((start + UINT64_C(977) * index) / divisor);
    divided = isaac_vita_get_process_time();
    for (index = 0U; index < TGT_RECIPROCAL_TIMING_REPS; ++index)
        sink += isaac_vita_tgt_ms_from_us(start + UINT64_C(977) * index);
    multiplied = isaac_vita_get_process_time();
    s_tgt_reciprocal_sink = sink;
    isaac_vita_log(
        "KAGE VITA TGT RECIPROCAL: selftest n=%u mismatch=%u use=%s "
        "div_us=%u mul_us=%u reps=%u",
        (unsigned)TGT_RECIPROCAL_SELFTEST_VALUES, (unsigned)mismatches,
        mismatches ? "divide" : "reciprocal",
        (unsigned)(divided - start), (unsigned)(multiplied - divided),
        (unsigned)TGT_RECIPROCAL_TIMING_REPS);
#else
    isaac_vita_log(
        "KAGE VITA TGT RECIPROCAL: selftest n=%u mismatch=%u use=%s",
        (unsigned)TGT_RECIPROCAL_SELFTEST_VALUES, (unsigned)mismatches,
        mismatches ? "divide" : "reciprocal");
#endif
}

static GUEST_STACK_COLD_NOINLINE uint32_t vita_post_com_tgt_slow(uint64_t t)
{
    if (s_tgt_reciprocal_state == TGT_RECIPROCAL_UNTESTED)
        vita_post_com_tgt_selftest();
    return s_tgt_reciprocal_state == TGT_RECIPROCAL_USE
        ? isaac_vita_tgt_ms_from_us(t)
        : (uint32_t)(t / UINT64_C(1000));
}
#endif

#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL) || \
    defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
/* One real clock read, narrowed exactly like the OFF handler. */
static uint32_t vita_post_com_fresh_ms(void)
{
#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
    ++g_isaac_vita_clear_latch_counters.clock_reads;
#endif
#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL)
    {
        const uint64_t t = isaac_vita_get_process_time();

        if (GUEST_STACK_UNLIKELY(
                s_tgt_reciprocal_state != TGT_RECIPROCAL_USE))
            return vita_post_com_tgt_slow(t);
        return isaac_vita_tgt_ms_from_us(t);
    }
#else
    return (uint32_t)(isaac_vita_get_process_time() / UINT64_C(1000));
#endif
}
#endif

#if defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
#if ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE != 1 && \
    ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE != 2
#error ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE must be 1 (OBSERVE) or 2 (ON)
#endif
#if ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE == 1 && \
    !defined(ISAAC_VITA_PHASE_PROFILE)
#error ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE only counts: it needs ISAAC_VITA_PHASE_PROFILE
#endif
/* ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH: the KAGE Clear hook sub_00564f50
 * (guest_0167.c:1987) and its twin sub_00561f40 (guest_0166.c:14827) walk the
 * pending-release vector [0x987e7d1c, 0x987e7d20) and call timeGetTime once
 * per element to test `(now - [image + 0x74]) > 0x1388` (5 s).  The keep path
 * (0x565014..0x565021 / 0x561fa9..0x561fb6) is `mov eax,[end]; add esi,4;
 * add edi,4; cmp; jne` back to the call: no guest call, no store.  The erase
 * path calls FreeBatches (vt+0x74) and memmove (0x5ec358), leaves esi and
 * decrements [0x987e7d20].  So a call whose element pointer is the previous
 * call's plus 4, at the same site, on the same CPU, with the vector bounds
 * unchanged, follows a keep iteration with no guest code in between, and ON
 * answers it with that keep run's first clock read (the run's latch); every
 * other call reads the clock and re-arms.  The first iteration of a Clear
 * (esi == begin) can never match, so every Clear starts with a real read.
 * eax is overwritten on both paths (0x564fe2 / 0x565014), so the returned
 * value only feeds the jbe.  ON is timing-legal, not identical: it is the
 * execution the original program has when the keep run takes 0 ms, and an
 * element that reaches 5 s inside a run whose clock ticks is freed by the
 * next Clear instead (heap only, no GL call in FreeBatches' closure).
 * OBSERVE (1) always returns the fresh read and only counts what ON would
 * do on this trajectory (ph120.tl); ON = 2.  Every constant below is copied
 * from the listing comment it cites (test_vita_clear_expiry_latch.py checks
 * the citation and the twin's instruction). */
#define TGT_LATCH_SITE_A 0x00564FD6U /* 00564fd0  call dword ptr [0x986064c0] */
#define TGT_LATCH_SITE_B 0x00561F6BU /* 00561f65  call dword ptr [0x986064c0] */
#define TGT_LATCH_VEC_BEGIN 0x007E7D1CU /* 00564fbd  mov esi, dword ptr [0x987e7d1c] */
#define TGT_LATCH_VEC_END 0x007E7D20U /* 00564fc3  cmp esi, dword ptr [0x987e7d20] */
#define TGT_LATCH_STAMP 0x74U /* 00564fd8  sub eax, dword ptr [ecx + 0x74] */
#define TGT_LATCH_LIMIT_MS 0x1388U /* 00564fdb  cmp eax, 0x1388 */

typedef struct vita_post_com_clear_latch {
    const CPU *cpu;   /* the CPU that armed it; NULL: never armed */
    uint32_t site;    /* its return word (a bare RVA, as GPUSH pushed it) */
    uint32_t esi;     /* element pointer of the latest site call */
    uint32_t begin;   /* [0x987e7d1c] when armed */
    uint32_t end;     /* [0x987e7d20] when armed */
    uint32_t ms;      /* the arming read */
} vita_post_com_clear_latch;
static vita_post_com_clear_latch s_clear_latch;
static uint32_t s_clear_latch_banner;

/* The word stdcall_return's gpop is about to read, or 0 when that gpop would
 * fault: the same ownership and range tests as gpop_at (GUEST_STACK_REQUIRED:
 * guest.c:1074-1091 and guest.h:1014-1040; legacy: guest_stack_legacy_guard,
 * guest.h:827-843), without the fault.  0 is no site, so a bad stack takes
 * the fresh path and faults in vita_post_com_stdcall_return exactly as OFF
 * does. */
static uint32_t vita_post_com_return_word(const CPU *__restrict c)
{
    uint32_t capacity, offset;

#if !GUEST_STACK_REQUIRED
    if (!c->stack_owner) {
        if (c->stack_floor || c->stack_ceiling || c->stack_low_water)
            return 0U;
        return ld32(c->esp);
    }
#endif
    if (!guest_stack_fast_bound(c))
        return 0U;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (capacity < 4U || offset > capacity - 4U)
        return 0U;
    return ld32(c->esp);
}

static GUEST_STACK_COLD_NOINLINE void vita_post_com_clear_latch_banner(void)
{
    s_clear_latch_banner = 1U;
    isaac_vita_log(
        "KAGE VITA CLEAR LATCH: mode=%u sites=0x%08x,0x%08x vec=0x%08x "
        "stamp=+0x%x limit=0x%x",
        (unsigned)ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE,
        (unsigned)TGT_LATCH_SITE_A, (unsigned)TGT_LATCH_SITE_B,
        (unsigned)TGT_LATCH_VEC_BEGIN, (unsigned)TGT_LATCH_STAMP,
        (unsigned)TGT_LATCH_LIMIT_MS);
}

static uint32_t vita_post_com_clear_latch_ms(CPU *__restrict c)
{
    vita_post_com_clear_latch *const latch = &s_clear_latch;
    const uint32_t site = vita_post_com_return_word(c);
    uint32_t begin, end, ms;
    int eligible;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    IsaacVitaClearLatchCounters *const k = &g_isaac_vita_clear_latch_counters;
    uint32_t stamp;
#endif

    if (site != TGT_LATCH_SITE_A && site != TGT_LATCH_SITE_B)
        return vita_post_com_fresh_ms();   /* the other 364 sites */
    if (GUEST_STACK_UNLIKELY(!s_clear_latch_banner))
        vita_post_com_clear_latch_banner();
    begin = ld32(GUEST_IMAGE_BASE + TGT_LATCH_VEC_BEGIN);
    end = ld32(GUEST_IMAGE_BASE + TGT_LATCH_VEC_END);
    eligible = latch->cpu == c &&                 /* key: cpu */
               latch->site == site &&             /* key: site */
               c->esi == latch->esi + 4U &&       /* key: esi */
               c->esi != begin &&                 /* key: first */
               latch->begin == begin &&           /* key: begin */
               latch->end == end;                 /* key: end */
#if defined(ISAAC_VITA_PHASE_PROFILE)
    ++k->site_calls;
    if (latch->cpu && latch->cpu != c)
        ++k->foreign_cpu;
    /* The two words the site's next instructions read (mov ecx,[esi];
     * sub eax,[ecx+0x74]), so this cannot fault where OFF would not. */
    stamp = ld32(ld32(c->esi) + TGT_LATCH_STAMP);
#endif
    if (eligible) {
        latch->esi = c->esi;
#if ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE == 2
        ms = latch->ms;
#else
        ms = vita_post_com_fresh_ms();
        if (ms != latch->ms)
            ++k->ticks;
        if (((uint32_t)(ms - stamp) > TGT_LATCH_LIMIT_MS) !=
            ((uint32_t)(latch->ms - stamp) > TGT_LATCH_LIMIT_MS))
            ++k->flips;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
        ++k->hits;
        if ((int32_t)(stamp - latch->ms) > 0)
            ++k->stamp_ahead;
#endif
    } else {
        ms = vita_post_com_fresh_ms();
        latch->cpu = c;
        latch->site = site;
        latch->esi = c->esi;
        latch->begin = begin;
        latch->end = end;
        latch->ms = ms;
#if defined(ISAAC_VITA_PHASE_PROFILE)
        ++k->fresh;
#endif
    }
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if ((uint32_t)(ms - stamp) > TGT_LATCH_LIMIT_MS)
        ++k->expiries;
#endif
    return ms;
}
#endif

static void vita_post_com_timeGetTime(CPU *__restrict c)
{
#if defined(ISAAC_VITA_PHASE_PROFILE)
    ++g_isaac_vita_post_com_time_get_time_calls;
#endif
    /* timeGetTime is the low 32 bits of a monotonic millisecond counter.
     * Vita process time is monotonic microseconds and has the same wrap
     * behavior after this narrowing conversion. */
#if defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
    c->eax = vita_post_com_clear_latch_ms(c);
#elif defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL)
    c->eax = vita_post_com_fresh_ms();
#else
    c->eax = (uint32_t)(isaac_vita_get_process_time() / UINT64_C(1000));
#endif
    vita_post_com_stdcall_return(c, 0U);
}

static const vita_post_com_import_entry s_vita_post_com_imports[] = {
    { ISAAC_VITA_POST_COM_EXECUTION_STATE_NAME,
      vita_post_com_SetThreadExecutionState },
    { ISAAC_VITA_POST_COM_GET_CAPS_NAME, vita_post_com_timeGetDevCaps },
    { ISAAC_VITA_POST_COM_BEGIN_PERIOD_NAME, vita_post_com_timeBeginPeriod },
    { ISAAC_VITA_POST_COM_END_PERIOD_NAME, vita_post_com_timeEndPeriod },
    { ISAAC_VITA_POST_COM_GET_TIME_NAME, vita_post_com_timeGetTime },
    { ISAAC_VITA_POST_COM_STEAM_INIT_NAME, vita_post_com_SteamAPI_Init },
};

_Static_assert(sizeof s_vita_post_com_imports /
                   sizeof s_vita_post_com_imports[0] ==
                   ISAAC_VITA_POST_COM_IMPORT_COUNT,
               "Vita post-COM import inventory drifted");
_Static_assert(ISAAC_VITA_POST_COM_STEAM_INIT_ORDINAL +
                   ISAAC_VITA_POST_COM_BOOT_CALL_COUNT ==
                   ISAAC_VITA_POST_COM_NEXT_ORDINAL,
               "Vita post-COM boot denominator drifted");

const char *isaac_vita_post_com_import_name(uint32_t index)
{
    return index < ISAAC_VITA_POST_COM_IMPORT_COUNT
        ? s_vita_post_com_imports[index].name : NULL;
}

int isaac_vita_post_com_import_indexed(CPU *__restrict c, uint32_t index,
                                       unsigned *call_count)
{
    if (index >= ISAAC_VITA_POST_COM_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_post_com_imports[index].handler(c);
    return 1;
}

static int vita_post_com_dispatch(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_POST_COM_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_post_com_imports[index].name) == 0)
            return isaac_vita_post_com_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_post_com_import(CPU *__restrict c, const char *name)
{
    return vita_post_com_dispatch(c, name, NULL);
}

int isaac_vita_post_com_import_counted(CPU *__restrict c, const char *name,
                                       unsigned *call_count)
{
    return vita_post_com_dispatch(c, name, call_count);
}
