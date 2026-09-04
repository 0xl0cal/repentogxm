/* Host oracle for the guest sampler's attribution (ISAAC_VITA_GUEST_SAMPLER).
 *
 * kage_vita_guest_sampler.c is compiled with
 * ISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE: its SceKernel thread and production
 * start routine are compiled out and the two memory accessors (guest stack
 * word, mapped PE byte) resolve to the arrays below.  Everything else --
 * marker validation, the x86 call decoder, indirect-callee naming from the
 * published word, phase bucketing, the 256-word bound, the hash and the
 * ph120.hot/hotc/hs reporter -- is the production code, so the records this
 * oracle checks byte-for-byte are the records the device prints.
 *
 * Scenarios (research-sampler-hotlists findings 9 and 5):
 *  1. Title lock path.  The innermost marker is the `call dword ptr [IAT]`
 *     inside the lock wrapper sub_00562e00 while EnterCriticalSection runs.
 *     With the indirect word still holding the previous vtable callee (what
 *     the runtime left before guest_try_direct_sync_import_call published)
 *     the sample is charged to the 2-instruction getter sub_005a0de0; with
 *     the IAT slot published it is "ext"; once the fast path restored the
 *     enclosing target the wrapper body is named by that target; a zero word
 *     stays "ind"; a GL token is "ext".
 *  2. Deep frame.  200 words of locals above the marker: found, deep=1, the
 *     legacy 96-word bound would have reported nomark.  A marker at word 300
 *     is still nomark, i.e. the bound is exactly 256.
 *  3. Pace bucket, through the production scheduler.  The Game::Update pacing
 *     sleep runs with the PACE phase set (sampled there, charged to
 *     Manager::Update in the pace bucket, not in upd), the phase is restored
 *     after the sleep, and pace_wait_calls/pace_waited_us count exactly those
 *     sleeps while the loop-head wait stays outside.
 *  4. Bad ESP is counted and never dereferenced.
 * The printed records double as the fixture for the external ph120 log-tool
 * tests. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "kage_vita_guest_sampler.h"
#include "kage_vita_fullspeed_scheduler.h"

/* Oracle entry points exported by kage_vita_guest_sampler.c in oracle mode. */
void kage_vita_guest_sampler_oracle_bind(
    CPU *c, const uint32_t *functions, uint32_t function_count,
    uint32_t text_present);
void kage_vita_guest_sampler_oracle_sample(void);
uint32_t kage_vita_guest_sampler_oracle_scan_words(void);

/* guest.c owns this in the real build; the oracle drives it directly. */
volatile uint32_t g_kage_guest_last_indirect_target;

#define ORACLE_BUILD_ID "sampler:oracle"

/* ------------------------------------------------------- fake machine ---- */

#define ORACLE_STACK_BASE   UINT32_C(0x0a100000)
#define ORACLE_STACK_WORDS  4096U
#define ORACLE_IMAGE_BYTES  UINT32_C(0x00606000)
#define ORACLE_FILLER_FLOAT UINT32_C(0x3f800000)   /* 1.0f: not a text RVA */
#define ORACLE_FILLER_HEAP  UINT32_C(0x0a2000f0)   /* heap pointer */
#define ORACLE_FILLER_VA    UINT32_C(0x98400000)   /* image VA, not an RVA */

static uint32_t s_stack[ORACLE_STACK_WORDS];
static unsigned char s_image[ORACLE_IMAGE_BYTES];
static uint32_t s_bad_reads;
static CPU s_cpu;

uint32_t kvgs_oracle_stack_word(uint32_t address)
{
    uint32_t offset = address - ORACLE_STACK_BASE;

    if (address < ORACLE_STACK_BASE || (address & 3U) != 0U ||
            offset / 4U >= ORACLE_STACK_WORDS) {
        ++s_bad_reads;
        return 0U;
    }
    return s_stack[offset / 4U];
}

unsigned kvgs_oracle_image_byte(uint32_t rva)
{
    if (rva >= ORACLE_IMAGE_BYTES) {
        ++s_bad_reads;
        return 0U;
    }
    return s_image[rva];
}

/* Registered function starts (sorted, as guest_table.c would list them). */
#define F_LEAF   UINT32_C(0x000023f0)  /* getter leaf, direct callee */
#define F_SPRITE UINT32_C(0x00007af0)  /* ReferenceCount root: call [eax+0xc] */
#define F_DEEP   UINT32_C(0x003cad40)  /* Room::Render-sized frame */
#define F_MAIN   UINT32_C(0x0048bc50)  /* Application::main */
#define F_MGR    UINT32_C(0x004b0010)  /* Manager::Update */
#define F_LOCK   UINT32_C(0x00562e00)  /* lock wrapper: call [0x986060fc] */
#define F_GETTER UINT32_C(0x005a0de0)  /* 2-instruction vtable getter */
static const uint32_t s_functions[] = {
    F_LEAF, F_SPRITE, F_DEEP, F_MAIN, F_MGR, F_LOCK, F_GETTER
};

#define IAT_SLOT_ENTER_CS UINT32_C(0x986060fc)  /* KERNEL32 EnterCriticalSection */
#define GL_TOKEN          UINT32_C(0x7d000031)  /* dynamic GL token */

/* Call sites in the fake .text and their return-site RVAs. */
#define SITE_VT   (F_SPRITE + 0x0cU)   /* FF 50 0C   call dword ptr [eax+0xc] */
#define R_VT      (SITE_VT + 3U)
#define SITE_IAT  UINT32_C(0x00562e29)  /* FF 15 FC 60 60 98  call [0x986060fc] */
#define R_IAT     (SITE_IAT + 6U)
#define SITE_MAIN (F_MAIN + 0x100U)    /* E8 rel32  call Manager::Update */
#define R_MAIN    (SITE_MAIN + 5U)
#define SITE_MGR  (F_MGR + 0x300U)     /* E8 rel32  call F_DEEP */
#define R_MGR     (SITE_MGR + 5U)
#define SITE_DEEP (F_DEEP + 0x50U)     /* E8 rel32  call F_LEAF */
#define R_DEEP    (SITE_DEEP + 5U)

static void emit_call_rel32(uint32_t site, uint32_t target)
{
    uint32_t rel = target - (site + 5U);

    s_image[site] = 0xE8U;
    s_image[site + 1U] = (unsigned char)(rel & 0xffU);
    s_image[site + 2U] = (unsigned char)((rel >> 8) & 0xffU);
    s_image[site + 3U] = (unsigned char)((rel >> 16) & 0xffU);
    s_image[site + 4U] = (unsigned char)((rel >> 24) & 0xffU);
}

static void build_image(void)
{
    memset(s_image, 0, sizeof s_image);
    s_image[0] = 'M';
    s_image[1] = 'Z';
    s_image[SITE_VT] = 0xFFU;
    s_image[SITE_VT + 1U] = 0x50U;
    s_image[SITE_VT + 2U] = 0x0CU;
    s_image[SITE_IAT] = 0xFFU;
    s_image[SITE_IAT + 1U] = 0x15U;
    s_image[SITE_IAT + 2U] = 0xFCU;
    s_image[SITE_IAT + 3U] = 0x60U;
    s_image[SITE_IAT + 4U] = 0x60U;
    s_image[SITE_IAT + 5U] = 0x98U;
    emit_call_rel32(SITE_MAIN, F_MGR);
    emit_call_rel32(SITE_MGR, F_DEEP);
    emit_call_rel32(SITE_DEEP, F_LEAF);
}

static void fill_stack(void)
{
    uint32_t i;

    for (i = 0U; i < ORACLE_STACK_WORDS; ++i) {
        switch (i & 3U) {
        case 0U: s_stack[i] = ORACLE_FILLER_FLOAT; break;
        case 1U: s_stack[i] = ORACLE_FILLER_HEAP; break;
        case 2U: s_stack[i] = ORACLE_FILLER_VA; break;
        default: s_stack[i] = 0U; break;
        }
    }
}

/* Place `words` at stack index `esp_index` upward and point ESP there. */
static void set_stack(uint32_t esp_index, const uint32_t *words, uint32_t count)
{
    uint32_t i;

    fill_stack();
    for (i = 0U; i < count; ++i)
        s_stack[esp_index + i] = words[i];
    s_cpu.esp = ORACLE_STACK_BASE + esp_index * 4U;
}

/* --------------------------------------------------------- log capture --- */

static char s_logs[16384];
static size_t s_log_length;
static uint32_t s_ticks;   /* mirrors the sampler's lifetime tick count */

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    size_t available = sizeof s_logs - s_log_length;
    int result;

    va_start(arguments, format);
    result = vsnprintf(s_logs + s_log_length, available, format, arguments);
    va_end(arguments);
    if (result > 0 && available != 0U) {
        size_t written = (size_t)result;
        if (written >= available)
            written = available - 1U;
        fwrite(s_logs + s_log_length, 1U, written, stdout);
        s_log_length += written;
    }
    return result;
}

int sceClibSnprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

static void sample(void)
{
    ++s_ticks;
    kage_vita_guest_sampler_oracle_sample();
}

static int s_failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, \
                    __LINE__, #condition); \
            ++s_failures; \
        } \
    } while (0)

/* Retire the window and compare the printed records byte-for-byte. */
static void report_expect(uint32_t window, const char *expected)
{
    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, window, 120U * window);
    if (strcmp(s_logs, expected) != 0) {
        fprintf(stderr, "window %u records differ\n--- expected\n%s"
                "--- actual\n%s", (unsigned)window, expected, s_logs);
        ++s_failures;
    }
}

static const char *hs_line(char *buffer, size_t size, uint32_t window,
                           const char *samples, uint32_t total,
                           uint32_t nomark, uint32_t badesp, uint32_t words,
                           uint32_t deep)
{
    snprintf(buffer, size,
             "[kage-vita] ph120.hs bid=" ORACLE_BUILD_ID " win=%u loops=%u "
             "period_us=1000 samples(svc,upd,rnd,swp,lim,oth,pace)=%s "
             "total=%u nomark=%u badesp=%u full=0 words=%u deep=%u "
             "ticks=%u spin=0\n",
             (unsigned)window, (unsigned)(120U * window), samples,
             (unsigned)total, (unsigned)nomark, (unsigned)badesp,
             (unsigned)words, (unsigned)deep, (unsigned)s_ticks);
    return buffer;
}

/* ----------------------------------------------------------- scenarios --- */

/* Scenario 1: the title lock path (finding 9(1)). */
static void scenario_lock_path(void)
{
    /* sub_00007af0 `call [eax+0xc]` -> sub_00562e00 `call [IAT EnterCS]`:
     * ESP -> [R_IAT] [locals] [R_VT] [local] [R_MAIN]. */
    static const uint32_t inside_import[] = {
        R_IAT, ORACLE_FILLER_FLOAT, ORACLE_FILLER_HEAP, R_VT,
        ORACLE_FILLER_HEAP, R_MAIN
    };
    /* Same frames after EnterCriticalSection returned: the wrapper body runs
     * and its own `call [IAT]` return slot is dead below ESP. */
    static const uint32_t inside_wrapper[] = {
        R_VT, ORACLE_FILLER_HEAP, R_MAIN
    };
    char hs[512];
    char expected[2048];

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);

    /* 1a. Runtime before finding 9: the fast path never published, so the
     * word still names the last vtable callee, the getter sub_005a0de0. */
    set_stack(100U, inside_import, 6U);
    g_kage_guest_last_indirect_target = GUEST_IMAGE_BASE + F_GETTER;
    sample();
    sample();
    sample();
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.hot bid=" ORACLE_BUILD_ID " win=1 loops=120 "
             "phase=rnd samples=3 top=5a0de0:3\n"
             "[kage-vita] ph120.hotc bid=" ORACLE_BUILD_ID " win=1 loops=120 "
             "phase=rnd samples=3 top=562e00:3\n%s",
             hs_line(hs, sizeof hs, 1U, "0,0,3,0,0,0,0", 3U, 0U, 0U, 3U, 0U));
    report_expect(1U, expected);
    puts("scenario 1a: stale vtable word -> EnterCS time charged to "
         "sub_005a0de0 (the pre-fix misattribution)");

    /* 1b. guest_try_direct_sync_import_call published the IAT slot. */
    g_kage_guest_last_indirect_target = IAT_SLOT_ENTER_CS;
    sample();
    sample();
    sample();
    /* 1c. The fast path restored the enclosing vtable target (sub_00562e00,
     * called through [eax+0xc]) and the wrapper body continues. */
    set_stack(103U, inside_wrapper, 3U);
    g_kage_guest_last_indirect_target = GUEST_IMAGE_BASE + F_LOCK;
    sample();
    sample();
    /* 1d. No live indirect dispatch: "ind". */
    g_kage_guest_last_indirect_target = 0U;
    sample();
    /* 1e. A dynamic GL token is native code: "ext". */
    g_kage_guest_last_indirect_target = GL_TOKEN;
    sample();
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.hot bid=" ORACLE_BUILD_ID " win=2 loops=240 "
             "phase=rnd samples=7 top=ext:4,562e00:2,ind:1\n"
             "[kage-vita] ph120.hotc bid=" ORACLE_BUILD_ID " win=2 loops=240 "
             "phase=rnd samples=7 top=7af0:4,562e00:3\n%s",
             hs_line(hs, sizeof hs, 2U, "0,0,7,0,0,0,0", 7U, 0U, 0U, 7U, 0U));
    report_expect(2U, expected);
    puts("scenario 1b-e: published slot -> ext; restored target -> "
         "sub_00562e00; zero -> ind; GL token -> ext");
}

/* Scenario 2: frames deeper than 384 bytes (finding 9(3)). */
static void scenario_deep_frame(void)
{
    uint32_t words[301];
    char hs[512];
    char expected[2048];
    uint32_t i;

    CHECK(kage_vita_guest_sampler_oracle_scan_words() == 256U);
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_UPD);
    g_kage_guest_last_indirect_target = 0U;
    for (i = 0U; i < 301U; ++i)
        words[i] = (i & 1U) ? ORACLE_FILLER_HEAP : ORACLE_FILLER_FLOAT;

    /* 2a. Marker at word 200: beyond the legacy 96-word bound, inside 256. */
    words[200] = R_DEEP;
    set_stack(100U, words, 201U);
    sample();
    words[200] = ORACLE_FILLER_FLOAT;
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.hot bid=" ORACLE_BUILD_ID " win=3 loops=360 "
             "phase=upd samples=1 top=23f0:1\n"
             "[kage-vita] ph120.hotc bid=" ORACLE_BUILD_ID " win=3 loops=360 "
             "phase=upd samples=1 top=3cad40:1\n%s",
             hs_line(hs, sizeof hs, 3U, "0,1,0,0,0,0,0", 1U, 0U, 0U, 201U,
                     1U));
    report_expect(3U, expected);
    CHECK(200U >= 96U);
    puts("scenario 2a: marker at word 200 found (deep=1); the 96-word scan "
         "would have counted nomark");

    /* 2b. Marker at word 300: outside the 256-word bound, nomark. */
    words[300] = R_DEEP;
    set_stack(100U, words, 301U);
    sample();
    snprintf(expected, sizeof expected, "%s",
             hs_line(hs, sizeof hs, 4U, "0,0,0,0,0,0,0", 0U, 1U, 0U, 256U,
                     0U));
    report_expect(4U, expected);
    puts("scenario 2b: marker at word 300 stays nomark (bound is 256 words)");
}

/* Scenario 3: the pacing sleep through the production scheduler
 * (finding 5).  The scheduler is linked with ISAAC_VITA_GUEST_SAMPLER so its
 * pace path brackets kage_vita_fullspeed_wait_until with the PACE phase. */
static uint64_t s_now;
static uint32_t s_in_game_update_begin;
static uint32_t s_pace_delays;
static uint32_t s_pace_delay_us;
static uint32_t s_other_delays;
static uint32_t s_phase_wrong;

uint64_t isaac_vita_get_process_time(void)
{
    return s_now;
}

int sceKernelDelayThread(unsigned int delay_us)
{
    uint32_t phase = g_kage_vita_guest_sampler_phase;

    if (s_in_game_update_begin) {
        if (phase != KAGE_VITA_GUEST_SAMPLER_PACE)
            ++s_phase_wrong;
        ++s_pace_delays;
        s_pace_delay_us += delay_us;
        /* The sampler fires while the game thread sleeps here. */
        sample();
    } else {
        if (phase == KAGE_VITA_GUEST_SAMPLER_PACE)
            ++s_phase_wrong;
        ++s_other_delays;
    }
    s_now += delay_us;
    return 0;
}

static void scenario_pace(void)
{
    static const uint32_t in_manager_update[] = { R_MAIN };
    KageVitaFullspeedSnapshot snapshot;
    uint32_t manager_counter = 0U;
    uint32_t game_frame = 0U;
    uint32_t tick;
    char hs[512];
    char expected[2048];

    /* Main called Manager::Update; the innermost marker is that direct call
     * (exactly the device situation behind "Manager::Update self"). */
    set_stack(100U, in_manager_update, 1U);
    g_kage_guest_last_indirect_target = 0U;
    s_now = 1000U;
    kage_vita_fullspeed_scheduler_reset();
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_OTH);
    kage_vita_fullspeed_scheduler_note_loop_head();
    for (tick = 1U; tick <= 8U; ++tick) {
        int render;

        KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_SVC);
        kage_vita_fullspeed_scheduler_note_service();
        /* Alternate a slow and a fast pre-update stretch so the Game::Update
         * floor (anchored at the previous update) is still ahead when the
         * next even tick reaches Manager::Update. */
        s_now += (manager_counter % 4U == 0U) ? 3000U : 200U;
        KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_UPD);
        kage_vita_fullspeed_scheduler_note_manager_dispatch();
        kage_vita_fullspeed_scheduler_note_manager_entry(
            0x10000000U, manager_counter, 0U, 0x20000000U, game_frame);
        if ((manager_counter & 1U) == 0U) {
            s_in_game_update_begin = 1U;
            kage_vita_fullspeed_scheduler_note_game_update_begin(
                manager_counter, 0x20000000U, game_frame);
            s_in_game_update_begin = 0U;
            /* The phase must be back to upd for Game::Update itself. */
            CHECK(g_kage_vita_guest_sampler_phase ==
                  KAGE_VITA_GUEST_SAMPLER_UPD);
            s_now += 2500U;
            ++game_frame;
            kage_vita_fullspeed_scheduler_note_game_update_end(
                0x20000000U, game_frame);
        } else {
            s_now += 500U;
        }
        ++manager_counter;
        render = kage_vita_fullspeed_scheduler_plan_render(
            0x10000000U, manager_counter, 0U, 0x20000000U, game_frame);
        if (render) {
            kage_vita_fullspeed_scheduler_note_render_entry();
            KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
            kage_vita_fullspeed_scheduler_note_render_body(
                0x10000000U, manager_counter, 0U);
            s_now += 4000U;
            KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_SWP);
            kage_vita_fullspeed_scheduler_note_present();
            s_now += 100U;
            kage_vita_fullspeed_scheduler_note_render_return();
        }
        KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_OTH);
        kage_vita_fullspeed_scheduler_note_loop_head();
    }
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    printf("scenario 3: scheduler waits total=%u pace=%u (us=%u) "
           "loop-head=%u runtime_disables=%u sequence_violations=%u\n",
           (unsigned)snapshot.wait_calls, (unsigned)snapshot.pace_wait_calls,
           (unsigned)snapshot.pace_waited_us, (unsigned)s_other_delays,
           (unsigned)snapshot.runtime_disables,
           (unsigned)snapshot.sequence_violations);
    CHECK(snapshot.runtime_disables == 0U);
    CHECK(snapshot.sequence_violations == 0U);
    CHECK(s_pace_delays >= 1U);
    CHECK(s_other_delays >= 1U);
    CHECK(s_phase_wrong == 0U);
    CHECK(snapshot.pace_wait_calls == s_pace_delays);
    CHECK(snapshot.pace_waited_us == s_pace_delay_us);
    CHECK(snapshot.wait_calls == s_pace_delays + s_other_delays);
    CHECK(snapshot.waited_us >= snapshot.pace_waited_us);
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.hot bid=" ORACLE_BUILD_ID " win=5 loops=600 "
             "phase=pace samples=%u top=4b0010:%u\n"
             "[kage-vita] ph120.hotc bid=" ORACLE_BUILD_ID " win=5 loops=600 "
             "phase=pace samples=%u top=48bc50:%u\n",
             (unsigned)s_pace_delays, (unsigned)s_pace_delays,
             (unsigned)s_pace_delays, (unsigned)s_pace_delays);
    {
        char samples[64];
        size_t used = strlen(expected);

        snprintf(samples, sizeof samples, "0,0,0,0,0,0,%u",
                 (unsigned)s_pace_delays);
        hs_line(hs, sizeof hs, 5U, samples, s_pace_delays, 0U, 0U,
                s_pace_delays, 0U);
        snprintf(expected + used, sizeof expected - used, "%s", hs);
    }
    report_expect(5U, expected);
    puts("scenario 3: every pacing sleep sampled in the pace bucket, "
         "none in upd; phase restored after the sleep");
}

/* Scenario 4: an ESP outside the stack is counted, never dereferenced. */
static void scenario_bad_esp(void)
{
    char hs[512];
    uint32_t bad_reads_before = s_bad_reads;

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    fill_stack();
    s_cpu.esp = ORACLE_STACK_BASE - 4U;
    sample();
    s_cpu.esp = ORACLE_STACK_BASE + 2U;   /* misaligned */
    sample();
    report_expect(6U, hs_line(hs, sizeof hs, 6U, "0,0,0,0,0,0,0", 0U, 0U, 2U,
                              0U, 0U));
    CHECK(s_bad_reads == bad_reads_before);
    puts("scenario 4: bad ESP counted twice, no stack read");
}

int main(void)
{
    build_image();
    fill_stack();
    memset(&s_cpu, 0, sizeof s_cpu);
    s_cpu.stack_floor = ORACLE_STACK_BASE;
    s_cpu.stack_ceiling = ORACLE_STACK_BASE + ORACLE_STACK_WORDS * 4U;
    s_cpu.esp = s_cpu.stack_ceiling;
    kage_vita_guest_sampler_oracle_bind(
        &s_cpu, s_functions,
        (uint32_t)(sizeof s_functions / sizeof s_functions[0]), 1U);

    scenario_lock_path();
    scenario_deep_frame();
    scenario_pace();
    scenario_bad_esp();

    CHECK(s_bad_reads == 0U);
    if (s_failures) {
        fprintf(stderr, "Vita guest sampler host oracle: FAIL (%d)\n",
                s_failures);
        return 1;
    }
    printf("Vita guest sampler host oracle: PASS (scan_words=%u, "
           "ticks=%u, pace_waits=%u)\n",
           (unsigned)kage_vita_guest_sampler_oracle_scan_words(),
           (unsigned)s_ticks, (unsigned)s_pace_delays);
    return 0;
}
