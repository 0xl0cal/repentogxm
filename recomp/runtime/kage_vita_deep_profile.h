#ifndef KAGE_VITA_DEEP_PROFILE_H
#define KAGE_VITA_DEEP_PROFILE_H

#include <stdint.h>

/* Diagnostic ABI 1. IDs are explicit: preserve them when adding categories.
 * All durations are elapsed wall time, including descheduling. `self` excludes
 * only observed nested children, not uninstrumented work or all native work.
 * The phase attached to a scope is its entry phase. Phase wall totals are
 * separate, nonoverlapping phase seams and are not sums of category times. */
enum kage_vita_deep_category {
    KVD_ROOM_TRANSITION = 0, KVD_ROOM_SWITCH = 1,
    KVD_GAME_CHANGE_ROOM = 2, KVD_LEVEL_CHANGE_ROOM = 3,
    KVD_ROOM_SETUP = 4, KVD_ROOM_INIT = 5, KVD_ROOM_CLEANUP = 6,
    KVD_ROOM_SAVE = 7, KVD_ROOM_PRERENDER = 8, KVD_ROOM_RENDER = 9,
    KVD_PNG = 10, KVD_ANM2 = 11, KVD_LUA_PCALL = 12,
    KVD_LUA_GC = 13, KVD_LUA_CALLBACK = 14,
    KVD_HEAP_ALLOC = 15, KVD_HEAP_FREE = 16,
    KVD_MEMCPY = 17, KVD_MEMMOVE = 18,
    KVD_TEX_UPLOAD = 19, KVD_TEX_SUBUPLOAD = 20,
    KVD_GL_CLEAR = 21, KVD_GL_FBO = 22, KVD_GL_DRAW = 23,
    KVD_GPU_WAIT = 24, KVD_STATIC_SFX = 25,
    KVD_IO_OPEN = 26, KVD_IO_CLOSE = 27, KVD_IO_READ = 28,
    KVD_IO_SEEK = 29, KVD_IO_WRITE = 30, KVD_IO_OTHER = 31,
    KVD_PNG_DECODE = 32, KVD_PNG_PREMULTIPLY = 33,
    KVD_AUDIO_PUMP = 34,
    /* Append only: `anm2`/11 remains the existing parser at 00009b40. */
    KVD_ANM2_LOAD = 35, KVD_ANM2_GRAPHICS = 36,
    /* Whole original invocations, not Room::SaveState or per-grid records. */
    KVD_ROOM_STATE_RESET = 37, KVD_ROOM_SNAPSHOT = 38,
    KVD_CATEGORY_COUNT = 39
};
enum kage_vita_deep_phase {
    KVD_PHASE_OTHER = 0, KVD_PHASE_UPDATE = 1,
    KVD_PHASE_RENDER = 2, KVD_PHASE_COUNT = 3
};

typedef struct kage_vita_deep_token {
    uint32_t frame_serial;
    uint32_t serial;
    uint32_t depth;
    uint32_t active;
} kage_vita_deep_token;

#if defined(ISAAC_VITA_DEEP_PROFILE) && ISAAC_VITA_DEEP_PROFILE

/* frame_begin is the only owner-binding API. A scope on another thread is
 * ignored before any stack access (an atomic ignored counter is still kept).
 * No CPU/guest/stack pointer is retained. All APIs preserve errno.
 * _at variants reuse caller-owned process-clock samples; internal clock-count
 * receipts therefore exclude caller-owned reads. Do not pass another clock. */
void kage_vita_deep_frame_begin(uint32_t loop, uint32_t window);
void kage_vita_deep_frame_begin_at(uint32_t loop, uint32_t window, uint64_t now);
void kage_vita_deep_phase_set(uint32_t phase);
void kage_vita_deep_phase_set_at(uint32_t phase, uint64_t now);
void kage_vita_deep_frame_end(void);
void kage_vita_deep_frame_end_at(uint64_t now);
/* dp.end records=N declares every dp.* core record from dp.h through dp.end,
 * including the one-time category dictionary and dp.end itself. It excludes
 * separate dp.png* resource-card records emitted by their own reporter. */
void kage_vita_deep_report(const char *bid, uint32_t window);
kage_vita_deep_token kage_vita_deep_enter(uint32_t category, uint64_t bytes);
void kage_vita_deep_leave(kage_vita_deep_token *token);
uint32_t kage_vita_deep_depth(void);
/* Explicit sentinel: no current owner frame (including startup/foreign). */
uint32_t kage_vita_deep_loop(void);
/* Abandon frames above mark after nonlocal return. They are incomplete, not
 * timed to the catch point. Enclosing inclusive spans may still be valid;
 * their exclusive time is suppressed if an escaped child makes it unknown. */
void kage_vita_deep_unwind(uint32_t mark);
void kage_vita_deep_mark_resource(void);
/* Native operation/class IDs exactly match kage_vita_io_profile.h: 10 physical
 * operations, 8 classes. `result` is native bytes/status, not fread items.
 * The leaf is charged to a parent only when wholly inside the active frame
 * and current top span, ends before now, and does not overlap an observed
 * sibling. Invalid provenance is counted, never given invented self time. */
void kage_vita_deep_io(uint32_t operation, uint32_t path_class,
                     uint64_t requested, int64_t result,
                     uint64_t begin, uint64_t end);

#if !defined(__GNUC__) && !defined(__clang__)
#error ISAAC_VITA_DEEP_PROFILE scopes require the cleanup attribute
#endif
#define KVD_JOIN_INNER(a,b) a##b
#define KVD_JOIN(a,b) KVD_JOIN_INNER(a,b)
#define KVD_SCOPE_IMPL(category, bytes, id) \
    kage_vita_deep_token KVD_JOIN(kvd_scope_, id) \
    __attribute__((cleanup(kage_vita_deep_leave))) = \
        kage_vita_deep_enter((category), (bytes))
#define KAGE_VITA_DEEP_SCOPE(category) KVD_SCOPE_IMPL((category), 0U, __COUNTER__)
#define KAGE_VITA_DEEP_SCOPE_BYTES(category, bytes) \
    KVD_SCOPE_IMPL((category), (bytes), __COUNTER__)

#else

/* Disabled instrumentation evaluates no scope arguments. Calls are provided
 * as no-op inline seams so an optional consumer cannot introduce link deps. */
#define KAGE_VITA_DEEP_SCOPE(category) ((void)0)
#define KAGE_VITA_DEEP_SCOPE_BYTES(category, bytes) ((void)0)
static inline void kage_vita_deep_frame_begin(uint32_t l, uint32_t w) { (void)l; (void)w; }
static inline void kage_vita_deep_frame_begin_at(uint32_t l, uint32_t w, uint64_t t) { (void)l; (void)w; (void)t; }
static inline void kage_vita_deep_phase_set(uint32_t p) { (void)p; }
static inline void kage_vita_deep_phase_set_at(uint32_t p, uint64_t t) { (void)p; (void)t; }
static inline void kage_vita_deep_frame_end(void) {}
static inline void kage_vita_deep_frame_end_at(uint64_t t) { (void)t; }
static inline void kage_vita_deep_report(const char *b, uint32_t w) { (void)b; (void)w; }
static inline kage_vita_deep_token kage_vita_deep_enter(uint32_t c, uint64_t b) {
    kage_vita_deep_token t = {0U, 0U, 0U, 0U}; (void)c; (void)b; return t;
}
static inline void kage_vita_deep_leave(kage_vita_deep_token *t) { (void)t; }
static inline uint32_t kage_vita_deep_depth(void) { return 0U; }
static inline uint32_t kage_vita_deep_loop(void) { return UINT32_MAX; }
static inline void kage_vita_deep_unwind(uint32_t m) { (void)m; }
static inline void kage_vita_deep_mark_resource(void) {}
static inline void kage_vita_deep_io(uint32_t o, uint32_t c, uint64_t r,
                                    int64_t v, uint64_t b, uint64_t e) {
    (void)o; (void)c; (void)r; (void)v; (void)b; (void)e;
}
#endif
#endif
