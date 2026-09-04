#ifndef ISAAC_VITA_SYNC_SERVICES_H
#define ISAAC_VITA_SYNC_SERVICES_H

#include <stdint.h>

/* Narrow target-service edge.  The x86 ABI implementation depends only on
 * these functions; the ARM oracle links the same production wrappers against
 * deterministic mock sceKernel calls. */
int32_t isaac_vita_sync_create_recursive_mutex(void);

/* Win32 keeps critical-section ownership in caller-provided storage.  Mirror
 * that model: every guest critical section keeps its recursive owner/depth in
 * the two reserved words of its own 24-byte object.  The fixed marker below
 * is not a kernel UID and must never be passed to a Vita mutex API.
 * Co-locating the state avoids both Vita's process UID limit and a second
 * process-wide object-count ceiling. */
#define ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE INT32_C(0x60000001)

typedef struct isaac_vita_sync_mutex_pool_snapshot {
    uint32_t enabled;
    uint32_t live;
    uint32_t peak;
    uint32_t creates;
    uint32_t deletes;
    uint32_t waits;
    uint32_t wait_failures;
    uint32_t owner_failures;
    uint32_t state_failures;
} isaac_vita_sync_mutex_pool_snapshot;

int isaac_vita_sync_is_userspace_mutex(int32_t handle);
int32_t isaac_vita_sync_current_thread_id(void);
int isaac_vita_sync_lock_userspace_mutex(uint32_t *owner_word,
                                         uint32_t *depth_word);
int isaac_vita_sync_lock_userspace_mutex_for_thread(
    uint32_t *owner_word, uint32_t *depth_word, int32_t thread_id);
int isaac_vita_sync_try_lock_userspace_mutex(uint32_t *owner_word,
                                             uint32_t *depth_word);
int isaac_vita_sync_try_lock_userspace_mutex_for_thread(
    uint32_t *owner_word, uint32_t *depth_word, int32_t thread_id);
int isaac_vita_sync_unlock_userspace_mutex(uint32_t *owner_word,
                                           uint32_t *depth_word);
int isaac_vita_sync_unlock_userspace_mutex_for_thread(
    uint32_t *owner_word, uint32_t *depth_word, int32_t thread_id);
int isaac_vita_sync_delete_userspace_mutex(uint32_t *owner_word,
                                           uint32_t *depth_word);
void isaac_vita_sync_get_mutex_pool_snapshot(
    isaac_vita_sync_mutex_pool_snapshot *snapshot);

int32_t isaac_vita_sync_create_manual_reset_event(int initial_state);
int isaac_vita_sync_set_event(int32_t uid);
int isaac_vita_sync_reset_event(int32_t uid);
int isaac_vita_sync_wait_event(int32_t uid);
int isaac_vita_sync_delete_event(int32_t uid);

/* sceKernelDelayThread accepts microseconds in one 32-bit word and rejects
 * zero.  Sleep(0) therefore uses a one-microsecond scheduler yield; long
 * sleeps split at this exact millisecond boundary without overflowing. */
#define ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS 4294967U
int isaac_vita_sync_sleep_milliseconds(uint32_t milliseconds);

void isaac_vita_sync_log_failure(const char *operation, int32_t result);

#endif
