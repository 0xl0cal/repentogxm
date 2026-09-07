#ifndef ISAAC_HOST_VITA_KAGE_REFCOUNT_SEAM_H
#define ISAAC_HOST_VITA_KAGE_REFCOUNT_SEAM_H

#include <stdint.h>

#include "guest.h"

/* Native seam for the three frozen KAGE::ReferenceCount<ImageBase> helpers
 * (ISAAC_VITA_KAGE_REFCOUNT_SEAM, default OFF):
 *
 *   sub_00007af0  Release()    (counter vtable slot 3, this = ecx, ret)
 *   sub_00007b50  AddRef()     (slot 2, this = ecx, tail-jmps Mutex::Unlock)
 *   sub_00007b70  weak-lock()  (slot 1, this = ecx, ret; calls AddRef when
 *                               the strong count is non-zero)
 *
 * Each body is one Mutex::Lock(-1) .. Mutex::Unlock pair (two pairs on the
 * weak-lock alive path) around a 16-bit count update.  gen_all emits
 * `if (isaac_vita_kage_refcount_*_try(c)) return;` at the top of each
 * translated body (after guest_coverage_function, inside the GPR
 * flush/reload bracket).  HANDLED (1) means the helper produced the complete
 * architectural, guest-memory and census state the translated body produces
 * when both of its Lock/Unlock calls are seam-handled by
 * host_vita_kage_mutex_seam.c; REJECTED (0) leaves CPU, guest stack, object,
 * critical section, counters and the sampler word untouched so the exact
 * translated body runs (its Lock/Unlock then go through the mutex seam as
 * today).
 *
 * Handled only when, before any store: the inline sync latch names this
 * bound CPU; ESP is word-aligned; the whole pushed frame plus the return
 * word lies inside the owned stack; both sync IAT words are loader tokens
 * and the import-ID table is registered; the mutex vtable's Lock/Unlock
 * slots hold the frozen wrappers (and, weak-lock alive, the counter
 * vtable's AddRef slot holds 0x7b50); the Mutex is initialized; its
 * critical section passes the inline fast path's object predicate, has no
 * foreign/claiming owner, no depth wrap and no busy byte; the pushed frame,
 * the object, the critical section, the vtable slots and the IAT words are
 * pairwise disjoint wherever one route writes what the other re-reads
 * (fail closed); and, Release only, the strong count is >= 2 (1 is the
 * destroy path, 0 a cannot-happen no-op left to the body).  Because
 * Lock then Unlock on one critical section is a net no-op on the object,
 * the seam stores nothing into the critical section.
 *
 * Flags: the seam writes nothing to c->fl.  That equals the translated
 * bodies only under GUEST_FLAGS_LOCAL=1 (the AddRef `inc word` result stays
 * in a body local and no handled path flushes it), so the TU refuses to
 * compile otherwise and CMake requires ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL.
 *
 * Thread safety: exactly that of the inline critical-section fast path and
 * the mutex seam (plain loads/stores gated on `c->vita_sync_thread_id ==
 * g_isaac_vita_sync_inline_owner`; see host_vita_sync_fastpath.h). */
int isaac_vita_kage_refcount_release_try(CPU *__restrict c);
int isaac_vita_kage_refcount_addref_try(CPU *__restrict c);
int isaac_vita_kage_refcount_weaklock_try(CPU *__restrict c);

/* Receipt counters (always compiled; read by the host oracle and printed by
 * the [isaac-kage] refcount seam stats line). */
enum {
    ISAAC_KRS_FN_ADDREF = 0,
    ISAAC_KRS_FN_WEAKLOCK = 1,
    ISAAC_KRS_FN_RELEASE = 2,
    ISAAC_KRS_FN_COUNT = 3
};
enum {
    ISAAC_KRS_HANDLED = 0,
    ISAAC_KRS_REJECT_LATCH,     /* latch does not name this CPU */
    ISAAC_KRS_REJECT_ESP,       /* misaligned ESP */
    ISAAC_KRS_REJECT_FRAME,     /* frame or return word outside the stack */
    ISAAC_KRS_REJECT_SLOT,      /* IAT word not a token / table not ready */
    ISAAC_KRS_REJECT_INIT,      /* Mutex not initialized (KAGE::Log path) */
    ISAAC_KRS_REJECT_VT,        /* vtable slot is not the frozen target */
    ISAAC_KRS_REJECT_CS,        /* not a registered critical section */
    ISAAC_KRS_REJECT_ALIAS,     /* frame/object/cs/slots/IAT overlap */
    ISAAC_KRS_REJECT_OWNER,     /* foreign/claiming owner or depth wrap */
    ISAAC_KRS_REJECT_BUSY,      /* busy byte set (Sleep loop) */
    ISAAC_KRS_REJECT_ZERO,      /* Release with strong == 0 */
    ISAAC_KRS_REJECT_LAST,      /* Release with strong == 1 (destroy path) */
    ISAAC_KRS_REASON_COUNT
};

typedef struct isaac_vita_kage_refcount_seam_stats {
    uint32_t calls[ISAAC_KRS_FN_COUNT];
    uint32_t handled[ISAAC_KRS_FN_COUNT];
    uint32_t weak_alive;
    uint32_t weak_dead;
    uint32_t reasons[ISAAC_KRS_REASON_COUNT];   /* [0] unused */
    uint32_t imports_replayed;
    uint32_t verify_runs;
    uint32_t verify_mismatches;
    uint32_t verify_skipped_fault;
    uint32_t last_reason;
} isaac_vita_kage_refcount_seam_stats;

const isaac_vita_kage_refcount_seam_stats *
isaac_vita_kage_refcount_seam_stats_get(void);
void isaac_vita_kage_refcount_seam_stats_reset(void);
void isaac_vita_kage_refcount_seam_stats_log(void);

#endif
