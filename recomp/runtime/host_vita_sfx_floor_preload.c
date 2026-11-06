/* Floor-load SFX preload (ISAAC_VITA_SFX_FLOOR_PRELOAD, default OFF).
 *
 * Why: with the native Vita audio manager active, LoadConfig and the game's
 * own Preload skip SoundEffect::Load (the generated advisory-SFX seams,
 * isaac_vita_audio_materialization_ready), so every sound is loaded on its
 * first Play: SFXManager::Play (0x488de0) finds the entry's loaded byte
 * clear, calls SoundEffect::Load (0x488070) and logs "[warn] Sound %d was
 * not preloaded.".  On the device that load reads and copies the WAV PCM
 * inside a gameplay frame: session P2b spent 1.51 s of WAV loading in play
 * windows (ph120.sfx), thunder (id 472, 3.4 MiB, four files) alone ~570 ms
 * in one window.  Sounds are never unloaded (0 repeat loads in
 * 90 device logs outside a language switch), so a sound loaded earlier is
 * the same sound, only loaded at a different time.
 *
 * What: a GNU ld --wrap seam on Level::Init (sub_00311e60).  After the
 * translated body returns -- inside the existing floor-load freeze, the game
 * thread already blocked for seconds -- it replays Play's lazy branch for a
 * fixed list of ids:
 *
 *   00488e08 mov ebx, ecx            ebx = SFXManager ([0x987fd680]+0x29de0)
 *   00488e23 mov edi, [ebp+8]        edi = id; js -> return
 *   00488e2e..44                     count = (end - begin) / 0x1b8 by the
 *                                    0x94f2095 multiply; jge -> return
 *   00488e4c imul esi, edi, 0x1b8; add esi, [ebx]
 *   00488e54 cmp [esi+0x190], 0; ja  no samples -> log + return
 *   00488e81 cmp byte [esi+0x198], 0; jne L_00488ea4 (already loaded)
 *   00488e8a mov ecx, esi
 *   00488e8c call 0x488070           return word 0x488e91
 *   00488e91..9e                     the "[warn] ... not preloaded" log
 *   00488ea1 inc dword [ebx+0xc]
 *
 * (generated-camera corpus guest_0139.c:21597-21712).  The replay calls the
 * translated SoundEffect::Load with the same ECX and return word and then
 * makes the same increment; it does not print the game's warning (the sound
 * was loaded on purpose) and logs its own line instead.  Load itself
 * (guest_0139.c:19260-19572) reads only ECX and globals and leaves with a
 * plain `ret`; it sets [esi+0x198] = 1 (0x4881e3) on every path.
 *
 * Which ids: the sounds that were first-play loads in at least half of 48
 * device sessions (13 ids, 1.53 MiB of WAV data), on every Level::Init, and
 * the five Downpour sounds (thunder, water flow, drips, wet steps, leech;
 * 4.04 MiB) when the new level is stage 1 or 2 of stage type 4: every
 * Downpour session loaded all five, dry sessions loaded none or two of them
 * (a survey of the device logs).  Level fields: Level::Init logs
 * "Level::Init m_Stage %d, m_StageType %d Seed %u" (RVA 0x74c03c) from
 * `push [edi+4]; push [edi]` with edi = ECX at entry (0x311e9e, 0x312156,
 * 0x312159; guest_0083.c:22805, 23173, 23175), so m_Stage = [this+0] and
 * m_StageType = [this+4]; they are read after the body returns.
 *
 * Declines (nothing is replayed, the OFF behaviour follows on first Play):
 * the eager policy (materialization not ready: LoadConfig loaded everything),
 * Load's audio-running byte [0x987e7d44] clear (Load would make null
 * sounds, 0x48812b), a terminal guest heap, a null manager or entry vector,
 * an id out of range, an entry with no samples; and per sound a guest heap
 * headroom (81 MiB contract - mallinfo().uordblks, the ph120.mem formula)
 * below RESERVE_MB + the sound's stock WAV bytes, or an OpenAL pool with less
 * than 2 MiB + those bytes free (the PCM ends up there: alBufferData copies
 * it, host_vita_audio.c vita_audio_buffer_data, and a READY pool never falls
 * back per allocation, host_vita_openal_pool.h -- an exhausted pool would
 * silence a sound the game later plays).  A guest fault or stop inside Load
 * ends the replay with the fault left for the guarded driver.
 *
 * Exactness: every architectural register Level::Init returned with, the
 * lazy flags, xmm, x87 and the Win32 last error are restored after the
 * replay, so Level::Init's caller resumes in the state the OFF build gives
 * it.  ESP is checked after each Load (plain `ret`: back at its value before
 * the push).  The guest heap sees the same blocks allocated earlier; the
 * entries' loaded bytes and [SFXManager+0xc] reach the values the first
 * Play would have given them.  Not replayed: Play's own warning line and the
 * order of these loads relative to other allocations.
 *
 * Mutually exclusive with ISAAC_VITA_UPDATE_PROBE, which wraps the same
 * symbol (CMake refuses the pair).
 *
 * ISAAC_VITA_SFX_FIGHT_PRELOAD (default OFF, needs this knob) appends a third
 * list, s_fight (22 sounds, 2.6 MiB), walked after the two lists above by the
 * same replay: the in-play first-use loads the seeded bench and the device
 * session logs still show.  It is declined as a whole
 * when the guest heap headroom is below RESERVE_MB + the bytes it would load
 * (or the OpenAL pool below its margin + those bytes), and logs its own
 * receipts ("KAGE VITA SFX PRELOAD: fight level=...").  Without the define
 * every line it adds is preprocessed away. */
#include "guest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(ISAAC_VITA_SFX_FLOOR_PRELOAD)
#error The floor-load SFX preload must only be compiled for its opt-in feature
#endif

#if !defined(ISAAC_VITA_SFX_FLOOR_PRELOAD_ORACLE)
#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include "host_vita_openal_pool.h"
#endif

#ifndef ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB
#define ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB 8
#endif
#ifndef ISAAC_VITA_SFX_FLOOR_PRELOAD_BUILD_ID
#define ISAAC_VITA_SFX_FLOOR_PRELOAD_BUILD_ID "unknown"
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);
/* entry_vita.c: 1 = native manager active, first-Play materialisation. */
int isaac_vita_audio_materialization_ready(void);
/* host_vita_heap.h: durable fail-closed heap state (0 without overflow). */
int isaac_vita_guest_heap_terminal(void);

void __real_sub_00311e60(CPU *__restrict c);
void __wrap_sub_00311e60(CPU *__restrict c);
/* SoundEffect::Load, the edge Play takes at 0x488e8c. */
void sub_00488070(CPU *__restrict c);

#define SFXP_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (uint32_t)(rva)))
/* 0038a4a4 mov ecx, [0x987fd680]; 0038a4aa lea ecx, [ecx + 0x29de0]; call
 * 0x488de0 (guest_0103.c:9571-9575; 635 of the 649 Play call sites in the
 * frozen corpus build ECX as lea ecx, [reg + 0x29de0], the rest pass a saved
 * copy). */
#define SFXP_G_MANAGER 0x007FD680U
#define SFXP_SFX_MANAGER 0x00029DE0U
/* 00488e31 mov eax, 0x94f2095; 00488e3a sar edx, 4 (guest_0139.c:21617,
 * 21627): count = (end - begin) / 0x1b8, rounded toward zero. */
#define SFXP_DIV_MAGIC 0x094F2095U
#define SFXP_DIV_SHIFT 4U
/* 00488e4c imul esi, edi, 0x1b8 (21646) */
#define SFXP_ENTRY_BYTES 0x000001B8U
/* 00488e54 cmp dword ptr [esi + 0x190], 0 (21655) */
#define SFXP_ENTRY_SAMPLES 0x00000190U
/* 00488e81 cmp byte ptr [esi + 0x198], 0 (21692); 004881e3 mov byte ptr
 * [esi + 0x198], 1 (19537) */
#define SFXP_ENTRY_LOADED 0x00000198U
/* 00488ea1 inc dword ptr [ebx + 0xc] (21712) */
#define SFXP_LOADED_COUNT 0x0000000CU
/* 00488e8c call 0x488070 (21697): target and return word. */
#define SFXP_LOAD_RVA 0x00488070U
#define SFXP_RET_LOAD 0x00488E91U
/* 0048812b cmp byte ptr [0x987e7d44], 0 (19383): 0 = null sounds. */
#define SFXP_AUDIO_RUNNING 0x007E7D44U
/* Level::Init this+0 / this+4 (see the header). */
#define SFXP_LEVEL_STAGE 0x0U
#define SFXP_LEVEL_TYPE 0x4U
/* Downpour = m_StageType 4 on m_Stage 1 and 2 (the Level::Init lines of the
 * three tagged Downpour sessions: "m_Stage 1, m_StageType 4", "m_Stage 2,
 * m_StageType 4"). */
#define SFXP_WATER_TYPE 4U
#define SFXP_WATER_STAGE_MIN 1U
#define SFXP_WATER_STAGE_MAX 2U
/* host_vita_heap.c:132 VITA_HEAP_REQUIRED_NEWLIB_BYTES (the 81 MiB contract
 * the heap init refuses to run without, host_vita_heap.c:2174), the total
 * ph120.mem's hr = total - uordblks is computed against. */
#define SFXP_HEAP_TOTAL 0x05100000U
#define SFXP_RESERVE_BYTES \
    ((uint32_t)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB * 1048576U)
/* OpenAL pool margin: the pool is 11.84 MiB (ISAAC_VITA_OPENAL_POOL_BYTES
 * 0x00bd6000) and peaked at 6.64 MiB requested in English sessions
 * (device RAM budget); live_requested_bytes leaves out the
 * mspace headers and padding, which the margin absorbs. */
#define SFXP_POOL_RESERVE_BYTES 0x00200000U

typedef struct sfxp_sound {
    uint16_t id;
    uint16_t kib;   /* bound on the resident bytes: stock WAV bytes / 1024,
                     * rounded up, + 1 KiB per file (sample objects) */
} sfxp_sound;

/* Stock archives (sfx.a < afterbirth.a < afterbirthp.a < repentance.a,
 * resources/ then resources-dlc3/, tools/isaac_vita_halve_sfx.py codec):
 * entry bytes and file counts per id, measured 2026-10-24. */
static const sfxp_sound s_water[] = {
    { 472, 3525 },   /* sfx/thunder1-4.wav: 3,605,474 B, 4 files */
    { 473, 454 },    /* sfx/water_flow_loop1.wav: 463,100 B */
    { 397, 79 },     /* sfx/Drip1, Drop2, Drop3: 77,250 B, 3 files */
    { 398, 50 },     /* sfx/WetFootstep1-3: 47,578 B, 3 files */
    { 221, 39 },     /* sfx/leech, leech 2: 37,120 B, 2 files */
};
static const sfxp_sound s_generic[] = {
    { 8, 45 },       /* book page turn: 45,032 B */
    { 30, 137 },     /* death burst small 1-3: 137,170 B, 3 files */
    { 286, 158 },    /* EnemyAppearSmoke01-04: 157,128 B, 4 files */
    { 35, 122 },     /* door heavy close: 123,266 B */
    { 36, 98 },      /* door heavy open: 98,866 B */
    { 233, 103 },    /* penny drop 1: 103,716 B */
    { 234, 60 },     /* Pickup_Penny_02: 59,666 B */
    { 5, 475 },      /* Blood Laser Medium 1-3: 482,946 B, 3 files */
    { 55, 86 },      /* hurt grunt, 1, 2: 84,294 B, 3 files */
    { 150, 16 },     /* tear block: 14,436 B */
    { 37, 254 },     /* fart 1-5: 254,366 B, 5 files */
    { 153, 39 },     /* tear fire 4, 5: 37,008 B, 2 files */
    { 258, 12 },     /* splatter 0-2: 8,468 B, 3 files */
};

#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
/* ISAAC_VITA_SFX_FIGHT_PRELOAD: the sounds the game
 * still loads on their first Play inside gameplay once the two lists above
 * are resident.  Evidence ('[warn] Sound N was not preloaded.' in device game
 * logs, frozen in tools/bench/sfx_fight_evidence.json and re-derived by
 * tools/bench/sfx_fight_list.py, which also emits this order):
 *   F  first-played inside a fight phase (between MARK P2 and MARK P4) of the
 *      seeded bench (bench-A2S/-A2R0/-AOFFS-20261025): the P3 spawn window's
 *      21-23 WAV loads (203-224 ms, ph120.sfx w23) and the in-fight 42-57 ms
 *      load of sound 28 (A2S w31, A2R0 w24);
 *   D  first-played on a Downpour floor in all three hand-played Downpour logs
 *      (session P, P2b x2) and in >= 10 % of 39 real sessions;
 *   R  first-played in >= 20 % of 39 real sessions.
 * Sizes as for the lists above: sounds.xml (resources-dlc3 overriding) and
 * the stock archives, kib = ceil(bytes / 1024) + files, 2,670,450 B in 47
 * files, 2,667 KiB; recomputed by test_vita_sfx_floor_preload.py --packed. */
#define SFXP_FIGHT_KIB 2667U
static const sfxp_sound s_fight[] = {
    { 165, 166 },    /* F zombie walker kid, 2: 167,830 B, 2 files */
    { 178, 66 },     /* F blood fire, 2, 3, 4: 62,922 B, 4 files */
    { 166, 39 },     /* F animal squish, 1, 2: 36,190 B, 3 files */
    { 226, 138 },    /* F meat head shoot, 1, 2: 137,466 B, 3 files */
    { 72, 41 },      /* F meat jumps0, 1, 2: 38,236 B, 3 files */
    { 28, 271 },     /* F death_burst_large_0, _2: 275,070 B, 2 files */
    { 3, 121 },      /* F blobby wiggle, 2, 3: 120,556 B, 3 files */
    { 135, 339 },    /* F RedLightning_Strong1-3: 343,896 B, 3 files */
    { 38, 29 },      /* F fetus jump, 1: 27,420 B, 2 files */
    { 116, 165 },    /* F monster roar 2, 3: 165,968 B, 2 files */
    { 17, 80 },      /* D paper_in: 80,494 B */
    { 59, 23 },      /* D key drop 2: 21,918 B */
    { 68, 37 },      /* D meat feet slow: 36,208 B */
    { 137, 299 },    /* D rock crumble 0, 2, 3: 302,372 B, 3 files */
    { 171, 8 },      /* R beep: 6,874 B */
    { 218, 61 },     /* R battery charge: 60,580 B */
    { 237, 14 },     /* R plop: 12,442 B */
    { 4, 66 },       /* R insect swarm (loop): 65,784 B */
    { 18, 105 },     /* R paper_out: 106,124 B */
    { 48, 464 },     /* R forest boss stomp, 2, 3: 471,472 B, 3 files */
    { 2, 71 },       /* R bird flap, 2, 3: 69,172 B, 3 files */
    { 60, 64 },      /* R baby hurt 1-3: 61,456 B, 3 files */
};
#endif

typedef enum {
    SFXP_SKIP_LOADED = 0,     /* [entry+0x198] set: Play would not load */
    SFXP_SKIP_RANGE,          /* id >= count: Play returns at 0x488e46 */
    SFXP_SKIP_NO_SAMPLES,     /* [entry+0x190] == 0: Play logs, returns */
    SFXP_SKIP_HEADROOM,       /* guest heap below reserve + the sound */
    SFXP_SKIP_POOL,           /* OpenAL pool below its margin + the sound */
    SFXP_SKIP_KINDS
} sfxp_skip;

typedef enum {
    SFXP_LEVEL_REPLAYED = 0,  /* the list was walked */
    SFXP_LEVEL_FAULT,         /* Level::Init itself faulted or stopped */
    SFXP_LEVEL_EAGER,         /* materialization not ready */
    SFXP_LEVEL_AUDIO_OFF,     /* [0x987e7d44] == 0 */
    SFXP_LEVEL_TERMINAL,      /* terminal guest heap */
    SFXP_LEVEL_NO_MANAGER,    /* Manager, vector begin null or end < begin */
    SFXP_LEVEL_KINDS
} sfxp_level;

typedef struct sfxp_counters {
    uint32_t levels;                  /* Level::Init returns seen */
    uint32_t level[SFXP_LEVEL_KINDS];
    uint32_t water_levels;            /* Downpour gate taken */
    uint32_t loaded;                  /* Load replays completed */
    uint32_t skip[SFXP_SKIP_KINDS];
    uint32_t load_faults;             /* fault/stop inside Load */
    uint32_t esp_breaches;            /* ESP not back after Load */
    uint32_t load_us;                 /* sum of Load spans */
} sfxp_counters;

static sfxp_counters s_counters;
static int s_banner;

/* ------------------------------------------------------------ platform --- */

#if defined(ISAAC_VITA_SFX_FLOOR_PRELOAD_ORACLE)
uint32_t isaac_sfxp_oracle_heap_used(void);
uint32_t isaac_sfxp_oracle_pool_room(void);
uint64_t isaac_sfxp_oracle_now_us(void);
static uint32_t sfxp_heap_used(void) { return isaac_sfxp_oracle_heap_used(); }
static uint32_t sfxp_pool_room(void) { return isaac_sfxp_oracle_pool_room(); }
static uint64_t sfxp_now_us(void) { return isaac_sfxp_oracle_now_us(); }
#else
static uint32_t sfxp_heap_used(void)
{
    /* mallinfo takes newlib's own lock and walks the bins; called outside
     * every runtime lock, once per sound about to load. */
    struct mallinfo heap = mallinfo();
    return (uint32_t)heap.uordblks;
}
/* Free bytes of the dedicated OpenAL pool; UINT32_MAX when OpenAL runs on
 * the whole-session newlib fallback (the heap reading already counts it), 0
 * (decline) when the pool is unreadable, not READY, has failed an allocation
 * or seen corruption.  The snapshot takes the pool mutex briefly. */
static uint32_t sfxp_pool_room(void)
{
    isaac_vita_openal_pool_snapshot pool;

    memset(&pool, 0, sizeof pool);
    if (!isaac_vita_openal_pool_snapshot_get(&pool))
        return 0U;
    if (pool.state == ISAAC_VITA_OPENAL_POOL_FALLBACK ||
            pool.state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK)
        return UINT32_MAX;
    if (pool.state != ISAAC_VITA_OPENAL_POOL_READY || pool.failures != 0U ||
            pool.corruption != 0U)
        return 0U;
    return pool.backing_bytes > pool.live_requested_bytes
        ? pool.backing_bytes - pool.live_requested_bytes : 0U;
}
static uint64_t sfxp_now_us(void) { return sceKernelGetProcessTimeWide(); }
#endif

static uint32_t sfxp_headroom(void)
{
    const uint32_t used = sfxp_heap_used();
    return used < SFXP_HEAP_TOTAL ? SFXP_HEAP_TOTAL - used : 0U;
}

/* ------------------------------------------------------- the replay ------ */

/* Play's bounds test (0x488e26..0x488e46) on the SFXManager at `sfx`:
 * returns the entry address, or 0 with the skip reason. */
static uint32_t sfxp_entry(uint32_t sfx, int32_t id, sfxp_skip *why)
{
    const uint32_t begin = ld32(sfx);
    const int32_t span = (int32_t)(ld32(sfx + 4U) - begin);
    const int64_t product = (int64_t)(int32_t)SFXP_DIV_MAGIC * (int64_t)span;
    int32_t high = (int32_t)(uint32_t)((uint64_t)product >> 32);
    int32_t count;
    uint32_t entry;

    high >>= SFXP_DIV_SHIFT;
    count = (int32_t)((uint32_t)high + ((uint32_t)high >> 31));
    if (id < 0 || id >= count) {
        *why = SFXP_SKIP_RANGE;
        return 0U;
    }
    entry = (uint32_t)id * SFXP_ENTRY_BYTES + begin;
    if (ld32(entry + SFXP_ENTRY_SAMPLES) == 0U) {
        *why = SFXP_SKIP_NO_SAMPLES;
        return 0U;
    }
    if (ld8(entry + SFXP_ENTRY_LOADED) != 0U) {
        *why = SFXP_SKIP_LOADED;
        return 0U;
    }
    return entry;
}

/* 0x488e8a..0x488e8c: ecx = entry, push 0x488e91, SoundEffect::Load.
 * Returns 1 when Load came back with ESP restored and no fault. */
static int sfxp_load(CPU *__restrict c, uint32_t entry)
{
    const uint32_t saved = c->esp;

    gpush(c, SFXP_RET_LOAD);
    if (c->fault) {
        ++s_counters.load_faults;
        return 0;
    }
    c->ecx = entry;
    sub_00488070(c);
    if (c->fault) {
        ++s_counters.load_faults;
        return 0;
    }
    if (c->esp != saved) {
        c->esp = saved;
        ++s_counters.esp_breaches;
        return 0;
    }
    return 1;
}

/* One list.  Returns 0 when the replay must stop (fault, breach). */
static int sfxp_walk(CPU *__restrict c, uint32_t sfx, const sfxp_sound *list,
                     size_t n)
{
    /* A variable, not the macro: RESERVE_MB=0 must not turn the test below
     * into an always-false constant comparison (-Wtype-limits, -Werror). */
    const uint32_t reserve = SFXP_RESERVE_BYTES;
    const uint32_t pool_reserve = SFXP_POOL_RESERVE_BYTES;
    size_t i;

    for (i = 0; i < n; ++i) {
        sfxp_skip why = SFXP_SKIP_LOADED;
        const uint32_t entry = sfxp_entry(sfx, (int32_t)list[i].id, &why);
        const uint32_t need = (uint32_t)list[i].kib * 1024U;
        uint32_t room, pool, samples;
        uint64_t t0;
        uint32_t us;

        if (!entry) {
            ++s_counters.skip[why];
            continue;
        }
        room = sfxp_headroom();
        if (room < reserve || room - reserve < need) {
            ++s_counters.skip[SFXP_SKIP_HEADROOM];
            isaac_vita_log("KAGE VITA SFX PRELOAD: sound=%u declined hr_kb=%u "
                           "need_kb=%u",
                           (unsigned)list[i].id, (unsigned)(room >> 10),
                           (unsigned)(list[i].kib + (reserve >> 10)));
            continue;
        }
        pool = sfxp_pool_room();
        if (pool < pool_reserve || pool - pool_reserve < need) {
            ++s_counters.skip[SFXP_SKIP_POOL];
            isaac_vita_log("KAGE VITA SFX PRELOAD: sound=%u declined pool_kb=%u "
                           "need_kb=%u",
                           (unsigned)list[i].id, (unsigned)(pool >> 10),
                           (unsigned)(list[i].kib + (pool_reserve >> 10)));
            continue;
        }
        samples = ld32(entry + SFXP_ENTRY_SAMPLES);
        t0 = sfxp_now_us();
        if (!sfxp_load(c, entry))
            return 0;
        /* 00488ea1 inc dword ptr [ebx + 0xc] */
        st32(sfx + SFXP_LOADED_COUNT, ld32(sfx + SFXP_LOADED_COUNT) + 1U);
        us = (uint32_t)(sfxp_now_us() - t0);
        ++s_counters.loaded;
        s_counters.load_us += us;
        isaac_vita_log("KAGE VITA SFX PRELOAD: sound=%u samples=%u us=%u "
                       "hr_kb=%u pool_kb=%u",
                       (unsigned)list[i].id, (unsigned)samples, (unsigned)us,
                       (unsigned)(room >> 10),
                       (unsigned)(pool == UINT32_MAX ? 0U : pool >> 10));
    }
    return 1;
}

typedef struct sfxp_saved {
    uint32_t r[8];
    xmm_t x[8];
    double st[8];
    int st_top;
    uint16_t fsw;
    guest_flags fl;
    uint8_t df, idf;
    uint32_t last_error;
} sfxp_saved;

static void sfxp_save(const CPU *__restrict c, sfxp_saved *s)
{
    memcpy(s->r, c->r, sizeof s->r);
    memcpy(s->x, c->x, sizeof s->x);
    memcpy(s->st, c->st, sizeof s->st);
    s->st_top = c->st_top;
    s->fsw = c->fsw;
    s->fl = c->fl;
    s->df = c->df;
    s->idf = c->idf;
    s->last_error = c->last_error;
}

static void sfxp_restore(CPU *__restrict c, const sfxp_saved *s)
{
    memcpy(c->r, s->r, sizeof s->r);
    memcpy(c->x, s->x, sizeof s->x);
    memcpy(c->st, s->st, sizeof s->st);
    c->st_top = s->st_top;
    c->fsw = s->fsw;
    c->fl = s->fl;
    c->df = s->df;
    c->idf = s->idf;
    c->last_error = s->last_error;
}

#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
typedef struct sfxp_fight_counters {
    uint32_t levels;          /* fight walks attempted (after a replayed level) */
    uint32_t declined_heap;   /* extension skipped: headroom < reserve + need */
    uint32_t declined_pool;   /* extension skipped: pool < margin + need */
    uint32_t stopped;         /* a Load faulted or breached ESP */
} sfxp_fight_counters;

static sfxp_fight_counters s_fight_counters;

/* The extension, after the two lists of the same Level::Init returned
 * cleanly (their registers already restored).  `need` is the guard bound of
 * every fight sound Play would load now (sfxp_entry: in range, samples, not
 * loaded); the whole extension is declined, before any Load, unless the
 * guest heap keeps RESERVE_MB + need and the OpenAL pool its 2 MiB margin +
 * need -- so it never half-loads on a tight heap, and at this Level::Init it
 * leaves at least the reserve.  That is a load-time check only: the list
 * stays resident, so for the rest of the run every lazy load of a sound
 * outside the lists sees the headroom lower than with the knob OFF by the
 * bytes of list sounds not played yet (up to SFXP_FIGHT_KIB).  The per-sound
 * guards of sfxp_walk still apply while it walks. */
static void sfxp_fight(CPU *__restrict c, uint32_t sfx)
{
    const uint32_t reserve = SFXP_RESERVE_BYTES;
    const uint32_t pool_reserve = SFXP_POOL_RESERVE_BYTES;
    const size_t n = sizeof s_fight / sizeof s_fight[0];
    sfxp_counters before = s_counters;
    uint32_t need = 0U, loadable = 0U, mask = 0U, room, pool, hr1;
    uint64_t t0;
    sfxp_saved saved;
    size_t i;

    ++s_fight_counters.levels;
    for (i = 0; i < n; ++i) {
        sfxp_skip why = SFXP_SKIP_LOADED;
        if (sfxp_entry(sfx, (int32_t)s_fight[i].id, &why)) {
            need += (uint32_t)s_fight[i].kib * 1024U;
            mask |= 1U << i;
            ++loadable;
        }
    }
    room = sfxp_headroom();
    pool = sfxp_pool_room();
    if (loadable != 0U && (room < reserve || room - reserve < need)) {
        ++s_fight_counters.declined_heap;
        isaac_vita_log("KAGE VITA SFX PRELOAD: fight level=%u declined "
                       "hr_kb=%u need_kb=%u sounds=%u mask=0x%06x",
                       (unsigned)s_counters.levels, (unsigned)(room >> 10),
                       (unsigned)((need + reserve) >> 10), (unsigned)loadable,
                       (unsigned)mask);
        return;
    }
    if (loadable != 0U && (pool < pool_reserve || pool - pool_reserve < need)) {
        ++s_fight_counters.declined_pool;
        isaac_vita_log("KAGE VITA SFX PRELOAD: fight level=%u declined "
                       "pool_kb=%u need_kb=%u sounds=%u mask=0x%06x",
                       (unsigned)s_counters.levels, (unsigned)(pool >> 10),
                       (unsigned)((need + pool_reserve) >> 10),
                       (unsigned)loadable, (unsigned)mask);
        return;
    }
    t0 = sfxp_now_us();
    sfxp_save(c, &saved);
    if (!sfxp_walk(c, sfx, s_fight, n)) {
        ++s_fight_counters.stopped;
        if (!c->fault)
            sfxp_restore(c, &saved);
        isaac_vita_log("KAGE VITA SFX PRELOAD: fight level=%u stopped "
                       "fault=%u breach=%u loaded=%u",
                       (unsigned)s_counters.levels,
                       (unsigned)(s_counters.load_faults - before.load_faults),
                       (unsigned)(s_counters.esp_breaches - before.esp_breaches),
                       (unsigned)(s_counters.loaded - before.loaded));
        return;
    }
    sfxp_restore(c, &saved);
    hr1 = sfxp_headroom();
    isaac_vita_log("KAGE VITA SFX PRELOAD: fight level=%u loaded=%u "
                   "skip(l,r,s,h,p)=%u,%u,%u,%u,%u us=%u need_kb=%u "
                   "hr_kb=%u..%u pool_kb=%u",
                   (unsigned)s_counters.levels,
                   (unsigned)(s_counters.loaded - before.loaded),
                   (unsigned)(s_counters.skip[0] - before.skip[0]),
                   (unsigned)(s_counters.skip[1] - before.skip[1]),
                   (unsigned)(s_counters.skip[2] - before.skip[2]),
                   (unsigned)(s_counters.skip[3] - before.skip[3]),
                   (unsigned)(s_counters.skip[4] - before.skip[4]),
                   (unsigned)(sfxp_now_us() - t0), (unsigned)(need >> 10),
                   (unsigned)(room >> 10), (unsigned)(hr1 >> 10),
                   (unsigned)(pool == UINT32_MAX ? 0U : pool >> 10));
}
#endif

/* The preload proper; `level` is ECX at Level::Init's entry. */
static void sfxp_after_level_init(CPU *__restrict c, uint32_t level)
{
    sfxp_counters before = s_counters;
    uint32_t manager, sfx, stage, type, hr0;
    int water;
    uint64_t t0;
    sfxp_saved saved;

    ++s_counters.levels;
    if (c->fault) {
        ++s_counters.level[SFXP_LEVEL_FAULT];
        return;
    }
    if (!isaac_vita_audio_materialization_ready()) {
        ++s_counters.level[SFXP_LEVEL_EAGER];
        return;
    }
    if (ld8(SFXP_VA(SFXP_AUDIO_RUNNING)) == 0U) {
        ++s_counters.level[SFXP_LEVEL_AUDIO_OFF];
        return;
    }
    if (isaac_vita_guest_heap_terminal()) {
        ++s_counters.level[SFXP_LEVEL_TERMINAL];
        return;
    }
    manager = ld32(SFXP_VA(SFXP_G_MANAGER));
    sfx = manager + SFXP_SFX_MANAGER;
    if (manager == 0U || ld32(sfx) == 0U || ld32(sfx + 4U) < ld32(sfx)) {
        ++s_counters.level[SFXP_LEVEL_NO_MANAGER];
        return;
    }
    stage = ld32(level + SFXP_LEVEL_STAGE);
    type = ld32(level + SFXP_LEVEL_TYPE);
    water = type == SFXP_WATER_TYPE && stage >= SFXP_WATER_STAGE_MIN &&
            stage <= SFXP_WATER_STAGE_MAX;
    ++s_counters.level[SFXP_LEVEL_REPLAYED];
    if (water)
        ++s_counters.water_levels;

    hr0 = sfxp_headroom();
    t0 = sfxp_now_us();
    sfxp_save(c, &saved);
    if (water && !sfxp_walk(c, sfx, s_water, sizeof s_water / sizeof s_water[0]))
        goto stopped;
    if (!sfxp_walk(c, sfx, s_generic, sizeof s_generic / sizeof s_generic[0]))
        goto stopped;
    sfxp_restore(c, &saved);
    isaac_vita_log("KAGE VITA SFX PRELOAD: level=%u stage=%u type=%u water=%u "
                   "loaded=%u skip(l,r,s,h,p)=%u,%u,%u,%u,%u us=%u "
                   "hr_kb=%u..%u",
                   (unsigned)s_counters.levels, (unsigned)stage,
                   (unsigned)type, (unsigned)water,
                   (unsigned)(s_counters.loaded - before.loaded),
                   (unsigned)(s_counters.skip[0] - before.skip[0]),
                   (unsigned)(s_counters.skip[1] - before.skip[1]),
                   (unsigned)(s_counters.skip[2] - before.skip[2]),
                   (unsigned)(s_counters.skip[3] - before.skip[3]),
                   (unsigned)(s_counters.skip[4] - before.skip[4]),
                   (unsigned)(sfxp_now_us() - t0), (unsigned)(hr0 >> 10),
                   (unsigned)(sfxp_headroom() >> 10));
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
    sfxp_fight(c, sfx);
#endif
    return;
stopped:
    /* A fault/stop stays for the guarded driver; an ESP breach was already
     * repaired, so the registers go back too. */
    if (!c->fault)
        sfxp_restore(c, &saved);
    isaac_vita_log("KAGE VITA SFX PRELOAD: level=%u stopped fault=%u "
                   "breach=%u loaded=%u",
                   (unsigned)s_counters.levels,
                   (unsigned)(s_counters.load_faults - before.load_faults),
                   (unsigned)(s_counters.esp_breaches - before.esp_breaches),
                   (unsigned)(s_counters.loaded - before.loaded));
}

void __wrap_sub_00311e60(CPU *__restrict c)
{
    /* 00311e9e mov edi, ecx: ECX at entry is the Level object. */
    const uint32_t level = c->ecx;

    if (!s_banner) {
        s_banner = 1;
#if defined(ISAAC_VITA_SFX_FIGHT_PRELOAD)
        isaac_vita_log("KAGE VITA SFX PRELOAD: armed reserve_mb=%u water=%u "
                       "generic=%u fight=%u fight_kb=%u build=%s",
                       (unsigned)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB,
                       (unsigned)(sizeof s_water / sizeof s_water[0]),
                       (unsigned)(sizeof s_generic / sizeof s_generic[0]),
                       (unsigned)(sizeof s_fight / sizeof s_fight[0]),
                       (unsigned)SFXP_FIGHT_KIB,
                       ISAAC_VITA_SFX_FLOOR_PRELOAD_BUILD_ID);
#else
        isaac_vita_log("KAGE VITA SFX PRELOAD: armed reserve_mb=%u water=%u "
                       "generic=%u build=%s",
                       (unsigned)ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB,
                       (unsigned)(sizeof s_water / sizeof s_water[0]),
                       (unsigned)(sizeof s_generic / sizeof s_generic[0]),
                       ISAAC_VITA_SFX_FLOOR_PRELOAD_BUILD_ID);
#endif
    }
    __real_sub_00311e60(c);
    sfxp_after_level_init(c, level);
}
