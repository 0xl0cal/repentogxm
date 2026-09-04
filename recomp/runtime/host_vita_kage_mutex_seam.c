#include "host_vita_kage_mutex_seam.h"

#include <stdint.h>

#include "host_vita_import_id.h"
#include "host_vita_sync_fastpath.h"
#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif

#if !defined(ISAAC_VITA_KAGE_MUTEX_SEAM) || !ISAAC_VITA_KAGE_MUTEX_SEAM
#error The KAGE mutex seam must only be compiled when enabled
#endif
#if !defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
#error The KAGE mutex seam replays the inline critical-section fast path
#endif
#if !GUEST_STACK_REQUIRED
#error The KAGE mutex seam mirrors production: GUEST_STACK_REQUIRED=1
#endif

/* The frozen x86 bodies this file replays (recomp/gen_all.py
 * VITA_KAGE_MUTEX_SEAM_SPECS pins every instruction named here).
 *
 * sub_00562e00 Mutex::Lock(timeout), fast path for timeout == -1:
 *   push ebp; mov ebp,esp; push ebx; mov ebx,ecx; push esi; push edi
 *   cmp byte [ebx+4],0 ; jne ok            (else log 'not initialized', fall
 *                                           through to the same lock code)
 *   mov edi,[ebp+8]    ; cmp edi,-1 ; jne timed
 *   mov esi,[ebx+8]    ; push esi ; call [EnterCriticalSection]
 *   cmp byte [esi+18h],0 ; je set          (else Sleep(1000) until clear)
 *   mov byte [esi+18h],1
 *   pop edi ; pop esi ; mov al,1 ; pop ebx ; pop ebp ; ret 4
 *
 * sub_00562ec0 Mutex::Unlock():
 *   push esi ; mov esi,ecx
 *   cmp byte [esi+4],0 ; jne ok            (same logging path)
 *   mov eax,[esi+8] ; push eax ; mov byte [eax+18h],0
 *   call [LeaveCriticalSection]
 *   pop esi ; ret
 *
 * Architectural result of the replayed paths: Lock writes al (upper 24 bits
 * of eax are the caller's, the sync imports never touch eax), esp += 8 and
 * restores ebp/ebx/esi/edi; Unlock writes eax = cs, esp += 4 and restores
 * esi.  ecx and edx are not written by either body.  The pushed words below
 * the entry ESP are stored too, so guest memory equals a translated run
 * byte for byte (a caller reading its own uninitialised locals sees the
 * same values).  Because the helpers evaluate every predicate before the
 * first store while the bodies read the object and the critical section
 * between their pushes, a frame whose pushes would land on either is
 * refused (kage_mutex_aliases_frame). */
enum {
    KAGE_MUTEX_INITIALIZED_OFFSET = 4U,
    KAGE_MUTEX_CS_OFFSET = 8U,
    KAGE_MUTEX_BUSY_OFFSET = 0x18U,
    /* Return words as the corpus pushes them: RVAs, never image VAs. */
    KAGE_MUTEX_LOCK_RETURN_RVA = 0x00562E2FU,
    KAGE_MUTEX_UNLOCK_RETURN_RVA = 0x00562EE6U,
    /* Bytes the bodies push below the entry ESP before the import call. */
    KAGE_MUTEX_LOCK_FRAME_BYTES = 24U,
    KAGE_MUTEX_UNLOCK_FRAME_BYTES = 12U,
    /* Bytes the bodies pop above the entry ESP (`ret 4` / `ret`). */
    KAGE_MUTEX_LOCK_RETURN_BYTES = 8U,
    KAGE_MUTEX_UNLOCK_RETURN_BYTES = 4U
};

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

/* kage_mutex_esp_aligned: both helpers refuse a misaligned ESP before any
 * other predicate.  GCC compiles the translated bodies' consecutive pushes
 * into word-pair stores (STRD), which take an ARM alignment fault when ESP
 * is not word-aligned; the helpers below store single words and would
 * complete instead.  Leaving such a frame to the body keeps the seam's
 * handled set inside the states the translated body completes (no x86 caller
 * of the two wrappers ever runs with a misaligned ESP). */

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

/* The object bytes the bodies read ([this+4] and the dword at [this+8]) and
 * the critical section bytes they read or write (the 24-byte object plus the
 * busy byte at +0x18). */
enum {
    KAGE_MUTEX_OBJECT_SPAN_BYTES = 8U,
    KAGE_MUTEX_CS_SPAN_BYTES = KAGE_MUTEX_BUSY_OFFSET + 1U
};

static inline int kage_mutex_aliases_frame(uint32_t object, uint32_t cs,
                                           uint32_t inner,
                                           uint32_t frame_bytes)
{
    return kage_mutex_ranges_overlap(object + KAGE_MUTEX_INITIALIZED_OFFSET,
                                     KAGE_MUTEX_OBJECT_SPAN_BYTES,
                                     inner, frame_bytes) ||
           kage_mutex_ranges_overlap(cs, KAGE_MUTEX_CS_SPAN_BYTES,
                                     inner, frame_bytes);
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
 * return counts.  The sampler word is published and restored around the
 * import exactly as there, so a sample taken inside the seam names the IAT
 * slot ("ext") and one taken after names the enclosing target again. */
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

int isaac_vita_kage_mutex_lock_try(CPU *__restrict c)
{
    uint32_t self = c->vita_sync_thread_id;
    uint32_t esp = c->esp;
    uint32_t object = c->ecx;
    uint32_t cs;
    uint32_t owner;
    uint32_t depth;
    uint32_t slot;
    uint32_t inner;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target;
#endif

    if (self != g_isaac_vita_sync_inline_owner)
        return 0;
    if ((esp & 3U) != 0U)
        return 0;                       /* see kage_mutex_esp_aligned */
    if (!kage_mutex_frame_inside_stack(c, KAGE_MUTEX_LOCK_FRAME_BYTES,
                                       KAGE_MUTEX_LOCK_RETURN_BYTES))
        return 0;
    if (ld8(object + KAGE_MUTEX_INITIALIZED_OFFSET) == 0U)
        return 0;                       /* logging path: translated body */
    if (ld32(esp + 4U) != UINT32_MAX)
        return 0;                       /* timed wait: translated body */
    cs = ld32(object + KAGE_MUTEX_CS_OFFSET);
    if (!kage_mutex_sync_slot_ready(ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
                                    &slot))
        return 0;
    if (!kage_mutex_cs_object(cs))
        return 0;
    inner = esp - KAGE_MUTEX_LOCK_FRAME_BYTES;
    if (kage_mutex_aliases_frame(object, cs, inner,
                                 KAGE_MUTEX_LOCK_FRAME_BYTES))
        return 0;                       /* the pushes would corrupt them */
    owner = ld32(cs + 16U);
    depth = ld32(cs + 20U);
    if (owner == self) {
        if (depth == 0U || depth == UINT32_MAX)
            return 0;
    } else if (owner != 0U || depth != 0U) {
        return 0;
    }
    /* Read only after the object proved to be a registered critical
     * section: the translated body reaches this byte after EnterCS. */
    if (ld8(cs + KAGE_MUTEX_BUSY_OFFSET) != 0U)
        return 0;                       /* Sleep loop: translated body */

    /* Commit.  push ebp; push ebx; push esi; push edi; push esi(cs); call. */
    st32(esp - 4U, c->ebp);
    st32(esp - 8U, c->ebx);
    st32(esp - 12U, c->esi);
    st32(esp - 16U, c->edi);
    st32(esp - 20U, cs);
    st32(inner, KAGE_MUTEX_LOCK_RETURN_RVA);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
    g_kage_guest_last_indirect_target = slot;
#endif
    kage_mutex_note_import(ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS);
    /* isaac_vita_sync_inline_enter's transition on the validated state. */
    if (owner == self) {
        st32(cs + 20U, depth + 1U);
    } else {
        st32(cs + 20U, 1U);
        st32(cs + 16U, self);
    }
    kage_mutex_note_low_water(c, inner);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
    /* mov byte [esi+18h],1; pop edi; pop esi; mov al,1; pop ebx; pop ebp;
     * ret 4 -- the pops restore the entry values. */
    st8(cs + KAGE_MUTEX_BUSY_OFFSET, 1U);
    c->eax = (c->eax & 0xFFFFFF00U) | 1U;
    c->esp = esp + KAGE_MUTEX_LOCK_RETURN_BYTES;
    (void)slot;
    return 1;
}

int isaac_vita_kage_mutex_unlock_try(CPU *__restrict c)
{
    uint32_t self = c->vita_sync_thread_id;
    uint32_t esp = c->esp;
    uint32_t object = c->ecx;
    uint32_t cs;
    uint32_t owner;
    uint32_t depth;
    uint32_t slot;
    uint32_t inner;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target;
#endif

    if (self != g_isaac_vita_sync_inline_owner)
        return 0;
    if ((esp & 3U) != 0U)
        return 0;                       /* see kage_mutex_esp_aligned */
    if (!kage_mutex_frame_inside_stack(c, KAGE_MUTEX_UNLOCK_FRAME_BYTES,
                                       KAGE_MUTEX_UNLOCK_RETURN_BYTES))
        return 0;
    if (ld8(object + KAGE_MUTEX_INITIALIZED_OFFSET) == 0U)
        return 0;                       /* logging path: translated body */
    cs = ld32(object + KAGE_MUTEX_CS_OFFSET);
    if (!kage_mutex_sync_slot_ready(ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS,
                                    &slot))
        return 0;
    if (!kage_mutex_cs_object(cs))
        return 0;
    inner = esp - KAGE_MUTEX_UNLOCK_FRAME_BYTES;
    if (kage_mutex_aliases_frame(object, cs, inner,
                                 KAGE_MUTEX_UNLOCK_FRAME_BYTES))
        return 0;                       /* the pushes would corrupt them */
    owner = ld32(cs + 16U);
    depth = ld32(cs + 20U);
    if (owner != self || depth == 0U)
        return 0;

    /* Commit.  push esi; mov eax,[esi+8]; push eax; mov byte [eax+18h],0;
     * call. */
    st32(esp - 4U, c->esi);
    st32(esp - 8U, cs);
    st32(inner, KAGE_MUTEX_UNLOCK_RETURN_RVA);
    st8(cs + KAGE_MUTEX_BUSY_OFFSET, 0U);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
    g_kage_guest_last_indirect_target = slot;
#endif
    kage_mutex_note_import(ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS);
    /* isaac_vita_sync_inline_leave's transition on the validated state. */
    if (depth > 1U) {
        st32(cs + 20U, depth - 1U);
    } else {
        st32(cs + 20U, 0U);
        st32(cs + 16U, 0U);
    }
    kage_mutex_note_low_water(c, inner);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
    /* pop esi; ret -- esi is restored, eax still holds cs. */
    c->eax = cs;
    c->esp = esp + KAGE_MUTEX_UNLOCK_RETURN_BYTES;
    (void)slot;
    return 1;
}
