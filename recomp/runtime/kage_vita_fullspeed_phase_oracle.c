/* Integration oracle: cadence decisions must line up with ph120 samples. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gl_vita_backend.h"
#include "guest.h"
#include "kage_vita_fullspeed_scheduler.h"
#include "kage_vita_phase_profile.h"

/* The production phase profiler resets its renderer-side fusion window
 * through gl_vita_backend.c.  This standalone scheduler oracle intentionally
 * does not link a renderer, so provide the same inert boundary endpoint. */
void gl_vita_backend_phase_profile_window_boundary(void)
{
}

GuestPhaseProfileCounters g_guest_phase_profile_counters;
/* host_vita_post_com.c owns this in production (ph120.a tgt=); the oracle
 * links without it. */
uint32_t g_isaac_vita_post_com_time_get_time_calls;
/* Records per window: t, c, a, [g], s, r, w, [fd, fs]. */
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
uint32_t g_guest_lookup_cache_hits;
uint32_t g_guest_lookup_cache_misses;
# define ORACLE_BASE_RECORDS 7u
#else
# define ORACLE_BASE_RECORDS 6u
#endif
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
# define ORACLE_PROFILE_RECORDS (ORACLE_BASE_RECORDS + 2u)
#else
# define ORACLE_PROFILE_RECORDS ORACLE_BASE_RECORDS
#endif
IsaacVitaGlPhaseProfileCounters g_isaac_vita_gl_phase_profile_counters;

static uint64_t s_now;
static char s_logs[8192];
static size_t s_log_length;
static uint32_t s_profile_log_calls;
static uint32_t s_scheduler_log_calls;

uint64_t sceKernelGetProcessTimeWide(void) { return s_now; }
uint64_t isaac_vita_get_process_time(void) { return s_now; }
int sceKernelDelayThread(unsigned int delay_us)
{
    s_now += delay_us;
    return 0;
}

static void append_log(const char *format, va_list arguments)
{
    size_t available = sizeof s_logs - s_log_length;
    int result = vsnprintf(
        s_logs + s_log_length, available, format, arguments);

    if (result > 0 && available != 0u) {
        size_t written = (size_t)result;
        if (written >= available)
            written = available - 1u;
        s_log_length += written;
    }
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    append_log(format, arguments);
    va_end(arguments);
    ++s_profile_log_calls;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    if (!format || strstr(format, "KAGE VITA CADENCE30:") != format)
        return;
    va_start(arguments, format);
    append_log(format, arguments);
    va_end(arguments);
    if (s_log_length + 1u < sizeof s_logs) {
        s_logs[s_log_length++] = '\n';
        s_logs[s_log_length] = '\0';
    }
    ++s_scheduler_log_calls;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "cadence30 phase oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static const char *find_line(const char *prefix)
{
    return strstr(s_logs, prefix);
}

static const char *find_nth_line(const char *prefix, uint32_t ordinal)
{
    const char *position = s_logs;

    while ((position = strstr(position, prefix)) != NULL) {
        const char *end;

        if (ordinal == 0u)
            return position;
        end = strchr(position, '\n');
        if (!end)
            return NULL;
        position = end + 1;
        --ordinal;
    }
    return NULL;
}

static int parse_scheduler_report_at(
    const char *line, uint64_t *report_at_us)
{
    const char *field;
    const char *end;
    uint64_t value = 0u;
    uint32_t index;

    if (!line || !report_at_us)
        return 0;
    field = strstr(line, " at=");
    end = strchr(line, '\n');
    if (!field || !end || field + 20 > end)
        return 0;
    field += 4;
    for (index = 0u; index < 16u; ++index) {
        uint32_t digit;

        if (field[index] >= '0' && field[index] <= '9')
            digit = (uint32_t)(field[index] - '0');
        else if (field[index] >= 'a' && field[index] <= 'f')
            digit = (uint32_t)(field[index] - 'a') + 10u;
        else if (field[index] >= 'A' && field[index] <= 'F')
            digit = (uint32_t)(field[index] - 'A') + 10u;
        else
            return 0;
        value = (value << 4) | digit;
    }
    if (field[16] != ' ')
        return 0;
    *report_at_us = value;
    return 1;
}

static size_t line_length(const char *line)
{
    const char *end = line ? strchr(line, '\n') : NULL;
    return end ? (size_t)(end - line + 1) : 0u;
}

#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
/* The unsigned decimal after "<key>" inside one record line. */
static int field_u32(const char *line, const char *key, uint32_t index,
                     uint32_t *value)
{
    const char *end = line ? strchr(line, '\n') : NULL;
    const char *field = line ? strstr(line, key) : NULL;
    uint64_t parsed = 0u;

    if (!field || !end || field > end)
        return 0;
    field += strlen(key);
    while (index--) {
        field = strchr(field, ',');
        if (!field || field > end)
            return 0;
        ++field;
    }
    if (*field < '0' || *field > '9')
        return 0;
    while (*field >= '0' && *field <= '9') {
        parsed = parsed * 10u + (uint64_t)(*field - '0');
        if (parsed > UINT32_MAX)
            return 0;
        ++field;
    }
    *value = (uint32_t)parsed;
    return 1;
}

/* The 16 hex digits after "<key>" inside one record line (ph120.fs floor=,
 * printed like ph120.s at=). */
static int field_hex64(const char *line, const char *key, uint64_t *value)
{
    const char *end = line ? strchr(line, '\n') : NULL;
    const char *field = line ? strstr(line, key) : NULL;
    uint64_t parsed = 0u;
    uint32_t i;

    if (!field || !end || field > end)
        return 0;
    field += strlen(key);
    for (i = 0u; i < 16u; ++i) {
        char c = field[i];
        uint32_t digit;

        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (uint32_t)(c - 'a' + 10);
        else
            return 0;
        parsed = (parsed << 4) | digit;
    }
    if (field[16] != '\n')
        return 0;
    *value = parsed;
    return 1;
}
#endif

int main(void)
{
    uint32_t loop;
    uint32_t window;
    uint32_t manager_counter = 0u;
    uint32_t game_frame = 0u;
    uint32_t renders[2] = { 0u, 0u };
    uint32_t late_skips[2] = { 0u, 0u };
    uint64_t expected_report_at_us[2] = { 0u, 0u };
    uint64_t logged_report_at_us[2] = { 0u, 0u };
    uint64_t report_delta_us;
    uint32_t game_update_rate_millihz;
    const char *timing[2];
    const char *counts[2];
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    const char *guest_cache[2];
#endif
    const char *scheduler[2];
    const char *game_pointer[2];
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    /* The last Game::Update start of each window (its floor's anchor). */
    uint64_t last_start_us[2] = { 0u, 0u };
    uint32_t window_updates[2] = { 0u, 0u };
    const char *floor_debt[2];
    const char *floor_slack[2];
#endif

    memset(s_logs, 0, sizeof s_logs);
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_phase_profile_reset();
    s_now = 1000u;
    /* Production order: close/profile the previous loop, then pace this one. */
    kage_vita_phase_profile_note_loop_head(1u);
    kage_vita_fullspeed_scheduler_note_loop_head();

    for (loop = 1u; loop <= 240u; ++loop) {
        KageVitaFullspeedSnapshot before;
        KageVitaFullspeedSnapshot after;
        uint32_t render_us;
        int render;

        window = (loop - 1u) / 120u;
        render_us = window == 0u ? 40000u : 55000u;
        kage_vita_phase_profile_note_service_entry();
        kage_vita_fullspeed_scheduler_note_service();
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        g_guest_lookup_cache_hits += 2u;
        ++g_guest_lookup_cache_misses;
#endif
        s_now += 500u;
        kage_vita_phase_profile_note_update_entry();
        kage_vita_fullspeed_scheduler_note_manager_dispatch();
        kage_vita_fullspeed_scheduler_note_manager_entry(
            0x10000000u, manager_counter, 0u,
            0x20000000u, game_frame);
        if ((manager_counter & 1u) == 0u) {
            kage_vita_fullspeed_scheduler_note_game_update_begin(
                manager_counter, 0x20000000u, game_frame);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            last_start_us[window] = s_now;
            ++window_updates[window];
#endif
            s_now += 2500u;
            ++game_frame;
            kage_vita_fullspeed_scheduler_note_game_update_end(
                0x20000000u, game_frame);
        } else {
            s_now += 500u;
        }
        ++manager_counter;
        kage_vita_fullspeed_scheduler_snapshot(&before);
        render = kage_vita_fullspeed_scheduler_plan_render(
            0x10000000u, manager_counter, 0u,
            0x20000000u, game_frame);
        if (render) {
            ++renders[window];
            kage_vita_fullspeed_scheduler_note_render_entry();
            kage_vita_phase_profile_note_render_entry();
            kage_vita_fullspeed_scheduler_note_render_body(
                0x10000000u, manager_counter, 0u);
            s_now += render_us;
            kage_vita_fullspeed_scheduler_note_present();
            kage_vita_phase_profile_note_present_enter();
            s_now += 100u;
            kage_vita_phase_profile_note_present_return();
            s_now += 400u;
            kage_vita_fullspeed_scheduler_note_render_return();
            kage_vita_phase_profile_note_render_return();
        } else {
            kage_vita_phase_profile_note_render_skipped();
        }
        kage_vita_fullspeed_scheduler_snapshot(&after);
        late_skips[window] +=
            after.late_full_skips - before.late_full_skips;
        CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
        if (loop == 120u || loop == 240u)
            expected_report_at_us[window] = s_now;
        kage_vita_phase_profile_note_loop_head(loop + 1u);
        kage_vita_fullspeed_scheduler_note_loop_head();
    }

    CHECK(s_scheduler_log_calls == 1u);
    CHECK(s_profile_log_calls == 2u * ORACLE_PROFILE_RECORDS);
    for (window = 0u; window < 2u; ++window) {
        char expected[256];
        uint32_t total_skips = 120u - renders[window];

        timing[window] = find_nth_line("[kage-vita] ph120.t ", window);
        counts[window] = find_nth_line("[kage-vita] ph120.c ", window);
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        guest_cache[window] = find_nth_line(
            "[kage-vita] ph120.g ", window);
#endif
        scheduler[window] = find_nth_line(
            "[kage-vita] ph120.s ", window);
        game_pointer[window] = find_nth_line(
            "[kage-vita] ph120.r ", window);
        CHECK(timing[window] != NULL && counts[window] != NULL &&
              scheduler[window] != NULL && game_pointer[window] != NULL);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
        floor_debt[window] = find_nth_line("[kage-vita] ph120.fd ", window);
        floor_slack[window] = find_nth_line("[kage-vita] ph120.fs ", window);
        CHECK(floor_debt[window] != NULL && floor_slack[window] != NULL);
        CHECK(game_pointer[window] < floor_debt[window] &&
              floor_debt[window] < floor_slack[window]);
        CHECK(line_length(floor_debt[window]) > 0u &&
              line_length(floor_debt[window]) < 384u);
        CHECK(line_length(floor_slack[window]) > 0u &&
              line_length(floor_slack[window]) < 384u);
        (void)snprintf(
            expected, sizeof expected,
            "bid=fullspeed:phase-oracle win=%u loops=%u p=",
            window + 1u, (window + 1u) * 120u);
        CHECK(strstr(floor_debt[window], expected) != NULL);
        (void)snprintf(
            expected, sizeof expected,
            "bid=fullspeed:phase-oracle win=%u loops=%u h=",
            window + 1u, (window + 1u) * 120u);
        CHECK(strstr(floor_slack[window], expected) != NULL);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        CHECK(guest_cache[window] != NULL);
        CHECK(timing[window] < counts[window] &&
              counts[window] < guest_cache[window] &&
              guest_cache[window] < scheduler[window] &&
              scheduler[window] < game_pointer[window]);
        (void)snprintf(
            expected, sizeof expected,
            "bid=fullspeed:phase-oracle win=%u loops=%u "
            "gc(h,m)=240,120\n",
            window + 1u, (window + 1u) * 120u);
        CHECK(strstr(guest_cache[window], expected) != NULL);
#else
        CHECK(timing[window] < counts[window] &&
              counts[window] < scheduler[window] &&
              scheduler[window] < game_pointer[window]);
#endif
        (void)snprintf(
            expected, sizeof expected,
            "n(s,u,r,p,l,w)=120,120,%u,%u,0,120 ",
            renders[window], renders[window]);
        CHECK(strstr(counts[window], expected) != NULL);
        (void)snprintf(
            expected, sizeof expected,
            "bid=fullspeed:phase-oracle win=%u loops=%u ",
            window + 1u, (window + 1u) * 120u);
        CHECK(strstr(scheduler[window], expected) != NULL);
        (void)snprintf(
            expected, sizeof expected,
            "cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,%u,%u,%u ",
            renders[window], renders[window], renders[window]);
        CHECK(strstr(scheduler[window], expected) != NULL);
        CHECK(strstr(scheduler[window], "phase(f,n,i)=60,60,0 ") != NULL);
        (void)snprintf(
            expected, sizeof expected,
            "skip(a,l,n,f)=%u,%u,60,",
            total_skips, late_skips[window]);
        CHECK(strstr(scheduler[window], expected) != NULL);
        CHECK(strstr(game_pointer[window],
                     "ctr(p,a,v)=0,0,0 gptr(p,a,v)=0,0,0 ") != NULL);
        (void)snprintf(
            expected, sizeof expected,
            "frame(last:s,b,e)=002ce74f,%u,%u "
            "frame(fail:s,b,e)=00000000,0,0\n",
            window == 0u ? 59u : 119u,
            window == 0u ? 60u : 120u);
        CHECK(strstr(game_pointer[window], expected) != NULL);
        CHECK(strstr(scheduler[window], "viol(d,p,g,s,o)=0,0,0,0,0 ") != NULL);
        CHECK(strstr(scheduler[window], "reset=0 drop(t,us)=") != NULL);
        CHECK(line_length(timing[window]) > 0u &&
              line_length(timing[window]) <= 512u);
        CHECK(strstr(timing[window], "gap(n,min,50,95,max)=") != NULL);
        CHECK(line_length(counts[window]) > 0u &&
              line_length(counts[window]) < 384u);
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        CHECK(line_length(guest_cache[window]) > 0u &&
              line_length(guest_cache[window]) < 384u);
#endif
        CHECK(line_length(scheduler[window]) > 0u &&
              line_length(scheduler[window]) <= 512u);
        CHECK(line_length(game_pointer[window]) > 0u &&
              line_length(game_pointer[window]) < 384u);
        CHECK(parse_scheduler_report_at(
            scheduler[window], &logged_report_at_us[window]));
        CHECK(logged_report_at_us[window] ==
              expected_report_at_us[window]);
    }
    CHECK(logged_report_at_us[1] > logged_report_at_us[0]);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    {
        /* ph120.fd attribution: from window 1's last Game::Update start to
         * window 2's, the floor advanced one period per pace plus what each
         * pace dropped (OBSERVE: late + stale; ON: over + stale, give or
         * take one kept amount at each boundary).  Thirds of a us. */
        uint32_t paces, late_us, kept_us, over_us, stale_us, gf_n, gf_pos;
        uint64_t wall = 3u * (last_start_us[1] - last_start_us[0]);
        uint64_t want = (uint64_t)window_updates[1] * 100000u;
        uint64_t slop = 4u * (uint64_t)window_updates[1] + 3u;

        CHECK(window_updates[1] == 60u && last_start_us[0] != 0u);
        CHECK(field_u32(floor_debt[1], " p=", 0u, &paces) && paces == 60u);
        CHECK(field_u32(floor_debt[1], " late(c,us,max)=", 1u, &late_us));
        CHECK(field_u32(floor_debt[1], " kept=", 0u, &kept_us));
        CHECK(field_u32(floor_debt[1], " over(c,us)=", 1u, &over_us));
        CHECK(field_u32(floor_debt[1], " stale(c,g,us)=", 2u, &stale_us));
        CHECK(field_u32(floor_debt[1], " gf(n,pos,max,min,last)=", 0u, &gf_n));
        CHECK(field_u32(floor_debt[1], " gf(n,pos,max,min,last)=", 1u, &gf_pos));
        CHECK(gf_n == 60u && gf_pos == 0u);
        CHECK(late_us == kept_us + over_us);
        /* 55-ms Render: every pace is late or stale, nothing waits */
        CHECK(late_us + stale_us > 0u);
# if ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 1
        want += 3u * ((uint64_t)late_us + stale_us);
        CHECK(wall + slop >= want && wall <= want + slop);
# else
        want += 3u * ((uint64_t)over_us + stale_us);
        CHECK(wall + slop + 3u * ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US >= want &&
              wall <= want + slop + 3u * ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US);
# endif
    }
    {
        /* The ph120.fs floor stamps of the two reports give
         * the identity exactly, with no boundary term: the floor advanced
         * p periods plus late + stale (OBSERVE) or over + stale (ON), up to
         * the 2 units each rounded sum and each stamp can lose.  The stamp
         * is never more than one period after its report's at= (the floor
         * comes from a pace that already ran).  Thirds of a us. */
        uint64_t floor_us[2], want, got;
        uint32_t paces, late_us, over, over_us, clamp, clamp_us, stale_us;
        uint32_t w;

        for (w = 0u; w < 2u; ++w) {
            CHECK(field_hex64(floor_slack[w], " floor=", &floor_us[w]));
            CHECK(floor_us[w] != 0u);
            CHECK(3u * floor_us[w] <= 3u * logged_report_at_us[w] + 100000u);
        }
        CHECK(field_u32(floor_debt[1], " p=", 0u, &paces));
        CHECK(field_u32(floor_debt[1], " late(c,us,max)=", 1u, &late_us));
        CHECK(field_u32(floor_debt[1], " over(c,us)=", 0u, &over));
        CHECK(field_u32(floor_debt[1], " over(c,us)=", 1u, &over_us));
        CHECK(field_u32(floor_debt[1], " clamp(c,us)=", 0u, &clamp));
        CHECK(field_u32(floor_debt[1], " clamp(c,us)=", 1u, &clamp_us));
        CHECK(field_u32(floor_debt[1], " stale(c,g,us)=", 2u, &stale_us));
        CHECK(clamp <= over && clamp_us <= over_us);
        CHECK(floor_us[1] > floor_us[0]);
        got = 3u * (floor_us[1] - floor_us[0]);
# if ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 1
        want = (uint64_t)paces * 100000u + 3u * ((uint64_t)late_us + stale_us);
# else
        want = (uint64_t)paces * 100000u + 3u * ((uint64_t)over_us + stale_us);
# endif
        CHECK(got + 2u * (uint64_t)paces + 2u >= want && got <= want + 2u);
    }
#endif
    report_delta_us = logged_report_at_us[1] - logged_report_at_us[0];
    game_update_rate_millihz = (uint32_t)(
        (60000000000ull + report_delta_us / 2u) / report_delta_us);
    CHECK(game_update_rate_millihz > 0u &&
          game_update_rate_millihz <= 30000u);
    CHECK(renders[0] > renders[1]);
    CHECK(renders[0] <= 60u && renders[1] <= 60u);

    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_profile_log_calls = 0u;
    kage_vita_phase_profile_oracle_emit_max_records();
    CHECK(s_profile_log_calls == ORACLE_PROFILE_RECORDS);
    timing[0] = find_line("[kage-vita] ph120.t ");
    counts[0] = find_line("[kage-vita] ph120.c ");
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    guest_cache[0] = find_line("[kage-vita] ph120.g ");
#endif
    scheduler[0] = find_line("[kage-vita] ph120.s ");
    game_pointer[0] = find_line("[kage-vita] ph120.r ");
    CHECK(timing[0] != NULL && counts[0] != NULL &&
          scheduler[0] != NULL && game_pointer[0] != NULL);
    /* The additive CPU Present-return gaps use the same durable bound as s.
     * The profile fixture separately pins 433 bytes (470 with oth). */
    CHECK(line_length(timing[0]) > 0u && line_length(timing[0]) <= 512u);
    CHECK(line_length(counts[0]) > 0u && line_length(counts[0]) < 384u);
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    CHECK(guest_cache[0] != NULL);
    CHECK(timing[0] < counts[0] && counts[0] < guest_cache[0] &&
          guest_cache[0] < scheduler[0] &&
          scheduler[0] < game_pointer[0]);
    CHECK(line_length(guest_cache[0]) > 0u &&
          line_length(guest_cache[0]) < 384u);
#else
    CHECK(timing[0] < counts[0] && counts[0] < scheduler[0] &&
          scheduler[0] < game_pointer[0]);
#endif
    CHECK(line_length(scheduler[0]) > 0u &&
          line_length(scheduler[0]) <= 512u);
    CHECK(line_length(game_pointer[0]) == 276u &&
          line_length(game_pointer[0]) < 384u);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    floor_debt[0] = find_line("[kage-vita] ph120.fd ");
    floor_slack[0] = find_line("[kage-vita] ph120.fs ");
    CHECK(floor_debt[0] != NULL && floor_slack[0] != NULL);
    CHECK(game_pointer[0] < floor_debt[0] && floor_debt[0] < floor_slack[0]);
    /* all-ones counters, every signed gf field at INT32_MIN */
    CHECK(line_length(floor_debt[0]) == 362u);
    CHECK(line_length(floor_slack[0]) == 239u);
    CHECK(strstr(floor_debt[0],
                 "gf(n,pos,max,min,last)=4294967295,4294967295,"
                 "-2147483648,-2147483648,-2147483648\n") != NULL);
#endif
    CHECK(strstr(
        game_pointer[0],
        "frame(last:s,b,e)=ffffffff,4294967295,4294967295 "
        "frame(fail:s,b,e)=ffffffff,4294967295,4294967295\n") != NULL);
    CHECK(parse_scheduler_report_at(scheduler[0], &logged_report_at_us[0]));
    CHECK(logged_report_at_us[0] == UINT64_MAX);

    /* One real fail-open window must carry the exact early return and both
     * frame values into the bounded reason record. */
    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_profile_log_calls = 0u;
    s_scheduler_log_calls = 0u;
    s_now = 1u;
    kage_vita_phase_profile_reset();
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_phase_profile_note_loop_head(1u);
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_early_return(0x002ce1c7u);
    kage_vita_fullspeed_scheduler_note_game_update_end(0x20000000u, 7u);
    for (loop = 1u; loop <= 120u; ++loop) {
        ++s_now;
        kage_vita_phase_profile_note_loop_head(loop + 1u);
    }
    CHECK(s_scheduler_log_calls == 1u);
    CHECK(s_profile_log_calls == ORACLE_PROFILE_RECORDS);
    scheduler[0] = find_line("[kage-vita] ph120.s ");
    game_pointer[0] = find_line("[kage-vita] ph120.r ");
    CHECK(scheduler[0] != NULL && game_pointer[0] != NULL);
    CHECK(strstr(scheduler[0], "viol(d,p,g,s,o)=0,0,1,0,1 ") != NULL);
    CHECK(strstr(
        game_pointer[0],
        "frame(last:s,b,e)=002ce1c7,7,7 "
        "frame(fail:s,b,e)=002ce1c7,7,7\n") != NULL);
    CHECK(line_length(game_pointer[0]) > 0u &&
          line_length(game_pointer[0]) < 384u);
    printf(
        "Vita cadence30/phase integration oracle: PASS "
        "(windows=2 loops=240 render=%u+%u late-skip=%u+%u; "
        "records=%u; ordered/bounded t,c,a%s,s,r,w%s; "
        "report-at exact/monotonic, early-return failure attributed, "
        "window-2 game-rate=%u.%03u Hz)\n",
        renders[0], renders[1], late_skips[0], late_skips[1],
        (unsigned)ORACLE_PROFILE_RECORDS,
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        ",g",
#else
        "",
#endif
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
        ",fd,fs (fd attribution exact; fs floor stamp exact)",
#else
        "",
#endif
        game_update_rate_millihz / 1000u,
        game_update_rate_millihz % 1000u
    );
    return 0;
}
