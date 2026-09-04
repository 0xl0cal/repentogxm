/* Exact 12-byte RoomConfig Entry[] allocations, packed into retained pages
 * from the already-created heap overflow mspace. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_heap_overflow_mspace.h"
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
#include "host_vita_room_entry_external.h"
#endif
#include "host_vita_room_entry_slab.h"

#if defined(ISAAC_VITA_ROOM_ENTRY_HYBRID) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING) && \
    !defined(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING)
#error "hybrid slab test reset requires external receipt test API"
#endif

void isaac_vita_log(const char *format, ...);

enum room_slab_event_edge {
    ROOM_SLAB_EDGE_FIRST_USE = 1U << 0,
    ROOM_SLAB_EDGE_PAGE_POWER2 = 1U << 1,
    ROOM_SLAB_EDGE_FALLBACK = 1U << 2,
    ROOM_SLAB_EDGE_TERMINAL = 1U << 3,
    ROOM_SLAB_EDGE_FINAL = 1U << 4,
    ROOM_SLAB_EDGE_EXTERNAL_FIRST = 1U << 5,
    ROOM_SLAB_EDGE_EXTERNAL_CHUNK_POWER2 = 1U << 6,
    ROOM_SLAB_EDGE_EXTERNAL_OOM_POWER2 = 1U << 7
};

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
#define ROOM_SLAB_PAGE_CAP ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES
#define ROOM_SLAB_SLOT_CAP ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_SLOTS
#else
#define ROOM_SLAB_PAGE_CAP ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES
#define ROOM_SLAB_SLOT_CAP ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_SLOTS
#endif
#define ROOM_SLAB_NONFULL_WORDS ((ROOM_SLAB_PAGE_CAP + 31U) / 32U)
#define ROOM_SLAB_FLOOR_PHASE_BYTES \
    ((ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE + 3U) / 4U)

typedef enum room_slab_page_source {
    ROOM_SLAB_PAGE_RAW = 1,
    ROOM_SLAB_PAGE_EXTERNAL = 2
} room_slab_page_source;

typedef struct room_slab_page {
    unsigned char *base;
    uint32_t source;
    uint32_t external_chunk_index;
    uint32_t external_page_index;
    uint16_t live_count;
    uint16_t moving_count;
    uint16_t next_hint;
} room_slab_page;

typedef struct room_slab_state {
    room_slab_page pages[ROOM_SLAB_PAGE_CAP];
    uint32_t nonfull_mask[ROOM_SLAB_NONFULL_WORDS];
    /* Descriptor rows move with pages.  Birth phase uses two bits per slot
     * (zero is unscoped); current-floor membership uses one independent bit.
     * These arrays are fixed-size metadata, never guest backing. */
    unsigned char floor_birth_phase[ROOM_SLAB_PAGE_CAP]
                                           [ROOM_SLAB_FLOOR_PHASE_BYTES];
    unsigned char floor_current[ROOM_SLAB_PAGE_CAP]
                         [ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES];
    uint32_t page_count;
    uint32_t raw_page_count;
    uint32_t external_page_count;
    uint32_t live_slots;
    uint32_t moving_slots;
    uint32_t peak_live_slots;
    uint32_t allocations;
    uint32_t frees;
    uint32_t reallocations;
    uint32_t move_attempts;
    uint32_t moves;
    uint32_t fallbacks;
    uint32_t known_normal_frees;
    uint32_t known_stage_frees;
    uint32_t unexpected_frees;
    uint32_t rejected;
    uint32_t backing_retry_remaining;
    uint32_t backing_retry_suppressed;
    uint32_t claimed_edges;
    uint32_t terminal;
    uint32_t counter_saturated;
    uint32_t floor_unscoped_slots;
    uint32_t floor_prior_slots;
    uint32_t floor_current_slots;
    uint32_t floor_phase_slots[3];
    uint32_t floor_epoch;
    uint32_t floor_phase;
    uint32_t floor_active;
    uint32_t floor_invalid;
    uint32_t floor_counter_saturated;
} room_slab_state;

typedef enum room_slab_pointer_kind {
    ROOM_SLAB_POINTER_FOREIGN = 0,
    ROOM_SLAB_POINTER_EXACT = 1,
    ROOM_SLAB_POINTER_INVALID = 2
} room_slab_pointer_kind;

static room_slab_state s_room_slab;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
static uint32_t s_room_slab_test_classify_probes;
static uint32_t s_room_slab_test_allocation_probes;
static unsigned s_room_slab_test_fail_commit;
static unsigned s_room_slab_test_fail_page_publish;
static uint32_t s_room_slab_test_floor_forget_row_moves;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
static unsigned s_room_slab_test_fail_after_external_commit;
static unsigned s_room_slab_test_reset_in_progress;
#endif
#endif

_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES * 8U ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE,
               "room-entry bitmap/slot count drifted");
_Static_assert(ROOM_SLAB_FLOOR_PHASE_BYTES * 4U ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE,
               "room-entry floor phase/slot count drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES ==
                   ISAAC_VITA_HEAP_OVERFLOW_INTERNAL_PAGE_BYTES,
               "room-entry/raw internal page sizes diverged");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET >=
                   ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES * 2U,
               "room-entry bitmap lanes overlap slot data");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET +
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE *
                       ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES,
               "room-entry page layout does not close exactly");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE >=
                   ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES &&
                   (ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE & 7U) == 0U,
               "room-entry slots lost size/alignment contract");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES *
                   ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES,
               "room-entry retained page budget drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES *
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_SLOTS,
               "room-entry raw slot bound drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES *
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_SLOTS,
               "room-entry total slot bound drifted");
_Static_assert(ROOM_SLAB_PAGE_CAP *
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ==
                   ROOM_SLAB_SLOT_CAP,
               "room-entry total slot bound drifted");
_Static_assert(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES -
                   ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_NOMINAL_RAW_REMAINDER,
               "room-entry nominal request remainder drifted");
_Static_assert(ROOM_SLAB_PAGE_CAP <=
                   ROOM_SLAB_NONFULL_WORDS * 32U,
               "room-entry non-full page mask must cover every page");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_BACKING_RETRY_INTERVAL ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE,
               "room-entry backing retry cadence drifted");
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
_Static_assert(ROOM_SLAB_PAGE_CAP ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES,
               "hybrid room-entry page bound drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES,
               "external and slab page sizes diverged");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS *
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES,
               "external compensation capacity drifted");
#else
_Static_assert(ROOM_SLAB_PAGE_CAP ==
                   ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES,
               "first-tier room-entry page bound drifted");
#endif

static int room_slab_nonfull_get(uint32_t page_index)
{
    return (s_room_slab.nonfull_mask[page_index >> 5U] &
            (UINT32_C(1) << (page_index & 31U))) != 0U;
}

static void room_slab_nonfull_set(uint32_t page_index)
{
    s_room_slab.nonfull_mask[page_index >> 5U] |=
        UINT32_C(1) << (page_index & 31U);
}

static void room_slab_nonfull_clear(uint32_t page_index)
{
    s_room_slab.nonfull_mask[page_index >> 5U] &=
        ~(UINT32_C(1) << (page_index & 31U));
}

static void room_slab_nonfull_insert(uint32_t page_index)
{
    uint32_t index = s_room_slab.page_count;

    while (index > page_index) {
        if (room_slab_nonfull_get(index - 1U))
            room_slab_nonfull_set(index);
        else
            room_slab_nonfull_clear(index);
        --index;
    }
    room_slab_nonfull_set(page_index);
}

static int room_slab_first_nonfull(uint32_t *page_index_out)
{
    uint32_t word_index;

    for (word_index = 0U; word_index < ROOM_SLAB_NONFULL_WORDS;
         ++word_index) {
        uint32_t word = s_room_slab.nonfull_mask[word_index];
        uint32_t bit = 0U;

        if (!word)
            continue;
        while (!(word & UINT32_C(1))) {
            word >>= 1U;
            ++bit;
        }
        *page_index_out = word_index * 32U + bit;
        return *page_index_out < s_room_slab.page_count ? 1 : -1;
    }
    return 0;
}

static unsigned char *room_slab_live_bitmap(room_slab_page *page)
{
    return page->base;
}

static unsigned char *room_slab_moving_bitmap(room_slab_page *page)
{
    return page->base + ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES;
}

static int room_slab_bit_get(const unsigned char *bitmap, uint32_t slot)
{
    return (bitmap[slot >> 3U] & (unsigned char)(1U << (slot & 7U))) != 0U;
}

static void room_slab_bit_set(unsigned char *bitmap, uint32_t slot)
{
    bitmap[slot >> 3U] |= (unsigned char)(1U << (slot & 7U));
}

static void room_slab_bit_clear(unsigned char *bitmap, uint32_t slot)
{
    bitmap[slot >> 3U] &= (unsigned char)~(1U << (slot & 7U));
}

static int room_slab_floor_phase_valid(uint32_t phase)
{
    return phase >= ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT &&
        phase <= ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY;
}

static uint32_t room_slab_floor_phase_get(
    uint32_t page_index, uint32_t slot)
{
    unsigned char packed =
        s_room_slab.floor_birth_phase[page_index][slot >> 2U];
    uint32_t shift = (slot & 3U) * 2U;

    return ((uint32_t)packed >> shift) &
        ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK;
}

static void room_slab_floor_phase_set(
    uint32_t page_index, uint32_t slot, uint32_t phase)
{
    unsigned char *packed =
        &s_room_slab.floor_birth_phase[page_index][slot >> 2U];
    uint32_t shift = (slot & 3U) * 2U;
    unsigned char mask = (unsigned char)(3U << shift);

    *packed = (unsigned char)((*packed & (unsigned char)~mask) |
        (unsigned char)((phase & 3U) << shift));
}

static uint32_t room_slab_floor_token_get(
    uint32_t page_index, uint32_t slot)
{
    uint32_t token = room_slab_floor_phase_get(page_index, slot);

    if (room_slab_bit_get(s_room_slab.floor_current[page_index], slot))
        token |= ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT;
    return token;
}

static int room_slab_floor_token_valid(uint32_t token)
{
    uint32_t phase = token &
        ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK;

    if (token & ~(ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK |
                  ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT))
        return 0;
    if (!phase)
        return !(token & ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT);
    return room_slab_floor_phase_valid(phase);
}

static void room_slab_floor_invalidate(int saturated)
{
    s_room_slab.floor_invalid = 1U;
    if (saturated)
        s_room_slab.floor_counter_saturated = 1U;
}

/* Scalar-only: safe for the guest-lock path.  Cold bitmap equality is a
 * testing/diagnostic oracle below and is never a lifecycle precondition. */
static int room_slab_floor_validate_fast(void)
{
    uint32_t live = s_room_slab.live_slots;
    uint32_t scoped;
    uint32_t phases;

    if (s_room_slab.floor_active > 1U ||
        s_room_slab.floor_invalid > 1U ||
        s_room_slab.floor_counter_saturated > 1U ||
        s_room_slab.floor_unscoped_slots > live ||
        s_room_slab.floor_prior_slots >
            live - s_room_slab.floor_unscoped_slots)
        return 0;
    scoped = live - s_room_slab.floor_unscoped_slots;
    if (s_room_slab.floor_current_slots !=
            scoped - s_room_slab.floor_prior_slots)
        return 0;
    if (s_room_slab.floor_phase_slots[0] >
            s_room_slab.floor_current_slots ||
        s_room_slab.floor_phase_slots[1] >
            s_room_slab.floor_current_slots -
                s_room_slab.floor_phase_slots[0])
        return 0;
    phases = s_room_slab.floor_phase_slots[0] +
        s_room_slab.floor_phase_slots[1];
    if (s_room_slab.floor_phase_slots[2] !=
            s_room_slab.floor_current_slots - phases)
        return 0;
    if (s_room_slab.floor_active) {
        if (!s_room_slab.floor_epoch ||
            !room_slab_floor_phase_valid(s_room_slab.floor_phase))
            return 0;
    }
    else if (s_room_slab.floor_epoch || s_room_slab.floor_phase ||
             s_room_slab.floor_prior_slots ||
             s_room_slab.floor_current_slots || scoped) {
        return 0;
    }
    return 1;
}

static uint32_t room_slab_floor_new_token(void)
{
    if (!s_room_slab.floor_active ||
        !room_slab_floor_phase_valid(s_room_slab.floor_phase))
        return 0U;
    return s_room_slab.floor_phase |
        ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT;
}

static int room_slab_floor_counter_add(uint32_t *counter)
{
    if (*counter == UINT32_MAX) {
        room_slab_floor_invalidate(1);
        return 0;
    }
    ++*counter;
    return 1;
}

static void room_slab_floor_slot_add(
    uint32_t page_index, uint32_t slot)
{
    uint32_t token = room_slab_floor_new_token();
    uint32_t phase = token &
        ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK;

    if (room_slab_floor_token_get(page_index, slot) != 0U) {
        room_slab_floor_invalidate(0);
        room_slab_floor_phase_set(page_index, slot, 0U);
        room_slab_bit_clear(s_room_slab.floor_current[page_index], slot);
    }
    if (!phase) {
        (void)room_slab_floor_counter_add(
            &s_room_slab.floor_unscoped_slots);
        return;
    }
    room_slab_floor_phase_set(page_index, slot, phase);
    room_slab_bit_set(s_room_slab.floor_current[page_index], slot);
    (void)room_slab_floor_counter_add(
        &s_room_slab.floor_current_slots);
    (void)room_slab_floor_counter_add(
        &s_room_slab.floor_phase_slots[phase - 1U]);
}

static uint32_t room_slab_floor_slot_remove(
    uint32_t page_index, uint32_t slot)
{
    uint32_t token = room_slab_floor_token_get(page_index, slot);
    uint32_t phase = token &
        ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK;
    uint32_t *age_counter;

    if (!room_slab_floor_token_valid(token)) {
        room_slab_floor_invalidate(0);
        room_slab_floor_phase_set(page_index, slot, 0U);
        room_slab_bit_clear(s_room_slab.floor_current[page_index], slot);
        return 0U;
    }
    if (!phase) {
        if (s_room_slab.floor_unscoped_slots)
            --s_room_slab.floor_unscoped_slots;
        else
            room_slab_floor_invalidate(0);
    }
    else {
        age_counter =
            token & ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT ?
                &s_room_slab.floor_current_slots :
                &s_room_slab.floor_prior_slots;
        if (*age_counter)
            --*age_counter;
        else
            room_slab_floor_invalidate(0);
        if (token & ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT) {
            if (s_room_slab.floor_phase_slots[phase - 1U])
                --s_room_slab.floor_phase_slots[phase - 1U];
            else
                room_slab_floor_invalidate(0);
        }
    }
    room_slab_floor_phase_set(page_index, slot, 0U);
    room_slab_bit_clear(s_room_slab.floor_current[page_index], slot);
    return token;
}

static void room_slab_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
    else
        s_room_slab.counter_saturated = 1U;
}

static int room_slab_is_power_of_two(uint32_t value)
{
    return value && (value & (value - 1U)) == 0U;
}

static int room_slab_validate_fast(void)
{
    uint32_t word_index;
    uint32_t capacity;
    int has_nonfull = 0;

    if (s_room_slab.page_count > ROOM_SLAB_PAGE_CAP)
        return 0;
    if (s_room_slab.raw_page_count >
            ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES ||
        s_room_slab.external_page_count >
            ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES ||
        s_room_slab.raw_page_count + s_room_slab.external_page_count !=
            s_room_slab.page_count ||
        s_room_slab.external_page_count >
            ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES -
                s_room_slab.raw_page_count ||
        s_room_slab.backing_retry_remaining >
            ISAAC_VITA_ROOM_ENTRY_SLAB_BACKING_RETRY_INTERVAL ||
        (s_room_slab.page_count == ROOM_SLAB_PAGE_CAP &&
         s_room_slab.backing_retry_remaining))
        return 0;
#ifndef ISAAC_VITA_ROOM_ENTRY_HYBRID
    if (s_room_slab.external_page_count ||
        s_room_slab.backing_retry_remaining ||
        s_room_slab.backing_retry_suppressed)
        return 0;
#endif
    capacity = s_room_slab.page_count *
        ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
    if (s_room_slab.moving_slots > s_room_slab.live_slots ||
        s_room_slab.live_slots > capacity)
        return 0;
    for (word_index = 0U; word_index < ROOM_SLAB_NONFULL_WORDS;
         ++word_index)
        has_nonfull |= s_room_slab.nonfull_mask[word_index] != 0U;
    if ((s_room_slab.live_slots < capacity) != has_nonfull)
        return 0;
    if (!s_room_slab.counter_saturated) {
        if (s_room_slab.allocations < s_room_slab.frees ||
            s_room_slab.allocations - s_room_slab.frees !=
                s_room_slab.live_slots ||
            s_room_slab.peak_live_slots < s_room_slab.live_slots ||
            s_room_slab.moves > s_room_slab.move_attempts ||
            s_room_slab.moving_slots >
                s_room_slab.move_attempts - s_room_slab.moves ||
            s_room_slab.move_attempts > s_room_slab.reallocations ||
            s_room_slab.moves > s_room_slab.frees ||
            s_room_slab.known_normal_frees > s_room_slab.frees ||
            s_room_slab.known_stage_frees >
                s_room_slab.frees - s_room_slab.known_normal_frees ||
            s_room_slab.unexpected_frees >
                s_room_slab.frees - s_room_slab.known_normal_frees -
                    s_room_slab.known_stage_frees ||
            s_room_slab.backing_retry_suppressed >
                s_room_slab.fallbacks)
            return 0;
    }
    return 1;
}

static uint32_t room_slab_bitmap_count(const unsigned char *bitmap)
{
    uint32_t count = 0U;
    uint32_t byte_index;

    for (byte_index = 0U;
         byte_index < ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES;
         ++byte_index) {
        unsigned char value = bitmap[byte_index];

        while (value) {
            count += value & 1U;
            value >>= 1U;
        }
    }
    return count;
}

static int room_slab_validate_cold(
    const isaac_vita_heap_overflow_mspace_snapshot *raw
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    , const isaac_vita_room_entry_external_snapshot *external
#endif
    )
{
    uint32_t live_sum = 0U;
    uint32_t moving_sum = 0U;
    uint32_t raw_pages = 0U;
    uint32_t external_pages = 0U;
    uint32_t page_index;

    if (!raw || !room_slab_validate_fast() ||
        raw->internal_live_count != s_room_slab.raw_page_count ||
        raw->internal_requested_bytes !=
            (size_t)s_room_slab.raw_page_count *
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES ||
        raw->live_count < raw->internal_live_count)
        return 0;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    /* The manager's exhaustive receipt validator must succeed before any
     * shallow page_matches query or external bitmap dereference below. */
    if (!external || external->pending ||
        external->chunks > ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        external->issued_pages != s_room_slab.external_page_count ||
        external->requested_bytes !=
            (size_t)external->chunks *
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES ||
        external->usable_bytes !=
            (size_t)external->chunks *
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES ||
        external->retained_bytes !=
            (size_t)external->chunks *
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES ||
        (!s_room_slab.terminal
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
         && !s_room_slab_test_reset_in_progress
#endif
         &&
         (external->state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY ||
          external->terminal_fault !=
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE ||
          external->counter_saturated)))
        return 0;
#endif
    for (page_index = 0U; page_index < s_room_slab.page_count;
         ++page_index) {
        room_slab_page *page = &s_room_slab.pages[page_index];
        uint32_t live;
        uint32_t moving;
        uint32_t byte_index;
        uintptr_t base = (uintptr_t)page->base;

        if (!page->base || (base & 7U) != 0U ||
            page->next_hint >=
                ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ||
            (page_index &&
             (base <=
                  (uintptr_t)s_room_slab.pages[page_index - 1U].base ||
              base - (uintptr_t)s_room_slab.pages[page_index - 1U].base <
                  ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)))
            return 0;
        if (page->source == ROOM_SLAB_PAGE_RAW) {
            if (page->external_chunk_index || page->external_page_index ||
                base < raw->base || base >= raw->end ||
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES > raw->end - base)
                return 0;
            ++raw_pages;
        }
        else if (page->source == ROOM_SLAB_PAGE_EXTERNAL) {
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
            if (!isaac_vita_room_entry_external_page_matches(
                    page->external_chunk_index,
                    page->external_page_index, page->base))
                return 0;
            ++external_pages;
#else
            return 0;
#endif
        }
        else {
            return 0;
        }
        live = room_slab_bitmap_count(room_slab_live_bitmap(page));
        moving = room_slab_bitmap_count(room_slab_moving_bitmap(page));
        if (live != page->live_count || moving != page->moving_count ||
            moving > live ||
            room_slab_nonfull_get(page_index) !=
                (live < ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE))
            return 0;
        for (byte_index = 0U;
             byte_index < ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES;
             ++byte_index) {
            if (room_slab_moving_bitmap(page)[byte_index] &
                (unsigned char)~room_slab_live_bitmap(page)[byte_index])
                return 0;
        }
        live_sum += live;
        moving_sum += moving;
    }
    /* A non-full bit may only describe an already-published descriptor.
     * Checking the unused tail here keeps the hot validator O(1) while the
     * exhaustive snapshot/claim validator rejects latent future-page bits. */
    for (; page_index < ROOM_SLAB_PAGE_CAP; ++page_index) {
        if (room_slab_nonfull_get(page_index))
            return 0;
    }
    return live_sum == s_room_slab.live_slots &&
        moving_sum == s_room_slab.moving_slots &&
        raw_pages == s_room_slab.raw_page_count &&
        external_pages == s_room_slab.external_page_count;
}

static int room_slab_snapshot_fill(
    isaac_vita_room_entry_slab_snapshot *snapshot)
{
    isaac_vita_heap_overflow_mspace_snapshot raw;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    isaac_vita_room_entry_external_snapshot external;
    int external_valid;
#endif
    int raw_valid;
    int valid;

    memset(snapshot, 0, sizeof *snapshot);
    memset(&raw, 0, sizeof raw);
    raw_valid = isaac_vita_heap_overflow_mspace_snapshot_get(&raw);
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    memset(&external, 0, sizeof external);
    external.orphan_uid = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID;
    external_valid =
        isaac_vita_room_entry_external_snapshot_get(&external);
    if (!external_valid)
        external.orphan_uid =
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID;
    valid = raw_valid && external_valid &&
        room_slab_validate_cold(&raw, &external);
#else
    valid = raw_valid && room_slab_validate_cold(&raw);
#endif
    snapshot->pages = s_room_slab.page_count;
    snapshot->raw_pages = s_room_slab.raw_page_count;
    snapshot->external_pages = s_room_slab.external_page_count;
    snapshot->live_slots = s_room_slab.live_slots;
    snapshot->moving_slots = s_room_slab.moving_slots;
    snapshot->peak_live_slots = s_room_slab.peak_live_slots;
    snapshot->allocations = s_room_slab.allocations;
    snapshot->frees = s_room_slab.frees;
    snapshot->reallocations = s_room_slab.reallocations;
    snapshot->move_attempts = s_room_slab.move_attempts;
    snapshot->moves = s_room_slab.moves;
    snapshot->fallbacks = s_room_slab.fallbacks;
    snapshot->known_normal_frees = s_room_slab.known_normal_frees;
    snapshot->known_stage_frees = s_room_slab.known_stage_frees;
    snapshot->unexpected_frees = s_room_slab.unexpected_frees;
    snapshot->rejected = s_room_slab.rejected;
    snapshot->backing_retry_remaining =
        s_room_slab.backing_retry_remaining;
    snapshot->backing_retry_suppressed =
        s_room_slab.backing_retry_suppressed;
    snapshot->terminal = s_room_slab.terminal || !valid;
    snapshot->counter_saturated = s_room_slab.counter_saturated;
    if (valid &&
        raw.internal_live_count <= UINT32_MAX &&
        raw.internal_requested_bytes <= UINT32_MAX) {
        snapshot->raw_internal_live = (uint32_t)raw.internal_live_count;
        snapshot->raw_internal_requested =
            (uint32_t)raw.internal_requested_bytes;
    }
    else if (raw.internal_live_count > UINT32_MAX ||
             raw.internal_requested_bytes > UINT32_MAX) {
        snapshot->raw_internal_live = UINT32_MAX;
        snapshot->raw_internal_requested = UINT32_MAX;
        snapshot->terminal = 1U;
    }
    else {
        snapshot->raw_internal_live = (uint32_t)raw.internal_live_count;
        snapshot->raw_internal_requested =
            (uint32_t)raw.internal_requested_bytes;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    snapshot->external_chunks = external.chunks;
    snapshot->external_allocation_attempts =
        external.allocation_attempts;
    snapshot->external_out_of_memory = external.out_of_memory;
    snapshot->external_post_uid_failures = external.post_uid_failures;
    snapshot->external_rollback_free_attempts =
        external.rollback_free_attempts;
    snapshot->external_rollback_free_failures =
        external.rollback_free_failures;
    snapshot->external_reset_free_attempts =
        external.reset_free_attempts;
    snapshot->external_reset_free_failures =
        external.reset_free_failures;
    snapshot->external_counter_saturated = external.counter_saturated;
    snapshot->external_state = (uint32_t)external.state;
    snapshot->external_terminal_fault =
        (uint32_t)external.terminal_fault;
    snapshot->external_last_syscall = (uint32_t)external.last_syscall;
    snapshot->external_last_syscall_result =
        external.last_syscall_result;
    snapshot->external_orphan_uid = external.orphan_uid;
    if (external.requested_bytes > UINT32_MAX ||
        external.usable_bytes > UINT32_MAX ||
        external.retained_bytes > UINT32_MAX ||
        external.orphan_requested_bytes > UINT32_MAX ||
        external.orphan_retained_bytes > UINT32_MAX) {
        snapshot->external_requested = UINT32_MAX;
        snapshot->external_usable = UINT32_MAX;
        snapshot->external_retained = UINT32_MAX;
        snapshot->external_orphan_requested = UINT32_MAX;
        snapshot->external_orphan_retained = UINT32_MAX;
        snapshot->terminal = 1U;
        valid = 0;
    }
    else {
        snapshot->external_requested =
            (uint32_t)external.requested_bytes;
        snapshot->external_usable = (uint32_t)external.usable_bytes;
        snapshot->external_retained =
            (uint32_t)external.retained_bytes;
        snapshot->external_orphan_requested =
            (uint32_t)external.orphan_requested_bytes;
        snapshot->external_orphan_retained =
            (uint32_t)external.orphan_retained_bytes;
    }
#else
    snapshot->external_orphan_uid = -1;
#endif
    return valid;
}

static int room_slab_event_snapshot(
    isaac_vita_room_entry_slab_event *event)
{
    if (room_slab_snapshot_fill(&event->snapshot))
        return 1;
    s_room_slab.terminal = 1U;
    s_room_slab.claimed_edges |= ROOM_SLAB_EDGE_TERMINAL;
    event->edges |= ROOM_SLAB_EDGE_TERMINAL;
    (void)room_slab_snapshot_fill(&event->snapshot);
    return 0;
}

static int room_slab_claim(
    isaac_vita_room_entry_slab_event *event, uint32_t edges)
{
    uint32_t fresh;

    if (!event)
        return 1;
    fresh = edges & ~s_room_slab.claimed_edges;
    if (!fresh)
        return 1;
    s_room_slab.claimed_edges |= fresh;
    event->edges |= fresh;
    return room_slab_event_snapshot(event);
}

static int room_slab_claim_page_cadence(
    isaac_vita_room_entry_slab_event *event)
{
    if (!event)
        return 1;
    event->edges |= ROOM_SLAB_EDGE_PAGE_POWER2;
    return room_slab_event_snapshot(event);
}

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
static int room_slab_claim_external_success(
    const isaac_vita_room_entry_external_ticket *ticket,
    isaac_vita_room_entry_slab_event *event)
{
    if (s_room_slab.external_page_count == 1U &&
        !room_slab_claim(event, ROOM_SLAB_EDGE_EXTERNAL_FIRST))
        return 0;
    if (event && ticket->new_chunk &&
        room_slab_is_power_of_two(ticket->chunk_index + 1U)) {
        event->edges |= ROOM_SLAB_EDGE_EXTERNAL_CHUNK_POWER2;
        if (!room_slab_event_snapshot(event))
            return 0;
    }
    return 1;
}

static int room_slab_claim_external_oom(
    isaac_vita_room_entry_slab_event *event)
{
    isaac_vita_room_entry_external_snapshot external;

    if (!event)
        return 1;
    memset(&external, 0, sizeof external);
    if (!isaac_vita_room_entry_external_snapshot_get(&external))
        return 0;
    if (room_slab_is_power_of_two(external.out_of_memory)) {
        event->edges |= ROOM_SLAB_EDGE_EXTERNAL_OOM_POWER2;
        return room_slab_event_snapshot(event);
    }
    return 1;
}
#endif

static void room_slab_terminal(
    isaac_vita_room_entry_slab_event *event)
{
    s_room_slab.terminal = 1U;
    (void)room_slab_claim(event, ROOM_SLAB_EDGE_TERMINAL);
}

static void room_slab_reject(
    isaac_vita_room_entry_slab_decision *decision,
    const void *pointer, const char *fault)
{
    room_slab_increment(&s_room_slab.rejected);
    if (decision) {
        decision->pointer = NULL;
        decision->fault = fault;
        decision->fault_value = (uint32_t)(uintptr_t)pointer;
    }
}

static void room_slab_decision_clear(
    isaac_vita_room_entry_slab_decision *decision)
{
    if (decision)
        memset(decision, 0, sizeof *decision);
}

static room_slab_page *room_slab_page_containing(uintptr_t value)
{
    uint32_t low = 0U;
    uint32_t high = s_room_slab.page_count;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
        room_slab_increment(&s_room_slab_test_classify_probes);
#endif
        if ((uintptr_t)s_room_slab.pages[middle].base <= value)
            low = middle + 1U;
        else
            high = middle;
    }
    if (!low)
        return NULL;
    --low;
    if (value - (uintptr_t)s_room_slab.pages[low].base >=
            ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
        return NULL;
    return &s_room_slab.pages[low];
}

static room_slab_pointer_kind room_slab_classify(
    const void *pointer, room_slab_page **page_out, uint32_t *slot_out)
{
    uintptr_t value = (uintptr_t)pointer;
    room_slab_page *page;
    uintptr_t data;
    uintptr_t offset;

    if (!pointer)
        return ROOM_SLAB_POINTER_FOREIGN;
    page = room_slab_page_containing(value);
    if (!page)
        return ROOM_SLAB_POINTER_FOREIGN;
    if (page_out)
        *page_out = page;
    if (slot_out)
        *slot_out = 0U;
    data = (uintptr_t)page->base +
        ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET;
    if (value < data)
        return ROOM_SLAB_POINTER_INVALID;
    offset = value - data;
    if ((offset & (ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE - 1U)) != 0U)
        return ROOM_SLAB_POINTER_INVALID;
    if (slot_out)
        *slot_out = (uint32_t)(offset /
            ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE);
    return ROOM_SLAB_POINTER_EXACT;
}

static int room_slab_page_find_free(room_slab_page *page, uint32_t *slot_out)
{
    unsigned char *live = room_slab_live_bitmap(page);
    unsigned char *moving = room_slab_moving_bitmap(page);
    uint32_t start_byte = (uint32_t)page->next_hint >> 3U;
    uint32_t step;

    for (step = 0U; step < ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES; ++step) {
        uint32_t byte_index = start_byte + step;
        unsigned char occupied;
        uint32_t bit;

        if (byte_index >= ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES)
            byte_index -= ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES;
        occupied = (unsigned char)(live[byte_index] | moving[byte_index]);
        if (occupied == 0xffU)
            continue;
        for (bit = 0U; bit < 8U; ++bit) {
            unsigned char mask = (unsigned char)(1U << bit);
            uint32_t slot = byte_index * 8U + bit;

            if (!(occupied & mask) &&
                slot < ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE) {
                *slot_out = slot;
                return 1;
            }
        }
    }
    return 0;
}

static int room_slab_insert_page(
    void *backing, room_slab_page_source source,
    uint32_t external_chunk_index, uint32_t external_page_index,
    uint32_t *inserted_index_out)
{
    room_slab_page *page;
    uintptr_t begin = (uintptr_t)backing;
    uint32_t insert_at = 0U;

    *inserted_index_out = UINT32_MAX;
    if (!backing || s_room_slab.page_count >= ROOM_SLAB_PAGE_CAP ||
        (begin & 7U) != 0U ||
        begin > UINTPTR_MAX - ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES ||
        (source != ROOM_SLAB_PAGE_RAW &&
         source != ROOM_SLAB_PAGE_EXTERNAL))
        return 0;
    while (insert_at < s_room_slab.page_count &&
           (uintptr_t)s_room_slab.pages[insert_at].base < begin)
        ++insert_at;
    if ((insert_at &&
         (begin <= (uintptr_t)s_room_slab.pages[insert_at - 1U].base ||
          begin - (uintptr_t)s_room_slab.pages[insert_at - 1U].base <
              ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)) ||
        (insert_at < s_room_slab.page_count &&
         ((uintptr_t)s_room_slab.pages[insert_at].base <= begin ||
          (uintptr_t)s_room_slab.pages[insert_at].base - begin <
              ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)))
        return 0;
    memset(backing, 0, ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET);
    if (insert_at < s_room_slab.page_count) {
        memmove(&s_room_slab.pages[insert_at + 1U],
                &s_room_slab.pages[insert_at],
                (s_room_slab.page_count - insert_at) *
                    sizeof s_room_slab.pages[0]);
        memmove(&s_room_slab.floor_birth_phase[insert_at + 1U],
                &s_room_slab.floor_birth_phase[insert_at],
                (s_room_slab.page_count - insert_at) *
                    sizeof s_room_slab.floor_birth_phase[0]);
        memmove(&s_room_slab.floor_current[insert_at + 1U],
                &s_room_slab.floor_current[insert_at],
                (s_room_slab.page_count - insert_at) *
                    sizeof s_room_slab.floor_current[0]);
    }
    room_slab_nonfull_insert(insert_at);
    page = &s_room_slab.pages[insert_at];
    memset(page, 0, sizeof *page);
    memset(s_room_slab.floor_birth_phase[insert_at], 0,
           sizeof s_room_slab.floor_birth_phase[0]);
    memset(s_room_slab.floor_current[insert_at], 0,
           sizeof s_room_slab.floor_current[0]);
    page->base = (unsigned char *)backing;
    page->source = (uint32_t)source;
    if (source == ROOM_SLAB_PAGE_EXTERNAL) {
        page->external_chunk_index = external_chunk_index;
        page->external_page_index = external_page_index;
        ++s_room_slab.external_page_count;
    }
    else {
        ++s_room_slab.raw_page_count;
    }
    ++s_room_slab.page_count;
    *inserted_index_out = insert_at;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
    if (s_room_slab_test_fail_page_publish) {
        --s_room_slab_test_fail_page_publish;
        return 0;
    }
#endif
    return room_slab_validate_fast();
}

/* Precondition: a full cold check made page_index/source/count authoritative.
 * This metadata-only forget never reads backing and cannot refuse after Free. */
static void room_slab_forget_page_unchecked(uint32_t page_index)
{
    room_slab_page removed;
    uint32_t index;

    removed = s_room_slab.pages[page_index];
    if (removed.source == ROOM_SLAB_PAGE_RAW)
        --s_room_slab.raw_page_count;
    else
        --s_room_slab.external_page_count;
    for (index = page_index; index + 1U < s_room_slab.page_count;
         ++index) {
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
        ++s_room_slab_test_floor_forget_row_moves;
#endif
        s_room_slab.pages[index] = s_room_slab.pages[index + 1U];
        memmove(s_room_slab.floor_birth_phase[index],
                s_room_slab.floor_birth_phase[index + 1U],
                sizeof s_room_slab.floor_birth_phase[0]);
        memmove(s_room_slab.floor_current[index],
                s_room_slab.floor_current[index + 1U],
                sizeof s_room_slab.floor_current[0]);
        if (room_slab_nonfull_get(index + 1U))
            room_slab_nonfull_set(index);
        else
            room_slab_nonfull_clear(index);
    }
    room_slab_nonfull_clear(s_room_slab.page_count - 1U);
    memset(&s_room_slab.pages[s_room_slab.page_count - 1U], 0,
           sizeof s_room_slab.pages[0]);
    memset(s_room_slab.floor_birth_phase[s_room_slab.page_count - 1U],
           0, sizeof s_room_slab.floor_birth_phase[0]);
    memset(s_room_slab.floor_current[s_room_slab.page_count - 1U],
           0, sizeof s_room_slab.floor_current[0]);
    --s_room_slab.page_count;
}

/* Return 1 for removed+globally-valid, -1 for removed+globally-invalid, and 0
 * only when no descriptor was removed. */
static int room_slab_remove_page(uint32_t page_index)
{
    room_slab_page removed;

    if (page_index >= s_room_slab.page_count)
        return 0;
    removed = s_room_slab.pages[page_index];
    if (!removed.base || removed.live_count || removed.moving_count ||
        (removed.source == ROOM_SLAB_PAGE_RAW &&
         !s_room_slab.raw_page_count) ||
        (removed.source == ROOM_SLAB_PAGE_EXTERNAL &&
         !s_room_slab.external_page_count) ||
        (removed.source != ROOM_SLAB_PAGE_RAW &&
         removed.source != ROOM_SLAB_PAGE_EXTERNAL))
        return 0;
    room_slab_forget_page_unchecked(page_index);
    return room_slab_validate_fast() ? 1 : -1;
}

static int room_slab_page_transaction_valid(void)
{
    isaac_vita_room_entry_slab_snapshot snapshot;

    return room_slab_snapshot_fill(&snapshot);
}

static int room_slab_finish_page_publish(
    uint32_t inserted_index, isaac_vita_room_entry_slab_event *event)
{
    if (!room_slab_page_transaction_valid())
        return 0;
    if (room_slab_is_power_of_two(s_room_slab.page_count) &&
        !room_slab_claim_page_cadence(event))
        return 0;
    return inserted_index < s_room_slab.page_count;
}

static room_slab_page *room_slab_try_raw_page(
    isaac_vita_room_entry_slab_event *event)
{
    isaac_vita_heap_overflow_mspace_snapshot before;
    isaac_vita_heap_overflow_mspace_snapshot after;
    void *backing;
    uintptr_t begin;
    uint32_t inserted_index;
    int raw_transaction_intact;

    if (s_room_slab.raw_page_count >=
            ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES)
        return NULL;
    memset(&before, 0, sizeof before);
    if (!isaac_vita_heap_overflow_mspace_snapshot_get(&before) ||
        before.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        before.internal_live_count != s_room_slab.raw_page_count ||
        before.internal_requested_bytes !=
            (size_t)s_room_slab.raw_page_count *
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES) {
        room_slab_terminal(event);
        return NULL;
    }
    backing = isaac_vita_heap_overflow_mspace_internal_page_malloc();
    memset(&after, 0, sizeof after);
    if (!backing) {
        if (!isaac_vita_heap_overflow_mspace_snapshot_get(&after) ||
            after.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
            after.internal_live_count != before.internal_live_count ||
            after.internal_requested_bytes !=
                before.internal_requested_bytes)
            room_slab_terminal(event);
        return NULL;
    }
    begin = (uintptr_t)backing;
    raw_transaction_intact =
        isaac_vita_heap_overflow_mspace_snapshot_get(&after) &&
        after.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY &&
        (begin & 7U) == 0U && begin >= after.base && begin < after.end &&
        ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES <= after.end - begin &&
        before.internal_live_count != SIZE_MAX &&
        before.internal_requested_bytes <=
            SIZE_MAX - ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES &&
        after.internal_live_count == before.internal_live_count + 1U &&
        after.internal_requested_bytes ==
            before.internal_requested_bytes +
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    if (!raw_transaction_intact) {
        /* Only an exactly-accounted page may be returned to the raw module.
         * Invalid out-of-range results remain its stranded ownership receipt. */
        if ((begin & 7U) == 0U && begin >= after.base && begin < after.end &&
            ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES <= after.end - begin &&
            before.internal_live_count != SIZE_MAX &&
            before.internal_requested_bytes <=
                SIZE_MAX - ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES &&
            after.internal_live_count == before.internal_live_count + 1U &&
            after.internal_requested_bytes ==
                before.internal_requested_bytes +
                    ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
            (void)isaac_vita_heap_overflow_mspace_internal_page_free(
                backing);
        room_slab_terminal(event);
        return NULL;
    }
    inserted_index = UINT32_MAX;
    if (!room_slab_insert_page(backing, ROOM_SLAB_PAGE_RAW, 0U, 0U,
                               &inserted_index) ||
        !room_slab_finish_page_publish(inserted_index, event)) {
        int unpublished = 1;

        if (inserted_index != UINT32_MAX)
            unpublished = room_slab_remove_page(inserted_index) != 0;
        /* Never consume backing behind a descriptor that we failed to
         * unpublish.  A removed-but-globally-invalid state is still safe to
         * release and then terminalize. */
        if (unpublished)
            (void)isaac_vita_heap_overflow_mspace_internal_page_free(
                backing);
        room_slab_terminal(event);
        return NULL;
    }
    s_room_slab.backing_retry_remaining = 0U;
    return &s_room_slab.pages[inserted_index];
}

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
static void room_slab_abort_external_ticket(
    const isaac_vita_room_entry_external_ticket *ticket, int committed)
{
    if (committed) {
        (void)isaac_vita_room_entry_external_unissue_page(ticket);
    }
    else {
        isaac_vita_room_entry_external_snapshot external;

        memset(&external, 0, sizeof external);
        if (isaac_vita_room_entry_external_snapshot_get(&external)) {
            if (external.pending)
                (void)isaac_vita_room_entry_external_cancel_page(ticket);
            else if (isaac_vita_room_entry_external_page_matches(
                         ticket->chunk_index, ticket->page_index,
                         ticket->page))
                (void)isaac_vita_room_entry_external_unissue_page(ticket);
        }
    }
}

static room_slab_page *room_slab_publish_external_page(
    const isaac_vita_room_entry_external_ticket *ticket,
    isaac_vita_room_entry_slab_event *event)
{
    uint32_t inserted_index = UINT32_MAX;
    int committed = 0;

    if (!ticket || !ticket->page ||
        !room_slab_insert_page(ticket->page, ROOM_SLAB_PAGE_EXTERNAL,
                               ticket->chunk_index, ticket->page_index,
                               &inserted_index))
        goto fail;
    if (!isaac_vita_room_entry_external_commit_page(ticket))
        goto fail;
    committed = 1;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
    if (s_room_slab_test_fail_after_external_commit) {
        --s_room_slab_test_fail_after_external_commit;
        goto fail;
    }
#endif
    if (!room_slab_finish_page_publish(inserted_index, event) ||
        !room_slab_claim_external_success(ticket, event))
        goto fail;
    s_room_slab.backing_retry_remaining = 0U;
    return &s_room_slab.pages[inserted_index];

fail:
    /* Unpublish before cancel/unissue: those calls may immediately Free the
     * chunk, and no later path may dereference its bitmaps. */
    {
        int unpublished = 1;

        if (inserted_index != UINT32_MAX)
            unpublished = room_slab_remove_page(inserted_index) != 0;
        if (unpublished)
            room_slab_abort_external_ticket(ticket, committed);
    }
    room_slab_terminal(event);
    return NULL;
}
#endif

static room_slab_page *room_slab_acquire_page(
    isaac_vita_room_entry_slab_event *event)
{
    room_slab_page *page;

    if (s_room_slab.page_count >= ROOM_SLAB_PAGE_CAP)
        return NULL;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    {
        isaac_vita_room_entry_external_ticket ticket;
        isaac_vita_room_entry_external_result result;

        memset(&ticket, 0, sizeof ticket);
        result = isaac_vita_room_entry_external_reserve_page(0, &ticket);
        if (result == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS)
            return room_slab_publish_external_page(&ticket, event);
        if (result != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_EMPTY) {
            room_slab_terminal(event);
            return NULL;
        }
    }
    if (s_room_slab.backing_retry_remaining > 1U) {
        --s_room_slab.backing_retry_remaining;
        room_slab_increment(&s_room_slab.backing_retry_suppressed);
        if (s_room_slab.counter_saturated)
            room_slab_terminal(event);
        return NULL;
    }
    if (s_room_slab.backing_retry_remaining == 1U)
        s_room_slab.backing_retry_remaining = 0U;
#endif
    page = room_slab_try_raw_page(event);
    if (page || s_room_slab.terminal)
        return page;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    {
        isaac_vita_room_entry_external_ticket ticket;
        isaac_vita_room_entry_external_result result;

        memset(&ticket, 0, sizeof ticket);
        result = isaac_vita_room_entry_external_reserve_page(1, &ticket);
        if (result == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS)
            return room_slab_publish_external_page(&ticket, event);
        if (result == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_OUT_OF_MEMORY) {
            s_room_slab.backing_retry_remaining =
                ISAAC_VITA_ROOM_ENTRY_SLAB_BACKING_RETRY_INTERVAL;
            if (!room_slab_claim_external_oom(event))
                room_slab_terminal(event);
            return NULL;
        }
        /* LIMIT below total192, or any manager terminal, contradicts the
         * retained-chunk/page compensation invariants. */
        room_slab_terminal(event);
    }
#endif
    return NULL;
}

void isaac_vita_room_entry_slab_event_init(
    isaac_vita_room_entry_slab_event *event)
{
    if (event)
        memset(event, 0, sizeof *event);
}

isaac_vita_room_entry_slab_result isaac_vita_room_entry_slab_malloc_locked(
    uint32_t owner_return_rva, size_t size,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event)
{
    room_slab_page *page = NULL;
    uint32_t page_index;
    int nonfull_result;
    uint32_t slot = 0U;
    unsigned char *pointer;

    room_slab_decision_clear(decision);
    if (owner_return_rva != ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA ||
        size != ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
    if (s_room_slab.terminal)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    if (!room_slab_validate_fast()) {
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    nonfull_result = room_slab_first_nonfull(&page_index);
    if (nonfull_result < 0 ||
        (!nonfull_result &&
         s_room_slab.live_slots < s_room_slab.page_count *
             ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE)) {
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    if (nonfull_result > 0) {
        room_slab_page *candidate = &s_room_slab.pages[page_index];
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
        room_slab_increment(&s_room_slab_test_allocation_probes);
#endif
        if (candidate->live_count >=
                ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ||
            !room_slab_page_find_free(candidate, &slot)) {
            room_slab_terminal(event);
            return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
        }
        page = candidate;
    }
    if (!page) {
        page = room_slab_acquire_page(event);
        if (s_room_slab.terminal)
            return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
        if (!page) {
            room_slab_increment(&s_room_slab.fallbacks);
            if (!room_slab_claim(event, ROOM_SLAB_EDGE_FALLBACK)) {
                room_slab_terminal(event);
                return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
            }
            return ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK;
        }
        if (!room_slab_page_find_free(page, &slot)) {
            /* A freshly published page has zeroed bitmaps and all 4032 slots
             * free.  Failure here is metadata corruption, never pressure. */
            room_slab_terminal(event);
            return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
        }
    }
    room_slab_bit_set(room_slab_live_bitmap(page), slot);
    ++page->live_count;
    page_index = (uint32_t)(page - s_room_slab.pages);
    room_slab_floor_slot_add(page_index, slot);
    if (page->live_count == ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE)
        room_slab_nonfull_clear(page_index);
    page->next_hint = (uint16_t)(slot + 1U ==
        ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ? 0U : slot + 1U);
    room_slab_increment(&s_room_slab.live_slots);
    room_slab_increment(&s_room_slab.allocations);
    if (s_room_slab.live_slots > s_room_slab.peak_live_slots)
        s_room_slab.peak_live_slots = s_room_slab.live_slots;
    pointer = page->base + ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET +
        slot * ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE;
    if (decision) {
        decision->pointer = pointer;
        decision->floorlife_token =
            room_slab_floor_token_get(page_index, slot);
    }
    if (!room_slab_validate_fast() ||
        !room_slab_claim(event, ROOM_SLAB_EDGE_FIRST_USE)) {
        room_slab_decision_clear(decision);
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    return ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED;
}

static int room_slab_classified_live(
    void *pointer, room_slab_pointer_kind kind, room_slab_page *page,
    uint32_t slot, room_slab_page **page_out, uint32_t *slot_out,
    isaac_vita_room_entry_slab_decision *decision)
{
    if (kind == ROOM_SLAB_POINTER_INVALID ||
        !room_slab_bit_get(room_slab_live_bitmap(page), slot)) {
        room_slab_reject(decision, pointer,
            "free/realloc received an interior or stale room-entry pointer");
        return -1;
    }
    if (room_slab_bit_get(room_slab_moving_bitmap(page), slot)) {
        room_slab_reject(decision, pointer,
            "room-entry allocation is already moving");
        return -1;
    }
    *page_out = page;
    *slot_out = slot;
    return 1;
}

static uint32_t room_slab_release_slot(room_slab_page *page, uint32_t slot)
{
    uint32_t page_index = (uint32_t)(page - s_room_slab.pages);
    uint32_t floorlife_token =
        room_slab_floor_slot_remove(page_index, slot);

    room_slab_bit_clear(room_slab_live_bitmap(page), slot);
    --page->live_count;
    page->next_hint = (uint16_t)slot;
    --s_room_slab.live_slots;
    room_slab_nonfull_set(page_index);
    room_slab_increment(&s_room_slab.frees);
    return floorlife_token;
}

isaac_vita_room_entry_slab_result isaac_vita_room_entry_slab_free_locked(
    void *pointer, uint32_t free_return_rva,
    isaac_vita_room_entry_slab_lease_predicate is_leased,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event)
{
    room_slab_page *page = NULL;
    uint32_t slot = 0U;
    room_slab_pointer_kind kind;
    int exact;

    room_slab_decision_clear(decision);
    kind = room_slab_classify(pointer, &page, &slot);
    if (kind == ROOM_SLAB_POINTER_FOREIGN)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
    if (s_room_slab.terminal)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    if (!room_slab_validate_fast()) {
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    exact = room_slab_classified_live(
        pointer, kind, page, slot, &page, &slot, decision);
    if (exact < 0)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED;
    if (is_leased && is_leased((uintptr_t)pointer)) {
        room_slab_reject(decision, pointer,
                         "free received a leased room-entry pointer");
        return ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED;
    }
    if (free_return_rva == ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA)
        room_slab_increment(&s_room_slab.known_normal_frees);
    else if (free_return_rva == ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA)
        room_slab_increment(&s_room_slab.known_stage_frees);
    else
        room_slab_increment(&s_room_slab.unexpected_frees);
    room_slab_release_slot(page, slot);
    if (!room_slab_validate_fast()) {
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    return ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED;
}

isaac_vita_room_entry_slab_result
isaac_vita_room_entry_slab_realloc_begin_locked(
    void *pointer, size_t size,
    isaac_vita_room_entry_slab_lease_predicate is_leased,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event)
{
    room_slab_page *page = NULL;
    uint32_t slot = 0U;
    room_slab_pointer_kind kind;
    int exact;

    room_slab_decision_clear(decision);
    if (!pointer)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
    kind = room_slab_classify(pointer, &page, &slot);
    if (kind == ROOM_SLAB_POINTER_FOREIGN)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
    if (s_room_slab.terminal)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    if (!room_slab_validate_fast()) {
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    exact = room_slab_classified_live(
        pointer, kind, page, slot, &page, &slot, decision);
    if (exact < 0)
        return ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED;
    if (is_leased && is_leased((uintptr_t)pointer)) {
        room_slab_reject(decision, pointer,
                         "realloc received a leased room-entry pointer");
        return ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED;
    }
    if (decision)
        decision->floorlife_token = room_slab_floor_token_get(
            (uint32_t)(page - s_room_slab.pages), slot);
    room_slab_increment(&s_room_slab.reallocations);
    if (!size) {
        room_slab_release_slot(page, slot);
        if (decision)
            decision->pointer = NULL;
        if (!room_slab_validate_fast()) {
            room_slab_terminal(event);
            return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
        }
        return ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED;
    }
    if (size == ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES) {
        if (decision)
            decision->pointer = pointer;
        return ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED;
    }
    room_slab_bit_set(room_slab_moving_bitmap(page), slot);
    ++page->moving_count;
    room_slab_increment(&s_room_slab.moving_slots);
    room_slab_increment(&s_room_slab.move_attempts);
    if (decision)
        decision->pointer = pointer;
    if (!room_slab_validate_fast()) {
        room_slab_terminal(event);
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
    return ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE;
}

static int room_slab_finish_move(
    void *pointer, int commit,
    isaac_vita_room_entry_slab_event *event)
{
    room_slab_page *page = NULL;
    uint32_t slot = 0U;

    if (s_room_slab.terminal || !room_slab_validate_fast() ||
        room_slab_classify(pointer, &page, &slot) !=
            ROOM_SLAB_POINTER_EXACT ||
        !room_slab_bit_get(room_slab_live_bitmap(page), slot) ||
        !room_slab_bit_get(room_slab_moving_bitmap(page), slot) ||
        !page->moving_count || !s_room_slab.moving_slots) {
        room_slab_terminal(event);
        return 0;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
    if (commit && s_room_slab_test_fail_commit) {
        --s_room_slab_test_fail_commit;
        room_slab_terminal(event);
        return 0;
    }
#endif
    room_slab_bit_clear(room_slab_moving_bitmap(page), slot);
    --page->moving_count;
    --s_room_slab.moving_slots;
    if (commit)
        room_slab_increment(&s_room_slab.moves);
    if (commit)
        room_slab_release_slot(page, slot);
    if (!room_slab_validate_fast()) {
        room_slab_terminal(event);
        return 0;
    }
    return 1;
}

int isaac_vita_room_entry_slab_realloc_cancel_locked(
    void *pointer, isaac_vita_room_entry_slab_event *event)
{
    return room_slab_finish_move(pointer, 0, event);
}

int isaac_vita_room_entry_slab_realloc_commit_locked(
    void *pointer, isaac_vita_room_entry_slab_event *event)
{
    return room_slab_finish_move(pointer, 1, event);
}

int isaac_vita_room_entry_slab_snapshot_locked(
    isaac_vita_room_entry_slab_snapshot *snapshot_out)
{
    if (!snapshot_out)
        return 0;
    return room_slab_snapshot_fill(snapshot_out);
}

int isaac_vita_room_entry_slab_fast_snapshot_locked(
    isaac_vita_room_entry_slab_fast_snapshot *snapshot_out)
{
    if (!snapshot_out || !room_slab_validate_fast())
        return 0;
    snapshot_out->pages = s_room_slab.page_count;
    snapshot_out->raw_pages = s_room_slab.raw_page_count;
    snapshot_out->external_pages = s_room_slab.external_page_count;
    snapshot_out->live_slots = s_room_slab.live_slots;
    snapshot_out->allocations = s_room_slab.allocations;
    snapshot_out->frees = s_room_slab.frees;
    snapshot_out->terminal = s_room_slab.terminal;
    snapshot_out->counter_saturated = s_room_slab.counter_saturated;
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
    uint32_t phase)
{
    if (s_room_slab.floor_invalid ||
        !room_slab_floor_validate_fast() ||
        s_room_slab.floor_active ||
        !room_slab_floor_phase_valid(phase)) {
        room_slab_floor_invalidate(0);
        return 0;
    }
    /* Existing allocations were born before an observed floor boundary and
     * intentionally remain unscoped.  No bitmap clear or census occurs. */
    s_room_slab.floor_active = 1U;
    s_room_slab.floor_epoch = 1U;
    s_room_slab.floor_phase = phase;
    if (!room_slab_floor_validate_fast()) {
        room_slab_floor_invalidate(0);
        return 0;
    }
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_rollover_locked(void)
{
    if (s_room_slab.floor_invalid ||
        !room_slab_floor_validate_fast() ||
        !s_room_slab.floor_active || s_room_slab.moving_slots) {
        /* A MOVE token may be outside the lock.  Refusing and invalidating is
         * safer than clearing its CURRENT bit and silently rejuvenating the
         * generic replacement when the transaction comes back to commit. */
        room_slab_floor_invalidate(0);
        return 0;
    }
    if (s_room_slab.floor_epoch == UINT32_MAX) {
        room_slab_floor_invalidate(1);
        return 0;
    }
    s_room_slab.floor_prior_slots +=
        s_room_slab.floor_current_slots;
    s_room_slab.floor_current_slots = 0U;
    memset(s_room_slab.floor_phase_slots, 0,
           sizeof s_room_slab.floor_phase_slots);
    memset(s_room_slab.floor_current, 0,
           sizeof s_room_slab.floor_current);
    ++s_room_slab.floor_epoch;
    if (!room_slab_floor_validate_fast()) {
        room_slab_floor_invalidate(0);
        return 0;
    }
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
    uint32_t phase)
{
    if (s_room_slab.floor_invalid ||
        !room_slab_floor_validate_fast() ||
        !s_room_slab.floor_active ||
        !room_slab_floor_phase_valid(phase)) {
        room_slab_floor_invalidate(0);
        return 0;
    }
    s_room_slab.floor_phase = phase;
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot_out)
{
    int structurally_valid;

    if (!snapshot_out)
        return 0;
    structurally_valid = room_slab_floor_validate_fast();
    snapshot_out->live_slots = s_room_slab.live_slots;
    snapshot_out->unscoped_slots =
        s_room_slab.floor_unscoped_slots;
    snapshot_out->prior_slots = s_room_slab.floor_prior_slots;
    snapshot_out->current_slots = s_room_slab.floor_current_slots;
    snapshot_out->level_init_slots =
        s_room_slab.floor_phase_slots[0];
    snapshot_out->room_load_slots =
        s_room_slab.floor_phase_slots[1];
    snapshot_out->play_slots = s_room_slab.floor_phase_slots[2];
    snapshot_out->epoch = s_room_slab.floor_epoch;
    snapshot_out->phase = s_room_slab.floor_phase;
    snapshot_out->active = s_room_slab.floor_active;
    snapshot_out->valid = structurally_valid &&
        !s_room_slab.floor_invalid;
    snapshot_out->terminal = s_room_slab.terminal;
    snapshot_out->counter_saturated =
        s_room_slab.floor_counter_saturated;
    return snapshot_out->valid != 0U;
}

int isaac_vita_room_entry_slab_raw_accounting_locked(
    uint32_t *pages_out, uint32_t *requested_bytes_out)
{
    if (!pages_out || !requested_bytes_out ||
        !room_slab_validate_fast())
        return 0;
    *pages_out = s_room_slab.raw_page_count;
    *requested_bytes_out = s_room_slab.raw_page_count *
        ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    return 1;
}

int isaac_vita_room_entry_slab_owns_exact_locked(const void *pointer)
{
    room_slab_page *page = NULL;
    uint32_t slot = 0U;

    return room_slab_classify(pointer, &page, &slot) ==
            ROOM_SLAB_POINTER_EXACT &&
        room_slab_bit_get(room_slab_live_bitmap(page), slot);
}

int isaac_vita_room_entry_slab_find_containing_locked(
    const void *range, size_t size, uintptr_t *allocation_base_out)
{
    uintptr_t first = (uintptr_t)range;

    if (allocation_base_out)
        *allocation_base_out = 0U;
    if (!range || !size || !allocation_base_out ||
        size - 1U > UINTPTR_MAX - first)
        return 0;
    {
        room_slab_page *page = room_slab_page_containing(first);
        uintptr_t data;
        uintptr_t data_end;
        uintptr_t offset;
        uintptr_t slot_base;
        uint32_t slot;
        size_t in_slot;

        if (!page)
            return 0;
        data = (uintptr_t)page->base +
            ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET;
        data_end = (uintptr_t)page->base +
            ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
        if (first < data || first >= data_end)
            return 0;
        offset = first - data;
        slot = (uint32_t)(offset /
            ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE);
        in_slot = (size_t)(offset &
            (ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE - 1U));
        if (slot >= ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE ||
            in_slot >= ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES ||
            size > ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES - in_slot ||
            !room_slab_bit_get(room_slab_live_bitmap(page), slot) ||
            room_slab_bit_get(room_slab_moving_bitmap(page), slot))
            return 0;
        slot_base = data +
            slot * ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE;
        *allocation_base_out = slot_base;
        return 1;
    }
    return 0;
}

void isaac_vita_room_entry_slab_claim_final_locked(
    isaac_vita_room_entry_slab_event *event)
{
    room_slab_claim(event, ROOM_SLAB_EDGE_FINAL);
}

static const char *room_slab_edge_name(uint32_t edge)
{
    switch (edge) {
    case ROOM_SLAB_EDGE_FIRST_USE:
        return "first-use";
    case ROOM_SLAB_EDGE_PAGE_POWER2:
        return "page-power2";
    case ROOM_SLAB_EDGE_FALLBACK:
        return "fallback";
    case ROOM_SLAB_EDGE_TERMINAL:
        return "terminal";
    case ROOM_SLAB_EDGE_FINAL:
        return "final";
    case ROOM_SLAB_EDGE_EXTERNAL_FIRST:
        return "external-first";
    case ROOM_SLAB_EDGE_EXTERNAL_CHUNK_POWER2:
        return "external-chunk-power2";
    case ROOM_SLAB_EDGE_EXTERNAL_OOM_POWER2:
        return "external-oom-power2";
    default:
        return "unknown";
    }
}

void isaac_vita_room_entry_slab_log_event(
    const isaac_vita_room_entry_slab_event *event)
{
    uint32_t edge;

    if (!event)
        return;
    for (edge = ROOM_SLAB_EDGE_FIRST_USE;
         edge <= ROOM_SLAB_EDGE_EXTERNAL_OOM_POWER2;
         edge <<= 1U) {
        const isaac_vita_room_entry_slab_snapshot *s = &event->snapshot;

        if (!(event->edges & edge))
            continue;
        isaac_vita_log(
            "roomslab: e=%s pages=%u(%u+%u) live/move/peak=%u/%u/%u "
            "a/f/r/ma/mc=%u/%u/%u/%u/%u fb=%u known=%u/%u unexpected=%u "
            "reject=%u raw=%u/%u retry=%u/%u term/sat=%u/%u",
            room_slab_edge_name(edge), (unsigned)s->pages,
            (unsigned)s->raw_pages, (unsigned)s->external_pages,
            (unsigned)s->live_slots, (unsigned)s->moving_slots,
            (unsigned)s->peak_live_slots, (unsigned)s->allocations,
            (unsigned)s->frees, (unsigned)s->reallocations,
            (unsigned)s->move_attempts, (unsigned)s->moves,
            (unsigned)s->fallbacks,
            (unsigned)s->known_normal_frees,
            (unsigned)s->known_stage_frees,
            (unsigned)s->unexpected_frees, (unsigned)s->rejected,
            (unsigned)s->raw_internal_live,
            (unsigned)s->raw_internal_requested,
            (unsigned)s->backing_retry_remaining,
            (unsigned)s->backing_retry_suppressed,
            (unsigned)s->terminal, (unsigned)s->counter_saturated);
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
        isaac_vita_log(
            "roomslabx: e=%s chunks=%u alloc/oom/post=%u/%u/%u "
            "rb=%u/%u reset=%u/%u xsat=%u bytes=req/use/ret=%u/%u/%u "
            "orphan=%d/%u/%u state/fault/sys/result=%u/%u/%u/%d",
            room_slab_edge_name(edge), (unsigned)s->external_chunks,
            (unsigned)s->external_allocation_attempts,
            (unsigned)s->external_out_of_memory,
            (unsigned)s->external_post_uid_failures,
            (unsigned)s->external_rollback_free_attempts,
            (unsigned)s->external_rollback_free_failures,
            (unsigned)s->external_reset_free_attempts,
            (unsigned)s->external_reset_free_failures,
            (unsigned)s->external_counter_saturated,
            (unsigned)s->external_requested,
            (unsigned)s->external_usable,
            (unsigned)s->external_retained,
            (int)s->external_orphan_uid,
            (unsigned)s->external_orphan_requested,
            (unsigned)s->external_orphan_retained,
            (unsigned)s->external_state,
            (unsigned)s->external_terminal_fault,
            (unsigned)s->external_last_syscall,
            (int)s->external_last_syscall_result);
#endif
    }
}

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
static int room_slab_state_is_all_zero(void)
{
    const unsigned char *bytes =
        (const unsigned char *)(const void *)&s_room_slab;
    size_t index;

    for (index = 0U; index < sizeof s_room_slab; ++index) {
        if (bytes[index])
            return 0;
    }
    return 1;
}
#endif

int isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked(void)
{
    uint32_t unscoped = 0U;
    uint32_t prior = 0U;
    uint32_t current = 0U;
    uint32_t phases[3] = {0U, 0U, 0U};
    uint32_t page_index;

    if (!room_slab_floor_validate_fast() ||
        s_room_slab.floor_invalid)
        return 0;
    for (page_index = 0U; page_index < ROOM_SLAB_PAGE_CAP;
         ++page_index) {
        uint32_t slot;

        for (slot = 0U;
             slot < ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
             ++slot) {
            uint32_t token = room_slab_floor_token_get(
                page_index, slot);
            int live = page_index < s_room_slab.page_count &&
                room_slab_bit_get(room_slab_live_bitmap(
                    &s_room_slab.pages[page_index]), slot);
            uint32_t phase;

            if (!room_slab_floor_token_valid(token) ||
                (!live && token))
                return 0;
            if (!live)
                continue;
            phase = token &
                ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK;
            if (!phase) {
                ++unscoped;
                continue;
            }
            if (token & ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT) {
                ++phases[phase - 1U];
                ++current;
            }
            else
                ++prior;
        }
    }
    return unscoped == s_room_slab.floor_unscoped_slots &&
        prior == s_room_slab.floor_prior_slots &&
        current == s_room_slab.floor_current_slots &&
        phases[0] == s_room_slab.floor_phase_slots[0] &&
        phases[1] == s_room_slab.floor_phase_slots[1] &&
        phases[2] == s_room_slab.floor_phase_slots[2] &&
        unscoped + prior + current == s_room_slab.live_slots;
}

void isaac_vita_room_entry_slab_test_floor_lifetime_force_epoch_max_locked(
    void)
{
    if (s_room_slab.floor_active)
        s_room_slab.floor_epoch = UINT32_MAX;
}

static int room_slab_test_first_live_slot(
    uint32_t *page_index_out, uint32_t *slot_out)
{
    uint32_t page_index;

    for (page_index = 0U; page_index < s_room_slab.page_count;
         ++page_index) {
        uint32_t slot;

        for (slot = 0U;
             slot < ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
             ++slot) {
            if (room_slab_bit_get(room_slab_live_bitmap(
                    &s_room_slab.pages[page_index]), slot)) {
                *page_index_out = page_index;
                *slot_out = slot;
                return 1;
            }
        }
    }
    return 0;
}

int isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_phase_locked(
    void)
{
    uint32_t page_index;
    uint32_t slot;
    unsigned char *packed;
    uint32_t shift;

    if (!room_slab_test_first_live_slot(&page_index, &slot))
        return 0;
    packed = &s_room_slab.floor_birth_phase[page_index][slot >> 2U];
    shift = (slot & 3U) * 2U;
    *packed ^= (unsigned char)(1U << shift);
    return 1;
}

int isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_current_locked(
    void)
{
    uint32_t page_index;
    uint32_t slot;

    if (!room_slab_test_first_live_slot(&page_index, &slot))
        return 0;
    s_room_slab.floor_current[page_index][slot >> 3U] ^=
        (unsigned char)(1U << (slot & 7U));
    return 1;
}

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
static int room_slab_external_receipt_descriptors_valid(
    const isaac_vita_room_entry_external_test_receipt *receipt)
{
    isaac_vita_room_entry_external_snapshot external;
    uint32_t seen = 0U;
    uint32_t count = 0U;
    uint32_t index;

    memset(&external, 0, sizeof external);
    if (!receipt ||
        !isaac_vita_room_entry_external_snapshot_get(&external) ||
        external.pending ||
        external.issued_pages != s_room_slab.external_page_count)
        return 0;
    if (receipt->orphan)
        return receipt->issued_pages == 0U &&
            receipt->chunk_index == UINT32_MAX;
    if (!receipt->issued_pages ||
        receipt->issued_pages >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK ||
        receipt->chunk_index + 1U != external.chunks)
        return 0;
    for (index = 0U; index < s_room_slab.page_count; ++index) {
        room_slab_page *page = &s_room_slab.pages[index];

        if (page->source != ROOM_SLAB_PAGE_EXTERNAL ||
            page->external_chunk_index != receipt->chunk_index)
            continue;
        if (page->live_count || page->moving_count ||
            page->external_page_index >= receipt->issued_pages ||
            (seen & (UINT32_C(1) << page->external_page_index)) ||
            !isaac_vita_room_entry_external_page_matches(
                page->external_chunk_index, page->external_page_index,
                page->base))
            return 0;
        seen |= UINT32_C(1) << page->external_page_index;
        ++count;
    }
    return count == receipt->issued_pages &&
        seen == (UINT32_C(1) << receipt->issued_pages) - 1U;
}

static int room_slab_remove_external_chunk_descriptors(
    uint32_t chunk_index, uint32_t expected_pages)
{
    uint32_t old_count = s_room_slab.page_count;
    uint32_t read_index;
    uint32_t write_index = 0U;
    uint32_t removed = 0U;

    memset(s_room_slab.nonfull_mask, 0,
           sizeof s_room_slab.nonfull_mask);
    for (read_index = 0U; read_index < old_count; ++read_index) {
        room_slab_page page = s_room_slab.pages[read_index];

        if (page.source == ROOM_SLAB_PAGE_EXTERNAL &&
            page.external_chunk_index == chunk_index) {
            ++removed;
            continue;
        }
        if (write_index != read_index) {
            s_room_slab.pages[write_index] = page;
            memmove(s_room_slab.floor_birth_phase[write_index],
                    s_room_slab.floor_birth_phase[read_index],
                    sizeof s_room_slab.floor_birth_phase[0]);
            memmove(s_room_slab.floor_current[write_index],
                    s_room_slab.floor_current[read_index],
                    sizeof s_room_slab.floor_current[0]);
        }
        if (page.live_count < ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE)
            room_slab_nonfull_set(write_index);
        ++write_index;
    }
    memset(&s_room_slab.pages[write_index], 0,
           (old_count - write_index) * sizeof s_room_slab.pages[0]);
    memset(&s_room_slab.floor_birth_phase[write_index], 0,
           (old_count - write_index) *
               sizeof s_room_slab.floor_birth_phase[0]);
    memset(&s_room_slab.floor_current[write_index], 0,
           (old_count - write_index) *
               sizeof s_room_slab.floor_current[0]);
    s_room_slab.page_count = write_index;
    if (removed > s_room_slab.external_page_count)
        s_room_slab.external_page_count = 0U;
    else
        s_room_slab.external_page_count -= removed;
    return removed == expected_pages && room_slab_validate_fast();
}

static int room_slab_reset_external(void)
{
    for (;;) {
        isaac_vita_room_entry_external_test_receipt receipt;
        int newest;
        int released;

        newest = isaac_vita_room_entry_external_test_newest_receipt(
            &receipt);
        if (newest < 0)
            return 0;
        if (!newest)
            break;
        {
            isaac_vita_room_entry_slab_snapshot snapshot;

            if (!room_slab_snapshot_fill(&snapshot))
                return 0;
        }
        if (!room_slab_external_receipt_descriptors_valid(&receipt))
            return 0;
        released = isaac_vita_room_entry_external_test_release_newest(
            &receipt);
        if (released <= 0)
            return 0;
        /* release==1 is consumed even if the manager terminalized during its
         * post-Free check.  Remove by host metadata before any cold snapshot
         * or bitmap access can observe the freed chunk. */
        if (!receipt.orphan &&
            !room_slab_remove_external_chunk_descriptors(
                receipt.chunk_index, receipt.issued_pages)) {
            s_room_slab.terminal = 1U;
            return 0;
        }
    }
    return isaac_vita_room_entry_external_test_finish_reset();
}
#endif

static void room_slab_test_controls_clear(void)
{
    s_room_slab_test_classify_probes = 0U;
    s_room_slab_test_allocation_probes = 0U;
    s_room_slab_test_fail_commit = 0U;
    s_room_slab_test_fail_page_publish = 0U;
    s_room_slab_test_floor_forget_row_moves = 0U;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    s_room_slab_test_fail_after_external_commit = 0U;
    s_room_slab_test_reset_in_progress = 0U;
#endif
}

int isaac_vita_room_entry_slab_test_reset_locked(void)
{
    isaac_vita_room_entry_slab_snapshot snapshot;
    uint32_t index;

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    {
        isaac_vita_room_entry_external_snapshot external;

        memset(&external, 0, sizeof external);
        if (isaac_vita_room_entry_external_snapshot_get(&external) &&
            (external.state ==
                 ISAAC_VITA_ROOM_ENTRY_EXTERNAL_UNINITIALIZED ||
             external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAILED)) {
            if (!room_slab_state_is_all_zero())
                return 0;
            if (!isaac_vita_room_entry_external_test_finish_reset())
                return 0;
            room_slab_test_controls_clear();
            return 1;
        }
    }
#endif
    if (s_room_slab.live_slots || s_room_slab.moving_slots)
        return 0;
    if (!room_slab_snapshot_fill(&snapshot))
        return 0;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    /* Once teardown begins, DRAIN_ONLY is an expected retry state.  Keep this
     * flag set after a failed Free so the next call can cold-validate and
     * resume from the exact retained receipt. */
    s_room_slab_test_reset_in_progress = 1U;
    if (!room_slab_reset_external())
        return 0;
#endif
    while (s_room_slab.page_count) {
        room_slab_page *page = NULL;

        if (!room_slab_snapshot_fill(&snapshot))
            return 0;
        for (index = s_room_slab.page_count; index > 0U; --index) {
            if (s_room_slab.pages[index - 1U].source ==
                    ROOM_SLAB_PAGE_RAW) {
                page = &s_room_slab.pages[index - 1U];
                break;
            }
        }

        if (!page) {
            if (!s_room_slab.raw_page_count)
                break;
            return 0;
        }
        if (page->live_count || page->moving_count || !page->base ||
            !isaac_vita_heap_overflow_mspace_internal_page_free(page->base))
            return 0;
        /* Cold preflight above made this exact descriptor authoritative.
         * Free has now consumed backing, so forgetting it cannot refuse. */
        room_slab_forget_page_unchecked(index - 1U);
        if (!room_slab_validate_fast()) {
            s_room_slab.terminal = 1U;
            return 0;
        }
        if (!s_room_slab.raw_page_count)
            break;
    }
    if (s_room_slab.page_count)
        return 0;
    if (s_room_slab.raw_page_count || s_room_slab.external_page_count)
        return 0;
    memset(&s_room_slab, 0, sizeof s_room_slab);
    room_slab_test_controls_clear();
    return 1;
}

void isaac_vita_room_entry_slab_test_probe_reset_locked(void)
{
    s_room_slab_test_classify_probes = 0U;
}

uint32_t isaac_vita_room_entry_slab_test_probe_count_locked(void)
{
    return s_room_slab_test_classify_probes;
}

void isaac_vita_room_entry_slab_test_allocation_probe_reset_locked(void)
{
    s_room_slab_test_allocation_probes = 0U;
}

uint32_t isaac_vita_room_entry_slab_test_allocation_probe_count_locked(void)
{
    return s_room_slab_test_allocation_probes;
}

uint32_t isaac_vita_room_entry_slab_test_floor_lifetime_forget_row_moves_locked(
    void)
{
    return s_room_slab_test_floor_forget_row_moves;
}

void isaac_vita_room_entry_slab_test_fail_next_commit_locked(void)
{
    s_room_slab_test_fail_commit = 1U;
}

void isaac_vita_room_entry_slab_test_fail_next_page_publish_locked(void)
{
    s_room_slab_test_fail_page_publish = 1U;
}

void isaac_vita_room_entry_slab_test_fail_after_external_commit_locked(void)
{
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    s_room_slab_test_fail_after_external_commit = 1U;
#endif
}

void isaac_vita_room_entry_slab_test_corrupt_nonfull_locked(void)
{
    if (s_room_slab.page_count)
        s_room_slab.nonfull_mask[0] ^= UINT32_C(1);
}

void isaac_vita_room_entry_slab_test_corrupt_future_nonfull_locked(void)
{
    if (s_room_slab.page_count < ROOM_SLAB_PAGE_CAP) {
        uint32_t page_index = ROOM_SLAB_PAGE_CAP - 1U;

        s_room_slab.nonfull_mask[page_index >> 5U] ^=
            UINT32_C(1) << (page_index & 31U);
    }
}

void isaac_vita_room_entry_slab_test_corrupt_first_page_source_locked(void)
{
    if (s_room_slab.page_count) {
        s_room_slab.pages[0].source =
            s_room_slab.pages[0].source == ROOM_SLAB_PAGE_RAW ?
                ROOM_SLAB_PAGE_EXTERNAL : ROOM_SLAB_PAGE_RAW;
    }
}

void isaac_vita_room_entry_slab_test_corrupt_first_external_tag_locked(void)
{
    uint32_t index;

    for (index = 0U; index < s_room_slab.page_count; ++index) {
        if (s_room_slab.pages[index].source ==
                ROOM_SLAB_PAGE_EXTERNAL) {
            s_room_slab.pages[index].external_page_index ^= UINT32_C(1);
            break;
        }
    }
}

void isaac_vita_room_entry_slab_test_corrupt_first_external_base_locked(void)
{
    uint32_t index;

    for (index = 0U; index < s_room_slab.page_count; ++index) {
        if (s_room_slab.pages[index].source ==
                ROOM_SLAB_PAGE_EXTERNAL) {
            s_room_slab.pages[index].base = (unsigned char *)(
                (uintptr_t)s_room_slab.pages[index].base ^ (uintptr_t)8U);
            break;
        }
    }
}
#endif
