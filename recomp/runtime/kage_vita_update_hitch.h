/* ISAAC_VITA_UPDATE_HITCH_PROBE: one-shot attribution of a long Game::Update.
 *
 * Diagnostic only, default OFF.  No source file includes this header unless
 * the knob is ON, so an OFF build cannot see any of it.
 *
 * What it answers: "this one Game::Update took 110 ms -- inside what?".  The
 * 120-loop window records already carry the same seams (ph120.sp, ph120.sfx,
 * ph120.pngi, ph120.fh, the Lua cost line, ph120.hot), but as window totals:
 * a window holds 120 updates, so a total cannot say which of them paid.  The
 * probe brackets ONE update with the scheduler's two existing clock values
 * (kage_vita_fullspeed_scheduler.c: s_update_begin_us and the end_us read in
 * ..._note_game_update_end; no clock read is added) and, when the update
 * lasted at least ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US, prints the
 * difference of every seam's cumulative counters across exactly that update.
 * s_update_begin_us is set AFTER the scheduler's floor wait, which runs
 * inside the phase profiler's upd phase: us= is therefore <= the same
 * loop's ph120.sp upd, short by that wait and by the Manager::Update code
 * around Game::Update.  An upd phase over the threshold with no ph120.uh
 * for its loop is time outside Game::Update.  The begin is not made at all
 * on the scheduler's early returns (runtime disabled, failed pace wait);
 * the matching end is then dropped as unarmed and nothing is printed:
 *
 *   ph120.uh   timing + GL uploads/draws, file lookups, PNG texel buffers,
 *              native PNG decode, static SFX loads/uploads/plays, Lua
 *              pcall/callback/GC time
 *   ph120.uhs  the guest sampler's 1 kHz samples that fell inside the update:
 *              top self and top caller functions (translated RVAs)
 *
 * Both records carry n= (event number) so they pair up; each is one log call
 * with one newline.  Nothing is filtered: every update at or above the
 * threshold is printed, every group is always present, and a group that is
 * not compiled in or could not be read says so ("=off" / "=na") instead of
 * printing zeros that would read as a measurement.
 *
 * Cost when ON: per update, one snapshot of the counters at begin (a few
 * loads, one spin-lock pair for the file-lookup hint, one try-lock for SFX);
 * per printed event, a second snapshot, a sort of at most
 * KAGE_VITA_UPDATE_HITCH_TRAIL sampler entries and two log lines -- all after
 * the end clock value was taken, so none of it is inside the reported us=. */
#ifndef KAGE_VITA_UPDATE_HITCH_H
#define KAGE_VITA_UPDATE_HITCH_H

#include <stddef.h>
#include <stdint.h>

#ifndef ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US
# define ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US 30000
#endif

#define KAGE_VITA_UPDATE_HITCH_ABI 1u
/* Sampler entries kept: 512 ms at the sampler's 1 kHz.  A longer update
 * (room load) reports the overwritten part as lost=. */
#define KAGE_VITA_UPDATE_HITCH_TRAIL 512u
#define KAGE_VITA_UPDATE_HITCH_TOP 6u

/* Scheduler -> phase profile (kage_vita_phase_profile.c owns the records). */
void kage_vita_update_hitch_begin(uint64_t begin_us);
void kage_vita_update_hitch_end(uint64_t begin_us, uint64_t end_us,
                                uint32_t frame_before, uint32_t frame_after);

/* Seam accessors; each is defined by the seam's own unit, under the knob. */
struct isaac_vita_static_sfx_window;
/* host_vita_static_sfx_profile.c: copy of the open window, nothing reset.
 * 0 = the scope lock was held (a load in flight on another thread). */
int isaac_vita_static_sfx_profile_peek_window(
    struct isaac_vita_static_sfx_window *out);
/* host_vita_lua_cost.c: lifetime (not 2 s window) modular totals
 * pcall n/us, callback n/us, GC n/us (vm + callback + external kinds). */
void isaac_vita_lua_cost_hitch_peek(uint32_t out[6]);
/* kage_vita_guest_sampler.c: see KageVitaUpdateHitchTrail below. */
uint32_t kage_vita_guest_sampler_trail_seq(void);
uint32_t kage_vita_guest_sampler_trail_copy(
    uint32_t begin_seq, uint32_t end_seq, uint32_t *self, uint32_t *caller,
    uint32_t *lost);

/* ---------------------------------------------------------- threshold --- */

/* An update is reported when its wall time is AT LEAST the threshold.  A
 * reversed clock pair is never a hitch. */
static inline int kage_vita_update_hitch_due(
    uint64_t begin_us, uint64_t end_us, uint64_t threshold_us)
{
    return end_us >= begin_us && end_us - begin_us >= threshold_us;
}

/* ------------------------------------------------------ sampler trail --- */

/* Single writer (the sampler thread), single reader (the game thread).  The
 * writer stores the entry, then publishes seq + 1.  The reader copies and
 * then re-reads seq: anything the writer could have overwritten meanwhile is
 * dropped and counted as lost, never returned. */
typedef struct KageVitaUpdateHitchTrail {
    uint32_t seq; /* entries pushed, lifetime, modular */
    uint32_t self[KAGE_VITA_UPDATE_HITCH_TRAIL];
    uint32_t caller[KAGE_VITA_UPDATE_HITCH_TRAIL];
} KageVitaUpdateHitchTrail;

static inline void kage_vita_update_hitch_trail_push(
    KageVitaUpdateHitchTrail *t, uint32_t self, uint32_t caller)
{
    uint32_t seq = __atomic_load_n(&t->seq, __ATOMIC_RELAXED);
    uint32_t at = seq % KAGE_VITA_UPDATE_HITCH_TRAIL;
    __atomic_store_n(&t->self[at], self, __ATOMIC_RELAXED);
    __atomic_store_n(&t->caller[at], caller, __ATOMIC_RELAXED);
    __atomic_store_n(&t->seq, seq + 1u, __ATOMIC_RELEASE);
}

static inline uint32_t kage_vita_update_hitch_trail_seq(
    const KageVitaUpdateHitchTrail *t)
{
    return __atomic_load_n(&t->seq, __ATOMIC_ACQUIRE);
}

/* Entries pushed in [begin_seq, end_seq), oldest first, into self[]/caller[]
 * (each KAGE_VITA_UPDATE_HITCH_TRAIL long).  Returns how many were copied;
 * *lost receives how many of the interval were not (overwritten before or
 * during the copy). */
static inline uint32_t kage_vita_update_hitch_trail_copy(
    const KageVitaUpdateHitchTrail *t, uint32_t begin_seq, uint32_t end_seq,
    uint32_t *self, uint32_t *caller, uint32_t *lost)
{
    uint32_t want = end_seq - begin_seq; /* modular */
    uint32_t first = begin_seq;
    uint32_t count, i, after, overwritten;

    if (want > KAGE_VITA_UPDATE_HITCH_TRAIL) {
        first = end_seq - KAGE_VITA_UPDATE_HITCH_TRAIL;
        count = KAGE_VITA_UPDATE_HITCH_TRAIL;
    } else {
        count = want;
    }
    for (i = 0u; i < count; ++i) {
        uint32_t at = (first + i) % KAGE_VITA_UPDATE_HITCH_TRAIL;
        self[i] = __atomic_load_n(&t->self[at], __ATOMIC_RELAXED);
        caller[i] = __atomic_load_n(&t->caller[at], __ATOMIC_RELAXED);
    }
    /* The writer may have started entry `after` (not yet published), so the
     * slots of sequence numbers <= after - TRAIL are not trustworthy. */
    after = __atomic_load_n(&t->seq, __ATOMIC_ACQUIRE);
    overwritten = 0u;
    if (after - first >= KAGE_VITA_UPDATE_HITCH_TRAIL) {
        overwritten = after - first - KAGE_VITA_UPDATE_HITCH_TRAIL + 1u;
        if (overwritten > count)
            overwritten = count;
    }
    if (overwritten != 0u) {
        for (i = overwritten; i < count; ++i) {
            self[i - overwritten] = self[i];
            caller[i - overwritten] = caller[i];
        }
        count -= overwritten;
    }
    *lost = want - count;
    return count;
}

/* Sorts values[0..n) in place and writes the `top` most frequent values:
 * higher count first, lower value first among equals.  Returns how many
 * distinct values were written (<= top). */
static inline uint32_t kage_vita_update_hitch_top(
    uint32_t *values, uint32_t n, uint32_t *keys, uint32_t *counts,
    uint32_t top)
{
    uint32_t i, used = 0u;

    for (i = 1u; i < n; ++i) {
        uint32_t value = values[i];
        uint32_t j = i;
        while (j != 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            --j;
        }
        values[j] = value;
    }
    i = 0u;
    while (i < n) {
        uint32_t run = 1u;
        uint32_t at;
        while (i + run < n && values[i + run] == values[i])
            ++run;
        /* Ascending scan: a later equal count never displaces an earlier
         * (lower) value. */
        for (at = 0u; at < used; ++at) {
            if (run > counts[at])
                break;
        }
        if (at < top) {
            uint32_t last = used < top ? used : top - 1u;
            uint32_t k;
            for (k = last; k > at; --k) {
                keys[k] = keys[k - 1u];
                counts[k] = counts[k - 1u];
            }
            keys[at] = values[i];
            counts[at] = run;
            if (used < top)
                ++used;
        }
        i += run;
    }
    return used;
}

#endif
