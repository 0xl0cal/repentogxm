#ifndef ISAAC_HOST_VITA_KAGE_MUTEX_SEAM_H
#define ISAAC_HOST_VITA_KAGE_MUTEX_SEAM_H

#include "guest.h"

/* Native seam for the two KAGE Mutex wrappers of the frozen PE
 * (ISAAC_VITA_KAGE_MUTEX_SEAM, default OFF):
 *
 *   sub_00562e00  Mutex::Lock(timeout)   (vtable slot 3, this = ecx, ret 4)
 *   sub_00562ec0  Mutex::Unlock()        (vtable slot 4, this = ecx, ret)
 *
 * gen_all emits `if (isaac_vita_kage_mutex_lock_try(c)) return;` /
 * `if (isaac_vita_kage_mutex_unlock_try(c)) return;` at the top of each
 * translated body (after guest_coverage_function).  HANDLED (1) means the
 * helper produced the complete architectural, guest-memory and census state
 * the translated body produces on its blocking-Enter / Leave success path;
 * REJECTED (0) leaves CPU, guest stack, mutex object, counters and the
 * sampler word untouched so the exact translated body runs.
 *
 * Handled only:
 *   Lock:   [this+4] != 0 (initialized), [esp+4] == 0xffffffff (timeout -1),
 *           cs = [this+8] passes every predicate of the inline
 *           EnterCriticalSection fast path (host_vita_sync_fastpath.h) for
 *           this bound CPU, and [cs+0x18] == 0 (no Sleep(1000) loop).
 *   Unlock: [this+4] != 0, cs = [this+8] passes every predicate of the inline
 *           LeaveCriticalSection fast path.
 * Every other state (timed wait, 'Trying to lock mutex that has not been
 * initialized' logging path, the busy-flag wait loop, foreign or CLAIMING
 * owner, depth wrap, unarmed/disabled thread latch, unbound CPU, misaligned
 * ESP, frame outside the owned stack, object or critical section lying
 * inside the pushed frame [esp - 24, esp) / [esp - 12, esp) where the body's
 * own pushes would overwrite them, IAT slot not the loader token, import-ID
 * table not registered) is rejected before any store.
 *
 * Thread safety: exactly that of the inline critical-section fast path and
 * nothing more.  The mutex words, the busy byte and the latch are plain
 * loads and stores gated on `c->vita_sync_thread_id ==
 * g_isaac_vita_sync_inline_owner`; see the header comment of
 * host_vita_sync_fastpath.h for the participant census that makes this
 * sound today and for the REQUIREMENT any future second guest-code thread
 * imposes (isaac_vita_sync_inline_disable() before the spawn). */
int isaac_vita_kage_mutex_lock_try(CPU *__restrict c);
int isaac_vita_kage_mutex_unlock_try(CPU *__restrict c);

#endif
