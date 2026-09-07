#ifndef ISAAC_HOST_VITA_STATIC_SFX_PROFILE_H
#define ISAAC_HOST_VITA_STATIC_SFX_PROFILE_H
#include <stdint.h>
struct CPU;

/* CPU wall-clock scopes, NOT playback time. RIFF is nested in WAV; native
 * upload/create normally occur in a LATER static Play/setup call, not Load.
 * Never add inclusive/subscope sums. A completed
 * scope belongs wholly to the window taking it, including a scope which spans
 * an attempted snapshot. No sample paths or names are retained. */
typedef struct isaac_vita_static_sfx_time {
    uint32_t count, sum_us, max_us;
} isaac_vita_static_sfx_time;

typedef struct isaac_vita_static_sfx_window {
    isaac_vita_static_sfx_time wav, ogg, riff, upload, create, play;
    /* create is native alGenBuffers CALLS, not object or buffer count.
     * Static Play normally reuses the preallocated manager buffer pool. */
    uint32_t upload_bytes; /* positive bytes requested, NOT successful bytes */
    uint32_t wav_failed;   /* original loader returned AL == 0 */
    uint32_t ogg_failed;
    uint32_t faulted;      /* any scoped original returned with c->fault */
    uint32_t error_queries;/* existing native alGetError calls in a scope */
    uint32_t al_errors;    /* nonzero raw results, before adapter suppression */
    uint32_t last_al_error;
    uint32_t skipped;      /* nested/concurrent whole scopes not opened */
    uint32_t bad_clock;    /* count retained, duration rejected */
    uint32_t overflow;     /* any count/bytes/us field saturated */
} isaac_vita_static_sfx_window;

/* 1: take/zero completed scopes. NULL or load in progress: 0, output and
 * counters untouched. Nonblocking; call once at profile start to discard. */
int isaac_vita_static_sfx_profile_take_window(
    isaac_vita_static_sfx_window *out);

/* Import adapters pass values already read. Outside a tracked static scope
 * begin returns UINT64_MAX without reading the clock. */
uint64_t isaac_vita_static_sfx_profile_native_begin(struct CPU *c);
void isaac_vita_static_sfx_profile_upload_end(
    struct CPU *c, uint64_t start_us, int32_t requested_bytes);
void isaac_vita_static_sfx_profile_create_end(
    struct CPU *c, uint64_t start_us);
void isaac_vita_static_sfx_profile_error(struct CPU *c, uint32_t raw_error);
#endif
