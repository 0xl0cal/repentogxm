/* White-box oracle: compile this one translation unit, no SDK or guest image.
 * clang -std=c11 -Wall -Wextra -Werror kage_vita_deep_profile_host_test.c -o test
 * Production reducer code is included unchanged; only clock/thread/log sources
 * are replaced. Script/native execution is never involved in this test. */
#if defined(KVD_HOST_TEST_DISABLED)
#define ISAAC_VITA_DEEP_PROFILE 0
#include "../../runtime/kage_vita_deep_profile.h"
#include <stdio.h>
int main(void)
{
    unsigned evaluated = 0U;
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_PNG, ++evaluated);
    KAGE_VITA_DEEP_SCOPE(++evaluated);
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_ANM2_LOAD, ++evaluated);
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_ANM2_GRAPHICS, ++evaluated);
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_ROOM_STATE_RESET, ++evaluated);
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_ROOM_SNAPSHOT, ++evaluated);
    if (evaluated || kage_vita_deep_loop() != UINT32_MAX) return 1;
    puts("deep profiler disabled header: PASS (scope arguments not evaluated)");
    return 0;
}
#else
#define ISAAC_VITA_DEEP_PROFILE 1
#define ISAAC_KAGE_VITA_DEEP_PROFILE_ORACLE 1
#include "../../runtime/kage_vita_deep_profile.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t test_now;
static uint32_t test_tid = 1U;
static uint32_t test_reads, test_thread_reads, test_log_calls;
static uint32_t test_report_base, test_report_ends;
static uint32_t test_log_recursion, test_checks;
static char test_log[1024U * 1024U];
static size_t test_log_length;

#define CHECK(expr) do { ++test_checks; if (!(expr)) { \
    fprintf(stderr, "deep profiler FAIL line %u: %s\n", (unsigned)__LINE__, #expr); \
    exit(1); } } while (0)

uint64_t kage_vita_deep_oracle_clock(void) { ++test_reads; errno = 987; return test_now; }
uint32_t kage_vita_deep_oracle_thread(void) { ++test_thread_reads; errno = 986; return test_tid; }
void kage_vita_deep_oracle_log(const char *format, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, format);
    int length = vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    CHECK(length > 0 && length < 384); /* native per-call durable log bound */
    CHECK(line[length - 1] == '\n');
    CHECK(test_log_length + (size_t)length + 1U < sizeof test_log);
    memcpy(test_log + test_log_length, line, (size_t)length);
    test_log_length += (size_t)length;
    test_log[test_log_length] = '\0';
    if (strstr(line, " dp.h ")) test_report_base = test_log_calls;
    ++test_log_calls;
    if (strstr(line, " dp.end ")) {
        const char *field = strstr(line, " records=");
        char *end = NULL;
        CHECK(field != NULL);
        unsigned long records = strtoul(field + strlen(" records="), &end, 10);
        CHECK(end != NULL && *end == '\n');
        CHECK(records == test_log_calls - test_report_base);
        ++test_report_ends;
    }
    if (test_log_recursion) {
        uint32_t hooks = s_kvd.hooks, clocks = s_kvd.clock_reads, outside = s_kvd.outside;
        kage_vita_deep_token ignored = kage_vita_deep_enter(KVD_HEAP_ALLOC, 3U);
        kage_vita_deep_io(6U, 6U, 3U, 3, test_now, test_now);
        CHECK(!ignored.active);
        CHECK(s_kvd.hooks == hooks && s_kvd.clock_reads == clocks && s_kvd.outside == outside);
        CHECK(kage_vita_deep_loop() == UINT32_MAX);
    }
    errno = 985;
}
static void reset_test(void)
{
    memset(&s_kvd, 0, sizeof s_kvd);
    __atomic_store_n(&s_kvd_owner, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_foreign, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_foreign_saturated, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_unbound, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_unbound_saturated, 0U, __ATOMIC_RELAXED);
    s_kvd_reporting = 0U;
    test_now = 1000U; test_tid = 1U; test_reads = test_thread_reads = test_log_calls = 0U;
    test_report_base = test_report_ends = 0U;
    test_log_recursion = 0U; test_log_length = 0U; test_log[0] = '\0';
    errno = 123;
}
static void begin_test(uint32_t loop, uint64_t start)
{
    test_now = start;
    kage_vita_deep_frame_begin_at(loop, 1U, start);
    CHECK(errno == 123);
    CHECK(kage_vita_deep_loop() == loop);
    CHECK(errno == 123);
}
static void nested_phase_io(void)
{
    reset_test(); begin_test(10U, 1000U);
    kage_vita_deep_phase_set_at(KVD_PHASE_UPDATE, 1010U);
    test_now = 1020U;
    kage_vita_deep_token png = kage_vita_deep_enter(KVD_PNG, 42U);
    test_now = 1040U;
    kage_vita_deep_token decode = kage_vita_deep_enter(KVD_PNG_DECODE, 100U);
    test_now = 1080U;
    kage_vita_deep_io(2U, 1U, 100U, 80, 1050U, 1070U);
    CHECK(errno == 123);
    test_now = 1090U; kage_vita_deep_leave(&decode);
    test_now = 1110U; kage_vita_deep_leave(&png);
    kage_vita_deep_phase_set_at(KVD_PHASE_RENDER, 1120U);
    test_now = 1130U;
    { KAGE_VITA_DEEP_SCOPE(KVD_GL_DRAW); test_now = 1140U; }
    kage_vita_deep_phase_set_at(KVD_PHASE_OTHER, 1150U);
    kage_vita_deep_frame_end_at(1160U);
    CHECK(errno == 123);
    CHECK(s_kvd.frames == 1U && s_kvd.retained_count == 1U);
    CHECK(s_kvd.total.phase_us[0] == 20U && s_kvd.total.phase_us[1] == 110U && s_kvd.total.phase_us[2] == 30U);
    kvd_stat *p = &s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_PNG];
    kvd_stat *d = &s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_PNG_DECODE];
    CHECK(p->calls == 1U && p->timed == 1U && p->self_timed == 1U);
    CHECK(p->inclusive_us == 90U && p->self_us == 40U && p->bytes == 42U);
    CHECK(d->inclusive_us == 50U && d->self_us == 30U && d->bytes == 100U);
    CHECK(s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_IO_READ].self_us == 20U);
    CHECK(s_kvd.total.stat[KVD_PHASE_RENDER][KVD_GL_DRAW].inclusive_us == 10U);
    CHECK(s_kvd.total.io[2].requested == 100U && s_kvd.total.io[2].returned == 80U);
    CHECK(s_kvd.total.cls[1].us == 20U);
    CHECK(s_kvd.total.errors == 0U);
    CHECK(kage_vita_deep_loop() == UINT32_MAX);
    CHECK(s_kvd.calibration_pairs == 32U && s_kvd.calibration_sum_us == 0U);
    CHECK(s_kvd.clock_reads == test_reads);
    test_log_recursion = 1U;
    kage_vita_deep_report("deep:oracle", 1U);
    CHECK(errno == 123 && !s_kvd.active && !s_kvd_reporting);
    CHECK(s_kvd.frames == 0U && s_kvd.outside == 0U && s_kvd.clock_reads == 0U);
    CHECK(strstr(test_log, "dp.h bid=deep:oracle win=1 abi=1") != NULL);
    CHECK(strstr(test_log, "cat=10 n=1 timed=1 selfn=1 incomplete=0 us(i,s,max)=90,40,90 bytes=0:42") != NULL);
    CHECK(strstr(test_log, "extra_clock_reads=1 excludes_this_line=1") != NULL);
    CHECK(test_report_ends == 1U);
    /* A second empty report excludes the one-time dictionary and starts a
     * fresh declared count, despite the first report's logger recursion. */
    kage_vita_deep_report("deep:oracle", 2U);
    CHECK(test_report_ends == 2U && !s_kvd_reporting && s_kvd.outside == 0U);
#if defined(KVD_HOST_TEST_EMIT)
    /* Optional real formatter fixture for the independent offline reader. */
    fputs(test_log, stdout);
#endif
}
static void anm2_binding_nesting(void)
{
    reset_test(); begin_test(21U, 1000U);
    kage_vita_deep_phase_set_at(KVD_PHASE_UPDATE, 1000U);
    test_now = 1010U;
    kage_vita_deep_token load = kage_vita_deep_enter(KVD_ANM2_LOAD, 0U);
    test_now = 1020U;
    { KAGE_VITA_DEEP_SCOPE(KVD_HEAP_ALLOC); test_now = 1030U; }
    /* The old parser stays an independently nested category, not renamed. */
    { KAGE_VITA_DEEP_SCOPE(KVD_ANM2); test_now = 1050U; }
    test_now = 1060U;
    kage_vita_deep_token graphics = kage_vita_deep_enter(KVD_ANM2_GRAPHICS, 0U);
    test_now = 1070U;
    kage_vita_deep_token png = kage_vita_deep_enter(KVD_PNG, 0U);
    test_now = 1080U;
    kage_vita_deep_token decode = kage_vita_deep_enter(KVD_PNG_DECODE, 0U);
    test_now = 1110U; kage_vita_deep_io(2U, 1U, 10U, 10, 1090U, 1100U);
    test_now = 1120U; kage_vita_deep_leave(&decode);
    test_now = 1130U; kage_vita_deep_leave(&png);
    test_now = 1140U;
    { KAGE_VITA_DEEP_SCOPE(KVD_HEAP_FREE); test_now = 1150U; }
    test_now = 1160U; kage_vita_deep_leave(&graphics);
    test_now = 1200U; kage_vita_deep_leave(&load);
    /* A separate PNG in the same frame must not be subtracted from graphics. */
    test_now = 1210U;
    { KAGE_VITA_DEEP_SCOPE(KVD_PNG); test_now = 1240U; }
    kage_vita_deep_frame_end_at(1250U);
    const kvd_stat *l = &s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_ANM2_LOAD];
    const kvd_stat *g = &s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_ANM2_GRAPHICS];
    CHECK(l->calls == 1U && l->timed == 1U && l->self_timed == 1U && !l->incomplete);
    CHECK(l->inclusive_us == 190U && l->self_us == 60U);
    CHECK(g->calls == 1U && g->inclusive_us == 100U && g->self_us == 30U);
    CHECK(s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_ANM2].inclusive_us == 20U);
    CHECK(s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_PNG].inclusive_us == 90U);
    CHECK(s_kvd.total.stat[KVD_PHASE_UPDATE][KVD_PNG_DECODE].self_us == 30U);
    CHECK(s_kvd.total.io[2].us == 10U && s_kvd.total.cls[1].us == 10U);
    CHECK(s_kvd.resource_frames == 1U && s_kvd.retained_count == 1U);
    CHECK(s_kvd.total.errors == 0U && errno == 123);
    CHECK(KVD_ANM2 == 11U && KVD_AUDIO_PUMP == 34U);
    CHECK(KVD_ANM2_LOAD == 35U && KVD_ANM2_GRAPHICS == 36U && KVD_CATEGORY_COUNT == 39U);
    CHECK(strcmp(s_kvd_names[KVD_ANM2], "anm2") == 0);
    CHECK(strcmp(s_kvd_names[KVD_ANM2_LOAD], "anm2_load") == 0);
    CHECK(strcmp(s_kvd_names[KVD_ANM2_GRAPHICS], "anm2_graphics") == 0);
    kage_vita_deep_report("deep:oracle", 3U);
    CHECK(strstr(test_log, "cat=35 n=1 timed=1 selfn=1 incomplete=0 us(i,s,max)=190,60,190") != NULL);
    CHECK(strstr(test_log, "cat=36 n=1 timed=1 selfn=1 incomplete=0 us(i,s,max)=100,30,100") != NULL);
#if defined(KVD_HOST_TEST_EMIT)
    fputs(test_log, stdout);
#endif
}
static void room_state_nesting(void)
{
    reset_test(); begin_test(31U, 1000U);
    kage_vita_deep_phase_set_at(KVD_PHASE_UPDATE, 1000U);
    test_now = 1010U;
    { KAGE_VITA_DEEP_SCOPE(KVD_ROOM_SAVE); test_now = 1020U; }
    test_now = 1030U;
    kage_vita_deep_token reset = kage_vita_deep_enter(KVD_ROOM_STATE_RESET, 0U);
    test_now = 1040U;
    { KAGE_VITA_DEEP_SCOPE(KVD_HEAP_ALLOC); test_now = 1050U; }
    test_now = 1060U;
    { KAGE_VITA_DEEP_SCOPE(KVD_HEAP_FREE); test_now = 1070U; }
    test_now = 1100U; kage_vita_deep_leave(&reset);
    test_now = 1110U;
    kage_vita_deep_token snapshot = kage_vita_deep_enter(KVD_ROOM_SNAPSHOT, 0U);
    test_now = 1140U;
    { KAGE_VITA_DEEP_SCOPE_BYTES(KVD_MEMMOVE, 14336U); test_now = 1170U; }
    test_now = 1210U; kage_vita_deep_leave(&snapshot);
    kage_vita_deep_frame_end_at(1250U);
    const kvd_stat *r = &s_kvd.total.stat[1][KVD_ROOM_STATE_RESET];
    const kvd_stat *s = &s_kvd.total.stat[1][KVD_ROOM_SNAPSHOT];
    CHECK(KVD_ROOM_SAVE == 7U && KVD_ROOM_STATE_RESET == 37U && KVD_ROOM_SNAPSHOT == 38U);
    CHECK(strcmp(s_kvd_names[37], "room_state_reset") == 0);
    CHECK(strcmp(s_kvd_names[38], "room_snapshot") == 0);
    CHECK(r->calls == 1U && r->timed == 1U && r->self_timed == 1U && !r->incomplete);
    CHECK(r->inclusive_us == 70U && r->self_us == 50U);
    CHECK(s->calls == 1U && s->timed == 1U && s->self_timed == 1U && !s->incomplete);
    CHECK(s->inclusive_us == 100U && s->self_us == 70U);
    CHECK(s_kvd.total.stat[1][KVD_ROOM_SAVE].inclusive_us == 10U);
    CHECK(s_kvd.total.stat[1][KVD_MEMMOVE].bytes == 14336U);
    CHECK(s_kvd.total.phase_us[1] == 250U && s_kvd.total.errors == 0U);
    CHECK(s_kvd.resource_frames == 1U && s_kvd.retained_count == 1U && errno == 123);
    kage_vita_deep_report("deep:oracle", 4U);
    CHECK(strstr(test_log, "cat=37 n=1 timed=1 selfn=1 incomplete=0 us(i,s,max)=70,50,70") != NULL);
    CHECK(strstr(test_log, "cat=38 n=1 timed=1 selfn=1 incomplete=0 us(i,s,max)=100,70,100") != NULL);
    /* A reset/snapshot alone is a resource event even if it becomes <50ms. */
    for (uint32_t cat = KVD_ROOM_STATE_RESET; cat <= KVD_ROOM_SNAPSHOT; ++cat) {
        reset_test(); begin_test(1U, 1000U);
        { KAGE_VITA_DEEP_SCOPE(cat); test_now = 1010U; }
        kage_vita_deep_frame_end_at(1020U);
        CHECK(s_kvd.resource_frames == 1U && s_kvd.retained_count == 1U && !s_kvd.slow_frames);
        CHECK(s_kvd.total.stat[0][cat].inclusive_us == 10U);
    }
}
static void anm2_ordinary_return(int early)
{
    KAGE_VITA_DEEP_SCOPE(KVD_ANM2_LOAD);
    test_now += 10U;
    if (early) return; /* An ordinary error return still closes its scope. */
    KAGE_VITA_DEEP_SCOPE(KVD_ANM2_GRAPHICS);
    test_now += 10U;
}
static void anm2_binding_exit_health(void)
{
    reset_test(); begin_test(1U, 1000U);
    anm2_ordinary_return(1);
    anm2_ordinary_return(0);
    kage_vita_deep_frame_end_at(1040U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].calls == 2U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].inclusive_us == 30U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].self_us == 20U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_GRAPHICS].self_us == 10U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2].calls == 0U);
    CHECK(s_kvd.resource_frames == 1U && s_kvd.retained_count == 1U);
    CHECK(s_kvd.total.errors == 0U && s_kvd.depth == 0U && errno == 123);

    reset_test(); begin_test(1U, 1000U);
    kage_vita_deep_token load = kage_vita_deep_enter(KVD_ANM2_LOAD, 0U);
    uint32_t mark = kage_vita_deep_depth();
    test_now = 1010U; (void)kage_vita_deep_enter(KVD_ANM2_GRAPHICS, 0U);
    test_now = 1020U; (void)kage_vita_deep_enter(KVD_PNG, 0U);
    test_now = 1040U; kage_vita_deep_unwind(mark);
    test_now = 1050U; kage_vita_deep_leave(&load);
    kage_vita_deep_frame_end_at(1060U);
    CHECK(s_kvd.abandoned == 2U && s_kvd.depth == 0U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_GRAPHICS].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_GRAPHICS].timed == 0U);
    CHECK(s_kvd.total.stat[0][KVD_PNG].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].inclusive_us == 50U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].self_timed == 0U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].self_us == 0U && errno == 123);

    reset_test(); begin_test(1U, 1000U);
    (void)kage_vita_deep_enter(KVD_ANM2_LOAD, 0U);
    (void)kage_vita_deep_enter(KVD_ANM2_GRAPHICS, 0U);
    kage_vita_deep_frame_end_at(1100U); /* Uncaught nonlocal escape. */
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_GRAPHICS].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_LOAD].inclusive_us == 0U);
    CHECK(s_kvd.total.stat[0][KVD_ANM2_GRAPHICS].inclusive_us == 0U);
    CHECK(s_kvd.abandoned == 2U && s_kvd.depth == 0U);

    reset_test();
    CHECK(!kage_vita_deep_enter(KVD_ANM2_LOAD, 0U).active && test_reads == 0U);
    begin_test(1U, 1000U);
    uint32_t hooks = s_kvd.hooks, reads = test_reads;
    test_tid = 2U;
    CHECK(!kage_vita_deep_enter(KVD_ANM2_LOAD, 0U).active);
    CHECK(!kage_vita_deep_enter(KVD_ANM2_GRAPHICS, 0U).active);
    CHECK(s_kvd.hooks == hooks && test_reads == reads && s_kvd.depth == 0U);
    CHECK(errno == 123);
    test_tid = 1U; kage_vita_deep_frame_end_at(1010U);
}
static void invalid_external_io(void)
{
    reset_test(); begin_test(1U, 1000U);
    kage_vita_deep_token parent = kage_vita_deep_enter(KVD_PNG, 0U);
    test_now = 1200U; kage_vita_deep_io(2U, 1U, 10U, 10, 1100U, 1200U);
    test_now = 1250U; kage_vita_deep_io(2U, 1U, 10U, 10, 1150U, 1250U); /* overlap */
    test_now = 1300U; kage_vita_deep_io(0U, 0U, 0U, -1, 900U, 950U); /* old frame */
    test_now = 1350U; kage_vita_deep_io(1U, 1U, 0U, 0, 1300U, 1400U); /* future */
    test_now = 1400U; kage_vita_deep_leave(&parent);
    kage_vita_deep_frame_end_at(1450U);
    kvd_stat *p = &s_kvd.total.stat[0][KVD_PNG];
    CHECK(p->timed == 1U && p->inclusive_us == 400U && p->self_timed == 0U && p->self_us == 0U);
    CHECK(s_kvd.bad_io == 3U && s_kvd.total.io[2].calls == 2U && s_kvd.total.io[2].timed == 1U);
    CHECK(s_kvd.total.io[0].errors == 1U && s_kvd.total.io[0].timed == 0U);
    CHECK(s_kvd.total.stat[0][KVD_IO_READ].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_IO_CLOSE].incomplete == 1U);
    CHECK(s_kvd.total.cls[0].errors == 1U);
}
static void unwind_and_tokens(void)
{
    reset_test(); begin_test(1U, 1000U);
    kage_vita_deep_token pcall = kage_vita_deep_enter(KVD_LUA_PCALL, 0U);
    uint32_t mark = kage_vita_deep_depth();
    test_now = 1010U; (void)kage_vita_deep_enter(KVD_LUA_CALLBACK, 0U);
    test_now = 1020U; (void)kage_vita_deep_enter(KVD_PNG, 0U);
    test_now = 1040U; kage_vita_deep_unwind(mark);
    CHECK(kage_vita_deep_depth() == 1U && s_kvd.abandoned == 2U);
    test_now = 1050U; kage_vita_deep_leave(&pcall);
    kage_vita_deep_frame_end_at(1060U);
    CHECK(s_kvd.total.stat[0][KVD_PNG].timed == 0U && s_kvd.total.stat[0][KVD_PNG].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_LUA_PCALL].inclusive_us == 50U && s_kvd.total.stat[0][KVD_LUA_PCALL].self_timed == 0U);
    reset_test(); begin_test(1U, 1000U);
    kage_vita_deep_token outer = kage_vita_deep_enter(KVD_ANM2, 0U);
    test_now = 1010U; (void)kage_vita_deep_enter(KVD_PNG, 0U);
    test_now = 1020U; kage_vita_deep_leave(&outer); /* non-LIFO: child incomplete */
    CHECK(s_kvd.bad_token == 1U && s_kvd.abandoned == 1U && s_kvd.depth == 0U);
    test_now = 1030U;
    kage_vita_deep_token stale = kage_vita_deep_enter(KVD_GL_FBO, 0U);
    kage_vita_deep_frame_end_at(1040U);
    begin_test(2U, 2000U);
    kage_vita_deep_token fresh = kage_vita_deep_enter(KVD_GL_DRAW, 0U);
    kage_vita_deep_leave(&stale);
    CHECK(s_kvd.depth == 1U && s_kvd.stack[0].serial == fresh.serial);
    test_now = 2010U; kage_vita_deep_leave(&fresh);
    CHECK(s_kvd.current.stat[0][KVD_GL_DRAW].self_timed == 1U);
    kage_vita_deep_frame_end_at(2020U);
}
static void depth_clock_foreign(void)
{
    uint32_t i;
    reset_test();
    CHECK(!kage_vita_deep_enter(KVD_PNG, 0U).active);
    CHECK(__atomic_load_n(&s_kvd_unbound, __ATOMIC_RELAXED) == 1U);
    CHECK(__atomic_load_n(&s_kvd_foreign, __ATOMIC_RELAXED) == 0U && test_reads == 0U);
    begin_test(1U, 1000U);
    for (i = 0U; i < KVD_STACK_CAP; ++i) CHECK(kage_vita_deep_enter(KVD_GL_FBO, 0U).active);
    CHECK(!kage_vita_deep_enter(KVD_GL_FBO, 0U).active);
    CHECK(s_kvd.bad_depth == 1U && s_kvd.depth == KVD_STACK_CAP);
    kage_vita_deep_unwind(0U); kage_vita_deep_frame_end_at(1100U);
    CHECK(s_kvd.total.stat[0][KVD_GL_FBO].calls == KVD_STACK_CAP + 1U);
    CHECK(s_kvd.total.stat[0][KVD_GL_FBO].incomplete == KVD_STACK_CAP + 1U);
    reset_test(); begin_test(1U, 1000U);
    kage_vita_deep_token a = kage_vita_deep_enter(KVD_PNG, 0U);
    test_now = 1010U; kage_vita_deep_token b = kage_vita_deep_enter(KVD_PNG_DECODE, 0U);
    test_now = 990U; kage_vita_deep_leave(&b);
    test_now = 1020U; kage_vita_deep_leave(&a); kage_vita_deep_frame_end_at(1030U);
    CHECK(s_kvd.bad_clock == 1U && s_kvd.total.stat[0][KVD_PNG_DECODE].incomplete == 1U);
    CHECK(s_kvd.total.stat[0][KVD_PNG].self_timed == 0U);
    reset_test(); begin_test(1U, 1000U);
    a = kage_vita_deep_enter(KVD_PNG, 0U);
    uint32_t hooks = s_kvd.hooks, reads = test_reads;
    test_tid = 2U;
    CHECK(!kage_vita_deep_enter(KVD_HEAP_ALLOC, 999U).active);
    kage_vita_deep_io(2U, 1U, 100U, 100, 1000U, 1000U);
    kage_vita_deep_leave(&a);
    CHECK(kage_vita_deep_loop() == UINT32_MAX);
    CHECK(s_kvd.hooks == hooks && test_reads == reads && s_kvd.depth == 1U && a.active);
    CHECK(__atomic_load_n(&s_kvd_foreign, __ATOMIC_RELAXED) == 4U);
    CHECK(errno == 123);
    test_tid = 1U; test_now = 1020U; kage_vita_deep_leave(&a); kage_vita_deep_frame_end_at(1030U);
}
static void retention_saturation(void)
{
    uint32_t i, min = UINT32_MAX;
    reset_test();
    for (i = 1U; i <= 20U; ++i) {
        uint64_t start = 1000U + i * 1000U;
        begin_test(i, start);
        kage_vita_deep_token p = kage_vita_deep_enter(KVD_PNG, 1U);
        test_now = start + 10U * i; kage_vita_deep_leave(&p);
        kage_vita_deep_frame_end_at(test_now);
    }
    for (i = 0U; i < KVD_RETAIN_CAP; ++i) if (s_kvd.retained[i].duration_us < min) min = s_kvd.retained[i].duration_us;
    CHECK(s_kvd.frames == 20U && s_kvd.resource_frames == 20U && s_kvd.retained_count == 16U);
    CHECK(s_kvd.candidate_frames == 20U && s_kvd.dropped_frames == 4U && s_kvd.dropped_max_us == 40U);
    CHECK(min == 50U && s_kvd.max_frame_loop == 20U && s_kvd.max_frame_us == 200U);
    CHECK(s_kvd.total.stat[0][KVD_PNG].inclusive_us == 2100U && s_kvd.total.stat[0][KVD_PNG].calls == 20U);
    reset_test(); begin_test(1U, 1000U);
    { KAGE_VITA_DEEP_SCOPE(KVD_ROOM_TRANSITION); }
    { KAGE_VITA_DEEP_SCOPE(KVD_ROOM_RENDER); }
    { KAGE_VITA_DEEP_SCOPE(KVD_ROOM_PRERENDER); }
    kage_vita_deep_frame_end_at(1010U);
    CHECK(s_kvd.resource_frames == 0U && s_kvd.retained_count == 0U);
    begin_test(2U, 2000U); kage_vita_deep_frame_end_at(52001U);
    CHECK(s_kvd.slow_frames == 1U && s_kvd.retained_count == 1U);
    reset_test(); begin_test(1U, 1000U);
    s_kvd.current.stat[0][KVD_MEMCPY].bytes = UINT64_MAX;
    s_kvd.current.stat[0][KVD_MEMCPY].calls = UINT32_MAX;
    { KAGE_VITA_DEEP_SCOPE_BYTES(KVD_MEMCPY, 1U); }
    CHECK(s_kvd.current.stat[0][KVD_MEMCPY].bytes == UINT64_MAX);
    CHECK(s_kvd.current.stat[0][KVD_MEMCPY].calls == UINT32_MAX && s_kvd.saturation >= 2U);
    kage_vita_deep_frame_end_at(1010U);
}
static void phase_cross_and_worst_records(void)
{
    reset_test(); begin_test(1U, 1000U);
    kage_vita_deep_phase_set_at(KVD_PHASE_UPDATE, 1000U);
    kage_vita_deep_token p = kage_vita_deep_enter(KVD_ANM2, 0U);
    kage_vita_deep_phase_set_at(KVD_PHASE_RENDER, 1050U);
    test_now = 1060U;
    { KAGE_VITA_DEEP_SCOPE(KVD_GL_DRAW); test_now = 1070U; }
    test_now = 1090U; kage_vita_deep_leave(&p);
    kage_vita_deep_phase_set_at(KVD_PHASE_OTHER, 1100U);
    kage_vita_deep_frame_end_at(1110U);
    CHECK(s_kvd.phase_cross == 1U);
    CHECK(s_kvd.total.stat[1][KVD_ANM2].inclusive_us == 90U && s_kvd.total.stat[1][KVD_ANM2].self_us == 80U);
    CHECK(s_kvd.total.stat[2][KVD_GL_DRAW].inclusive_us == 10U);
    CHECK(s_kvd.total.phase_us[1] == 50U && s_kvd.total.phase_us[2] == 50U && s_kvd.total.phase_us[0] == 10U);
    reset_test(); begin_test(UINT32_MAX, 1000U); kage_vita_deep_frame_end_at(1000U);
    memset(&s_kvd.total, 255, sizeof s_kvd.total);
    s_kvd.retained[0] = s_kvd.total; s_kvd.retained_count = 1U;
    s_kvd.frames = s_kvd.resource_frames = s_kvd.slow_frames = s_kvd.invalid_frames = UINT32_MAX;
    s_kvd.candidate_frames = s_kvd.dropped_frames = s_kvd.dropped_max_us = UINT32_MAX;
    s_kvd.max_frame_us = s_kvd.max_frame_loop = UINT32_MAX;
    s_kvd.bad_clock = s_kvd.bad_token = s_kvd.bad_depth = s_kvd.abandoned = s_kvd.bad_io = UINT32_MAX;
    s_kvd.bad_category = s_kvd.bad_frame = s_kvd.saturation = s_kvd.phase_cross = s_kvd.child_invalid = UINT32_MAX;
    s_kvd.outside = s_kvd.hooks = s_kvd.clock_reads = s_kvd.max_hooks = s_kvd.max_clock_reads = UINT32_MAX;
    s_kvd.calibration_pairs = s_kvd.calibration_sum_us = s_kvd.calibration_min_us = s_kvd.calibration_max_us = UINT32_MAX;
    s_kvd.previous_report_us = UINT32_MAX;
    __atomic_store_n(&s_kvd_foreign, UINT32_MAX, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_foreign_saturated, UINT32_MAX, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_unbound, UINT32_MAX, __ATOMIC_RELAXED);
    __atomic_store_n(&s_kvd_unbound_saturated, UINT32_MAX, __ATOMIC_RELAXED);
    kage_vita_deep_report("12345678901234567890123456789012", UINT32_MAX);
    CHECK(errno == 123); /* logger asserts every maximally-wide line <384 */
}
int main(void)
{
    nested_phase_io(); anm2_binding_nesting(); anm2_binding_exit_health(); room_state_nesting();
    invalid_external_io(); unwind_and_tokens();
    depth_clock_foreign(); retention_saturation(); phase_cross_and_worst_records();
    printf("deep profiler host oracle: PASS (%u checks, static=%u bytes, phases/nesting/IO/unwind/foreign/retention/recursion/errno)\n",
           test_checks, (unsigned)(sizeof s_kvd + 6U * sizeof(uint32_t)));
    return 0;
}
#endif
