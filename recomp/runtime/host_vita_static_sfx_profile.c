/* Optional observations of frozen memory-sample loaders. No guest state,
 * reads, allocation policy, AL calls, or return values are changed.
 * --wrap references originate in guest_table.c; RIFF calls are cross-TU.
 * WAV 005a3b30 calls RIFF 005bf4e0 twice (metadata, then PCM read). These PCM
 * WAVs have no sample decode loop: RIFF is parsing/read wall time, not decode.
 * OGG 005a36a0 includes decode, but decode is not separately measured.
 * Loader virtual +4 (005be530) only stores PCM/format. The actual upload is
 * LATER in static Play/setup 005be5b0 (AL return005be6e3), hence its own scope.
 * WAV and memory-OGG vtables use override 005bf4c0, whose direct base-Play
 * call shares guest_0177.c and is NOT intercepted by GNU ld --wrap. Wrap
 * that table entry too, without replacing or repeating either original. */
#include "host_vita_static_sfx_profile.h"
#include "kage_vita_deep_profile.h"
#if defined(ISAAC_VITA_STATIC_SFX_PROFILE) && ISAAC_VITA_STATIC_SFX_PROFILE
#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include "guest.h"
#include "vita_host_services.h"

void __real_sub_005a3b30(CPU *c);
void __real_sub_005a36a0(CPU *c);
void __real_sub_005bf4e0(CPU *c);
void __real_sub_005be5b0(CPU *c);
void __real_sub_005bf4c0(CPU *c);

/* A guest CPU cannot execute concurrently on two threads. This nonblocking
 * lock serializes complete scopes/snapshots; the atomic CPU key excludes
 * unrelated imports. Worker Vorbis decode never enters these guest loaders.
 * Nested loads call the original, count as skipped, and their native work
 * remains inside the outer scope. */
static atomic_flag s_scope_lock = ATOMIC_FLAG_INIT;
static atomic_uintptr_t s_cpu = ATOMIC_VAR_INIT((uintptr_t)0);
static atomic_uint s_skipped = ATOMIC_VAR_INIT(0);
static isaac_vita_static_sfx_window s_window;
static int s_is_wav;

static uint64_t sfx_clock(void)
{
    int saved_errno = errno;
    uint64_t result = isaac_vita_get_process_time();
    errno = saved_errno;
    return result;
}

static void sfx_add(uint32_t *value, uint64_t add)
{
    if (add > UINT32_MAX - *value) {
        *value = UINT32_MAX;
        s_window.overflow = 1U;
    } else {
        *value += (uint32_t)add;
    }
}

static void sfx_time(isaac_vita_static_sfx_time *time, uint64_t start,
                     uint64_t end)
{
    uint64_t elapsed;
    sfx_add(&time->count, 1U);
    if (start == UINT64_MAX || end < start) {
        sfx_add(&s_window.bad_clock, 1U);
        return;
    }
    elapsed = end - start;
    sfx_add(&time->sum_us, elapsed);
    if (elapsed > time->max_us) {
        time->max_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
        if (elapsed > UINT32_MAX)
            s_window.overflow = 1U;
    }
}

static int sfx_owns(CPU *c)
{
    return c != NULL && atomic_load_explicit(&s_cpu, memory_order_acquire) ==
        (uintptr_t)c;
}

static void sfx_skip(void)
{
    unsigned value = atomic_load_explicit(&s_skipped, memory_order_relaxed);
    while (value != UINT32_MAX && !atomic_compare_exchange_weak_explicit(
        &s_skipped, &value, value + 1U, memory_order_relaxed,
        memory_order_relaxed)) { }
}

enum { SFX_OGG, SFX_WAV, SFX_PLAY };
static void sfx_scope(CPU *c, unsigned kind, void (*original)(CPU *))
{
    KAGE_VITA_DEEP_SCOPE(KVD_STATIC_SFX);
    uint64_t start;
    isaac_vita_static_sfx_time *time;
    if (c == NULL || atomic_flag_test_and_set_explicit(
            &s_scope_lock, memory_order_acquire)) {
        sfx_skip();
        original(c);
        return;
    }
    s_is_wav = kind == SFX_WAV;
    atomic_store_explicit(&s_cpu, (uintptr_t)c, memory_order_release);
    start = sfx_clock();
    original(c);
    time = kind == SFX_PLAY ? &s_window.play :
        (kind == SFX_WAV ? &s_window.wav : &s_window.ogg);
    sfx_time(time, start, sfx_clock());
    /* Play/setup returns void: its leftover AL is NOT a failure result. */
    if (kind != SFX_PLAY && (c->eax & 0xffU) == 0U)
        sfx_add(kind == SFX_WAV ? &s_window.wav_failed :
                &s_window.ogg_failed, 1U);
    if (c->fault != NULL)
        sfx_add(&s_window.faulted, 1U);
    atomic_store_explicit(&s_cpu, (uintptr_t)0, memory_order_release);
    atomic_flag_clear_explicit(&s_scope_lock, memory_order_release);
}

void __wrap_sub_005a3b30(CPU *c)
{ sfx_scope(c, SFX_WAV, __real_sub_005a3b30); }
void __wrap_sub_005a36a0(CPU *c)
{ sfx_scope(c, SFX_OGG, __real_sub_005a36a0); }
void __wrap_sub_005be5b0(CPU *c)
{ sfx_scope(c, SFX_PLAY, __real_sub_005be5b0); }
void __wrap_sub_005bf4c0(CPU *c)
{ sfx_scope(c, SFX_PLAY, __real_sub_005bf4c0); }

void __wrap_sub_005bf4e0(CPU *c)
{
    uint64_t start;
    if (!sfx_owns(c) || !s_is_wav) {
        __real_sub_005bf4e0(c);
        return;
    }
    start = sfx_clock();
    __real_sub_005bf4e0(c);
    sfx_time(&s_window.riff, start, sfx_clock());
}

uint64_t isaac_vita_static_sfx_profile_native_begin(CPU *c)
{
    return sfx_owns(c) ? sfx_clock() : UINT64_MAX;
}

void isaac_vita_static_sfx_profile_upload_end(
    CPU *c, uint64_t start_us, int32_t requested_bytes)
{
    if (start_us == UINT64_MAX || !sfx_owns(c))
        return;
    sfx_time(&s_window.upload, start_us, sfx_clock());
    if (requested_bytes > 0)
        sfx_add(&s_window.upload_bytes, (uint32_t)requested_bytes);
}

void isaac_vita_static_sfx_profile_create_end(CPU *c, uint64_t start_us)
{
    if (start_us != UINT64_MAX && sfx_owns(c))
        sfx_time(&s_window.create, start_us, sfx_clock());
}

void isaac_vita_static_sfx_profile_error(CPU *c, uint32_t raw_error)
{
    if (!sfx_owns(c))
        return;
    sfx_add(&s_window.error_queries, 1U);
    if (raw_error != 0U) {
        sfx_add(&s_window.al_errors, 1U);
        s_window.last_al_error = raw_error;
    }
}

int isaac_vita_static_sfx_profile_take_window(
    isaac_vita_static_sfx_window *out)
{
    if (out == NULL || atomic_flag_test_and_set_explicit(
            &s_scope_lock, memory_order_acquire))
        return 0;
    *out = s_window;
    out->skipped = atomic_exchange_explicit(&s_skipped, 0U,
                                           memory_order_relaxed);
    if (out->skipped == UINT32_MAX)
        out->overflow = 1U;
    memset(&s_window, 0, sizeof s_window);
    atomic_flag_clear_explicit(&s_scope_lock, memory_order_release);
    return 1;
}
#endif
