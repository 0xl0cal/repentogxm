#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr/eventflag.h>

#include "vita_sync_services.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define STRESS_OBJECTS 8193U
#define RACE_ITERATIONS 20000U

typedef struct race_worker_argument {
    int32_t thread_id;
} race_worker_argument;

static _Thread_local int32_t
    s_thread_id = (int32_t)UINT32_C(0x40010003);
static unsigned s_delay_calls;
static int32_t s_delay_result;
static unsigned s_log_calls;
static uint32_t s_contended_state[2];
static uint32_t s_worker_started;
static uint32_t s_worker_acquired;
static uint32_t s_race_start;
static uint32_t s_race_error;
static uint32_t s_race_state[2];
static uint32_t s_race_counter;
static uint32_t s_stress_states[STRESS_OBJECTS][2];

void isaac_vita_log(const char *format, ...)
{
    (void)format;
    ++s_log_calls;
}

int sceClibPrintf(const char *format, ...)
{
    (void)format;
    return 0;
}

int sceKernelGetThreadId(void)
{
    return s_thread_id;
}

int sceKernelDelayThread(unsigned int usec)
{
    __atomic_add_fetch(&s_delay_calls, 1U, __ATOMIC_RELAXED);
    if (usec != 1U)
        return -1;
    if (s_delay_result)
        return s_delay_result;
    sched_yield();
    return 0;
}

SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *option)
{
    (void)name; (void)attr; (void)bits; (void)option;
    return 1;
}
int sceKernelSetEventFlag(SceUID uid, unsigned int bits)
{ (void)uid; (void)bits; return 0; }
int sceKernelClearEventFlag(SceUID uid, unsigned int bits)
{ (void)uid; (void)bits; return 0; }
int sceKernelWaitEventFlag(SceUID uid, unsigned int bits,
                           unsigned int wait, unsigned int *out,
                           unsigned int *timeout)
{ (void)uid; (void)bits; (void)wait; (void)out; (void)timeout; return 0; }
int sceKernelDeleteEventFlag(SceUID uid)
{ (void)uid; return 0; }

static void *contention_worker(void *unused)
{
    int result;

    (void)unused;
    s_thread_id = (int32_t)UINT32_C(0x40010005);
    __atomic_store_n(&s_worker_started, 1U, __ATOMIC_RELEASE);
    result = isaac_vita_sync_lock_userspace_mutex(
        &s_contended_state[0], &s_contended_state[1]);
    if (result == 0) {
        __atomic_store_n(&s_worker_acquired, 1U, __ATOMIC_RELEASE);
        result = isaac_vita_sync_unlock_userspace_mutex(
            &s_contended_state[0], &s_contended_state[1]);
    }
    return (void *)(intptr_t)result;
}

static void *race_worker(void *opaque)
{
    const race_worker_argument *argument =
        (const race_worker_argument *)opaque;
    uint32_t iteration;

    s_thread_id = argument->thread_id;
    while (!__atomic_load_n(&s_race_start, __ATOMIC_ACQUIRE))
        sched_yield();
    for (iteration = 0U; iteration < RACE_ITERATIONS; ++iteration) {
        if (isaac_vita_sync_lock_userspace_mutex(
                &s_race_state[0], &s_race_state[1]) != 0) {
            __atomic_store_n(&s_race_error, 1U, __ATOMIC_RELEASE);
            return (void *)(intptr_t)-1;
        }
        ++s_race_counter; /* deliberately non-atomic: the lock owns it */
        if (isaac_vita_sync_unlock_userspace_mutex(
                &s_race_state[0], &s_race_state[1]) != 0) {
            __atomic_store_n(&s_race_error, 1U, __ATOMIC_RELEASE);
            return (void *)(intptr_t)-1;
        }
    }
    return NULL;
}

int main(void)
{
    isaac_vita_sync_mutex_pool_snapshot snapshot;
    uint32_t state_a[2] = { 0U, 0U };
    uint32_t state_b[2] = { 0U, 0U };
    uint32_t bad_state[2] = { 0U, 0U };
    int32_t marker_a;
    int32_t marker_b;
    pthread_t worker;
    pthread_t race_workers[2];
    race_worker_argument race_arguments[2] = {
        { (int32_t)UINT32_C(0x40010101) },
        { (int32_t)UINT32_C(0x40010102) }
    };
    void *worker_result;
    unsigned delay_before;
    unsigned i;

    marker_a = isaac_vita_sync_create_recursive_mutex();
    marker_b = isaac_vita_sync_create_recursive_mutex();
    CHECK(marker_a == ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
    CHECK(marker_b == marker_a && isaac_vita_sync_is_userspace_mutex(marker_a));
    CHECK(!isaac_vita_sync_is_userspace_mutex(0x40010003));

    CHECK(isaac_vita_sync_lock_userspace_mutex(&state_a[0], &state_a[1]) == 0);
    CHECK(state_a[0] == UINT32_C(0x40010003) && state_a[1] == 1U);
    CHECK(isaac_vita_sync_lock_userspace_mutex(&state_a[0], &state_a[1]) == 0);
    CHECK(state_a[1] == 2U);
    CHECK(isaac_vita_sync_lock_userspace_mutex(&state_b[0], &state_b[1]) == 0);
    CHECK(state_b[0] == UINT32_C(0x40010003) && state_b[1] == 1U);

    s_thread_id = (int32_t)UINT32_C(0x40010004);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &state_a[0], &state_a[1]) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_MUTEX_NOT_OWNED);
    delay_before = s_delay_calls;
    CHECK(isaac_vita_sync_try_lock_userspace_mutex(
              &state_a[0], &state_a[1]) == 0);
    CHECK(s_delay_calls == delay_before && state_a[1] == 2U);
    s_delay_result = -0x4401;
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &state_a[0], &state_a[1]) == -0x4401);
    CHECK(s_delay_calls == delay_before + 1U && state_a[1] == 2U);
    s_delay_result = 0;

    s_thread_id = (int32_t)UINT32_C(0x40010003);
    CHECK(isaac_vita_sync_try_lock_userspace_mutex(
              &state_a[0], &state_a[1]) == 1);
    CHECK(state_a[1] == 3U);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &state_a[0], &state_a[1]) == 0);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &state_a[0], &state_a[1]) == 0);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &state_a[0], &state_a[1]) == 0);
    CHECK(!state_a[0] && !state_a[1]);
    CHECK(isaac_vita_sync_delete_userspace_mutex(
              &state_a[0], &state_a[1]) == 0);

    CHECK(isaac_vita_sync_delete_userspace_mutex(
              &state_b[0], &state_b[1]) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &state_b[0], &state_b[1]) == 0);
    CHECK(isaac_vita_sync_delete_userspace_mutex(
              &state_b[0], &state_b[1]) == 0);

    CHECK(isaac_vita_sync_create_recursive_mutex() == marker_a);
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &s_contended_state[0], &s_contended_state[1]) == 0);
    delay_before = __atomic_load_n(&s_delay_calls, __ATOMIC_RELAXED);
    CHECK(pthread_create(&worker, NULL, contention_worker, NULL) == 0);
    for (i = 0; i < 1000000U; ++i) {
        if (__atomic_load_n(&s_worker_started, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&s_delay_calls, __ATOMIC_RELAXED) > delay_before)
            break;
        sched_yield();
    }
    CHECK(i < 1000000U);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &s_contended_state[0], &s_contended_state[1]) == 0);
    CHECK(pthread_join(worker, &worker_result) == 0);
    CHECK((intptr_t)worker_result == 0);
    CHECK(__atomic_load_n(&s_worker_acquired, __ATOMIC_ACQUIRE) == 1U);
    CHECK(!s_contended_state[0] && !s_contended_state[1]);
    CHECK(isaac_vita_sync_delete_userspace_mutex(
              &s_contended_state[0], &s_contended_state[1]) == 0);

    /* Both contenders start from a free word and repeatedly race the same
     * 0->owner transition.  This catches publication gaps that a waiter on
     * an already-owned lock cannot exercise; the plain counter also gives
     * ThreadSanitizer an acquire/release visibility oracle. */
    CHECK(isaac_vita_sync_create_recursive_mutex() == marker_a);
    CHECK(pthread_create(
              &race_workers[0], NULL, race_worker, &race_arguments[0]) == 0);
    CHECK(pthread_create(
              &race_workers[1], NULL, race_worker, &race_arguments[1]) == 0);
    __atomic_store_n(&s_race_start, 1U, __ATOMIC_RELEASE);
    CHECK(pthread_join(race_workers[0], &worker_result) == 0);
    CHECK((intptr_t)worker_result == 0);
    CHECK(pthread_join(race_workers[1], &worker_result) == 0);
    CHECK((intptr_t)worker_result == 0);
    CHECK(!__atomic_load_n(&s_race_error, __ATOMIC_ACQUIRE));
    CHECK(s_race_counter == 2U * RACE_ITERATIONS);
    CHECK(!s_race_state[0] && !s_race_state[1]);
    CHECK(isaac_vita_sync_delete_userspace_mutex(
              &s_race_state[0], &s_race_state[1]) == 0);

    /* The hardware created 4096 live image-reference critical sections in
     * 0.207 seconds.  Keep more than twice that many objects live together;
     * their state is caller-owned, so this has no backend capacity edge. */
    for (i = 0U; i < STRESS_OBJECTS; ++i)
        CHECK(isaac_vita_sync_create_recursive_mutex() == marker_a);
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &s_stress_states[0][0], &s_stress_states[0][1]) == 0);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &s_stress_states[0][0], &s_stress_states[0][1]) == 0);
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &s_stress_states[STRESS_OBJECTS - 1U][0],
              &s_stress_states[STRESS_OBJECTS - 1U][1]) == 0);
    CHECK(isaac_vita_sync_unlock_userspace_mutex(
              &s_stress_states[STRESS_OBJECTS - 1U][0],
              &s_stress_states[STRESS_OBJECTS - 1U][1]) == 0);
    for (i = 0U; i < 512U; ++i) {
        s_thread_id = (int32_t)(UINT32_C(0x40011000) + i);
        CHECK(isaac_vita_sync_lock_userspace_mutex(
                  &s_stress_states[0][0], &s_stress_states[0][1]) == 0);
        CHECK(isaac_vita_sync_unlock_userspace_mutex(
                  &s_stress_states[0][0], &s_stress_states[0][1]) == 0);
    }
    s_thread_id = (int32_t)UINT32_C(0x40010003);
    for (i = 0U; i < STRESS_OBJECTS; ++i)
        CHECK(isaac_vita_sync_delete_userspace_mutex(
                  &s_stress_states[i][0], &s_stress_states[i][1]) == 0);

    CHECK(isaac_vita_sync_create_recursive_mutex() == marker_a);
    s_thread_id = 0;
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &bad_state[0], &bad_state[1]) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    CHECK(!bad_state[0] && !bad_state[1]);
    s_thread_id = -0x4501;
    CHECK(isaac_vita_sync_try_lock_userspace_mutex(
              &bad_state[0], &bad_state[1]) == -0x4501);
    CHECK(!bad_state[0] && !bad_state[1]);
    s_thread_id = (int32_t)UINT32_C(0x40010003);
    bad_state[0] = (uint32_t)s_thread_id;
    bad_state[1] = UINT32_MAX;
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &bad_state[0], &bad_state[1]) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_MUTEX_LOCK_OVF);
    CHECK(isaac_vita_sync_try_lock_userspace_mutex(
              &bad_state[0], &bad_state[1]) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_MUTEX_LOCK_OVF);
    bad_state[0] = 0U;
    bad_state[1] = 1U;
    CHECK(isaac_vita_sync_lock_userspace_mutex(
              &bad_state[0], &bad_state[1]) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
    bad_state[1] = 0U;
    CHECK(isaac_vita_sync_lock_userspace_mutex(NULL, NULL) ==
          (int32_t)(uint32_t)SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
    CHECK(isaac_vita_sync_delete_userspace_mutex(
              &bad_state[0], &bad_state[1]) == 0);

    isaac_vita_sync_get_mutex_pool_snapshot(&snapshot);
    CHECK(snapshot.enabled && !snapshot.live &&
          snapshot.peak == STRESS_OBJECTS &&
          snapshot.creates == STRESS_OBJECTS + 5U &&
          snapshot.deletes == snapshot.creates &&
          snapshot.waits >= 2U && snapshot.wait_failures == 1U &&
          snapshot.owner_failures == 1U && snapshot.state_failures >= 2U);
    CHECK(s_log_calls >= 14U);

    puts("Vita embedded recursive mutex backend oracle: PASS");
    return 0;
}
