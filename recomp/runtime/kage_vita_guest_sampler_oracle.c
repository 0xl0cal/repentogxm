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
 *  5. kage_vita_guest_sampler_stop unbinds the CPU: no stack read and no
 *     sample after it, the reporter still prints.
 *  6. (CHAIN) the inclusive chain view; 7. (PRESENT_LABEL) swp = present.
 *  8. (RESIDUAL) the Lua callback boundary: a translated C function called
 *     by the Lua VM (caller marker 8 bytes above the boundary word), the
 *     guest allocator (20 bytes), a boundary with no caller marker, a sample
 *     whose innermost marker is above the boundary, the hotr Lua bit, the
 *     invoker split (ph120.hotl), rootless samples by outermost frame
 *     (ph120.hotu) and the watch list (ph120.hotw).
 *  9. (PROFILE_FUNCTION_ENTRY_CENSUS) ph120.fe: watched body entries per
 *     phase through the production guest_coverage_function hook, unwatched
 *     and empty-argument ids add nothing, multi-line split under 384 bytes,
 *     take-and-zero; with a stale id space only ids=STALE prints.
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

/* The chain walk's clock (ISAAC_VITA_GUEST_SAMPLER_CHAIN): +1 per read, so
 * every walk spans exactly 1 us. */
static uint64_t s_now_us;
uint64_t kvgs_oracle_now_us(void);
uint64_t kvgs_oracle_now_us(void)
{
    return ++s_now_us;
}

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
static uint32_t s_stack_reads;   /* in-range guest stack word reads */
static CPU s_cpu;

uint32_t kvgs_oracle_stack_word(uint32_t address)
{
    uint32_t offset = address - ORACLE_STACK_BASE;

    if (address < ORACLE_STACK_BASE || (address & 3U) != 0U ||
            offset / 4U >= ORACLE_STACK_WORDS) {
        ++s_bad_reads;
        return 0U;
    }
    ++s_stack_reads;
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
#define F_THUNK  UINT32_C(0x002dbe30)  /* add ecx,0x40; jmp F_DEEP */
#define F_JMP    UINT32_C(0x002dbe40)  /* jmp F_DEEP */
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
/* Scenario 8 only, at the RVAs the sampler's watch list and root list name
 * (registered starts; their bodies here are only the call sites below). */
#define F_LUABIND UINT32_C(0x003f8e30)  /* translated C function the VM calls */
#define F_LUAPASS UINT32_C(0x003ffe10)  /* chain root: calls the invoker */
#define F_INVOKER UINT32_C(0x0040eef0)  /* chain root: call [IAT] into Lua */
#define F_LUAALLOC UINT32_C(0x00439e80) /* guest allocator the VM calls */
#endif
static const uint32_t s_functions[] = {
    F_LEAF, F_SPRITE, F_THUNK, F_JMP, F_DEEP,
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    F_LUABIND, F_LUAPASS, F_INVOKER, F_LUAALLOC,
#endif
    F_MAIN, F_MGR, F_LOCK, F_GETTER
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
#define SITE_MGR2 (F_MGR + 0x400U)     /* E8 rel32  call F_THUNK */
#define R_MGR2    (SITE_MGR2 + 5U)
#define SITE_MGR3 (F_MGR + 0x500U)     /* E8 rel32  call F_JMP */
#define R_MGR3    (SITE_MGR3 + 5U)
#define SITE_DEEP2 (F_DEEP + 0x80U)    /* E8 rel32  call F_DEEP (recursion) */
#define SITE_MAIN2 (F_MAIN + 0x200U)   /* E8 rel32  call F_SPRITE */
#define R_MAIN2   (SITE_MAIN2 + 5U)
#define R_DEEP2   (SITE_DEEP2 + 5U)
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
#define IAT_SLOT_LUA  UINT32_C(0x98606200)  /* a Lua import slot (not text) */
#define LUA_RETURN    UINT32_C(0xfff15333)  /* host_vita_lua.c:71 */
#define SITE_INV  (F_INVOKER + 0x40U)   /* FF 15 slot   call [IAT_SLOT_LUA] */
#define R_INV     (SITE_INV + 6U)
#define SITE_BIND (F_LUABIND + 0x30U)   /* FF 15 slot   call [IAT_SLOT_LUA] */
#define R_BIND    (SITE_BIND + 6U)
#define SITE_PASS (F_LUAPASS + 0x20U)   /* E8 rel32  call F_INVOKER */
#define R_PASS    (SITE_PASS + 5U)
#define SITE_MAIN3 (F_MAIN + 0x300U)    /* E8 rel32  call F_LUAPASS */
#define R_MAIN3   (SITE_MAIN3 + 5U)
#define SITE_ALLOC (F_LUAALLOC + 0x10U) /* E8 rel32  call F_LEAF */
#define R_ALLOC   (SITE_ALLOC + 5U)
#endif

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
    emit_call_rel32(SITE_MGR2, F_THUNK);
    s_image[F_THUNK] = 0x83U;          /* add ecx, 0x40 */
    s_image[F_THUNK + 1U] = 0xC1U;
    s_image[F_THUNK + 2U] = 0x40U;
    emit_call_rel32(F_THUNK + 3U, F_DEEP);
    s_image[F_THUNK + 3U] = 0xE9U;     /* jmp rel32 (same encoding, E9) */
    emit_call_rel32(SITE_MGR3, F_JMP);
    emit_call_rel32(F_JMP, F_DEEP);
    s_image[F_JMP] = 0xE9U;
    emit_call_rel32(SITE_DEEP2, F_DEEP);
    emit_call_rel32(SITE_MAIN2, F_SPRITE);
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    {
        static const uint32_t sites[] = { SITE_INV, SITE_BIND };
        unsigned k;
        for (k = 0U; k < 2U; ++k) {
            s_image[sites[k]] = 0xFFU;
            s_image[sites[k] + 1U] = 0x15U;
            s_image[sites[k] + 2U] = (unsigned char)(IAT_SLOT_LUA & 0xffU);
            s_image[sites[k] + 3U] = (unsigned char)((IAT_SLOT_LUA >> 8) & 0xffU);
            s_image[sites[k] + 4U] = (unsigned char)((IAT_SLOT_LUA >> 16) & 0xffU);
            s_image[sites[k] + 5U] = (unsigned char)(IAT_SLOT_LUA >> 24);
        }
    }
    emit_call_rel32(SITE_PASS, F_INVOKER);
    emit_call_rel32(SITE_MAIN3, F_LUAPASS);
    emit_call_rel32(SITE_ALLOC, F_LEAF);
#endif
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
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN) || \
    defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
    /* The self/caller/hs records must be unchanged by the chain view, the
     * residual records and the entry census; their own records are checked
     * by scenarios 6, 8 and 9. */
    {
        char *line = s_logs;
        char *out = s_logs;
        while (*line) {
            char *end = strchr(line, '\n');
            size_t length = end ? (size_t)(end - line) + 1U : strlen(line);
            if (strncmp(line, "[kage-vita] ph120.hoti ", 23U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.hotr ", 23U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.hotw ", 23U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.hotl ", 23U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.hotu ", 23U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.hcl ", 22U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.hci ", 22U) != 0 &&
                    strncmp(line, "[kage-vita] ph120.fe ", 21U) != 0) {
                memmove(out, line, length);
                out += length;
            }
            line += length;
        }
        *out = '\0';
    }
#endif
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

#if defined(ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL)
/* Scenario 7: the swp phase is the native present.  A stale direct marker
 * (the device shape: MusicManager::Update's return in 0x4B0600's locals)
 * must not be named; both views say "present" and no stack is read. */
static void scenario_present(void)
{
    static const uint32_t stale[] = { R_MAIN };
    char hs[512];
    char expected[2048];
    uint32_t reads_before;

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_SWP);
    set_stack(100U, stale, 1U);
    g_kage_guest_last_indirect_target = 0U;
    reads_before = s_stack_reads;
    sample();
    sample();
    sample();
    CHECK(s_stack_reads == reads_before);
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.hot bid=" ORACLE_BUILD_ID " win=9 loops=1080 "
             "phase=swp samples=3 top=present:3\n"
             "[kage-vita] ph120.hotc bid=" ORACLE_BUILD_ID " win=9 loops=1080 "
             "phase=swp samples=3 top=present:3\n%s",
             hs_line(hs, sizeof hs, 9U, "0,0,0,3,0,0,0", 3U, 0U, 0U, 0U, 0U));
    report_expect(9U, expected);
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    puts("scenario 7: swp samples named present, no stack read");
}
#endif

/* Scenario 5: kage_vita_guest_sampler_stop (entry tail, before
 * guest_stack_free).  One sample before the stop reads the stack and is
 * charged; after the stop a tick makes no stack read and no sample (not
 * even bad_esp: the CPU is gone, not its stack bounds), while the reporter
 * still prints the window.  Production pairs this with the game thread's
 * guest_stack_free, whose ISAAC_VITA_GUEST_STACK_MEMBLOCK arm unmaps the
 * stack. */
static void scenario_stop(void)
{
    static const uint32_t in_manager_update[] = { R_MAIN };
    char hs[512];
    char expected[2048];
    uint32_t reads_before;

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    set_stack(100U, in_manager_update, 1U);
    g_kage_guest_last_indirect_target = 0U;
    reads_before = s_stack_reads;
    sample();
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
    /* rnd: the chain walk reads every word above the marker to the ceiling */
    CHECK(s_stack_reads == reads_before + 1U + (ORACLE_STACK_WORDS - 101U));
#else
    CHECK(s_stack_reads == reads_before + 1U);
#endif

    kage_vita_guest_sampler_stop();
    reads_before = s_stack_reads;
    sample();
    s_cpu.esp = ORACLE_STACK_BASE - 4U;   /* would count bad_esp when bound */
    sample();
    CHECK(s_stack_reads == reads_before);
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.hot bid=" ORACLE_BUILD_ID " win=7 loops=840 "
             "phase=rnd samples=1 top=4b0010:1\n"
             "[kage-vita] ph120.hotc bid=" ORACLE_BUILD_ID " win=7 loops=840 "
             "phase=rnd samples=1 top=48bc50:1\n%s",
             hs_line(hs, sizeof hs, 7U, "0,0,1,0,0,0,0", 1U, 0U, 0U, 1U, 0U));
    report_expect(7U, expected);
    puts("scenario 5: stop unbinds the CPU: one sample before it, no stack "
         "read and no sample (not even badesp) after it, report still prints");
}

#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
static void expect_item_in(const char *logs, const char *record,
                           const char *item)
{
    const char *line = strstr(logs, record);
    const char *end;
    char needle[64];

    snprintf(needle, sizeof needle, "%s", item);
    if (!line) {
        fprintf(stderr, "no ph120.hoti record\n");
        ++s_failures;
        return;
    }
    end = strchr(line, '\n');
    /* item preceded by '=' or ',' and followed by ',' or end of line */
    for (const char *at = strstr(line, needle); at && at < end;
            at = strstr(at + 1, needle)) {
        size_t n = strlen(needle);
        if ((at[-1] == '=' || at[-1] == ',') && (at[n] == ',' || at[n] == '\n'))
            return;
    }
    fprintf(stderr, "%s lacks %s\n%s", record, item, logs);
    ++s_failures;
}

static void expect_item(const char *logs, const char *item)
{
    expect_item_in(logs, "] ph120.hoti ", item);
}

/* Scenario 6: the inclusive chain view (ISAAC_VITA_GUEST_SAMPLER_CHAIN). */
static void scenario_chain(void)
{
    /* s1: leaf in F_DEEP; a stale direct word (call F_MGR) inside F_DEEP's
     * frame must be skipped; then F_MGR -> F_DEEP, F_MAIN -> F_MGR. */
    static const uint32_t s1[] = { R_DEEP, ORACLE_FILLER_HEAP, R_MAIN, R_MGR,
                                   ORACLE_FILLER_FLOAT, R_MAIN };
    /* s2: F_MGR reaches F_DEEP through the this-adjust thunk. */
    static const uint32_t s2[] = { R_DEEP, R_MGR2, R_MAIN };
    /* s3: EnterCS (ext, no inclusive row) in F_LOCK; the vtable marker in
     * F_SPRITE is frame linked ([A-4] = EBP, [EBP+4] = R_MAIN) but R_MAIN
     * calls F_MGR, not F_SPRITE: the indirect link is rejected (indrej),
     * then R_MAIN does not link to F_LOCK either (skip). */
    static const uint32_t s3[] = { R_IAT, ORACLE_STACK_BASE + 4U * 104U, R_VT,
                                   ORACLE_FILLER_HEAP, ORACLE_FILLER_FLOAT,
                                   R_MAIN };
    /* s8: the same vtable marker, now linked to F_MAIN's call of F_SPRITE:
     * accepted (ind), and the walk resumes at EBP+4. */
    static const uint32_t s8[] = { R_IAT, ORACLE_STACK_BASE + 4U * 104U, R_VT,
                                   R_MAIN, ORACLE_FILLER_FLOAT, R_MAIN2 };
    /* s9: a stale vtable word inside F_DEEP's frame; [A-4] is a return
     * word, not a stack address: rejected; R_MGR still links. */
    static const uint32_t s9[] = { R_DEEP, R_VT, R_MGR };
    /* s4: the chain stops where the stack does: F_LEAF, F_DEEP, F_MGR. */
    static const uint32_t s4[] = { R_DEEP, R_MGR };
    /* s5: upd phase: no walk. */
    static const uint32_t s5[] = { R_DEEP, R_MGR, R_MAIN };
    /* s6: F_DEEP recursing into itself counts once. */
    static const uint32_t s6[] = { R_DEEP, R_DEEP2, R_MGR };
    /* s7: F_MGR reaches F_DEEP through a plain `jmp rel32` thunk. */
    static const uint32_t s7[] = { R_DEEP, R_MGR3 };

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    set_stack(100U, s1, 6U);
    g_kage_guest_last_indirect_target = 0U;
    sample();
    set_stack(100U, s2, 3U);
    sample();
    set_stack(100U, s3, 6U);
    g_kage_guest_last_indirect_target = IAT_SLOT_ENTER_CS;
    sample();
    set_stack(100U, s8, 6U);
    sample();
    g_kage_guest_last_indirect_target = 0U;
    set_stack(100U, s4, 2U);
    sample();
    set_stack(100U, s6, 3U);
    sample();
    set_stack(100U, s7, 2U);
    sample();
    set_stack(100U, s9, 3U);
    sample();
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_UPD);
    set_stack(100U, s5, 3U);
    sample();

    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 8U, 960U);
    expect_item(s_logs, "23f0:6");
    expect_item(s_logs, "3cad40:6");
    expect_item(s_logs, "4b0010:6");
    expect_item(s_logs, "48bc50:3");
    expect_item(s_logs, "562e00:2");
    expect_item(s_logs, "7af0:1");
    /* ph120.hotr: innermost listed root (F_DEEP = 3cad40, F_MAIN = 48bc50);
     * +1000000 = the sample itself is not translated (EnterCS). */
    expect_item_in(s_logs, "] ph120.hotr ", "3cad40:6");
    expect_item_in(s_logs, "] ph120.hotr ", "1000000:1");
    expect_item_in(s_logs, "] ph120.hotr ", "148bc50:1");
    CHECK(strstr(s_logs, "phase=upd samples=1 top=") != NULL);
    CHECK(strstr(s_logs, "] ph120.hoti bid=" ORACLE_BUILD_ID
                 " win=8 loops=960 phase=upd") == NULL);
    CHECK(strstr(s_logs, "[kage-vita] ph120.hci bid=" ORACLE_BUILD_ID
                 " win=8 n=8 links=11 ind=1 thunk=2 skip=2 full=0 indrej=2 "
                 "us=8,1\n") != NULL);
    {
        /* every hoti item names a function, never ext/ind */
        const char *line = strstr(s_logs, "] ph120.hoti ");
        CHECK(line && !strstr(line, "ext:") && !strstr(line, "ind:"));
    }
    puts("scenario 6: chain view counts each live ancestor once, skips a stale "
         "direct word, follows a this-adjust thunk, accepts an indirect link, "
         "walks only the render phase");
}
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL) || \
    defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
static void expect_line(const char *logs, const char *line)
{
    if (!strstr(logs, line)) {
        fprintf(stderr, "missing record\n%s--- in\n%s", line, logs);
        ++s_failures;
    }
}
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
#include "kage_vita_render_watch.h"
#define ORACLE_WATCH_ONE(rva) + 1U
static const unsigned s_watch_count = 0U KAGE_VITA_RENDER_WATCH(ORACLE_WATCH_ONE);
#undef ORACLE_WATCH_ONE

/* host_vita_lua.c's publication of the live boundary words
 * (kage_vita_guest_sampler.h): the guest address of the LUA_RETURN word of a
 * running C-function callback (by frame slot) or of the running allocator. */
static void lua_publish_none(void)
{
    uint32_t k;

    for (k = 0U; k < KAGE_VITA_GUEST_SAMPLER_LUA_CFUNCTION_SLOTS; ++k)
        g_kage_vita_guest_sampler_lua_cfunction[k] = 0U;
    g_kage_vita_guest_sampler_lua_allocator = 0U;
}

static void lua_publish_cfunction(uint32_t slot, uint32_t stack_index)
{
    g_kage_vita_guest_sampler_lua_cfunction[slot] =
        ORACLE_STACK_BASE + stack_index * 4U;
}

static void lua_publish_allocator(uint32_t stack_index)
{
    g_kage_vita_guest_sampler_lua_allocator =
        ORACLE_STACK_BASE + stack_index * 4U;
}

/* Scenario 8: the render residual records (ISAAC_VITA_GUEST_SAMPLER_RESIDUAL).
 * Stack shapes are the pushes of host_vita_lua.c: the C-function trampoline
 * pushes the state, then LUA_RETURN, then guest_call (:1004-1006), over the
 * translated caller's `call [IAT]` return word, which the import bridge left
 * at [esp] (vita_lua_arg reads esp+4.., :269-273); the allocator pushes four
 * arguments, then LUA_RETURN (:1460-1464). */
static void scenario_residual(void)
{
    /* s10: inside the C function's own code: the boundary is the first
     * marker-like word, the innermost marker is the invoker's. */
    static const uint32_t s10[] = { ORACLE_FILLER_FLOAT, LUA_RETURN,
                                    ORACLE_FILLER_HEAP, R_INV,
                                    ORACLE_FILLER_FLOAT, R_PASS, R_MAIN3 };
    /* s11: the C function called a Lua import: its marker is innermost, the
     * walk crosses the boundary to the invoker 8 bytes above it. */
    static const uint32_t s11[] = { R_BIND, ORACLE_FILLER_FLOAT, LUA_RETURN,
                                    ORACLE_FILLER_HEAP, R_INV,
                                    ORACLE_FILLER_FLOAT, R_PASS, R_MAIN3 };
    /* s12: the guest allocator called a translated leaf: ud, pointer, old
     * and new size between the boundary and the caller marker (+20). */
    static const uint32_t s12[] = { R_ALLOC, ORACLE_FILLER_FLOAT, LUA_RETURN,
                                    ORACLE_FILLER_HEAP, ORACLE_FILLER_HEAP,
                                    0x20U, 0x40U, R_INV, R_PASS, R_MAIN3 };
    /* s13: a boundary with no return site at +8 or +20 (lbm): the chain
     * stays in the C function, R_MAIN3 does not link to it, no root. */
    static const uint32_t s13[] = { R_BIND, LUA_RETURN, ORACLE_FILLER_HEAP,
                                    ORACLE_FILLER_FLOAT, ORACLE_FILLER_HEAP,
                                    ORACLE_FILLER_FLOAT, ORACLE_FILLER_HEAP,
                                    R_MAIN3 };
    /* s14: translated, rootless, no Lua: the lock wrapper alone. */
    static const uint32_t s14[] = { R_IAT };
    /* s15: the Lua VM itself (no callback active) under the invoker. */
    static const uint32_t s15[] = { R_INV, ORACLE_FILLER_FLOAT, R_PASS, R_MAIN3 };
    /* s16: the invoker with no root outside it. */
    static const uint32_t s16[] = { R_INV };
    char hcl[256];
    const char *at;

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    /* Each boundary word is live: host_vita_lua.c published its address. */
    lua_publish_none();
    lua_publish_cfunction(0U, 101U);
    set_stack(100U, s10, 7U);
    g_kage_guest_last_indirect_target = GUEST_IMAGE_BASE + F_LUABIND;
    sample();
    lua_publish_none();
    lua_publish_cfunction(0U, 102U);
    set_stack(100U, s11, 8U);
    g_kage_guest_last_indirect_target = IAT_SLOT_LUA;
    sample();
    lua_publish_none();
    lua_publish_allocator(102U);
    set_stack(100U, s12, 10U);
    g_kage_guest_last_indirect_target = 0U;
    sample();
    lua_publish_none();
    lua_publish_cfunction(0U, 101U);
    set_stack(100U, s13, 8U);
    g_kage_guest_last_indirect_target = IAT_SLOT_LUA;
    sample();
    lua_publish_none();
    set_stack(100U, s14, 1U);
    g_kage_guest_last_indirect_target = GUEST_IMAGE_BASE + F_GETTER;
    sample();
    set_stack(100U, s15, 4U);
    g_kage_guest_last_indirect_target = IAT_SLOT_LUA;
    sample();
    set_stack(100U, s16, 1U);
    sample();
    g_kage_guest_last_indirect_target = 0U;

    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 10U, 1200U);
    /* w=rva:self/caller/incl/root/root_native/lua_root/lua_root_native, in
     * watch order, zero rows omitted. */
    expect_line(s_logs,
                "[kage-vita] ph120.hotw bid=" ORACLE_BUILD_ID " win=10 "
                "loops=1200 phase=rnd samples=7 w=48bc50:0/0/4/0/0/0/0,"
                "562e00:0/1/1/0/0/0/0,40eef0:0/3/5/0/2/2/1,"
                "3ffe10:0/0/4/0/0/0/0,3f8e30:1/2/3/0/0/0/0,"
                "439e80:0/1/1/0/0/0/0\n");
    /* o=outer:t/n/lt/ln: s15 n, s10+s12 lt, s11 ln under 3ffe10; s16 n with
     * no outer root. */
    expect_line(s_logs,
                "[kage-vita] ph120.hotl bid=" ORACLE_BUILD_ID " win=10 "
                "loops=1200 phase=rnd samples=7 inv=40eef0 "
                "o=3ffe10:0/1/2/1,0:0/1/0/0\n");
    snprintf(hcl, sizeof hcl,
             "[kage-vita] ph120.hcl bid=" ORACLE_BUILD_ID " win=10 lb=2 lbm=1 "
             "lbs=1 lbx=0 watch=%u\n", (unsigned)s_watch_count);
    expect_line(s_logs, hcl);
    /* The boundary links are not chain links: 2 each for s10/s11/s12/s15,
     * s13's R_MAIN3 is a skip, no indirect marker is walked. */
    expect_line(s_logs,
                "[kage-vita] ph120.hci bid=" ORACLE_BUILD_ID " win=10 n=7 "
                "links=8 ind=0 thunk=0 skip=1 full=0 indrej=0 us=7,1\n");
    /* hotr: +800000 = the root's code started the Lua callback the sample is
     * in; +1000000 = native sample; 0 = no root. */
    expect_item_in(s_logs, "] ph120.hotr ", "c0eef0:2");
    expect_item_in(s_logs, "] ph120.hotr ", "1c0eef0:1");
    expect_item_in(s_logs, "] ph120.hotr ", "140eef0:2");
    expect_item_in(s_logs, "] ph120.hotr ", "1000000:1");
    expect_item_in(s_logs, "] ph120.hotr ", "0:1");
    /* hotu: rootless samples by outermost function, +1000000 = native. */
    expect_item_in(s_logs, "] ph120.hotu ", "13f8e30:1");
    expect_item_in(s_logs, "] ph120.hotu ", "562e00:1");
    /* inclusive view across the boundary */
    expect_item(s_logs, "40eef0:5");
    expect_item(s_logs, "3ffe10:4");
    expect_item(s_logs, "48bc50:4");
    expect_item(s_logs, "3f8e30:3");
    expect_item(s_logs, "439e80:1");
    expect_item(s_logs, "23f0:1");
    /* every record line stays under the 384-byte durable log bound */
    for (at = s_logs; *at;) {
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) + 1U : strlen(at);
        CHECK(length < 384U);
        at += length;
    }
    puts("scenario 8: residual: Lua boundary crossed at +8 (C function) and "
         "+20 (allocator), miss counted, innermost-above-boundary marked, "
         "hotr Lua bit, invoker split, rootless by outer frame, watch rows");
}

/* The widest header (all-ones window and loops) still leaves room for the
 * items: one bounded ph120.hotw line.  The multi-line split itself is the
 * shared kvgs_lines helper, driven to several lines by scenario 9. */
static void scenario_residual_split(void)
{
    static const uint32_t s17[] = { R_INV, ORACLE_FILLER_FLOAT, R_PASS, R_MAIN3 };
    char *line;
    uint32_t lines = 0U;
    uint32_t k;

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    set_stack(100U, s17, 4U);
    g_kage_guest_last_indirect_target = IAT_SLOT_LUA;
    for (k = 0U; k < 3U; ++k)
        sample();
    g_kage_guest_last_indirect_target = 0U;
    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 4294967295U, 4294967295U);
    for (line = strstr(s_logs, "[kage-vita] ph120.hotw "); line;
            line = strstr(line + 1, "[kage-vita] ph120.hotw ")) {
        const char *end = strchr(line, '\n');
        CHECK(end && (size_t)(end - line) + 1U < 384U);
        CHECK(strncmp(line, "[kage-vita] ph120.hotw bid=" ORACLE_BUILD_ID
                      " win=4294967295 loops=4294967295 phase=rnd samples=3 w=",
                      78U) == 0);
        ++lines;
    }
    CHECK(lines == 1U);
    puts("scenario 8b: hotw at all-ones window/loops stays one bounded line");
}

/* Count of item `name` in the first `record` line of `logs`; -1 = no such
 * item, -2 = no such record. */
static long item_count(const char *logs, const char *record, const char *name)
{
    const char *line = strstr(logs, record);
    const char *end;
    const char *at;
    size_t n = strlen(name);

    if (!line)
        return -2;
    end = strchr(line, '\n');
    if (!end)
        end = line + strlen(line);
    for (at = strstr(line, name); at && at < end; at = strstr(at + 1, name))
        if ((at[-1] == '=' || at[-1] == ',') && at[n] == ':')
            return strtol(at + n + 1, NULL, 10);
    return -1;
}

/* Scenario 8c: stale boundary words.  A C-function callback
 * that returned leaves [LUA_RETURN][state][caller marker] below ESP (pushed at
 * host_vita_lua.c:1004-1005, popped by moving ESP only), and the next calls at
 * that depth reuse the slots without overwriting all of them: per-entity EID
 * callbacks run from the render pass, so render code later runs over such
 * words.  Only a word at an address host_vita_lua.c published as live is a
 * boundary; t1/t2/t4 carry a stale word, t1/t2 with unrelated live
 * addresses published elsewhere. */
static void scenario_residual_stale(void)
{
    /* t1: a live F_SPRITE <- F_MAIN chain (vtable call into the getter) with a
     * stale triple in F_SPRITE's locals whose caller marker is the invoker's:
     * root 48bc50, no 40eef0, no Lua bit. */
    static const uint32_t t1[] = { R_VT, ORACLE_FILLER_FLOAT, LUA_RETURN,
                                   ORACLE_FILLER_HEAP, R_INV,
                                   ORACLE_FILLER_FLOAT, R_MAIN2 };
    /* t2: a stale word below the innermost marker: not inside a binding. */
    static const uint32_t t2[] = { ORACLE_FILLER_FLOAT, LUA_RETURN,
                                   ORACLE_FILLER_HEAP, R_MAIN2 };
    /* t3 (B2): a live crossing into a binding whose walk then breaks: no
     * root, Lua bit (key 800000), hotu names the binding.  The analyzer files
     * it under "no root" (bench_run_test.py leg residual). */
    static const uint32_t t3[] = { R_VT, ORACLE_FILLER_FLOAT, LUA_RETURN,
                                   ORACLE_FILLER_HEAP, R_BIND };
    /* t4: inside a live callback (F_LUABIND called a Lua import; its
     * boundary at word 106 published in slot 5) with a stale triple of an
     * earlier callback in its own frame whose marker names F_MAIN: the stale
     * word is skipped, R_MAIN2 does not link to F_LUABIND, the live boundary
     * links to the invoker: root 40eef0 inside the callback (1c0eef0, the
     * sample is the import), outer root 3ffe10.  Taking the stale word would
     * make it root 48bc50 (1c8bc50). */
    static const uint32_t t4[] = { R_BIND, ORACLE_FILLER_FLOAT, LUA_RETURN,
                                   ORACLE_FILLER_HEAP, R_MAIN2,
                                   ORACLE_FILLER_FLOAT, LUA_RETURN,
                                   ORACLE_FILLER_HEAP, R_INV,
                                   ORACLE_FILLER_FLOAT, R_PASS, R_MAIN3 };
    char hcl[256];

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    lua_publish_none();
    lua_publish_cfunction(3U, 300U);
    lua_publish_allocator(200U);
    set_stack(100U, t1, 7U);
    g_kage_guest_last_indirect_target = GUEST_IMAGE_BASE + F_GETTER;
    sample();
    set_stack(100U, t2, 4U);
    sample();
    lua_publish_none();
    lua_publish_cfunction(0U, 102U);
    set_stack(100U, t3, 5U);
    sample();
    lua_publish_none();
    lua_publish_cfunction(5U, 106U);
    set_stack(100U, t4, 12U);
    g_kage_guest_last_indirect_target = IAT_SLOT_LUA;
    sample();
    g_kage_guest_last_indirect_target = 0U;
    lua_publish_none();

    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 11U, 1320U);
    CHECK(item_count(s_logs, "] ph120.hotr ", "48bc50") == 2);
    CHECK(item_count(s_logs, "] ph120.hotr ", "800000") == 1);
    CHECK(item_count(s_logs, "] ph120.hotr ", "1c0eef0") == 1);
    CHECK(item_count(s_logs, "] ph120.hotr ", "c0eef0") == -1);
    CHECK(item_count(s_logs, "] ph120.hotr ", "c8bc50") == -1);
    CHECK(item_count(s_logs, "] ph120.hotr ", "1c8bc50") == -1);
    CHECK(item_count(s_logs, "] ph120.hoti ", "48bc50") == 3);
    CHECK(item_count(s_logs, "] ph120.hoti ", "7af0") == 3);
    CHECK(item_count(s_logs, "] ph120.hoti ", "40eef0") == 1);
    CHECK(item_count(s_logs, "] ph120.hoti ", "3f8e30") == 2);
    CHECK(item_count(s_logs, "] ph120.hotu ", "3f8e30") == 1);
    expect_line(s_logs,
                "[kage-vita] ph120.hotl bid=" ORACLE_BUILD_ID " win=11 "
                "loops=1320 phase=rnd samples=4 inv=40eef0 o=3ffe10:0/0/0/1\n");
    snprintf(hcl, sizeof hcl,
             "[kage-vita] ph120.hcl bid=" ORACLE_BUILD_ID " win=11 lb=2 lbm=0 "
             "lbs=0 lbx=3 watch=%u\n", (unsigned)s_watch_count);
    expect_line(s_logs, hcl);
    if (s_failures)
        fprintf(stderr, "scenario 8c records:\n%s", s_logs);
    puts("scenario 8c: stale boundary words (unpublished addresses) are "
         "counted (lbx) and never linked; a live crossing with no root keeps "
         "the Lua bit on root 0");
}
#endif

#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
unsigned char *g_guest_coverage_functions;   /* guest.c owns it */

static const uint32_t s_watch_rvas[GUEST_ENTRY_CENSUS_COUNT] =
    GUEST_ENTRY_CENSUS_RVAS;

/* Coverage id of a watched RVA through the generated switch itself. */
static uint32_t census_id(uint32_t rva)
{
    uint32_t id;

    for (id = 0U; id < GUEST_ENTRY_CENSUS_FUNCTION_COUNT; ++id) {
        uint32_t slot = guest_entry_census_slot(id);
        if (slot != UINT32_MAX && s_watch_rvas[slot] == rva)
            return id;
    }
    fprintf(stderr, "rva %x is not watched\n", (unsigned)rva);
    ++s_failures;
    return 0U;
}

static void enter(uint32_t id, uint32_t times)
{
    while (times--)
        guest_coverage_function(id);
}

/* Scenario 9: ph120.fe through the production hook (guest.h ->
 * guest_entry_census.h -> generated guest_entry_census_ids.h). */
static void scenario_census(void)
{
    char expected[1024];
    char *line;
    uint32_t lines = 0U;
    uint32_t unwatched = UINT32_MAX;
    uint32_t id;
    uint32_t i;

    for (id = 1U; id < GUEST_ENTRY_CENSUS_FUNCTION_COUNT; ++id)
        if (guest_entry_census_slot(id) == UINT32_MAX) {
            unwatched = id;
            break;
        }
    CHECK(unwatched != UINT32_MAX);
    CHECK(guest_entry_census_slot(0U) == UINT32_MAX);
    CHECK(guest_entry_census_slot(GUEST_ENTRY_CENSUS_FUNCTION_COUNT) ==
          UINT32_MAX);

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_UPD);
    enter(census_id(0x00003e50U), 3U);
    enter(unwatched, 5U);
    GUEST_PROFILE_NOTE_FUNCTION_ENTRY();   /* the runtime seams' spelling */
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    enter(census_id(0x00004b60U), 2U);
    enter(census_id(0x00003e50U), 1U);
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_SWP);
    enter(census_id(0x00562e00U), 1U);
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);

    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 11U, 1320U);
#if defined(ORACLE_CENSUS_STALE)
    expect_line(s_logs, "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID
                " win=11 loops=1320 ids=STALE\n");
    CHECK(strstr(s_logs, " e=") == NULL);
    puts("scenario 9: stale id space prints ids=STALE only");
    (void)expected;
    (void)line;
    (void)lines;
    (void)i;
#else
    snprintf(expected, sizeof expected,
             "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID " win=11 loops=1320 "
             "phase=upd e=3e50:3\n"
             "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID " win=11 loops=1320 "
             "phase=rnd e=3e50:1,4b60:2\n"
             "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID " win=11 loops=1320 "
             "phase=swp e=562e00:1\n");
    expect_line(s_logs, expected);
    /* take-and-zero: the next window prints the two always-on phases empty */
    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 12U, 1440U);
    expect_line(s_logs,
                "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID " win=12 loops=1440 "
                "phase=upd e=\n"
                "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID " win=12 loops=1440 "
                "phase=rnd e=\n");
    CHECK(strstr(s_logs, "phase=swp e=") == NULL);
    /* every watched function at the largest count: whole items, full header
     * on every line, all under 384 bytes, every item exactly once */
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    for (i = 0U; i < GUEST_ENTRY_CENSUS_COUNT; ++i)
        g_guest_entry_census[KAGE_VITA_GUEST_SAMPLER_RND][i] = 4294967295U;
    s_log_length = 0U;
    s_logs[0] = '\0';
    kage_vita_guest_sampler_report(ORACLE_BUILD_ID, 4294967295U, 4294967295U);
    for (line = strstr(s_logs, "[kage-vita] ph120.fe "); line;
            line = strstr(line + 1, "[kage-vita] ph120.fe ")) {
        const char *end = strchr(line, '\n');
        const char *head = "[kage-vita] ph120.fe bid=" ORACLE_BUILD_ID
                           " win=4294967295 loops=4294967295 phase=";
        CHECK(end && (size_t)(end - line) + 1U < 384U);
        CHECK(strncmp(line, head, strlen(head)) == 0);
        if (strstr(line, "phase=rnd e=") == line + strlen(head) - 6U)
            ++lines;
    }
    CHECK(lines >= 2U);
    for (i = 0U; i < GUEST_ENTRY_CENSUS_COUNT; ++i) {
        char item[32];
        const char *hit;
        uint32_t hits = 0U;
        snprintf(item, sizeof item, "%x:4294967295", (unsigned)s_watch_rvas[i]);
        for (hit = strstr(s_logs, item); hit; hit = strstr(hit + 1, item))
            if ((hit[-1] == '=' || hit[-1] == ',') &&
                    (hit[strlen(item)] == ',' || hit[strlen(item)] == '\n'))
                ++hits;
        CHECK(hits == 1U);
    }
    printf("scenario 9: entry census per phase, unwatched/empty ids silent, "
           "take-and-zero, %u bounded rnd lines at all-ones\n",
           (unsigned)lines);
#endif
}
#endif

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
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
    scenario_chain();
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL)
    scenario_present();
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    scenario_residual();
    scenario_residual_split();
    scenario_residual_stale();
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
    scenario_census();
#endif
    scenario_stop();   /* last: it unbinds the CPU */

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
