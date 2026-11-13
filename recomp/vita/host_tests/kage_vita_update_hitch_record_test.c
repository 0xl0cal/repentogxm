/* ISAAC_VITA_UPDATE_HITCH_PROBE: the production records, executed.
 *
 * Compiles the production kage_vita_phase_profile.c (which pulls in
 * kage_vita_update_hitch_record.h and kage_vita_update_hitch.h), not a
 * reimplementation.  Two legs, selected by the driver
 * (recomp/vita/test_kage_vita_update_hitch.py):
 *   KVUH_TEST_FULL   every seam compiled in, stubbed counters
 *   (unset)          no optional seam: every optional group must say "off"
 * The include is by bare name so the driver's mutants can shadow the
 * production files from a directory placed first on the include path. */
#define ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE 1
#define ISAAC_VITA_PHASE_PROFILE 1
#define ISAAC_VITA_UPDATE_HITCH_PROBE 1
#define ISAAC_VITA_FRAME_SPIKE_PROFILE 1
#define ISAAC_VITA_PHASE_PROFILE_BUILD_ID "hitch-test"
#if defined(KVUH_TEST_FULL)
# define ISAAC_VITA_GUEST_SAMPLER 1
# define ISAAC_VITA_STATIC_SFX_PROFILE 1
# define ISAAC_VITA_CRT_FILE_LOOKUP_HINT 1
# define ISAAC_VITA_PNG_TEXEL_INIT_ELISION 1
# define KAGE_VITA_UPDATE_HITCH_LUA_COST 1
#endif
#include "kage_vita_phase_profile.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US) || \
    ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US != 30000
# error "this test is written for the default 30000 us threshold"
#endif

GuestPhaseProfileCounters g_guest_phase_profile_counters;
IsaacVitaGlPhaseProfileCounters g_isaac_vita_gl_phase_profile_counters;
uint32_t g_isaac_vita_post_com_time_get_time_calls;

static uint64_t test_now = 1000u;
static uint32_t test_clocks;
static char test_uh[16][1024], test_uhs[16][1024], test_sp[12][1024];
static char test_banner[4][1024];
static uint32_t test_uh_n, test_uhs_n, test_sp_n, test_banner_n;

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++test_clocks;
    return test_now;
}
int sceKernelGetThreadId(void) { return 0x40010003; }
void gl_vita_backend_phase_profile_window_boundary(void) {}

static void test_keep(char (*store)[1024], uint32_t *count, uint32_t cap,
                      const char *line, int n)
{
    assert(*count < cap && n > 0 && n < 1024);
    /* One record per log call: exactly one newline, and it is the last
     * byte. */
    assert(line[n - 1] == '\n' && strchr(line, '\n') == line + n - 1);
    memcpy(store[(*count)++], line, (size_t)n + 1u);
}

int sceClibPrintf(const char *fmt, ...)
{
    char line[2048];
    va_list args;
    int n;
    va_start(args, fmt);
    n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    assert(n > 0 && n < (int)sizeof line);
    if (strstr(line, "ph120.uh "))
        test_keep(test_uh, &test_uh_n, 16u, line, n);
    else if (strstr(line, "ph120.uhs "))
        test_keep(test_uhs, &test_uhs_n, 16u, line, n);
    else if (strstr(line, "ph120.sp "))
        test_keep(test_sp, &test_sp_n, 12u, line, n);
    else if (strstr(line, "UPDATE HITCH PROBE"))
        test_keep(test_banner, &test_banner_n, 4u, line, n);
    return n;
}

#if defined(KVUH_TEST_FULL)
/* ---- stub seams: cumulative counters the test moves between snapshots --- */
volatile uint32_t g_kage_vita_guest_sampler_phase;
void kage_vita_guest_sampler_report(const char *b, uint32_t w, uint32_t l)
{
    (void)b; (void)w; (void)l;
}

static uint32_t stub_fh[2];
static int stub_fh_ok = 1;
int isaac_vita_crt_file_lookup_hint_get(uint32_t out[2])
{
    if (!stub_fh_ok)
        return 0;
    out[0] = stub_fh[0];
    out[1] = stub_fh[1];
    return 1;
}

static IsaacVitaPngTexelInitSnapshot stub_pngi;
void isaac_vita_png_texel_init_snapshot(IsaacVitaPngTexelInitSnapshot *out)
{
    *out = stub_pngi;
}

static isaac_vita_static_sfx_window stub_sfx;
static int stub_sfx_ok = 1;
static uint32_t stub_sfx_takes;
int isaac_vita_static_sfx_profile_peek_window(isaac_vita_static_sfx_window *out)
{
    if (!stub_sfx_ok)
        return 0;
    *out = stub_sfx;
    return 1;
}
int isaac_vita_static_sfx_profile_take_window(isaac_vita_static_sfx_window *out)
{
    ++stub_sfx_takes;
    *out = stub_sfx;
    memset(&stub_sfx, 0, sizeof stub_sfx);
    return 1;
}

static uint32_t stub_lua[6];
void isaac_vita_lua_cost_hitch_peek(uint32_t out[6])
{
    memcpy(out, stub_lua, sizeof stub_lua);
}

/* The sampler's glue, on the production trail type. */
static KageVitaUpdateHitchTrail stub_trail;
uint32_t kage_vita_guest_sampler_trail_seq(void)
{
    return kage_vita_update_hitch_trail_seq(&stub_trail);
}
uint32_t kage_vita_guest_sampler_trail_copy(
    uint32_t begin_seq, uint32_t end_seq, uint32_t *self, uint32_t *caller,
    uint32_t *lost)
{
    return kage_vita_update_hitch_trail_copy(
        &stub_trail, begin_seq, end_seq, self, caller, lost);
}
#endif

static void expect(const char *line, const char *needle)
{
    if (!strstr(line, needle)) {
        fprintf(stderr, "MISSING: %s\n     IN: %s", needle, line);
        exit(1);
    }
}

/* One Game::Update of `us` microseconds bracketed the way the scheduler
 * does it; `work` moves the seam counters in between.  `pre_us` is the
 * scheduler's floor wait: it runs inside the profiler's upd phase but BEFORE
 * the probe's begin (kage_vita_fullspeed_pace_game_update takes the begin
 * value after kage_vita_fullspeed_wait_until), so ph120.sp upd = pre + us
 * while ph120.uh us = us. */
static void update(uint64_t pre_us, uint64_t us, uint32_t frame,
                   void (*work)(void))
{
    uint64_t begin;
    test_now += pre_us;
    begin = test_now;
    kage_vita_update_hitch_begin(begin);
    if (work)
        work();
    test_now += us;
    kage_vita_update_hitch_end(begin, test_now, frame, frame + 1u);
}

#if defined(KVUH_TEST_FULL)
static void work_fight(void)
{
    uint32_t i;
    g_isaac_vita_gl_phase_profile_counters.tex_image += 2u;
    g_isaac_vita_gl_phase_profile_counters.draw_elements += 7u;
    g_isaac_vita_gl_phase_profile_counters.bind_framebuffer += 3u;
    stub_fh[0] += 5u;
    stub_fh[1] += 14u;
    stub_pngi.attempts += 2u;
    stub_pngi.elided += 1u;
    stub_pngi.bytes += 262144u;
    /* Ten distinct values, and five distinct max_us the record must NOT
     * print: a field mapped to its neighbour or to max_us shows. */
    stub_sfx.wav.count += 1u;
    stub_sfx.wav.sum_us += 5000u;
    stub_sfx.wav.max_us = 4900u;
    stub_sfx.ogg.count += 3u;
    stub_sfx.ogg.sum_us += 6100u;
    stub_sfx.ogg.max_us = 3300u;
    stub_sfx.riff.count += 2u;
    stub_sfx.riff.sum_us += 4100u;
    stub_sfx.riff.max_us = 2600u;
    stub_sfx.upload.count += 5u;
    stub_sfx.upload.sum_us += 7300u;
    stub_sfx.upload.max_us = 1900u;
    stub_sfx.play.count += 4u;
    stub_sfx.play.sum_us += 2100u;
    stub_sfx.play.max_us = 800u;
    stub_lua[0] += 30u;
    stub_lua[1] += 9000u;
    stub_lua[2] += 11u;
    stub_lua[3] += 700u;
    stub_lua[4] += 1u;
    stub_lua[5] += 450u;
    /* 100 samples: self 5b1500 x60, 5c2fd0 x30, 2120 x5, 9b40 x5 (a tie,
     * the lower RVA first); caller 5a0e10 x90, 5c30b0 x10. */
    for (i = 0u; i < 100u; ++i)
        kage_vita_update_hitch_trail_push(&stub_trail,
            i < 60u ? 0x5b1500u : (i < 90u ? 0x5c2fd0u :
                (i < 95u ? 0x9b40u : 0x2120u)),
            i % 10u == 9u ? 0x5c30b0u : 0x5a0e10u);
}

static void work_sfx_unreadable_later(void)
{
    stub_sfx.ogg.count += 9u;
    stub_sfx_ok = 0; /* the end snapshot fails */
}

static void work_sfx_readable_again(void)
{
    stub_sfx.ogg.count += 9u;
    stub_sfx_ok = 1; /* only the begin snapshot failed */
}

static void work_long(void)
{
    uint32_t i;
    for (i = 0u; i < 600u; ++i)
        kage_vita_update_hitch_trail_push(&stub_trail, 0x1000u + (i & 1u),
                                          0x2000u);
}

/* Widest record the formats can produce: every counter ends one below its
 * begin value, so every delta is UINT32_MAX; the trail holds 8-digit keys
 * and its sequence stepped back by one (interval = UINT32_MAX samples). */
static void work_max(void)
{
    IsaacVitaGlPhaseProfileCounters *gl =
        &g_isaac_vita_gl_phase_profile_counters;
    uint32_t i;
    gl->tex_image -= 1u;
    gl->draw_elements -= 1u;
    gl->clear -= 1u;
    gl->bind_framebuffer -= 1u;
    stub_fh[0] -= 1u;
    stub_fh[1] -= 1u;
    stub_pngi.attempts -= 1u;
    stub_pngi.elided -= 1u;
    stub_pngi.bytes -= 1u;
    stub_sfx.wav.count -= 1u;
    stub_sfx.wav.sum_us -= 1u;
    stub_sfx.ogg.count -= 1u;
    stub_sfx.ogg.sum_us -= 1u;
    stub_sfx.riff.count -= 1u;
    stub_sfx.riff.sum_us -= 1u;
    stub_sfx.upload.count -= 1u;
    stub_sfx.upload.sum_us -= 1u;
    stub_sfx.play.count -= 1u;
    stub_sfx.play.sum_us -= 1u;
    for (i = 0u; i < 6u; ++i)
        stub_lua[i] -= 1u;
    for (i = 0u; i < KAGE_VITA_UPDATE_HITCH_TRAIL; ++i) {
        stub_trail.self[i] = 0xfffffff0u + i % 6u;
        stub_trail.caller[i] = 0xffffffe0u + i % 6u;
    }
    stub_trail.seq -= 1u;
}

static uint32_t count_char(const char *s, char c)
{
    uint32_t n = 0u;
    for (; *s; ++s)
        n += *s == c;
    return n;
}
#endif

int main(int argc, char **argv)
{
    uint32_t i;
    kage_vita_phase_profile_reset();
    kage_vita_phase_profile_note_loop_head(0u);
    for (i = 0u; i < 360u; ++i) {
        uint64_t us = 1000u;
        uint64_t pre = 0u;
        void (*work)(void) = NULL;
        kage_vita_phase_profile_note_service_entry();
        test_now += 100u;
        kage_vita_phase_profile_note_update_entry();
        /* Boundary triple around the 30000 us threshold, then one hitch. */
        if (i == 2u) us = 29999u;
        if (i == 3u) us = 30000u;
        if (i == 4u) us = 30001u;
#if defined(KVUH_TEST_FULL)
        if (i == 5u) { pre = 7000u; us = 120000u; work = work_fight; }
        if (i == 130u) { us = 40000u; work = work_sfx_unreadable_later; }
        if (i == 131u) { us = 40000u; work = work_sfx_readable_again; }
        if (i == 132u) { us = 700000u; work = work_long; }
#else
        if (i == 5u) { pre = 7000u; us = 120000u; }
#endif
        /* A 50 ms floor wait in front of a 1 ms update: the upd PHASE is
         * over the threshold, Game::Update is not. */
        if (i == 250u) pre = 50000u;
        update(pre, us, 100u + i, work);
        kage_vita_phase_profile_note_render_entry();
        test_now += 1000u;
        kage_vita_phase_profile_note_present_enter();
        test_now += 2000u;
        kage_vita_phase_profile_note_present_return();
        kage_vita_phase_profile_note_render_return();
        kage_vita_phase_profile_note_limiter_entry();
        test_now += 1000u;
        kage_vita_phase_profile_note_limiter_exit();
        kage_vita_phase_profile_note_loop_head(i + 1u);
    }
    /* Not reported: an end whose begin never ran, a reversed clock pair. */
    kage_vita_update_hitch_end(test_now, test_now + 500000u, 1u, 2u);
    kage_vita_update_hitch_begin(test_now);
    kage_vita_update_hitch_end(test_now, test_now - 1u, 1u, 2u);
    /* Not reported: a stale begin value (the scheduler passes its own). */
    kage_vita_update_hitch_begin(test_now);
    kage_vita_update_hitch_end(test_now - 500000u, test_now, 1u, 2u);
    /* Reported once: a second end for the same begin is dropped (the probe
     * disarms at the first one). */
    kage_vita_update_hitch_begin(test_now);
    kage_vita_update_hitch_end(test_now, test_now + 40000u, 7u, 8u);
    kage_vita_update_hitch_end(test_now, test_now + 90000u, 7u, 8u);
#if defined(KVUH_TEST_FULL)
    /* The widest record: a clock interval above UINT32_MAX us is clamped. */
    update(0u, UINT64_C(5000000000), UINT32_MAX - 1u, work_max);
#endif

    /* The banner: once, with the resolved threshold and the seam list. */
    assert(test_banner_n == 1u);
    expect(test_banner[0], "KAGE VITA UPDATE HITCH PROBE: bid=hitch-test "
                           "abi=1 threshold_us=30000 trail=512 top=6 seams: ");
#if defined(KVUH_TEST_FULL)
    expect(test_banner[0], "seams: gl=on fh=on pngi=on png=off sfx=on "
                           "lua=on smp=on\n");
    assert(test_uh_n == 8u && test_uhs_n == 8u);
#else
    expect(test_banner[0], "seams: gl=on fh=off pngi=off png=off sfx=off "
                           "lua=off smp=off\n");
    assert(test_uh_n == 4u && test_uhs_n == 0u);
#endif
    {
        /* The doubly-ended update: its first end only. */
        const char *once = test_uh[test_uh_n -
#if defined(KVUH_TEST_FULL)
            2u
#else
            1u
#endif
            ];
        expect(once, " thr_us=30000 us=40000 ");
        expect(once, " frame=7>8 ");
    }

    /* 29999 is silent; 30000 (equal) and 30001 are reported, in order. */
    expect(test_uh[0], "ph120.uh bid=hitch-test abi=1 n=1 win=1 loop=3 "
                       "tid=40010003 thr_us=30000 us=30000 ");
    expect(test_uh[0], " frame=103>104 ");
    expect(test_uh[1], " n=2 win=1 loop=4 tid=40010003 thr_us=30000 us=30001 ");
    expect(test_uh[2], " n=3 win=1 loop=5 tid=40010003 thr_us=30000 us=120000 ");
    {
        /* begin= is the exact 64-bit begin clock value in hex. */
        char want[64];
        uint64_t begin = 1000u + 6u * 100u + 1000u + 1000u + 29999u + 30000u
            + 30001u + 5u * 4000u + 7000u /* this update's floor wait */;
        snprintf(want, sizeof want, " begin=%08x%08x ",
                 (unsigned)(begin >> 32), (unsigned)begin);
        expect(test_uh[2], want);
    }
    /* Same loop and window as the frame-spike record of that update -- but
     * NOT the same duration: ph120.sp upd is the whole upd phase and holds
     * the floor wait (7000 here), ph120.uh us starts after it. */
    assert(test_sp_n == 9u);
    expect(test_sp[1], "win=1 loops=119 kind=upd loop=5 ");
    expect(test_sp[1], " upd=127000 ");
    /* upd phase >= threshold with Game::Update itself below it: ph120.sp
     * names the loop and there is no ph120.uh for it. */
    expect(test_sp[7], "win=3 loops=359 kind=upd loop=250 ");
    expect(test_sp[7], " upd=51000 ");
    for (i = 0u; i < test_uh_n; ++i)
        assert(!strstr(test_uh[i], " loop=250 "));

#if defined(KVUH_TEST_FULL)
    /* An update that touched nothing prints zeros, not "na". */
    expect(test_uh[0], " gl(tex,draw,clear,fbo)=0,0,0,0 fh(hit,miss)=0,0 "
        "pngi(attempt,elided,bytes)=0,0,0 "
        "png(images,native,decode_us,io_us)=off "
        "sfx(wav_c,wav_us,ogg_c,ogg_us,riff_c,riff_us,up_c,up_us,play_c,"
        "play_us)=0,0,0,0,0,0,0,0,0,0 "
        "lua(pc_n,pc_us,cb_n,cb_us,gc_n,gc_us)=0,0,0,0,0,0 smp(seq)=0\n");
    expect(test_uhs[0], "ph120.uhs bid=hitch-test abi=1 n=1 samples=0 lost=0 "
                        "self=- caller=-\n");
    /* The fight update: every delta is end minus begin of THIS update. */
    expect(test_uh[2], " gl(tex,draw,clear,fbo)=2,7,0,3 fh(hit,miss)=5,14 "
        "pngi(attempt,elided,bytes)=2,1,262144 "
        "png(images,native,decode_us,io_us)=off "
        "sfx(wav_c,wav_us,ogg_c,ogg_us,riff_c,riff_us,up_c,up_us,play_c,"
        "play_us)=1,5000,3,6100,2,4100,5,7300,4,2100 "
        "lua(pc_n,pc_us,cb_n,cb_us,gc_n,gc_us)=30,9000,11,700,1,450 "
        "smp(seq)=100\n");
    expect(test_uhs[2], "ph120.uhs bid=hitch-test abi=1 n=3 samples=100 "
        "lost=0 self=5b1500:60,5c2fd0:30,2120:5,9b40:5 "
        "caller=5a0e10:90,5c30b0:10\n");
    /* The window boundary (ph120.sfx take, loop 120) did not disturb it. */
    assert(stub_sfx_takes >= 1u);
    /* A snapshot that could not be read at the end (n=4) or at the begin
     * (n=5) is "na", never a number. */
    expect(test_uh[3], " n=4 win=2 loop=130 ");
    expect(test_uh[3], "play_us)=na lua(");
    expect(test_uh[3], " fh(hit,miss)=0,0 ");
    expect(test_uh[4], " n=5 win=2 loop=131 ");
    expect(test_uh[4], "play_us)=na lua(");
    /* 700 ms: 600 samples pushed, 512 kept; the oldest kept slot is the one
     * the writer may be rewriting, so 511 are returned and 89 are lost. */
    expect(test_uh[5], " n=6 win=2 loop=132 tid=40010003 thr_us=30000 "
                       "us=700000 ");
    expect(test_uh[5], "play_us)=0,0,0,0,0,0,0,0,0,0 lua(");
    expect(test_uh[5], " smp(seq)=600\n");
    expect(test_uhs[5], " n=6 samples=511 lost=89 self=1001:256,1000:255 "
                        "caller=2000:511\n");
    {
        /* The widest records.  Every group field prints 4294967295 and the
         * line is complete (ends with the last group and one newline).
         * What this run cannot widen is added by arithmetic: bid prints up
         * to 32 characters (%.32s), n/win/loop up to 10 digits each, thr_us
         * up to 8 (CMake caps it at 10000000).  The sum must fit the
         * record's line buffer (char line[768] in
         * kage_vita_update_hitch_record.h; mutant M10 shrinks it) and so
         * the async logger's 1024-byte line (host_vita_log_async.h,
         * ISAAC_VITA_LOG_ASYNC_LINE_MAX). */
        const char *wide = test_uh[7], *tops = test_uhs[7];
        size_t slack = (32u - strlen("hitch-test")) + (8u - strlen("30000"));
        size_t bound;
        expect(wide, " thr_us=30000 us=4294967295 ");
        expect(wide, " frame=4294967294>4294967295 "
            "gl(tex,draw,clear,fbo)=4294967295,4294967295,4294967295,"
            "4294967295 fh(hit,miss)=4294967295,4294967295 "
            "pngi(attempt,elided,bytes)=4294967295,4294967295,4294967295 "
            "png(images,native,decode_us,io_us)=off ");
        expect(wide, "play_us)=4294967295,4294967295,4294967295,4294967295,"
            "4294967295,4294967295,4294967295,4294967295,4294967295,"
            "4294967295 lua(pc_n,pc_us,cb_n,cb_us,gc_n,gc_us)=4294967295,"
            "4294967295,4294967295,4294967295,4294967295,4294967295 "
            "smp(seq)=4294967295\n");
        bound = strlen(wide) + slack + 3u * 9u; /* n, win, loop: 1+ digit now */
        if (bound > 767u) {
            fprintf(stderr, "ph120.uh worst case %u bytes > 767\n",
                    (unsigned)bound);
            return 1;
        }
        expect(tops, " samples=511 lost=4294966784 self=fffffff");
        expect(tops, " caller=ffffffe");
        assert(count_char(tops, ':') == 12u && count_char(tops, ',') == 10u);
        /* n up to 10 digits, samples 3 already, each count up to 3 digits
         * (<= 512; two here). */
        bound = strlen(tops) + (32u - strlen("hitch-test")) + 9u + 12u;
        if (bound > 767u) {
            fprintf(stderr, "ph120.uhs worst case %u bytes > 767\n",
                    (unsigned)bound);
            return 1;
        }
        printf("widest: uh=%u uhs=%u bytes (bound incl. unwidened fields)\n",
               (unsigned)(strlen(wide) + slack + 27u), (unsigned)bound);
    }
#else
    expect(test_uh[2], " gl(tex,draw,clear,fbo)=0,0,0,0 fh(hit,miss)=off "
        "pngi(attempt,elided,bytes)=off "
        "png(images,native,decode_us,io_us)=off "
        "sfx(wav_c,wav_us,ogg_c,ogg_us,riff_c,riff_us,up_c,up_us,play_c,"
        "play_us)=off lua(pc_n,pc_us,cb_n,cb_us,gc_n,gc_us)=off "
        "smp(seq)=off\n");
#endif
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        /* The exact production records, for the log parser's test. */
        fputs(test_banner[0], stdout);
        for (i = 0u; i < test_uh_n; ++i) {
            fputs(test_uh[i], stdout);
            if (i < test_uhs_n)
                fputs(test_uhs[i], stdout);
        }
        /* and the frame-spike records of the same run (the reader's
         * ph120.sp cross-check) */
        for (i = 0u; i < test_sp_n; ++i)
            fputs(test_sp[i], stdout);
        return 0;
    }
    printf("update hitch record: PASS leg=%s uh=%u uhs=%u clocks=%u\n",
#if defined(KVUH_TEST_FULL)
           "full",
#else
           "bare",
#endif
           test_uh_n, test_uhs_n, test_clocks);
    return 0;
}
