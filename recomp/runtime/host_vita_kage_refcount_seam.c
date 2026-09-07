#include "host_vita_kage_refcount_seam.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_import_id.h"
#include "host_vita_sync_fastpath.h"
#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

#if !defined(ISAAC_VITA_KAGE_REFCOUNT_SEAM) || !ISAAC_VITA_KAGE_REFCOUNT_SEAM
#error The KAGE refcount seam must only be compiled when enabled
#endif
#if !defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
#error The KAGE refcount seam replays the inline critical-section fast path
#endif
#if !GUEST_STACK_REQUIRED
#error The KAGE refcount seam mirrors production: GUEST_STACK_REQUIRED=1
#endif
#if !defined(GUEST_FLAGS_LOCAL) || !GUEST_FLAGS_LOCAL
/* The translated AddRef body computes `inc word [esi+4]` flags.  With
 * GUEST_FLAGS_LOCAL=1 they live in a body local that no handled path ever
 * flushes to c->fl (no GUEST_FLAGS_FLUSH on ret or on the predicted
 * tail-jmp), so a seam that writes nothing to c->fl is exact.  With
 * GUEST_FLAGS_LOCAL=0 the body would leave the INC result in c->fl and the
 * two routes would diverge.  CMake mirrors the corpus define onto this TU
 * and fails at configure when ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL is OFF. */
#error The KAGE refcount seam requires the GUEST_FLAGS_LOCAL=1 corpus
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_KAGE_REFCOUNT_SEAM_BUILD_ID
#define ISAAC_VITA_KAGE_REFCOUNT_SEAM_BUILD_ID "refcount-seam:unstamped"
#endif

/* The frozen x86 bodies this file replays (recomp/gen_all.py
 * VITA_KAGE_REFCOUNT_SEAM_SPECS pins every instruction of all three).
 *
 * Control block KAGE::ReferenceCount<T> (0x18 bytes): +0 vtable, +4 u16
 * strong, +6 u16 weak, +8 KAGE::System::Mutex {+8 vtable (slot 3 = Lock
 * sub_00562e00 `ret 4`, slot 4 = Unlock sub_00562ec0), +0xc initialized
 * byte, +0x10 CRITICAL_SECTION* (0x1c bytes, busy byte at +0x18)}, +0x14 T*.
 *
 * sub_00007b50 AddRef:
 *   push esi ; mov esi,ecx ; push edi ; push -1
 *   mov eax,[esi+8] ; lea ecx,[esi+8] ; call [eax+0Ch]      (Lock(-1))
 *   inc word [esi+4]
 *   lea ecx,[esi+8] ; mov eax,[esi+8] ; pop edi ; pop esi ; jmp [eax+10h]
 *
 * sub_00007af0 Release:
 *   push esi ; mov esi,ecx ; push edi ; push -1
 *   mov eax,[esi+8] ; lea ecx,[esi+8] ; call [eax+0Ch]      (Lock(-1))
 *   movzx eax,word [esi+4] ; test ax,ax ; je tail
 *   dec eax ; mov [esi+4],ax ; test ax,ax ; jne tail
 *   ... destroy path (Unlock, [this+14h]->vt[60h](1), tail-jmp vt[14h])
 *   tail: mov eax,[esi+8] ; lea ecx,[esi+8] ; call [eax+10h] (Unlock)
 *   pop edi ; mov al,1 ; pop esi ; ret
 *
 * sub_00007b70 weak-lock:
 *   push esi ; push edi ; mov edi,ecx ; push -1
 *   mov eax,[edi+8] ; lea ecx,[edi+8] ; call [eax+0Ch]      (Lock(-1))
 *   mov eax,[edi+8] ; lea ecx,[edi+8] ; add eax,10h
 *   cmp word [edi+4],0 ; jne alive
 *   call [eax] ; pop edi ; xor al,al ; pop esi ; ret       (dead: Unlock)
 *   alive: call [eax] ; mov eax,[edi] ; mov ecx,edi ; call [eax+8] (Unlock,
 *   then AddRef) ; pop edi ; mov al,1 ; pop esi ; ret
 *
 * The mutex seam proves what a seam-handled Lock(-1) / Unlock writes (al=1,
 * esp+=8, six dead words, owner/depth/busy transition, census; eax=cs,
 * esp+=4, three dead words, reverse transition, census).  Lock then Unlock
 * on one critical section is a net no-op on it, so this seam stores nothing
 * into the critical section and needs only the Enter predicate (Leave is
 * valid by construction after Enter).  Whenever this seam's predicates
 * pass, both nested mutex seams would have handled in the translated route
 * (their frame/alias/owner predicates are subsets of the ones below), which
 * makes the dead-word tables and the census replay exact. */
enum {
    RC_STRONG_OFFSET = 4U,
    RC_MUTEX_VTABLE_OFFSET = 8U,
    RC_INIT_OFFSET = 0xCU,
    RC_CS_OFFSET = 0x10U,
    RC_OBJECT_SPAN_BYTES = 0x18U,
    RC_CS_SPAN_BYTES = 0x1CU,
    RC_BUSY_OFFSET = 0x18U,
    RC_MUTEX_VT_LOCK_SLOT = 0xCU,
    RC_MUTEX_VT_UNLOCK_SLOT = 0x10U,
    RC_MUTEX_VT_SLOTS_SPAN_BYTES = 8U,      /* [mvt+0xc, mvt+0x14) */
    RC_COUNTER_VT_ADDREF_SLOT = 8U,
    RC_COUNTER_VT_SLOT_SPAN_BYTES = 4U,     /* [cvt+8, cvt+0xc) */
    RC_IAT_SYNC_RVA = ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS,   /* 0x6060f8 */
    RC_IAT_SYNC_SPAN_BYTES = 8U,            /* Leave word, Enter word */
    /* Frozen targets (RVAs; guest_direct_translated_target also accepts the
     * image VA, exactly like the generated direct edges). */
    RC_LOCK_RVA = 0x00562E00U,
    RC_UNLOCK_RVA = 0x00562EC0U,
    RC_ADDREF_RVA = 0x00007B50U,
    /* Return words as the corpus pushes them: RVAs, never image VAs. */
    RC_LOCK_RETURN_RVA = 0x00562E2FU,       /* EnterCS return word */
    RC_UNLOCK_RETURN_RVA = 0x00562EE6U,     /* LeaveCS return word */
    RC_RELEASE_LOCK_RETURN_RVA = 0x00007AFFU,
    RC_RELEASE_UNLOCK_RETURN_RVA = 0x00007B44U,
    RC_ADDREF_LOCK_RETURN_RVA = 0x00007B5FU,
    RC_WEAKLOCK_LOCK_RETURN_RVA = 0x00007B7FU,
    RC_WEAKLOCK_DEAD_UNLOCK_RETURN_RVA = 0x00007B91U,
    RC_WEAKLOCK_ALIVE_UNLOCK_RETURN_RVA = 0x00007B98U,
    RC_WEAKLOCK_ADDREF_RETURN_RVA = 0x00007B9FU,
    /* Bytes the bodies push below the entry ESP (the frame predicate spans
     * this plus the return word the final `ret` pops). */
    RC_FRAME_BYTES = 40U,                   /* AddRef, Release, weak dead */
    RC_FRAME_BYTES_WEAKLOCK = 52U,          /* weak-lock, both paths */
    RC_RETURN_BYTES = 4U,
    /* Body entry census IDs of the elided translated bodies (guest_0166.c /
     * guest_0000.c `guest_coverage_function(...)`). */
    RC_COVERAGE_LOCK = 7968U,
    RC_COVERAGE_UNLOCK = 7969U,
    RC_COVERAGE_ADDREF = 260U,
    /* Receipt cadence: first line at 1000 calls, then every 2^20 calls or
     * (checked every 4096 calls) every >= 10 s of process time. */
    RC_STATS_FIRST = 1000U,
    RC_STATS_PERIOD_MASK = 0xFFFFFU,
    RC_STATS_CLOCK_MASK = 0xFFFU,
    RC_VERIFY_LOG_FIRST = 64U,
    RC_VERIFY_LOG_EVERY = 4096U
};
#define RC_STATS_WALL_US 10000000ULL

/* ---- verbatim predicate copies from host_vita_kage_mutex_seam.c ---------
 * Copies, not a shared header, on purpose: that TU and its test stay
 * untouched and the option-OFF objects keep their byte identity.
 * recomp/test_vita_kage_refcount_seam.py pins the texts against each other. */

/* Object predicate of the inline fast path, verbatim from
 * isaac_vita_sync_inline_cs_address (host_vita_sync_fastpath.h): non-zero
 * when `address` names a boundary-valid, initialized, user-embedded critical
 * section of the current version.  No side effects.  It is a copy, not a
 * shared helper, on purpose: that header is included by guest.c, and the
 * option-OFF guest.c object must stay byte-identical to the base build (an
 * added static inline there renumbers gcc's local CSWTCH symbols).
 * recomp/test_vita_kage_mutex_seam.py pins the two texts against each other. */
static inline int kage_mutex_cs_object(uint32_t address)
{
    /* address != 0, four-byte aligned, and address + 23 does not wrap:
     * the endpoint's vita_sync_cs_boundary predicate in one compare. */
    if ((address & 3U) != 0U || address - 4U > UINT32_MAX - 27U)
        return 0;
    if (ld32(address) != ISAAC_VITA_SYNC_CS_MAGIC)
        return 0;
    if (ld32(address + 4U) != (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE)
        return 0;
    if (ld32(address + 12U) != ISAAC_VITA_SYNC_CS_VERSION)
        return 0;
    return 1;
}

/* The translated frame is fault-free and inline-eligible exactly when the
 * whole [esp - frame, esp + ret) range lies inside the owned stack: every
 * push lands at or above the floor, the import's argument fetch and
 * return-word pop (esp - frame .. esp - frame + 8) are inside, and the final
 * pops/adjust end at or below the ceiling.  Wrap-free because
 * floor < ceiling is checked first. */
static inline int kage_mutex_frame_inside_stack(const CPU *__restrict c,
                                                uint32_t frame_bytes,
                                                uint32_t return_bytes)
{
    uint32_t floor;
    uint32_t ceiling;
    uint32_t capacity;
    uint32_t span = frame_bytes + return_bytes;

    if (c->stack_owner != c)
        return 0;
    floor = c->stack_floor;
    ceiling = c->stack_ceiling;
    if (floor >= ceiling)
        return 0;
    capacity = ceiling - floor;
    if (capacity < span)
        return 0;
    return c->esp - floor - frame_bytes <= capacity - span;
}

/* Non-zero when [a, a + a_len) and [b, b + b_len) share a byte.  Exact for
 * the non-wrapping ranges compared here (a - b lies in (-a_len, b_len) iff
 * the shifted difference is below a_len + b_len - 1), and modular otherwise,
 * which can only reject more.
 *
 * Why it is needed: the translated body reads the object byte/pointer AFTER
 * its first pushes and the critical section AFTER all of them, so an object
 * or CS that lies inside the pushed frame [esp - frame, esp) is overwritten
 * by the call itself (the x86 body does exactly the same to such dead stack
 * memory).  The seam evaluates every predicate before it stores anything, so
 * it must refuse those frames outright or it would complete a Lock/Unlock
 * the translated body faults on or performs on a different object.  No
 * well-formed guest can reach the state (an object below ESP at the call is
 * dead memory); the guard exists so the seam stays fail-closed on hostile
 * frames.  Cost: two compares per helper. */
static inline int kage_mutex_ranges_overlap(uint32_t a, uint32_t a_len,
                                            uint32_t b, uint32_t b_len)
{
    return a - b + a_len - 1U < a_len + b_len - 1U;
}

/* Low-water mark of the translated run.  With the checked synthetic stack
 * (GUEST_GENERATED_STACK_GUARD=1) every push notes its address, so the
 * lowest pushed word wins; with the raw corpus (=0) only the import's
 * isaac_vita_sync_inline_return notes its argument address, inner + 4.  The
 * seam is compiled with the same GUEST_GENERATED_STACK_GUARD as the
 * generated corpus (CMakeLists.txt). */
static inline void kage_mutex_note_low_water(CPU *__restrict c, uint32_t inner)
{
#if GUEST_GENERATED_STACK_GUARD
    if (inner < c->stack_low_water)
        c->stack_low_water = inner;
#endif
    if (inner + 4U < c->stack_low_water)
        c->stack_low_water = inner + 4U;
}

/* The census guest_try_direct_sync_import_call records for a handled inline
 * import (guest.c): one logical call, one sync fast-path hit, the per-import
 * count and coverage of the dense ID, and the host import call the inline
 * return counts. */
static inline void kage_mutex_note_import(uint32_t import_id)
{
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH();
    GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id);
    guest_coverage_import(import_id);
    ++g_host_import_calls;
}

/* The translated site reads the IAT slot and enters the sync fast path only
 * when the word is the loader token (the slot's own RVA or image VA) and
 * the 413-row import table registered.  g_isaac_vita_import_ids_ready is the
 * return value of that registration, which also set guest.c's sync
 * readiness and proved row 60/61 == the two sync slots, so it is the exact
 * proxy for guest_try_direct_sync_import_call's row checks. */
static inline int kage_mutex_sync_slot_ready(uint32_t slot_rva,
                                             uint32_t *slot_word)
{
    uint32_t word;

    if (!g_isaac_vita_import_ids_ready)
        return 0;
    word = ld32((uint32_t)(GUEST_IMAGE_BASE + slot_rva));
    if (!guest_direct_translated_target(word, slot_rva))
        return 0;
    *slot_word = word;
    return 1;
}

/* ---- composite predicate --------------------------------------------- */

typedef struct kage_refcount_state {
    uint32_t esp;           /* E: entry ESP, [E] = the caller's return word */
    uint32_t object;        /* this = ecx */
    uint32_t mvt;           /* [this+8]  mutex vtable */
    uint32_t cs;            /* [this+0x10] critical section */
    uint32_t cvt;           /* [this]  counter vtable (weak-lock alive only) */
    uint32_t strong;        /* [this+4] */
    uint32_t inner;         /* E - frame: the lowest word the run stores */
    uint32_t enter_slot;    /* IAT words as read (published to the sampler) */
    uint32_t leave_slot;
} kage_refcount_state;

/* Every read the translated bodies perform, in the bodies' order, evaluated
 * before any store.  Returns ISAAC_KRS_HANDLED (0) when the whole helper
 * can be completed natively, else the reject reason.
 *
 * Read order is the body's for fault-address parity on a garbage `this`:
 * [this+8] (mvt) and its Lock slot come first (0x7af6/0x7afc, 0x7b56/0x7b5c,
 * 0x7b76/0x7b7c), the adjacent Unlock slot is taken with it, then Lock's
 * own reads: the initialized byte [this+0xc], the CS pointer [this+0x10],
 * the CS words, the busy byte; then the strong count [this+4] and, weak-lock
 * alive only, [this] and its AddRef slot (the body dereferences [this] only
 * on that path).
 *
 * Alias (fail closed): the bodies re-read the count, the vtable slots, the
 * CS pointer and the IAT words AFTER their pushes, after Lock's CS stores
 * (owner/depth/busy) and after their own count store, while this seam reads
 * everything once and stores nothing into the CS.  Any overlap between a
 * writer's range and a later reader's range on either route is refused so
 * both routes agree on every state they both complete:
 *   pushed frame [inner, E)          vs object, CS, mutex slots, IAT words
 *   CS [cs, cs+0x1c)                 vs object, mutex slots, IAT words
 *   count halfword [this+4, this+6)  vs mutex slots, IAT words
 *   weak-lock alive: the same three writers vs the AddRef slot [cvt+8, +4). */
static int kage_refcount_admit(CPU *__restrict c, uint32_t frame_bytes,
                               int weaklock, kage_refcount_state *st)
{
    uint32_t self = c->vita_sync_thread_id;
    uint32_t esp = c->esp;
    uint32_t object = c->ecx;
    uint32_t mvt;
    uint32_t cs;
    uint32_t cvt = 0U;
    uint32_t strong;
    uint32_t owner;
    uint32_t depth;
    uint32_t inner;
    uint32_t iat = (uint32_t)(GUEST_IMAGE_BASE + RC_IAT_SYNC_RVA);
    int alive;

    if (self != g_isaac_vita_sync_inline_owner)
        return ISAAC_KRS_REJECT_LATCH;
    if ((esp & 3U) != 0U)
        return ISAAC_KRS_REJECT_ESP;    /* STRD alignment: mutex seam */
    if (!kage_mutex_frame_inside_stack(c, frame_bytes, RC_RETURN_BYTES))
        return ISAAC_KRS_REJECT_FRAME;
    /* Both IAT words must be loader tokens (each handled helper performs one
     * Enter and one Leave).  Stricter than the translated route only in the
     * mixed case (Enter token, Leave not): there the body's Unlock takes the
     * generic guest_call route, a rejection-only state for the oracle anyway,
     * so no terminating state is over-rejected. */
    if (!kage_mutex_sync_slot_ready(ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
                                    &st->enter_slot) ||
        !kage_mutex_sync_slot_ready(ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS,
                                    &st->leave_slot))
        return ISAAC_KRS_REJECT_SLOT;   /* generic guest_call: translated */
    mvt = ld32(object + RC_MUTEX_VTABLE_OFFSET);
    if (!guest_direct_translated_target(ld32(mvt + RC_MUTEX_VT_LOCK_SLOT),
                                        RC_LOCK_RVA) ||
        !guest_direct_translated_target(ld32(mvt + RC_MUTEX_VT_UNLOCK_SLOT),
                                        RC_UNLOCK_RVA))
        return ISAAC_KRS_REJECT_VT;     /* unpredicted target: translated */
    if (ld8(object + RC_INIT_OFFSET) == 0U)
        return ISAAC_KRS_REJECT_INIT;   /* logging path: translated body */
    cs = ld32(object + RC_CS_OFFSET);
    if (!kage_mutex_cs_object(cs))
        return ISAAC_KRS_REJECT_CS;
    owner = ld32(cs + 16U);
    depth = ld32(cs + 20U);
    if (owner == self) {
        if (depth == 0U || depth == UINT32_MAX)
            return ISAAC_KRS_REJECT_OWNER;
    } else if (owner != 0U || depth != 0U) {
        return ISAAC_KRS_REJECT_OWNER;  /* contended: translated body waits */
    }
    /* Read only after the object proved to be a registered critical
     * section: the translated body reaches this byte after EnterCS. */
    if (ld8(cs + RC_BUSY_OFFSET) != 0U)
        return ISAAC_KRS_REJECT_BUSY;   /* Sleep loop: translated body */
    strong = ld16(object + RC_STRONG_OFFSET);
    alive = weaklock && strong != 0U;
    if (alive) {
        cvt = ld32(object);
        if (!guest_direct_translated_target(
                ld32(cvt + RC_COUNTER_VT_ADDREF_SLOT), RC_ADDREF_RVA))
            return ISAAC_KRS_REJECT_VT;
    }
    inner = esp - frame_bytes;
    if (kage_mutex_ranges_overlap(inner, frame_bytes,
                                  object, RC_OBJECT_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(inner, frame_bytes,
                                  cs, RC_CS_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(inner, frame_bytes,
                                  mvt + RC_MUTEX_VT_LOCK_SLOT,
                                  RC_MUTEX_VT_SLOTS_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(inner, frame_bytes,
                                  iat, RC_IAT_SYNC_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(cs, RC_CS_SPAN_BYTES,
                                  object, RC_OBJECT_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(cs, RC_CS_SPAN_BYTES,
                                  mvt + RC_MUTEX_VT_LOCK_SLOT,
                                  RC_MUTEX_VT_SLOTS_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(cs, RC_CS_SPAN_BYTES,
                                  iat, RC_IAT_SYNC_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(object + RC_STRONG_OFFSET, 2U,
                                  mvt + RC_MUTEX_VT_LOCK_SLOT,
                                  RC_MUTEX_VT_SLOTS_SPAN_BYTES) ||
        kage_mutex_ranges_overlap(object + RC_STRONG_OFFSET, 2U,
                                  iat, RC_IAT_SYNC_SPAN_BYTES))
        return ISAAC_KRS_REJECT_ALIAS;
    if (alive &&
        (kage_mutex_ranges_overlap(inner, frame_bytes,
                                   cvt + RC_COUNTER_VT_ADDREF_SLOT,
                                   RC_COUNTER_VT_SLOT_SPAN_BYTES) ||
         kage_mutex_ranges_overlap(cs, RC_CS_SPAN_BYTES,
                                   cvt + RC_COUNTER_VT_ADDREF_SLOT,
                                   RC_COUNTER_VT_SLOT_SPAN_BYTES) ||
         kage_mutex_ranges_overlap(object + RC_STRONG_OFFSET, 2U,
                                   cvt + RC_COUNTER_VT_ADDREF_SLOT,
                                   RC_COUNTER_VT_SLOT_SPAN_BYTES)))
        return ISAAC_KRS_REJECT_ALIAS;
    st->esp = esp;
    st->object = object;
    st->mvt = mvt;
    st->cs = cs;
    st->cvt = cvt;
    st->strong = strong;
    st->inner = inner;
    return ISAAC_KRS_HANDLED;
}

/* ---- receipts ---------------------------------------------------------- */

static isaac_vita_kage_refcount_seam_stats s_stats;
static int s_banner_logged;
static uint32_t s_reason_logged;        /* bit per reason: first occurrence */
#if defined(__vita__)
static uint64_t s_stats_last_us;
#endif

static const char *const s_fn_names[ISAAC_KRS_FN_COUNT] = {
    "AddRef", "weak-lock", "Release"
};
static const char *const s_reason_names[ISAAC_KRS_REASON_COUNT] = {
    "handled", "latch", "esp", "frame", "slot", "init", "vt", "cs", "alias",
    "owner", "busy", "zero", "last"
};

const isaac_vita_kage_refcount_seam_stats *
isaac_vita_kage_refcount_seam_stats_get(void)
{
    return &s_stats;
}

void isaac_vita_kage_refcount_seam_stats_reset(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_reason_logged = 0U;
}

void isaac_vita_kage_refcount_seam_stats_log(void)
{
    isaac_vita_log(
        "[isaac-kage] refcount seam: addref=%u/%u weaklock=%u/%u"
        "(alive=%u,dead=%u) release=%u/%u rejected(latch,esp,frame,slot,init,"
        "vt,cs,alias,owner,busy,zero,last)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u"
        " imports=%u verify=%u/%u skipped=%u",
        (unsigned)s_stats.handled[ISAAC_KRS_FN_ADDREF],
        (unsigned)s_stats.calls[ISAAC_KRS_FN_ADDREF],
        (unsigned)s_stats.handled[ISAAC_KRS_FN_WEAKLOCK],
        (unsigned)s_stats.calls[ISAAC_KRS_FN_WEAKLOCK],
        (unsigned)s_stats.weak_alive, (unsigned)s_stats.weak_dead,
        (unsigned)s_stats.handled[ISAAC_KRS_FN_RELEASE],
        (unsigned)s_stats.calls[ISAAC_KRS_FN_RELEASE],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_LATCH],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_ESP],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_FRAME],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_SLOT],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_INIT],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_VT],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_CS],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_ALIAS],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_OWNER],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_BUSY],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_ZERO],
        (unsigned)s_stats.reasons[ISAAC_KRS_REJECT_LAST],
        (unsigned)s_stats.imports_replayed,
        (unsigned)s_stats.verify_mismatches, (unsigned)s_stats.verify_runs,
        (unsigned)s_stats.verify_skipped_fault);
}

static void kage_refcount_log_banner(void)
{
    if (s_banner_logged)
        return;
    s_banner_logged = 1;
    isaac_vita_log(
        "[isaac-kage] refcount seam: roots=7af0,7b50,7b70 lock=%08x "
        "unlock=%08x addref=%08x slots=%08x,%08x frame=%u/%u verify=%d "
        "build=%s",
        (unsigned)RC_LOCK_RVA, (unsigned)RC_UNLOCK_RVA,
        (unsigned)RC_ADDREF_RVA,
        (unsigned)ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
        (unsigned)ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS,
        (unsigned)RC_FRAME_BYTES, (unsigned)RC_FRAME_BYTES_WEAKLOCK,
#if defined(ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY)
        1,
#else
        0,
#endif
        ISAAC_VITA_KAGE_REFCOUNT_SEAM_BUILD_ID);
}

/* One call entered a helper: banner on the first, the stats line at 1000
 * calls, then every 2^20 calls or every >= 10 s of process time (the clock
 * is read once per 4096 calls; a quiet scene therefore prints late, never
 * more often). */
static void kage_refcount_note_call(unsigned fn)
{
    uint32_t total;

    ++s_stats.calls[fn];
    if (!s_banner_logged)
        kage_refcount_log_banner();
    total = s_stats.calls[0] + s_stats.calls[1] + s_stats.calls[2];
    if (total == RC_STATS_FIRST || (total & RC_STATS_PERIOD_MASK) == 0U) {
#if defined(__vita__)
        s_stats_last_us = sceKernelGetProcessTimeWide();
#endif
        isaac_vita_kage_refcount_seam_stats_log();
        return;
    }
#if defined(__vita__)
    if ((total & RC_STATS_CLOCK_MASK) == 0U) {
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now - s_stats_last_us >= RC_STATS_WALL_US) {
            s_stats_last_us = now;
            isaac_vita_kage_refcount_seam_stats_log();
        }
    }
#endif
}

static int kage_refcount_reject(unsigned fn, int reason)
{
    ++s_stats.reasons[reason];
    s_stats.last_reason = (uint32_t)reason;
    if ((s_reason_logged & (1U << reason)) == 0U) {
        s_reason_logged |= 1U << reason;
        isaac_vita_log("[isaac-kage] refcount seam fallback: fn=%s reason=%s "
                       "call=%u (translated body used)",
                       s_fn_names[fn], s_reason_names[reason],
                       (unsigned)s_stats.calls[fn]);
    }
    return 0;
}

/* ---- census replay ------------------------------------------------------
 * Identity with the translated route where the mutex seam handled every
 * Lock/Unlock.
 *
 * The elided direct edges (render_vita_refcount_direct_edge: NOTE_CALL +
 * NOTE_LOOKUP + a fenced lookup_cache_hit) record NOTHING on the
 * translated route: ISAAC_VITA_PHASE_PROFILE never reaches a generated
 * unit (recomp/vita/CMakeLists.txt applies it to guest.c and the runtime
 * owners only -- the python test pins that list), so in guest_0000.c the
 * two macros are ((void)0) and the hook call is compiled out.  Replaying
 * them here would inflate ph120.c g(c)/g(l) and the lookup-cache record
 * by two per handled helper and turn every VERIFY compare into a
 * MISMATCH; this seam therefore records nothing for the edges either.
 *
 * What the translated route does record, and this seam replays: per
 * elided Lock/Unlock body its entry hook (guest_coverage_function: the
 * ISAAC_VITA_PROFILE_FUNCTION_ENTRIES increment, target-wide, plus the
 * coverage byte, which stays a NULL-pointer no-op on Vita) and the mutex
 * seam's import census (kage_mutex_note_import: guest.c's NOTE_CALL +
 * NOTE_SYNC_FASTPATH + NOTE_IMPORT + coverage + host import count -- the
 * guest.c route IS compiled with the profile defines, and so is this TU);
 * per elided AddRef body (weak-lock alive) its entry hook.
 *
 * Sampler word: the translated route publishes the IAT slot only for the
 * duration of the seam census (host_vita_kage_mutex_seam.c), so the word
 * names "ext" around the 61/60 notes and the enclosing target otherwise;
 * this helper pushes no guest return word, so the sampler's stack scan
 * sees the caller's call site exactly as during the bodies' prologues. */
#if defined(ISAAC_VITA_GUEST_SAMPLER)
#define RC_SAMPLER_PUBLISH(word) (g_kage_guest_last_indirect_target = (word))
#else
#define RC_SAMPLER_PUBLISH(word) ((void)(word))
#endif

/* Lock body -> EnterCS ; Unlock body -> LeaveCS (the edges into the two
 * bodies record nothing, see above). */
static inline void kage_refcount_replay_pair(const kage_refcount_state *st)
{
    guest_coverage_function(RC_COVERAGE_LOCK);
    RC_SAMPLER_PUBLISH(st->enter_slot);
    kage_mutex_note_import(ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS);
    guest_coverage_function(RC_COVERAGE_UNLOCK);
    RC_SAMPLER_PUBLISH(st->leave_slot);
    kage_mutex_note_import(ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS);
    s_stats.imports_replayed += 2U;
}

/* ---- VERIFY -------------------------------------------------------------
 * There is no __real for a body-top seam: after a handled call the
 * translated body is rerun from the entry snapshot with a re-entrancy
 * bypass (nested *_try calls return 0 while it is set, so weak-lock's inner
 * sub_00007b50 runs translated too) and the two resulting states are
 * compared field by field; the rerun's state ships.  The rerun executes
 * under a private run scope so a guest_fault/guest_exit inside it clears
 * the bypass, is counted, and is then re-raised to the real run scope
 * exactly as guest_fault would have done. */
#if defined(ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY)
extern void sub_00007af0(CPU *__restrict c);
extern void sub_00007b50(CPU *__restrict c);
extern void sub_00007b70(CPU *__restrict c);

/* Layout of guest.c's private `guest_run_scope` (one jmp_buf); guest_fault
 * longjmps to ((guest_run_scope *)c->run_scope)->env with value 1. */
typedef struct kage_refcount_run_scope {
    jmp_buf env;
} kage_refcount_run_scope;

typedef struct kage_refcount_snapshot {
    uint32_t r[8];
    uint32_t stack_low_water;
    guest_flags fl;
    uint8_t frame[RC_FRAME_BYTES_WEAKLOCK + RC_RETURN_BYTES];
    uint8_t object[RC_OBJECT_SPAN_BYTES];
    uint8_t cs[RC_CS_SPAN_BYTES];
    unsigned host_import_calls;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    GuestPhaseProfileCounters counters;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    uint32_t import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
#endif
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    uint32_t function_entries;
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_word;
#endif
} kage_refcount_snapshot;

static int s_verify_bypass;
static kage_refcount_snapshot s_verify_entry;
static kage_refcount_snapshot s_verify_seam;

static void kage_refcount_snapshot_take(kage_refcount_snapshot *s, CPU *c,
                                        const kage_refcount_state *st,
                                        uint32_t frame_bytes)
{
    uint32_t i;

    memcpy(s->r, c->r, sizeof s->r);
    s->stack_low_water = c->stack_low_water;
    s->fl = c->fl;
    memset(s->frame, 0, sizeof s->frame);
    for (i = 0U; i < frame_bytes + RC_RETURN_BYTES; ++i)
        s->frame[i] = ld8(st->esp - frame_bytes + i);
    for (i = 0U; i < RC_OBJECT_SPAN_BYTES; ++i)
        s->object[i] = ld8(st->object + i);
    for (i = 0U; i < RC_CS_SPAN_BYTES; ++i)
        s->cs[i] = ld8(st->cs + i);
    s->host_import_calls = g_host_import_calls;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    s->counters = g_guest_phase_profile_counters;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    memcpy(s->import_calls, g_guest_phase_profile_import_calls,
           sizeof s->import_calls);
#endif
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    s->function_entries = g_guest_function_entries;
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    s->sampler_word = g_kage_guest_last_indirect_target;
#endif
}

static void kage_refcount_snapshot_restore(const kage_refcount_snapshot *s,
                                           CPU *c,
                                           const kage_refcount_state *st,
                                           uint32_t frame_bytes)
{
    uint32_t i;

    memcpy(c->r, s->r, sizeof s->r);
    c->stack_low_water = s->stack_low_water;
    c->fl = s->fl;
    for (i = 0U; i < frame_bytes + RC_RETURN_BYTES; ++i)
        st8(st->esp - frame_bytes + i, s->frame[i]);
    for (i = 0U; i < RC_OBJECT_SPAN_BYTES; ++i)
        st8(st->object + i, s->object[i]);
    for (i = 0U; i < RC_CS_SPAN_BYTES; ++i)
        st8(st->cs + i, s->cs[i]);
    g_host_import_calls = s->host_import_calls;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    g_guest_phase_profile_counters = s->counters;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    memcpy(g_guest_phase_profile_import_calls, s->import_calls,
           sizeof s->import_calls);
#endif
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    g_guest_function_entries = s->function_entries;
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = s->sampler_word;
#endif
}

static int kage_refcount_first_diff(const void *want, const void *got,
                                    uint32_t bytes, uint32_t *offset)
{
    const uint8_t *w = (const uint8_t *)want;
    const uint8_t *g = (const uint8_t *)got;
    uint32_t i;

    for (i = 0U; i < bytes; ++i) {
        if (w[i] != g[i]) {
            *offset = i;
            return 1;
        }
    }
    return 0;
}

/* Compare the seam's result (want) with the rerun's (got).  The rerun's own
 * entry hook was already counted by the body that called the seam, so
 * ISAAC_VITA_PROFILE_FUNCTION_ENTRIES is expected one higher (documented:
 * VERIFY builds double-count ent for handled calls and are not census
 * gates); every other counter, the phase-profile struct included, must
 * be equal.  Logs the first mismatching field. */
static int kage_refcount_verify_compare(unsigned fn,
                                        const kage_refcount_snapshot *want,
                                        const kage_refcount_snapshot *got,
                                        int loud)
{
    const char *field = NULL;
    uint32_t offset = 0U;
    uint32_t w = 0U;
    uint32_t g = 0U;
    uint32_t i;
    static const char *const reg_names[8] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
    };

    for (i = 0U; i < 8U && field == NULL; ++i) {
        if (want->r[i] != got->r[i]) {
            field = reg_names[i];
            w = want->r[i];
            g = got->r[i];
        }
    }
    if (field == NULL && want->stack_low_water != got->stack_low_water) {
        field = "stack_low_water";
        w = want->stack_low_water;
        g = got->stack_low_water;
    }
    if (field == NULL &&
        kage_refcount_first_diff(&want->fl, &got->fl, sizeof want->fl,
                                 &offset)) {
        field = "fl";
        w = ((const uint8_t *)&want->fl)[offset];
        g = ((const uint8_t *)&got->fl)[offset];
    }
    if (field == NULL &&
        kage_refcount_first_diff(want->frame, got->frame, sizeof want->frame,
                                 &offset)) {
        field = "frame";
        w = want->frame[offset];
        g = got->frame[offset];
    }
    if (field == NULL &&
        kage_refcount_first_diff(want->object, got->object,
                                 sizeof want->object, &offset)) {
        field = "object";
        w = want->object[offset];
        g = got->object[offset];
    }
    if (field == NULL &&
        kage_refcount_first_diff(want->cs, got->cs, sizeof want->cs,
                                 &offset)) {
        field = "cs";
        w = want->cs[offset];
        g = got->cs[offset];
    }
    if (field == NULL && want->host_import_calls != got->host_import_calls) {
        field = "host_import_calls";
        w = want->host_import_calls;
        g = got->host_import_calls;
    }
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (field == NULL &&
        kage_refcount_first_diff(&want->counters, &got->counters,
                                 sizeof want->counters, &offset)) {
        field = "phase_profile_counters";
        w = ((const uint8_t *)&want->counters)[offset];
        g = ((const uint8_t *)&got->counters)[offset];
    }
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    if (field == NULL &&
        kage_refcount_first_diff(want->import_calls, got->import_calls,
                                 sizeof want->import_calls, &offset)) {
        field = "import_calls";
        offset /= 4U;
        w = want->import_calls[offset];
        g = got->import_calls[offset];
    }
#endif
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    if (field == NULL &&
        want->function_entries + 1U != got->function_entries) {
        field = "function_entries(+1)";
        w = want->function_entries + 1U;
        g = got->function_entries;
    }
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    if (field == NULL && want->sampler_word != got->sampler_word) {
        field = "sampler_word";
        w = want->sampler_word;
        g = got->sampler_word;
    }
#endif
    if (field == NULL) {
        /* MATCH receipts only on the loud runs (the first 8, then every
         * 4096th): a healthy build must not log per handled call. */
        if (loud)
            isaac_vita_log("[isaac-kage] refcount seam VERIFY: fn=%s n=%u "
                           "result=MATCH mismatches=%u",
                           s_fn_names[fn], (unsigned)s_stats.verify_runs,
                           (unsigned)s_stats.verify_mismatches);
        return 1;
    }
    /* MISMATCH receipts: the first RC_VERIFY_LOG_FIRST (the caller
     * increments the count after this returns) and every loud run. */
    if (loud || s_stats.verify_mismatches < RC_VERIFY_LOG_FIRST)
        isaac_vita_log("[isaac-kage] refcount seam VERIFY: fn=%s n=%u "
                       "result=MISMATCH field=%s+%u want=%08x got=%08x "
                       "mismatches=%u",
                       s_fn_names[fn], (unsigned)s_stats.verify_runs, field,
                       (unsigned)offset, (unsigned)w, (unsigned)g,
                       (unsigned)s_stats.verify_mismatches);
    return 0;
}

/* Run the translated body under a private run scope with the bypass set.
 * Returns 0 when the body returned, 1 when it left through guest_fault /
 * guest_exit (which already recorded c->fault / c->exit_api / stop_kind).
 * The bypass is cleared and the real run scope restored on BOTH exits. */
static int kage_refcount_verify_rerun(CPU *__restrict c,
                                      void (*body)(CPU *__restrict))
{
    kage_refcount_run_scope scope;
    void *volatile outer = c->run_scope;
    volatile int faulted = 0;

    c->run_scope = &scope;
    s_verify_bypass = 1;
    if (setjmp(scope.env) == 0)
        body(c);
    else
        faulted = 1;
    s_verify_bypass = 0;
    c->run_scope = outer;
    return faulted;
}

static void kage_refcount_verify(CPU *__restrict c, unsigned fn,
                                 void (*body)(CPU *__restrict),
                                 const kage_refcount_state *st,
                                 uint32_t frame_bytes)
{
    int loud;

    kage_refcount_snapshot_take(&s_verify_seam, c, st, frame_bytes);
    kage_refcount_snapshot_restore(&s_verify_entry, c, st, frame_bytes);
    if (kage_refcount_verify_rerun(c, body)) {
        /* Count the skipped comparison, then re-raise to the real run
         * scope exactly as guest_fault / guest_exit would have. */
        ++s_stats.verify_skipped_fault;
        isaac_vita_log("[isaac-kage] refcount seam VERIFY: fn=%s n=%u "
                       "verify skipped (fault) what=%s addr=%08x skipped=%u",
                       s_fn_names[fn], (unsigned)s_stats.verify_runs,
                       c->fault ? c->fault : (c->exit_api ? c->exit_api
                                                          : "<none>"),
                       (unsigned)c->fault_addr,
                       (unsigned)s_stats.verify_skipped_fault);
        if (c->run_scope)
            longjmp(((kage_refcount_run_scope *)c->run_scope)->env, 1);
        abort();
    }
    ++s_stats.verify_runs;
    kage_refcount_snapshot_take(&s_verify_entry, c, st, frame_bytes);
    loud = s_stats.verify_runs <= 8U ||
           (s_stats.verify_runs % RC_VERIFY_LOG_EVERY) == 0U;
    if (!kage_refcount_verify_compare(fn, &s_verify_seam, &s_verify_entry,
                                      loud)) {
        ++s_stats.verify_mismatches;
    }
}
#define RC_VERIFY_ENTER(c, st, frame) \
    kage_refcount_snapshot_take(&s_verify_entry, (c), (st), (frame))
#define RC_VERIFY_LEAVE(c, fn, body, st, frame) \
    kage_refcount_verify((c), (fn), (body), (st), (frame))
#define RC_VERIFY_BYPASSED() (s_verify_bypass)
#else
#define RC_VERIFY_ENTER(c, st, frame) ((void)0)
#define RC_VERIFY_LEAVE(c, fn, body, st, frame) ((void)0)
#define RC_VERIFY_BYPASSED() 0
#endif

/* ---- the three helpers ------------------------------------------------- */

/* Common exit state of every handled path: esp = E + 4 (the final `ret`),
 * ecx = this + 8 (the last writer is `lea ecx,[this+8]` in every path;
 * Unlock never writes ecx); edx/ebx/ebp/esi/edi are the entry values (the
 * bodies restore them); nothing is written to c->fl (see the #error above);
 * eax is set by the caller (path-specific). */
static inline void kage_refcount_exit(CPU *__restrict c,
                                      const kage_refcount_state *st,
                                      uint32_t eax, uint32_t low_water_inner)
{
    kage_mutex_note_low_water(c, low_water_inner);
    c->eax = eax;
    c->ecx = st->object + RC_MUTEX_VTABLE_OFFSET;
    c->esp = st->esp + RC_RETURN_BYTES;
}

int isaac_vita_kage_refcount_addref_try(CPU *__restrict c)
{
    kage_refcount_state st;
    uint32_t esp;
    uint32_t cs;
    int reason;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target;
#endif

    if (RC_VERIFY_BYPASSED())
        return 0;
    kage_refcount_note_call(ISAAC_KRS_FN_ADDREF);
    reason = kage_refcount_admit(c, RC_FRAME_BYTES, 0, &st);
    if (reason != ISAAC_KRS_HANDLED)
        return kage_refcount_reject(ISAAC_KRS_FN_ADDREF, reason);
    RC_VERIFY_ENTER(c, &st, RC_FRAME_BYTES);
    esp = st.esp;
    cs = st.cs;

    /* Commit.  Dead words as the x86 leaves them (last writer):
     *   [E-4]  esi0        push esi; Unlock's push esi (esi restored)
     *   [E-8]  cs          Unlock's push eax (over edi0)
     *   [E-12] 0x562ee6    LeaveCS return word (over -1)
     *   [E-16] 0x7b5f      Lock call return word
     *   [E-20] ebp0        Lock push ebp
     *   [E-24] ebx0        Lock push ebx
     *   [E-28] this        Lock push esi (esi = this)
     *   [E-32] edi0        Lock push edi
     *   [E-36] cs          Lock push esi (cs)
     *   [E-40] 0x562e2f    EnterCS return word */
    st32(esp - 4U, c->esi);
    st32(esp - 8U, cs);
    st32(esp - 12U, RC_UNLOCK_RETURN_RVA);
    st32(esp - 16U, RC_ADDREF_LOCK_RETURN_RVA);
    st32(esp - 20U, c->ebp);
    st32(esp - 24U, c->ebx);
    st32(esp - 28U, st.object);
    st32(esp - 32U, c->edi);
    st32(esp - 36U, cs);
    st32(esp - 40U, RC_LOCK_RETURN_RVA);
    /* inc word [this+4] (wraps like the x86). */
    st16(st.object + RC_STRONG_OFFSET, (uint16_t)(st.strong + 1U));
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
#endif
    kage_refcount_replay_pair(&st);       /* 0x7b5c Lock, 0x7b6b Unlock */
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
    /* Unlock's `mov eax,[esi+8]` is the last eax writer (tail jmp). */
    kage_refcount_exit(c, &st, cs, esp - RC_FRAME_BYTES);
    ++s_stats.handled[ISAAC_KRS_FN_ADDREF];
    s_stats.last_reason = ISAAC_KRS_HANDLED;
    RC_VERIFY_LEAVE(c, ISAAC_KRS_FN_ADDREF, sub_00007b50, &st,
                    RC_FRAME_BYTES);
    return 1;
}

int isaac_vita_kage_refcount_release_try(CPU *__restrict c)
{
    kage_refcount_state st;
    uint32_t esp;
    uint32_t cs;
    int reason;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target;
#endif

    if (RC_VERIFY_BYPASSED())
        return 0;
    kage_refcount_note_call(ISAAC_KRS_FN_RELEASE);
    reason = kage_refcount_admit(c, RC_FRAME_BYTES, 0, &st);
    if (reason != ISAAC_KRS_HANDLED)
        return kage_refcount_reject(ISAAC_KRS_FN_RELEASE, reason);
    /* strong == 1 is the destroy path (Unlock, [this+0x14]->vt[0x60](1),
     * tail-jmp vt[0x14]); strong == 0 is a cannot-happen no-op state.  Both
     * run translated. */
    if (st.strong < 2U)
        return kage_refcount_reject(ISAAC_KRS_FN_RELEASE,
                                    st.strong == 0U ? ISAAC_KRS_REJECT_ZERO
                                                    : ISAAC_KRS_REJECT_LAST);
    RC_VERIFY_ENTER(c, &st, RC_FRAME_BYTES);
    esp = st.esp;
    cs = st.cs;

    /* Commit.  Dead words (last writer):
     *   [E-4]  esi0        push esi
     *   [E-8]  edi0        push edi
     *   [E-12] 0x7b44      Unlock call return word (over -1)
     *   [E-16] this        Unlock push esi (esi = this; over 0x7aff)
     *   [E-20] cs          Unlock push eax (over ebp0)
     *   [E-24] 0x562ee6    LeaveCS return word (over ebx0)
     *   [E-28] this        Lock push esi
     *   [E-32] edi0        Lock push edi
     *   [E-36] cs          Lock push esi (cs)
     *   [E-40] 0x562e2f    EnterCS return word */
    st32(esp - 4U, c->esi);
    st32(esp - 8U, c->edi);
    st32(esp - 12U, RC_RELEASE_UNLOCK_RETURN_RVA);
    st32(esp - 16U, st.object);
    st32(esp - 20U, cs);
    st32(esp - 24U, RC_UNLOCK_RETURN_RVA);
    st32(esp - 28U, st.object);
    st32(esp - 32U, c->edi);
    st32(esp - 36U, cs);
    st32(esp - 40U, RC_LOCK_RETURN_RVA);
    /* dec eax; mov word [this+4],ax  (strong >= 2, so the result != 0). */
    st16(st.object + RC_STRONG_OFFSET, (uint16_t)(st.strong - 1U));
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
#endif
    kage_refcount_replay_pair(&st);       /* 0x7afc Lock, 0x7b41 Unlock */
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
    /* Unlock left eax = cs; `mov al,1`. */
    kage_refcount_exit(c, &st, (cs & 0xFFFFFF00U) | 1U,
                       esp - RC_FRAME_BYTES);
    ++s_stats.handled[ISAAC_KRS_FN_RELEASE];
    s_stats.last_reason = ISAAC_KRS_HANDLED;
    RC_VERIFY_LEAVE(c, ISAAC_KRS_FN_RELEASE, sub_00007af0, &st,
                    RC_FRAME_BYTES);
    return 1;
}

int isaac_vita_kage_refcount_weaklock_try(CPU *__restrict c)
{
    kage_refcount_state st;
    uint32_t esp;
    uint32_t cs;
    uint32_t esi0;
    uint32_t edi0;
    int reason;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target;
#endif

    if (RC_VERIFY_BYPASSED())
        return 0;
    kage_refcount_note_call(ISAAC_KRS_FN_WEAKLOCK);
    /* The frame predicate uses the alive path's 52 bytes on both paths
     * (over-rejects only frames whose floor lies in (E-52, E-40] on the dead
     * path; the oracle's acceptance predicate is defined the same way). */
    reason = kage_refcount_admit(c, RC_FRAME_BYTES_WEAKLOCK, 1, &st);
    if (reason != ISAAC_KRS_HANDLED)
        return kage_refcount_reject(ISAAC_KRS_FN_WEAKLOCK, reason);
    RC_VERIFY_ENTER(c, &st, RC_FRAME_BYTES_WEAKLOCK);
    esp = st.esp;
    cs = st.cs;
    esi0 = c->esi;
    edi0 = c->edi;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
#endif
    if (st.strong == 0U) {
        /* Dead (weak pointer's target is gone): no count change.
         *   [E-4]  esi0        push esi
         *   [E-8]  edi0        push edi
         *   [E-12] 0x7b91      Unlock call return word (over -1)
         *   [E-16] esi0        Unlock push esi (7b70 keeps this in edi)
         *   [E-20] cs          Unlock push eax (over ebp0)
         *   [E-24] 0x562ee6    LeaveCS return word (over ebx0)
         *   [E-28] esi0        Lock push esi
         *   [E-32] this        Lock push edi (edi = this)
         *   [E-36] cs          Lock push esi (cs)
         *   [E-40] 0x562e2f    EnterCS return word
         * The deepest push is E-40, so the low-water note is E-40 (guard) /
         * E-36 (raw), not the 52-byte frame. */
        st32(esp - 4U, esi0);
        st32(esp - 8U, edi0);
        st32(esp - 12U, RC_WEAKLOCK_DEAD_UNLOCK_RETURN_RVA);
        st32(esp - 16U, esi0);
        st32(esp - 20U, cs);
        st32(esp - 24U, RC_UNLOCK_RETURN_RVA);
        st32(esp - 28U, esi0);
        st32(esp - 32U, st.object);
        st32(esp - 36U, cs);
        st32(esp - 40U, RC_LOCK_RETURN_RVA);
        kage_refcount_replay_pair(&st);   /* 0x7b7c Lock, 0x7b8f Unlock */
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
        /* Unlock left eax = cs; `xor al,al`. */
        kage_refcount_exit(c, &st, cs & 0xFFFFFF00U, esp - RC_FRAME_BYTES);
        ++s_stats.weak_dead;
    } else {
        /* Alive: Unlock, then AddRef (its own Lock/Unlock pair).
         *   [E-4]  esi0        push esi
         *   [E-8]  edi0        push edi
         *   [E-12] 0x7b9f      AddRef call return word (over 0x7b98, -1)
         *   [E-16] esi0        AddRef's tail Unlock push esi
         *   [E-20] cs          AddRef's tail Unlock push eax (over this)
         *   [E-24] 0x562ee6    AddRef's tail LeaveCS return word (over -1)
         *   [E-28] 0x7b5f      AddRef's Lock call return word
         *   [E-32] ebp0        inner Lock push ebp
         *   [E-36] ebx0        inner Lock push ebx
         *   [E-40] this        inner Lock push esi (over 0x562e2f)
         *   [E-44] this        inner Lock push edi (AddRef's edi = this)
         *   [E-48] cs          inner Lock push esi (cs)
         *   [E-52] 0x562e2f    inner EnterCS return word */
        st32(esp - 4U, esi0);
        st32(esp - 8U, edi0);
        st32(esp - 12U, RC_WEAKLOCK_ADDREF_RETURN_RVA);
        st32(esp - 16U, esi0);
        st32(esp - 20U, cs);
        st32(esp - 24U, RC_UNLOCK_RETURN_RVA);
        st32(esp - 28U, RC_ADDREF_LOCK_RETURN_RVA);
        st32(esp - 32U, c->ebp);
        st32(esp - 36U, c->ebx);
        st32(esp - 40U, st.object);
        st32(esp - 44U, st.object);
        st32(esp - 48U, cs);
        st32(esp - 52U, RC_LOCK_RETURN_RVA);
        /* AddRef's inc word [this+4] (wraps like the x86). */
        st16(st.object + RC_STRONG_OFFSET, (uint16_t)(st.strong + 1U));
        kage_refcount_replay_pair(&st);   /* 0x7b7c Lock, 0x7b96 Unlock */
        guest_coverage_function(RC_COVERAGE_ADDREF);  /* 0x7b9c AddRef */
        kage_refcount_replay_pair(&st);   /* 0x7b5c Lock, 0x7b6b Unlock */
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
        /* AddRef's tail Unlock left eax = cs; `mov al,1`. */
        kage_refcount_exit(c, &st, (cs & 0xFFFFFF00U) | 1U,
                           esp - RC_FRAME_BYTES_WEAKLOCK);
        ++s_stats.weak_alive;
    }
    ++s_stats.handled[ISAAC_KRS_FN_WEAKLOCK];
    s_stats.last_reason = ISAAC_KRS_HANDLED;
    RC_VERIFY_LEAVE(c, ISAAC_KRS_FN_WEAKLOCK, sub_00007b70, &st,
                    RC_FRAME_BYTES_WEAKLOCK);
    return 1;
}
