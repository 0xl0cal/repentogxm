/* VitaSDK implementations behind the portable guest synchronization layer. */
#include <stdint.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/thread.h>

#include "vita_sync_services.h"

void isaac_vita_log(const char *format, ...);

static uint32_t s_user_enabled;
static uint32_t s_user_live;
static uint32_t s_user_peak;
static uint32_t s_user_creates;
static uint32_t s_user_deletes;
static uint32_t s_user_waits;
static uint32_t s_user_wait_failures;
static uint32_t s_user_owner_failures;
static uint32_t s_user_state_failures;

/* A successful Vita thread UID is positive, so this transient publication
 * value cannot alias an owner.  It lets acquisition initialize depth before
 * publishing the real owner and keeps the two-word invariant observable. */
#define ISAAC_VITA_SYNC_USER_CLAIMING UINT32_MAX

int isaac_vita_sync_is_userspace_mutex(int32_t handle)
{
    return handle == ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE;
}

static void isaac_vita_sync_user_note_create(void)
{
    uint32_t live = __atomic_add_fetch(&s_user_live, 1U, __ATOMIC_RELAXED);
    uint32_t peak = __atomic_load_n(&s_user_peak, __ATOMIC_RELAXED);
    uint32_t creates =
        __atomic_add_fetch(&s_user_creates, 1U, __ATOMIC_RELAXED);

    while (live > peak &&
           !__atomic_compare_exchange_n(
               &s_user_peak, &peak, live, 0,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (live > peak && (live & (live - 1U)) == 0U)
        isaac_vita_log(
            "Vita recursive mutex embedded state: creates=%u live=%u "
            "peak=%u storage=guest kernel_uids=0",
            (unsigned)creates, (unsigned)live, (unsigned)live);
}

static int32_t isaac_vita_sync_create_userspace_mutex(void)
{
    isaac_vita_sync_user_note_create();
    return ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE;
}

static int isaac_vita_sync_user_words_valid(uint32_t *owner_word,
                                            uint32_t *depth_word)
{
    uintptr_t owner = (uintptr_t)owner_word;
    uintptr_t depth = (uintptr_t)depth_word;

    return owner_word && depth_word && !(owner & 3U) &&
           depth == owner + sizeof *owner_word;
}

int32_t isaac_vita_sync_current_thread_id(void)
{
    int32_t thread_id = (int32_t)sceKernelGetThreadId();

    return thread_id > 0
               ? thread_id
               : (thread_id < 0
                      ? thread_id
                      : (int32_t)(uint32_t)
                            SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
}

static int isaac_vita_sync_user_state_failure(void)
{
    __atomic_add_fetch(&s_user_state_failures, 1U, __ATOMIC_RELAXED);
    return (int)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
}

#if defined(ISAAC_VITA_LUA_RECEIPT)
/* Startup-stall receipt (bounded).  Every guest critical section lives in
 * guest memory as an owner/depth word pair; a corrupted owner word (or a real
 * cross-thread deadlock) leaves the guest thread in the wait loop below with
 * no fault and no log line.  Report the wait at power-of-two spin counts from
 * 4096 (about a second of 1 us delays) so the values that keep it waiting are
 * visible; at most twelve lines per process. */
static void isaac_vita_sync_user_wait_receipt(
    const uint32_t *owner_word, const uint32_t *depth_word,
    uint32_t owner, uint32_t self, uint32_t spins)
{
    static uint32_t s_lines;

    if (spins < 4096U || (spins & (spins - 1U)) != 0U || s_lines >= 12U)
        return;
    ++s_lines;
    isaac_vita_log(
        "Vita recursive mutex wait: self=0x%08x owner=0x%08x depth=%u "
        "words=0x%08x spins=%u waits=%u",
        self, owner, __atomic_load_n(depth_word, __ATOMIC_RELAXED),
        (unsigned)(uintptr_t)owner_word, spins,
        __atomic_load_n(&s_user_waits, __ATOMIC_RELAXED));
}
#endif

int isaac_vita_sync_lock_userspace_mutex_for_thread(
    uint32_t *owner_word, uint32_t *depth_word, int32_t thread_id)
{
    uint32_t owner_id;
    int32_t result;
#if defined(ISAAC_VITA_LUA_RECEIPT)
    uint32_t spins = 0U;
#endif

    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    if (thread_id <= 0)
        return thread_id < 0
                   ? thread_id
                   : (int)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    owner_id = (uint32_t)thread_id;
    for (;;) {
        uint32_t owner = __atomic_load_n(owner_word, __ATOMIC_ACQUIRE);

        if (owner == owner_id) {
            uint32_t depth =
                __atomic_load_n(depth_word, __ATOMIC_RELAXED);

            if (!depth)
                return isaac_vita_sync_user_state_failure();
            if (depth == UINT32_MAX)
                return (int)(uint32_t)SCE_KERNEL_ERROR_MUTEX_LOCK_OVF;
            __atomic_store_n(depth_word, depth + 1U, __ATOMIC_RELAXED);
            return 0;
        }
        if (!owner) {
            uint32_t expected = 0U;

            if (__atomic_compare_exchange_n(
                    owner_word, &expected,
                    ISAAC_VITA_SYNC_USER_CLAIMING, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                if (__atomic_load_n(depth_word, __ATOMIC_RELAXED) != 0U) {
                    __atomic_store_n(owner_word, 0U, __ATOMIC_RELEASE);
                    return isaac_vita_sync_user_state_failure();
                }
                __atomic_store_n(depth_word, 1U, __ATOMIC_RELAXED);
                __atomic_store_n(owner_word, owner_id, __ATOMIC_RELEASE);
                return 0;
            }
        } else {
            __atomic_add_fetch(&s_user_waits, 1U, __ATOMIC_RELAXED);
#if defined(ISAAC_VITA_LUA_RECEIPT)
            isaac_vita_sync_user_wait_receipt(
                owner_word, depth_word, owner, owner_id, ++spins);
#endif
            result = sceKernelDelayThread(1U);
            if (result < 0) {
                __atomic_add_fetch(
                    &s_user_wait_failures, 1U, __ATOMIC_RELAXED);
                return result;
            }
        }
    }
}

int isaac_vita_sync_lock_userspace_mutex(uint32_t *owner_word,
                                         uint32_t *depth_word)
{
    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    return isaac_vita_sync_lock_userspace_mutex_for_thread(
        owner_word, depth_word, isaac_vita_sync_current_thread_id());
}

int isaac_vita_sync_try_lock_userspace_mutex_for_thread(
    uint32_t *owner_word, uint32_t *depth_word, int32_t thread_id)
{
    uint32_t owner_id;

    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    if (thread_id <= 0)
        return thread_id < 0
                   ? thread_id
                   : (int)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    owner_id = (uint32_t)thread_id;
    for (;;) {
        uint32_t owner = __atomic_load_n(owner_word, __ATOMIC_ACQUIRE);

        if (owner == owner_id) {
            uint32_t depth =
                __atomic_load_n(depth_word, __ATOMIC_RELAXED);

            if (!depth)
                return isaac_vita_sync_user_state_failure();
            if (depth == UINT32_MAX)
                return (int)(uint32_t)SCE_KERNEL_ERROR_MUTEX_LOCK_OVF;
            __atomic_store_n(depth_word, depth + 1U, __ATOMIC_RELAXED);
            return 1;
        }
        if (owner)
            return 0;
        {
            uint32_t expected = 0U;

            if (__atomic_compare_exchange_n(
                    owner_word, &expected,
                    ISAAC_VITA_SYNC_USER_CLAIMING, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                if (__atomic_load_n(depth_word, __ATOMIC_RELAXED) != 0U) {
                    __atomic_store_n(owner_word, 0U, __ATOMIC_RELEASE);
                    return isaac_vita_sync_user_state_failure();
                }
                __atomic_store_n(depth_word, 1U, __ATOMIC_RELAXED);
                __atomic_store_n(owner_word, owner_id, __ATOMIC_RELEASE);
                return 1;
            }
        }
    }
}

int isaac_vita_sync_try_lock_userspace_mutex(uint32_t *owner_word,
                                             uint32_t *depth_word)
{
    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    return isaac_vita_sync_try_lock_userspace_mutex_for_thread(
        owner_word, depth_word, isaac_vita_sync_current_thread_id());
}

int isaac_vita_sync_unlock_userspace_mutex_for_thread(
    uint32_t *owner_word, uint32_t *depth_word, int32_t thread_id)
{
    uint32_t owner;
    uint32_t depth;

    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    if (thread_id <= 0)
        return thread_id < 0
                   ? thread_id
                   : (int)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    owner = __atomic_load_n(owner_word, __ATOMIC_ACQUIRE);
    depth = __atomic_load_n(depth_word, __ATOMIC_RELAXED);
    if (!owner)
        return depth ? isaac_vita_sync_user_state_failure()
                     : (int)(uint32_t)SCE_KERNEL_ERROR_MUTEX_UNLOCK_UDF;
    if (owner != (uint32_t)thread_id) {
        __atomic_add_fetch(&s_user_owner_failures, 1U, __ATOMIC_RELAXED);
        return (int)(uint32_t)SCE_KERNEL_ERROR_MUTEX_NOT_OWNED;
    }
    if (!depth)
        return isaac_vita_sync_user_state_failure();
    if (depth > 1U) {
        __atomic_store_n(depth_word, depth - 1U, __ATOMIC_RELAXED);
        return 0;
    }
    __atomic_store_n(depth_word, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(owner_word, 0U, __ATOMIC_RELEASE);
    return 0;
}

int isaac_vita_sync_unlock_userspace_mutex(uint32_t *owner_word,
                                           uint32_t *depth_word)
{
    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    return isaac_vita_sync_unlock_userspace_mutex_for_thread(
        owner_word, depth_word, isaac_vita_sync_current_thread_id());
}

int isaac_vita_sync_delete_userspace_mutex(uint32_t *owner_word,
                                           uint32_t *depth_word)
{
    uint32_t live;

    if (!isaac_vita_sync_user_words_valid(owner_word, depth_word))
        return isaac_vita_sync_user_state_failure();
    if (__atomic_load_n(owner_word, __ATOMIC_ACQUIRE) != 0U)
        return (int)(uint32_t)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
    if (__atomic_load_n(depth_word, __ATOMIC_RELAXED) != 0U)
        return isaac_vita_sync_user_state_failure();
    live = __atomic_load_n(&s_user_live, __ATOMIC_RELAXED);
    do {
        if (!live)
            return isaac_vita_sync_user_state_failure();
    } while (!__atomic_compare_exchange_n(
        &s_user_live, &live, live - 1U, 0,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    __atomic_add_fetch(&s_user_deletes, 1U, __ATOMIC_RELAXED);
    return 0;
}

int32_t isaac_vita_sync_create_recursive_mutex(void)
{
    uint32_t expected = 0U;

    if (__atomic_compare_exchange_n(
            &s_user_enabled, &expected, 1U, 0,
            __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        isaac_vita_log(
            "Vita recursive mutex backend: status=enabled "
            "route=embedded storage=guest kernel_uids=0");
    return isaac_vita_sync_create_userspace_mutex();
}

void isaac_vita_sync_get_mutex_pool_snapshot(
    isaac_vita_sync_mutex_pool_snapshot *snapshot)
{
    if (!snapshot)
        return;
    snapshot->enabled =
        __atomic_load_n(&s_user_enabled, __ATOMIC_ACQUIRE);
    snapshot->live = __atomic_load_n(&s_user_live, __ATOMIC_RELAXED);
    snapshot->peak = __atomic_load_n(&s_user_peak, __ATOMIC_RELAXED);
    snapshot->creates = __atomic_load_n(&s_user_creates, __ATOMIC_RELAXED);
    snapshot->deletes = __atomic_load_n(&s_user_deletes, __ATOMIC_RELAXED);
    snapshot->waits = __atomic_load_n(&s_user_waits, __ATOMIC_RELAXED);
    snapshot->wait_failures =
        __atomic_load_n(&s_user_wait_failures, __ATOMIC_RELAXED);
    snapshot->owner_failures =
        __atomic_load_n(&s_user_owner_failures, __ATOMIC_RELAXED);
    snapshot->state_failures =
        __atomic_load_n(&s_user_state_failures, __ATOMIC_RELAXED);
}

int32_t isaac_vita_sync_create_manual_reset_event(int initial_state)
{
    return (int32_t)sceKernelCreateEventFlag(
        "isaac_crt_event", SCE_EVENT_WAITMULTIPLE,
        initial_state ? 1 : 0, NULL);
}

int isaac_vita_sync_set_event(int32_t uid)
{
    return sceKernelSetEventFlag((SceUID)uid, 1U);
}

int isaac_vita_sync_reset_event(int32_t uid)
{
    /* sceKernelClearEventFlag keeps the bits selected by its mask. */
    return sceKernelClearEventFlag((SceUID)uid, ~1U);
}

int isaac_vita_sync_wait_event(int32_t uid)
{
    /* Manual-reset: wait for bit 0 without either CLEAR flag. */
    return sceKernelWaitEventFlag((SceUID)uid, 1U, SCE_EVENT_WAITOR,
                                  NULL, NULL);
}

int isaac_vita_sync_delete_event(int32_t uid)
{
    return sceKernelDeleteEventFlag((SceUID)uid);
}

int isaac_vita_sync_sleep_milliseconds(uint32_t milliseconds)
{
    int result;

    /* Vita rejects a zero delay, while Win32 Sleep(0) must yield the
     * remainder of the current time slice.  One microsecond is the narrow
     * scheduler-backed yield used here; it never spins a guest or ARM core. */
    if (!milliseconds)
        return sceKernelDelayThread(1U);
    while (milliseconds > ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS) {
        result = sceKernelDelayThread(
            ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS * 1000U);
        if (result < 0)
            return result;
        milliseconds -= ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS;
    }
    return sceKernelDelayThread(milliseconds * 1000U);
}

void isaac_vita_sync_log_failure(const char *operation, int32_t result)
{
    sceClibPrintf("[isaac-sync] %s failed: 0x%08x\n",
                  operation, (unsigned)result);
    isaac_vita_log(
        "Vita sync failure: operation=%s result=0x%08x",
        operation, (unsigned)result);
}
