/* Bounded, allocation-free owner-thread elapsed-span recorder. See header for
 * the deliberately narrow meaning of exclusive time. No per-span logging.
 * Duration/call fields saturate explicitly instead of wrapping; bytes are u64
 * and printed hi:lo because the device logger's %lli is not a safe contract. */
#include "kage_vita_deep_profile.h"

#if defined(ISAAC_VITA_DEEP_PROFILE) && ISAAC_VITA_DEEP_PROFILE
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#if defined(ISAAC_KAGE_VITA_DEEP_PROFILE_ORACLE)
extern uint64_t kage_vita_deep_oracle_clock(void);
extern uint32_t kage_vita_deep_oracle_thread(void);
extern void kage_vita_deep_oracle_log(const char *, ...);
#define KVD_CLOCK() kage_vita_deep_oracle_clock()
#define KVD_THREAD() kage_vita_deep_oracle_thread()
#define KVD_LOG kage_vita_deep_oracle_log
#else
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
extern void isaac_vita_log(const char *, ...);
#define KVD_CLOCK() sceKernelGetProcessTimeWide()
#define KVD_THREAD() ((uint32_t)sceKernelGetThreadId())
#define KVD_LOG isaac_vita_log
#endif

#define KVD_ABI 1U
#define KVD_STACK_CAP 64U
#define KVD_RETAIN_CAP 16U
#define KVD_SLOW_US 50000U
#define KVD_IO_OPS 10U
#define KVD_IO_CLASSES 8U

enum {
    KVD_ERR_CLOCK = 1U, KVD_ERR_TOKEN = 2U, KVD_ERR_DEPTH = 4U,
    KVD_ERR_UNWIND = 8U, KVD_ERR_IO = 16U, KVD_ERR_SAT = 32U,
    KVD_ERR_FRAME = 64U, KVD_ERR_CATEGORY = 128U
};
typedef struct kvd_stat {
    uint64_t bytes;
    uint32_t calls, timed, self_timed, incomplete;
    uint32_t inclusive_us, self_us, max_us;
} kvd_stat;
typedef struct kvd_io_stat {
    uint64_t requested, returned;
    uint32_t calls, errors, timed, us;
} kvd_io_stat;
typedef struct kvd_io_class {
    uint32_t calls, timed, us, errors;
} kvd_io_class;
typedef struct kvd_frame {
    kvd_stat stat[KVD_PHASE_COUNT][KVD_CATEGORY_COUNT];
    kvd_io_stat io[KVD_IO_OPS];
    kvd_io_class cls[KVD_IO_CLASSES];
    uint64_t start;
    uint32_t loop, window, serial, duration_us, wall_valid;
    uint32_t phase_us[KVD_PHASE_COUNT];
    uint32_t resource, errors, hooks, clock_reads, max_depth;
} kvd_frame;
typedef struct kvd_span {
    uint64_t start, child_us, last_child_end;
    uint32_t serial, category, phase, bad_child;
} kvd_span;
typedef struct kvd_state {
    kvd_frame current, total, retained[KVD_RETAIN_CAP];
    kvd_span stack[KVD_STACK_CAP];
    uint64_t phase_start, last_now, last_root_io_end;
    uint32_t active, phase, depth, frame_serial, span_serial;
    uint32_t retained_count, candidate_frames, dropped_frames, dropped_max_us;
    uint32_t frames, resource_frames, slow_frames, invalid_frames;
    uint32_t max_frame_us, max_frame_loop, max_hooks, max_clock_reads;
    uint32_t hooks, clock_reads, outside;
    uint32_t bad_clock, bad_token, bad_depth, abandoned, bad_io, bad_category;
    uint32_t bad_frame, saturation, phase_cross, child_invalid;
    uint32_t previous_report_us, calibrated, calibration_pairs;
    uint32_t calibration_sum_us, calibration_min_us, calibration_max_us;
} kvd_state;
static kvd_state s_kvd;
/* Separate atomic diagnostics are the only state touched by a foreign thread.
 * Their values are lifetime counters; report() does not reset them. */
static uint32_t s_kvd_owner;
static uint32_t s_kvd_foreign;
static uint32_t s_kvd_foreign_saturated;
static uint32_t s_kvd_reporting;
static uint32_t s_kvd_unbound;
static uint32_t s_kvd_unbound_saturated;
_Static_assert(sizeof(kvd_state) + 6U * sizeof(uint32_t) <= 96U * 1024U,
               "deep profiler static storage exceeds its declared hard cap");

static const char *const s_kvd_names[KVD_CATEGORY_COUNT] = {
    "room_transition", "room_switch", "game_change_room", "level_change_room",
    "room_setup", "room_init", "room_cleanup", "room_save", "room_prerender",
    "room_render", "png", "anm2", "lua_pcall", "lua_gc", "lua_callback",
    "heap_alloc", "heap_free", "memcpy", "memmove", "tex_upload", "tex_subupload",
    "gl_clear", "gl_fbo", "gl_draw", "present_api", "static_sfx",
    "io_open", "io_close", "io_read", "io_seek", "io_write", "io_other",
    "png_decode", "png_premultiply", "audio_pump", "anm2_load", "anm2_graphics",
    "room_state_reset", "room_snapshot"
};

static void kvd_inc_plain(uint32_t *value) { if (*value != UINT32_MAX) ++*value; }
static void kvd_saturated(void)
{
    kvd_inc_plain(&s_kvd.saturation);
    if (s_kvd.active) s_kvd.current.errors |= KVD_ERR_SAT;
}
static void kvd_add32(uint32_t *value, uint64_t add)
{
    if (add > UINT32_MAX - *value) { *value = UINT32_MAX; kvd_saturated(); }
    else *value += (uint32_t)add;
}
static uint32_t kvd_u32(uint64_t value)
{
    if (value > UINT32_MAX) { kvd_saturated(); return UINT32_MAX; }
    return (uint32_t)value;
}
static void kvd_add64(uint64_t *value, uint64_t add)
{
    if (add > UINT64_MAX - *value) { *value = UINT64_MAX; kvd_saturated(); }
    else *value += add;
}
static void kvd_atomic_diagnostic(uint32_t *counter, uint32_t *saturated)
{
    uint32_t value = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (value != UINT32_MAX &&
           !__atomic_compare_exchange_n(counter, &value, value + 1U, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (value == UINT32_MAX)
        __atomic_store_n(saturated, 1U, __ATOMIC_RELAXED);
}
static int kvd_owner(int bind)
{
    uint32_t owner = __atomic_load_n(&s_kvd_owner, __ATOMIC_ACQUIRE);
    uint32_t tid = KVD_THREAD();
    if (!owner && bind && tid) {
        uint32_t zero = 0U;
        (void)__atomic_compare_exchange_n(&s_kvd_owner, &zero, tid, 0,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        owner = __atomic_load_n(&s_kvd_owner, __ATOMIC_ACQUIRE);
    }
    /* s_kvd_reporting is read only after proving this is the owner thread.
     * Reporter-induced native I/O must not mutate the batch being emitted. */
    if (owner && tid == owner) return !s_kvd_reporting;
    if (!owner) kvd_atomic_diagnostic(&s_kvd_unbound, &s_kvd_unbound_saturated);
    else kvd_atomic_diagnostic(&s_kvd_foreign, &s_kvd_foreign_saturated);
    return 0;
}
static void kvd_hook(void)
{
    kvd_add32(&s_kvd.hooks, 1U);
    if (s_kvd.active) kvd_add32(&s_kvd.current.hooks, 1U);
}
static uint64_t kvd_now(void)
{
    uint64_t now = KVD_CLOCK();
    kvd_add32(&s_kvd.clock_reads, 1U);
    if (s_kvd.active) kvd_add32(&s_kvd.current.clock_reads, 1U);
    return now;
}
static void kvd_error(uint32_t flag, uint32_t *counter)
{
    kvd_add32(counter, 1U);
    if (s_kvd.active) s_kvd.current.errors |= flag;
}
static void kvd_bad_parent(void)
{
    if (s_kvd.depth) s_kvd.stack[s_kvd.depth - 1U].bad_child = 1U;
}
static int kvd_clock_valid(uint64_t now)
{
    if (now < s_kvd.current.start || now < s_kvd.last_now) {
        kvd_error(KVD_ERR_CLOCK, &s_kvd.bad_clock);
        kvd_bad_parent();
        return 0;
    }
    s_kvd.last_now = now;
    return 1;
}
static int kvd_resource_category(uint32_t cat)
{
    /* Transition::Update can early-return every tick; Render/PreRender are
     * steady work. Do not fill all 16 retention slots with those categories. */
    return (cat >= KVD_ROOM_SWITCH && cat <= KVD_ROOM_SAVE) ||
           cat == KVD_PNG || cat == KVD_ANM2 || cat == KVD_TEX_UPLOAD ||
           cat == KVD_TEX_SUBUPLOAD || cat == KVD_STATIC_SFX ||
           cat == KVD_PNG_DECODE || cat == KVD_PNG_PREMULTIPLY ||
           cat == KVD_ANM2_LOAD || cat == KVD_ANM2_GRAPHICS ||
           cat == KVD_ROOM_STATE_RESET || cat == KVD_ROOM_SNAPSHOT;
}
static void kvd_calibrate(void)
{
    uint32_t i;
    if (s_kvd.calibrated) return;
    s_kvd.calibrated = 1U;
    s_kvd.calibration_min_us = UINT32_MAX;
    for (i = 0U; i < 32U; ++i) {
        uint64_t begin = kvd_now(), end = kvd_now();
        if (end < begin) { kvd_error(KVD_ERR_CLOCK, &s_kvd.bad_clock); continue; }
        uint32_t elapsed = kvd_u32(end - begin);
        kvd_add32(&s_kvd.calibration_pairs, 1U);
        kvd_add32(&s_kvd.calibration_sum_us, elapsed);
        if (elapsed < s_kvd.calibration_min_us) s_kvd.calibration_min_us = elapsed;
        if (elapsed > s_kvd.calibration_max_us) s_kvd.calibration_max_us = elapsed;
    }
    if (!s_kvd.calibration_pairs) s_kvd.calibration_min_us = 0U;
}
static void kvd_abandon(uint32_t mark)
{
    while (s_kvd.depth > mark) {
        kvd_span *span = &s_kvd.stack[--s_kvd.depth];
        kvd_add32(&s_kvd.current.stat[span->phase][span->category].incomplete, 1U);
        kvd_error(KVD_ERR_UNWIND, &s_kvd.abandoned);
    }
    kvd_bad_parent();
}
static void kvd_merge_stat(kvd_stat *to, const kvd_stat *from)
{
    kvd_add64(&to->bytes, from->bytes);
    kvd_add32(&to->calls, from->calls);
    kvd_add32(&to->timed, from->timed);
    kvd_add32(&to->self_timed, from->self_timed);
    kvd_add32(&to->incomplete, from->incomplete);
    kvd_add32(&to->inclusive_us, from->inclusive_us);
    kvd_add32(&to->self_us, from->self_us);
    if (from->max_us > to->max_us) to->max_us = from->max_us;
}
static void kvd_merge_frame(void)
{
    uint32_t p, c;
    for (p = 0U; p < KVD_PHASE_COUNT; ++p) {
        kvd_add32(&s_kvd.total.phase_us[p], s_kvd.current.phase_us[p]);
        for (c = 0U; c < KVD_CATEGORY_COUNT; ++c)
            kvd_merge_stat(&s_kvd.total.stat[p][c], &s_kvd.current.stat[p][c]);
    }
    for (c = 0U; c < KVD_IO_OPS; ++c) {
        kvd_io_stat *to = &s_kvd.total.io[c], *from = &s_kvd.current.io[c];
        kvd_add64(&to->requested, from->requested); kvd_add64(&to->returned, from->returned);
        kvd_add32(&to->calls, from->calls); kvd_add32(&to->errors, from->errors);
        kvd_add32(&to->timed, from->timed); kvd_add32(&to->us, from->us);
    }
    for (c = 0U; c < KVD_IO_CLASSES; ++c) {
        kvd_io_class *to = &s_kvd.total.cls[c], *from = &s_kvd.current.cls[c];
        kvd_add32(&to->calls, from->calls); kvd_add32(&to->errors, from->errors);
        kvd_add32(&to->timed, from->timed); kvd_add32(&to->us, from->us);
    }
    kvd_add32(&s_kvd.total.duration_us, s_kvd.current.duration_us);
    s_kvd.total.errors |= s_kvd.current.errors;
}
static void kvd_finish(uint64_t now)
{
    uint32_t slot, minimum;
    if (!s_kvd.active) { kvd_error(KVD_ERR_FRAME, &s_kvd.bad_frame); return; }
    kvd_hook();
    if (s_kvd.depth) kvd_abandon(0U);
    if (kvd_clock_valid(now) && now >= s_kvd.phase_start) {
        s_kvd.current.duration_us = kvd_u32(now - s_kvd.current.start);
        s_kvd.current.wall_valid = 1U;
        kvd_add32(&s_kvd.current.phase_us[s_kvd.phase], now - s_kvd.phase_start);
    } else { s_kvd.current.wall_valid = 0U; kvd_add32(&s_kvd.invalid_frames, 1U); }
    kvd_add32(&s_kvd.frames, 1U);
    if (s_kvd.current.resource) kvd_add32(&s_kvd.resource_frames, 1U);
    if (s_kvd.current.duration_us > KVD_SLOW_US) kvd_add32(&s_kvd.slow_frames, 1U);
    if (s_kvd.current.duration_us > s_kvd.max_frame_us) {
        s_kvd.max_frame_us = s_kvd.current.duration_us;
        s_kvd.max_frame_loop = s_kvd.current.loop;
    }
    if (s_kvd.current.hooks > s_kvd.max_hooks) s_kvd.max_hooks = s_kvd.current.hooks;
    if (s_kvd.current.clock_reads > s_kvd.max_clock_reads) s_kvd.max_clock_reads = s_kvd.current.clock_reads;
    kvd_merge_frame();
    if (s_kvd.current.resource || s_kvd.current.duration_us > KVD_SLOW_US || s_kvd.current.errors) {
        kvd_add32(&s_kvd.candidate_frames, 1U);
        if (s_kvd.retained_count < KVD_RETAIN_CAP) {
            s_kvd.retained[s_kvd.retained_count++] = s_kvd.current;
        } else {
            minimum = 0U;
            for (slot = 1U; slot < KVD_RETAIN_CAP; ++slot)
                if (s_kvd.retained[slot].duration_us < s_kvd.retained[minimum].duration_us) minimum = slot;
            uint32_t rejected_us = s_kvd.current.duration_us;
            if (s_kvd.current.duration_us > s_kvd.retained[minimum].duration_us) {
                rejected_us = s_kvd.retained[minimum].duration_us;
                s_kvd.retained[minimum] = s_kvd.current;
            }
            kvd_add32(&s_kvd.dropped_frames, 1U);
            if (rejected_us > s_kvd.dropped_max_us) s_kvd.dropped_max_us = rejected_us;
        }
    }
    s_kvd.active = 0U;
}
static void kvd_begin(uint32_t loop, uint32_t window, uint64_t now)
{
    if (s_kvd.active) {
        kvd_error(KVD_ERR_FRAME, &s_kvd.bad_frame);
        kvd_finish(now);
    }
    memset(&s_kvd.current, 0, sizeof s_kvd.current);
    ++s_kvd.frame_serial;
    if (!s_kvd.frame_serial) { kvd_saturated(); ++s_kvd.frame_serial; }
    s_kvd.current.loop = loop; s_kvd.current.window = window;
    s_kvd.current.serial = s_kvd.frame_serial; s_kvd.current.start = now;
    s_kvd.active = 1U; s_kvd.phase = KVD_PHASE_OTHER; s_kvd.depth = 0U;
    s_kvd.phase_start = s_kvd.last_now = now; s_kvd.last_root_io_end = now;
    kvd_hook();
}
void kage_vita_deep_frame_begin(uint32_t loop, uint32_t window)
{
    int saved = errno;
    if (kvd_owner(1)) { kvd_calibrate(); kvd_begin(loop, window, kvd_now()); }
    errno = saved;
}
void kage_vita_deep_frame_begin_at(uint32_t loop, uint32_t window, uint64_t now)
{
    int saved = errno;
    if (kvd_owner(1)) { kvd_calibrate(); kvd_begin(loop, window, now); }
    errno = saved;
}
static void kvd_phase(uint32_t phase, uint64_t now)
{
    kvd_hook();
    if (!s_kvd.active) { kvd_add32(&s_kvd.outside, 1U); return; }
    if (phase >= KVD_PHASE_COUNT) { kvd_error(KVD_ERR_CATEGORY, &s_kvd.bad_category); return; }
    if (!kvd_clock_valid(now)) return;
    kvd_add32(&s_kvd.current.phase_us[s_kvd.phase], now - s_kvd.phase_start);
    if (s_kvd.depth && phase != s_kvd.phase) kvd_add32(&s_kvd.phase_cross, 1U);
    s_kvd.phase_start = now; s_kvd.phase = phase;
}
void kage_vita_deep_phase_set(uint32_t phase)
{
    int saved = errno;
    if (kvd_owner(0)) kvd_phase(phase, kvd_now());
    errno = saved;
}
void kage_vita_deep_phase_set_at(uint32_t phase, uint64_t now)
{
    int saved = errno;
    if (kvd_owner(0)) kvd_phase(phase, now);
    errno = saved;
}
void kage_vita_deep_frame_end(void)
{
    int saved = errno;
    if (kvd_owner(0)) kvd_finish(kvd_now());
    errno = saved;
}
void kage_vita_deep_frame_end_at(uint64_t now)
{
    int saved = errno;
    if (kvd_owner(0)) kvd_finish(now);
    errno = saved;
}
kage_vita_deep_token kage_vita_deep_enter(uint32_t cat, uint64_t bytes)
{
    int saved = errno;
    kage_vita_deep_token token = {0U, 0U, 0U, 0U};
    if (!kvd_owner(0)) goto done;
    kvd_hook();
    if (!s_kvd.active) { kvd_add32(&s_kvd.outside, 1U); goto done; }
    if (cat >= KVD_CATEGORY_COUNT) { kvd_error(KVD_ERR_CATEGORY, &s_kvd.bad_category); kvd_bad_parent(); goto done; }
    kvd_stat *stat = &s_kvd.current.stat[s_kvd.phase][cat];
    kvd_add32(&stat->calls, 1U); kvd_add64(&stat->bytes, bytes);
    if (kvd_resource_category(cat)) s_kvd.current.resource = 1U;
    if (s_kvd.depth == KVD_STACK_CAP) {
        kvd_error(KVD_ERR_DEPTH, &s_kvd.bad_depth); kvd_add32(&stat->incomplete, 1U);
        kvd_bad_parent(); goto done;
    }
    uint64_t now = kvd_now();
    if (!kvd_clock_valid(now)) { kvd_add32(&stat->incomplete, 1U); goto done; }
    kvd_span *span = &s_kvd.stack[s_kvd.depth];
    memset(span, 0, sizeof *span);
    ++s_kvd.span_serial; if (!s_kvd.span_serial) { kvd_saturated(); ++s_kvd.span_serial; }
    span->serial = s_kvd.span_serial; span->category = cat; span->phase = s_kvd.phase;
    span->start = span->last_child_end = now;
    token.frame_serial = s_kvd.frame_serial; token.serial = span->serial;
    token.depth = ++s_kvd.depth; token.active = 1U;
    if (s_kvd.depth > s_kvd.current.max_depth) s_kvd.current.max_depth = s_kvd.depth;
done:
    errno = saved; return token;
}
void kage_vita_deep_leave(kage_vita_deep_token *token)
{
    int saved = errno;
    if (!token || !token->active) goto done;
    if (!kvd_owner(0)) goto done;
    kvd_hook();
    if (!s_kvd.active || token->frame_serial != s_kvd.frame_serial ||
        !token->depth || token->depth > s_kvd.depth ||
        s_kvd.stack[token->depth - 1U].serial != token->serial) {
        kvd_error(KVD_ERR_TOKEN, &s_kvd.bad_token);
        if (s_kvd.active && token->frame_serial == s_kvd.frame_serial) kvd_bad_parent();
        token->active = 0U; goto done;
    }
    if (token->depth != s_kvd.depth) { kvd_error(KVD_ERR_TOKEN, &s_kvd.bad_token); kvd_abandon(token->depth); }
    uint64_t now = kvd_now();
    kvd_span *span = &s_kvd.stack[s_kvd.depth - 1U];
    kvd_stat *stat = &s_kvd.current.stat[span->phase][span->category];
    int valid = kvd_clock_valid(now) && now >= span->start;
    --s_kvd.depth; token->active = 0U;
    if (!valid) { kvd_add32(&stat->incomplete, 1U); kvd_bad_parent(); goto done; }
    uint64_t elapsed = now - span->start;
    uint32_t duration = kvd_u32(elapsed);
    kvd_add32(&stat->timed, 1U); kvd_add32(&stat->inclusive_us, elapsed);
    if (duration > stat->max_us) stat->max_us = duration;
    if (!span->bad_child && span->child_us <= elapsed) {
        kvd_add32(&stat->self_timed, 1U); kvd_add32(&stat->self_us, elapsed - span->child_us);
    } else { kvd_add32(&s_kvd.child_invalid, 1U); kvd_bad_parent(); }
    if (s_kvd.depth) {
        kvd_span *parent = &s_kvd.stack[s_kvd.depth - 1U];
        if (span->start < parent->start || span->start < parent->last_child_end) {
            parent->bad_child = 1U; kvd_error(KVD_ERR_IO, &s_kvd.bad_io);
        } else { kvd_add64(&parent->child_us, elapsed); parent->last_child_end = now; }
    } else s_kvd.last_root_io_end = now;
done:
    errno = saved;
}
uint32_t kage_vita_deep_depth(void)
{
    int saved = errno; uint32_t depth = 0U;
    if (kvd_owner(0)) { kvd_hook(); depth = s_kvd.active ? s_kvd.depth : 0U; }
    errno = saved; return depth;
}
uint32_t kage_vita_deep_loop(void)
{
    int saved = errno; uint32_t loop = UINT32_MAX;
    if (kvd_owner(0)) loop = s_kvd.active ? s_kvd.current.loop : UINT32_MAX;
    errno = saved; return loop;
}
void kage_vita_deep_unwind(uint32_t mark)
{
    int saved = errno;
    if (kvd_owner(0)) {
        kvd_hook();
        if (!s_kvd.active || mark > s_kvd.depth) kvd_error(KVD_ERR_TOKEN, &s_kvd.bad_token);
        else if (mark < s_kvd.depth) kvd_abandon(mark);
    }
    errno = saved;
}
void kage_vita_deep_mark_resource(void)
{
    int saved = errno;
    if (kvd_owner(0)) { kvd_hook(); if (s_kvd.active) s_kvd.current.resource = 1U; }
    errno = saved;
}
void kage_vita_deep_io(uint32_t op, uint32_t cls, uint64_t req, int64_t result,
                     uint64_t begin, uint64_t end)
{
    /* Mapping copied from kage_vita_io_profile.h; keep pread/pwrite distinct
     * in operation rows while category rows combine them with read/write. */
    static const uint32_t cats[KVD_IO_OPS] = {
        KVD_IO_OPEN, KVD_IO_CLOSE, KVD_IO_READ, KVD_IO_READ,
        KVD_IO_SEEK, KVD_IO_SEEK, KVD_IO_WRITE, KVD_IO_WRITE, KVD_IO_OTHER, KVD_IO_OTHER
    };
    int saved = errno;
    if (!kvd_owner(0)) goto done;
    kvd_hook();
    if (!s_kvd.active) { kvd_add32(&s_kvd.outside, 1U); goto done; }
    if (op >= KVD_IO_OPS || cls >= KVD_IO_CLASSES) {
        kvd_error(KVD_ERR_IO, &s_kvd.bad_io); kvd_bad_parent(); goto done;
    }
    kvd_io_stat *io = &s_kvd.current.io[op];
    kvd_io_class *cl = &s_kvd.current.cls[cls];
    kvd_stat *stat = &s_kvd.current.stat[s_kvd.phase][cats[op]];
    kvd_add32(&io->calls, 1U); kvd_add32(&cl->calls, 1U); kvd_add32(&stat->calls, 1U);
    kvd_add64(&io->requested, req); kvd_add64(&stat->bytes, req);
    if (result < 0) { kvd_add32(&io->errors, 1U); kvd_add32(&cl->errors, 1U); }
    else if (op == 2U || op == 3U || op == 6U || op == 7U) kvd_add64(&io->returned, (uint64_t)result);
    uint64_t now = kvd_now();
    kvd_span *parent = s_kvd.depth ? &s_kvd.stack[s_kvd.depth - 1U] : NULL;
    uint64_t floor = parent ? parent->start : s_kvd.current.start;
    uint64_t sibling_end = parent ? parent->last_child_end : s_kvd.last_root_io_end;
    if (!kvd_clock_valid(now) || begin > end || begin < s_kvd.current.start ||
        begin < floor || begin < sibling_end || end > now) {
        kvd_error(KVD_ERR_IO, &s_kvd.bad_io); kvd_bad_parent();
        kvd_add32(&stat->incomplete, 1U); goto done;
    }
    uint64_t elapsed = end - begin;
    uint32_t duration = kvd_u32(elapsed);
    kvd_add32(&io->timed, 1U); kvd_add32(&io->us, elapsed);
    kvd_add32(&cl->timed, 1U); kvd_add32(&cl->us, elapsed);
    kvd_add32(&stat->timed, 1U); kvd_add32(&stat->self_timed, 1U);
    kvd_add32(&stat->inclusive_us, elapsed); kvd_add32(&stat->self_us, elapsed);
    if (duration > stat->max_us) stat->max_us = duration;
    if (parent) { kvd_add64(&parent->child_us, elapsed); parent->last_child_end = end; }
    else s_kvd.last_root_io_end = end;
done:
    errno = saved;
}

/* Report-local count; no runtime hook or static-state overhead. Increment
 * before formatting so dp.end can include itself in the declared count.
 * At most 2356 records: 4 headers + 39 dictionary + 17 * (1+117+10+8) + end. */
#define KVD_LOG_RECORD(count, ...) do { ++(count); KVD_LOG(__VA_ARGS__); } while (0)
static void kvd_print_frame(const char *bid, uint32_t win, uint32_t index,
                           const kvd_frame *frame, uint32_t *records)
{
    uint32_t phase, cat;
    KVD_LOG_RECORD(*records, "[kage-vita] dp.f bid=%.32s win=%u frame=%u loop=%u srcwin=%u wall=%u valid=%u resource=%u err=%u phase(o,u,r)=%u,%u,%u hooks=%u clocks=%u depth=%u\n",
            bid, win, index, frame->loop, frame->window, frame->duration_us, frame->wall_valid,
            frame->resource, frame->errors, frame->phase_us[0], frame->phase_us[1], frame->phase_us[2],
            frame->hooks, frame->clock_reads, frame->max_depth);
    for (phase = 0U; phase < KVD_PHASE_COUNT; ++phase) for (cat = 0U; cat < KVD_CATEGORY_COUNT; ++cat) {
        const kvd_stat *v = &frame->stat[phase][cat];
        if (!v->calls) continue;
        KVD_LOG_RECORD(*records, "[kage-vita] dp.s bid=%.32s win=%u frame=%u loop=%u phase=%u cat=%u n=%u timed=%u selfn=%u incomplete=%u us(i,s,max)=%u,%u,%u bytes=%u:%u\n",
                bid, win, index, frame->loop, phase, cat, v->calls, v->timed, v->self_timed,
                v->incomplete, v->inclusive_us, v->self_us, v->max_us,
                (uint32_t)(v->bytes >> 32), (uint32_t)v->bytes);
    }
    for (cat = 0U; cat < KVD_IO_OPS; ++cat) {
        const kvd_io_stat *v = &frame->io[cat];
        if (!v->calls) continue;
        KVD_LOG_RECORD(*records, "[kage-vita] dp.io bid=%.32s win=%u frame=%u loop=%u op=%u n=%u timed=%u errors=%u us=%u req=%u:%u ret=%u:%u\n",
                bid, win, index, frame->loop, cat, v->calls, v->timed, v->errors, v->us,
                (uint32_t)(v->requested >> 32), (uint32_t)v->requested,
                (uint32_t)(v->returned >> 32), (uint32_t)v->returned);
    }
    for (cat = 0U; cat < KVD_IO_CLASSES; ++cat) {
        const kvd_io_class *v = &frame->cls[cat];
        if (v->calls) KVD_LOG_RECORD(*records, "[kage-vita] dp.ic bid=%.32s win=%u frame=%u loop=%u cls=%u n=%u timed=%u errors=%u us=%u\n",
                            bid, win, index, frame->loop, cat, v->calls, v->timed, v->errors, v->us);
    }
}
void kage_vita_deep_report(const char *bid, uint32_t window)
{
    int saved = errno;
    if (!kvd_owner(0)) goto done;
    if (s_kvd.active || s_kvd.depth) { kvd_error(KVD_ERR_FRAME, &s_kvd.bad_frame); goto done; }
    s_kvd_reporting = 1U;
    uint64_t begin = kvd_now();
    uint32_t slot, records = 0U;
    if (!bid) bid = "unstamped";
    KVD_LOG_RECORD(records, "[kage-vita] dp.h bid=%.32s win=%u abi=%u statebytes=%u cap=%u slow_us=%u frames=%u resource=%u slow=%u invalid=%u candidates=%u retained=%u dropped=%u dropmax=%u max=%u maxloop=%u\n",
            bid, window, KVD_ABI, (unsigned)(sizeof s_kvd + 6U * sizeof(uint32_t)), KVD_RETAIN_CAP,
            KVD_SLOW_US, s_kvd.frames, s_kvd.resource_frames, s_kvd.slow_frames, s_kvd.invalid_frames,
            s_kvd.candidate_frames, s_kvd.retained_count, s_kvd.dropped_frames, s_kvd.dropped_max_us,
            s_kvd.max_frame_us, s_kvd.max_frame_loop);
    KVD_LOG_RECORD(records, "[kage-vita] dp.e bid=%.32s win=%u clock=%u token=%u depth=%u abandoned=%u io=%u category=%u frame=%u sat=%u phasecross=%u childinvalid=%u outside=%u foreign=%u foreignsat=%u\n",
            bid, window, s_kvd.bad_clock, s_kvd.bad_token, s_kvd.bad_depth, s_kvd.abandoned,
            s_kvd.bad_io, s_kvd.bad_category, s_kvd.bad_frame, s_kvd.saturation, s_kvd.phase_cross,
            s_kvd.child_invalid, s_kvd.outside, __atomic_load_n(&s_kvd_foreign, __ATOMIC_RELAXED),
            __atomic_load_n(&s_kvd_foreign_saturated, __ATOMIC_RELAXED));
    KVD_LOG_RECORD(records, "[kage-vita] dp.thread bid=%.32s win=%u owner=%u unbound=%u unboundsat=%u lifetime=1\n",
            bid, window, __atomic_load_n(&s_kvd_owner, __ATOMIC_RELAXED),
            __atomic_load_n(&s_kvd_unbound, __ATOMIC_RELAXED),
            __atomic_load_n(&s_kvd_unbound_saturated, __ATOMIC_RELAXED));
    KVD_LOG_RECORD(records, "[kage-vita] dp.cost bid=%.32s win=%u hooks=%u clocks=%u maxhooks=%u maxclocks=%u calib(n,sum,min,max)=%u,%u,%u,%u previous_report_us=%u\n",
            bid, window, s_kvd.hooks, s_kvd.clock_reads, s_kvd.max_hooks, s_kvd.max_clock_reads,
            s_kvd.calibration_pairs, s_kvd.calibration_sum_us, s_kvd.calibration_min_us,
            s_kvd.calibration_max_us, s_kvd.previous_report_us);
    /* Emit the stable category dictionary once per process, alongside the
     * first report. No dynamic strings or pointers survive this call. */
    static uint32_t dictionary_printed;
    if (!dictionary_printed) {
        for (slot = 0U; slot < KVD_CATEGORY_COUNT; ++slot)
            KVD_LOG_RECORD(records, "[kage-vita] dp.cat bid=%.32s win=%u cat=%u name=%s\n", bid, window, slot, s_kvd_names[slot]);
        dictionary_printed = 1U;
    }
    /* frame=0 is the ALL-frame aggregate, never a retained-frame subtotal. */
    s_kvd.total.wall_valid = s_kvd.invalid_frames == 0U;
    kvd_print_frame(bid, window, 0U, &s_kvd.total, &records);
    for (slot = 0U; slot < s_kvd.retained_count; ++slot)
        kvd_print_frame(bid, window, slot + 1U, &s_kvd.retained[slot], &records);
    /* Preserve lifetime token/owner/calibration state across report windows. */
    uint32_t fs = s_kvd.frame_serial, ss = s_kvd.span_serial;
    uint32_t cn = s_kvd.calibration_pairs, cs = s_kvd.calibration_sum_us;
    uint32_t cmin = s_kvd.calibration_min_us, cmax = s_kvd.calibration_max_us;
    memset(&s_kvd, 0, sizeof s_kvd);
    s_kvd.frame_serial = fs; s_kvd.span_serial = ss; s_kvd.calibrated = 1U;
    s_kvd.calibration_pairs = cn; s_kvd.calibration_sum_us = cs;
    s_kvd.calibration_min_us = cmin; s_kvd.calibration_max_us = cmax;
    /* Count reset/memory-clear cost too. Only the final receipt line itself
     * is outside this measurement; phase profiler can time the complete call. */
    uint64_t end = KVD_CLOCK();
    uint32_t elapsed = end >= begin ? kvd_u32(end - begin) : 0U;
    uint32_t report_clock_bad = end < begin;
    s_kvd.previous_report_us = elapsed;
    KVD_LOG_RECORD(records, "[kage-vita] dp.end bid=%.32s win=%u report_us=%u report_clock_bad=%u extra_clock_reads=1 excludes_this_line=1 records=%u\n", bid, window, elapsed, report_clock_bad, records);
    s_kvd_reporting = 0U;
done:
    errno = saved;
}
#undef KVD_LOG_RECORD
#endif
