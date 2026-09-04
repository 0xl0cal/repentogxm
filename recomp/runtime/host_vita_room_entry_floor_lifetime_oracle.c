#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_heap_overflow_mspace.h"
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
#include "host_vita_room_entry_external.h"
#endif
#include "host_vita_room_entry_slab.h"

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
#define ORACLE_PAGE_CAP ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES
#else
#define ORACLE_PAGE_CAP ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES
#endif
#define ORACLE_SLOT_CAP \
    (ORACLE_PAGE_CAP * ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE)

_Alignas(4096) static unsigned char s_raw_backing[
    ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES];
static unsigned char s_raw_page_live[
    ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES];
static void *s_pointers[ORACLE_SLOT_CAP];
static uint32_t s_raw_limit;
static uint32_t s_raw_issued;
static uint32_t s_raw_live;

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
#define ORACLE_EXTERNAL_PAGES \
    (ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES - \
     ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES)
_Alignas(4096) static unsigned char s_external_backing[
    ORACLE_EXTERNAL_PAGES * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES];
static uint32_t s_external_issued;
static uint32_t s_external_pending;
static int s_external_drain;
#endif

#define CHECK(expression) do {                                              \
    if (!(expression)) {                                                    \
        fprintf(stderr, "floor-lifetime oracle failed at %s:%d: %s\n",   \
                __FILE__, __LINE__, #expression);                           \
        return 0;                                                           \
    }                                                                       \
} while (0)

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot)
{
    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    snapshot->uid = 1;
    snapshot->base = (uintptr_t)s_raw_backing;
    snapshot->end = (uintptr_t)s_raw_backing + sizeof s_raw_backing;
    snapshot->retained_end = snapshot->end;
    snapshot->capacity_bytes = sizeof s_raw_backing;
    snapshot->retained_bytes = sizeof s_raw_backing;
    snapshot->live_count = s_raw_live;
    snapshot->internal_live_count = s_raw_live;
    snapshot->internal_requested_bytes =
        (size_t)s_raw_live * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    snapshot->has_mspace = 1;
    return 1;
}

/* Alternate high/low pages.  The second publication sorts before the first,
 * which makes every functional case exercise descriptor-side-row movement. */
void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void)
{
    uint32_t ordinal;
    uint32_t index;
    unsigned char *page;

    if (s_raw_issued >= s_raw_limit)
        return NULL;
    ordinal = s_raw_issued++;
    index = (ordinal & 1U) ? ordinal / 2U :
        ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES - 1U - ordinal / 2U;
    if (index >= ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES ||
        s_raw_page_live[index])
        return NULL;
    s_raw_page_live[index] = 1U;
    ++s_raw_live;
    page = s_raw_backing +
        (size_t)index * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    memset(page, 0xa5, ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
    return page;
}

int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer)
{
    uintptr_t begin = (uintptr_t)s_raw_backing;
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t offset;
    uint32_t index;

    if (value < begin || value >= begin + sizeof s_raw_backing)
        return 0;
    offset = value - begin;
    if (offset % ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
        return 0;
    index = (uint32_t)(offset /
        ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
    if (!s_raw_page_live[index] || !s_raw_live)
        return 0;
    s_raw_page_live[index] = 0U;
    --s_raw_live;
    return 1;
}

#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
static uint32_t external_chunks(void)
{
    return (s_external_issued +
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK - 1U) /
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
}

isaac_vita_room_entry_external_result
isaac_vita_room_entry_external_reserve_page(
    int allow_new_chunk, isaac_vita_room_entry_external_ticket *ticket)
{
    uint32_t page_in_chunk;

    if (!ticket || s_external_pending || s_external_drain)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL;
    if (s_external_issued >= ORACLE_EXTERNAL_PAGES)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_LIMIT;
    page_in_chunk = s_external_issued %
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
    if (!allow_new_chunk && page_in_chunk == 0U)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_EMPTY;
    memset(ticket, 0, sizeof *ticket);
    ticket->page = s_external_backing +
        (size_t)s_external_issued *
            ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    ticket->chunk_index = s_external_issued /
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
    ticket->page_index = page_in_chunk;
    ticket->new_chunk = page_in_chunk == 0U;
    memset(ticket->page, 0xa5,
           ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
    s_external_pending = 1U;
    return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS;
}

int isaac_vita_room_entry_external_commit_page(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    if (!ticket || !s_external_pending ||
        ticket->page != s_external_backing +
            (size_t)s_external_issued *
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES ||
        ticket->chunk_index != s_external_issued /
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK ||
        ticket->page_index != s_external_issued %
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK)
        return 0;
    s_external_pending = 0U;
    ++s_external_issued;
    return 1;
}

int isaac_vita_room_entry_external_cancel_page(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    if (!ticket || !s_external_pending)
        return 0;
    s_external_pending = 0U;
    return 1;
}

int isaac_vita_room_entry_external_unissue_page(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    if (!ticket || s_external_pending || !s_external_issued ||
        ticket->page != s_external_backing +
            (size_t)(s_external_issued - 1U) *
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
        return 0;
    --s_external_issued;
    return 1;
}

int isaac_vita_room_entry_external_page_matches(
    uint32_t chunk_index, uint32_t page_index, const void *page)
{
    uint32_t ordinal = chunk_index *
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK + page_index;

    return ordinal < s_external_issued &&
        page == s_external_backing +
            (size_t)ordinal * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
}

int isaac_vita_room_entry_external_snapshot_get(
    isaac_vita_room_entry_external_snapshot *snapshot)
{
    uint32_t chunks;

    if (!snapshot)
        return 0;
    chunks = external_chunks();
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->state = s_external_drain ?
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY :
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY;
    snapshot->chunks = chunks;
    snapshot->issued_pages = s_external_issued;
    snapshot->pending = s_external_pending;
    snapshot->requested_bytes = (size_t)chunks *
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES;
    snapshot->usable_bytes = (size_t)chunks *
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES;
    snapshot->retained_bytes = (size_t)chunks *
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES;
    snapshot->orphan_uid = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID;
    return 1;
}

int isaac_vita_room_entry_external_test_newest_receipt(
    isaac_vita_room_entry_external_test_receipt *receipt)
{
    uint32_t chunk;
    uint32_t pages;

    if (!receipt || s_external_pending)
        return -1;
    s_external_drain = 1;
    if (!s_external_issued)
        return 0;
    chunk = (s_external_issued - 1U) /
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
    pages = s_external_issued - chunk *
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
    memset(receipt, 0, sizeof *receipt);
    receipt->uid = (int32_t)(1000U + chunk);
    receipt->base = s_external_backing +
        (size_t)chunk * ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES;
    receipt->chunk_index = chunk;
    receipt->issued_pages = pages;
    return 1;
}

int isaac_vita_room_entry_external_test_release_newest(
    const isaac_vita_room_entry_external_test_receipt *receipt)
{
    uint32_t chunk;
    uint32_t pages;

    if (!receipt || !s_external_issued || !s_external_drain)
        return -1;
    chunk = (s_external_issued - 1U) /
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
    pages = s_external_issued - chunk *
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK;
    if (receipt->chunk_index != chunk ||
        receipt->issued_pages != pages)
        return -1;
    s_external_issued -= pages;
    return 1;
}

int isaac_vita_room_entry_external_test_finish_reset(void)
{
    if (s_external_pending || s_external_issued)
        return 0;
    s_external_drain = 0;
    return 1;
}
#endif

static isaac_vita_room_entry_slab_result slab_malloc(void **pointer_out)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_result result;

    result = isaac_vita_room_entry_slab_malloc_locked(
        ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
        ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, &decision, NULL);
    if (pointer_out)
        *pointer_out = decision.pointer;
    return result;
}

static isaac_vita_room_entry_slab_result slab_free(void *pointer)
{
    isaac_vita_room_entry_slab_decision decision;

    return isaac_vita_room_entry_slab_free_locked(
        pointer, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        NULL, &decision, NULL);
}

static int floor_snapshot(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot)
{
    memset(snapshot, 0xcc, sizeof *snapshot);
    return isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(
        snapshot);
}

static int pristine_floor_state(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;

    return floor_snapshot(&snapshot) && snapshot.live_slots == 0U &&
        snapshot.unscoped_slots == 0U && snapshot.prior_slots == 0U &&
        snapshot.current_slots == 0U && snapshot.level_init_slots == 0U &&
        snapshot.room_load_slots == 0U && snapshot.play_slots == 0U &&
        snapshot.epoch == 0U && snapshot.phase == 0U &&
        snapshot.active == 0U && snapshot.valid == 1U &&
        snapshot.terminal == 0U && snapshot.counter_saturated == 0U &&
        isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked();
}

static int reset_and_assert_pristine(void)
{
    return isaac_vita_room_entry_slab_test_reset_locked() &&
        pristine_floor_state();
}

static int clean_state(uint32_t raw_limit)
{
    uint32_t index;

    if (!reset_and_assert_pristine())
        return 0;
    if (s_raw_live)
        return 0;
    for (index = 0U;
         index < ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES; ++index) {
        if (s_raw_page_live[index])
            return 0;
    }
    s_raw_limit = raw_limit;
    s_raw_issued = 0U;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    if (s_external_issued || s_external_pending)
        return 0;
    s_external_drain = 0;
#endif
    memset(s_pointers, 0, sizeof s_pointers);
    return 1;
}

static int test_epoch_phase_and_realloc(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    isaac_vita_room_entry_slab_decision decision;
    void *unscoped;
    void *level;
    void *room;
    void *play;

    CHECK(clean_state(4U));
    CHECK(slab_malloc(&unscoped) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(floor_snapshot(&snapshot) && snapshot.live_slots == 1U &&
          snapshot.unscoped_slots == 1U && !snapshot.active &&
          snapshot.valid && !snapshot.counter_saturated);
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT));
    CHECK(slab_malloc(&level) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD));
    CHECK(slab_malloc(&room) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY));
    CHECK(slab_malloc(&play) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(floor_snapshot(&snapshot) && snapshot.live_slots == 4U &&
          snapshot.unscoped_slots == 1U && snapshot.prior_slots == 0U &&
          snapshot.current_slots == 3U && snapshot.level_init_slots == 1U &&
          snapshot.room_load_slots == 1U && snapshot.play_slots == 1U &&
          snapshot.epoch == 1U && snapshot.phase ==
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());

    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              play, 24U, NULL, &decision, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE &&
          decision.floorlife_token ==
              (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY |
               ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT));
    CHECK(isaac_vita_room_entry_slab_realloc_cancel_locked(play, NULL));
    CHECK(floor_snapshot(&snapshot) && snapshot.current_slots == 3U &&
          snapshot.play_slots == 1U);
    /* This second cancel is the generic-allocation-failure path: the old
     * birth token and every category remain byte-for-byte live. */
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              play, 24U, NULL, &decision, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE);
    CHECK(isaac_vita_room_entry_slab_realloc_cancel_locked(play, NULL));
    CHECK(floor_snapshot(&snapshot) && snapshot.live_slots == 4U &&
          snapshot.current_slots == 3U && snapshot.play_slots == 1U);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              play, 24U, NULL, &decision, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE);
    CHECK(isaac_vita_room_entry_slab_realloc_commit_locked(play, NULL));
    CHECK(floor_snapshot(&snapshot) && snapshot.live_slots == 3U &&
          snapshot.current_slots == 2U && snapshot.play_slots == 0U);

    CHECK(isaac_vita_room_entry_slab_floor_lifetime_rollover_locked());
    CHECK(floor_snapshot(&snapshot) && snapshot.unscoped_slots == 1U &&
          snapshot.prior_slots == 2U && snapshot.current_slots == 0U &&
          snapshot.level_init_slots == 0U &&
          snapshot.room_load_slots == 0U && snapshot.play_slots == 0U &&
          snapshot.epoch == 2U);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              level, 24U, NULL, &decision, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE &&
          decision.floorlife_token ==
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT);
    CHECK(isaac_vita_room_entry_slab_realloc_commit_locked(level, NULL));
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              room, 0U, NULL, &decision, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.floorlife_token ==
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD);
    CHECK(slab_free(unscoped) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(floor_snapshot(&snapshot) && snapshot.live_slots == 0U &&
          snapshot.unscoped_slots == 0U && snapshot.prior_slots == 0U &&
          snapshot.current_slots == 0U);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(reset_and_assert_pristine());
    return 1;
}

static int test_three_page_phase_permutation(
    uint32_t raw_limit, int expect_high_low_middle)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    isaac_vita_room_entry_slab_decision decision;
    const uint32_t slots = ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
    const uint32_t total = ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE * 2U + 1U;
    uint32_t index;

    CHECK(clean_state(raw_limit));
    CHECK(slab_malloc(&s_pointers[0]) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT));
    for (index = 1U; index < slots; ++index) {
        CHECK(slab_malloc(&s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_rollover_locked());
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD));
    for (index = slots; index < slots * 2U; ++index) {
        CHECK(slab_malloc(&s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY));
    CHECK(slab_malloc(&s_pointers[slots * 2U]) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    if (expect_high_low_middle) {
        CHECK((uintptr_t)s_pointers[slots] <
                  (uintptr_t)s_pointers[slots * 2U] &&
              (uintptr_t)s_pointers[slots * 2U] <
                  (uintptr_t)s_pointers[0]);
    }
    CHECK(floor_snapshot(&snapshot) && snapshot.live_slots == total &&
          snapshot.unscoped_slots == 1U &&
          snapshot.prior_slots == slots - 1U &&
          snapshot.current_slots == slots + 1U &&
          snapshot.level_init_slots == 0U &&
          snapshot.room_load_slots == slots &&
          snapshot.play_slots == 1U);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              s_pointers[0], ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
              NULL, &decision, NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.pointer == s_pointers[0] &&
          decision.floorlife_token == 0U);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              s_pointers[1], ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
              NULL, &decision, NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.pointer == s_pointers[1] &&
          decision.floorlife_token ==
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              s_pointers[slots], ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
              NULL, &decision, NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.pointer == s_pointers[slots] &&
          decision.floorlife_token ==
              (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD |
               ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT));
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              s_pointers[slots * 2U],
              ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
              NULL, &decision, NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.pointer == s_pointers[slots * 2U] &&
          decision.floorlife_token ==
              (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY |
               ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT));
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    for (index = 0U; index < total; ++index) {
        CHECK(slab_free(s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(reset_and_assert_pristine());
    return 1;
}

static int test_full_page_bound(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot floor;
    isaac_vita_room_entry_slab_snapshot slab;
    uint32_t index;

    CHECK(clean_state(ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES));
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY));
    for (index = 0U; index < ORACLE_SLOT_CAP; ++index) {
        CHECK(slab_malloc(&s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&slab) &&
          slab.pages == ORACLE_PAGE_CAP &&
          slab.live_slots == ORACLE_SLOT_CAP);
    CHECK(floor_snapshot(&floor) && floor.live_slots == ORACLE_SLOT_CAP &&
          floor.unscoped_slots == 0U && floor.prior_slots == 0U &&
          floor.current_slots == ORACLE_SLOT_CAP &&
          floor.play_slots == ORACLE_SLOT_CAP);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_rollover_locked());
    CHECK(floor_snapshot(&floor) && floor.prior_slots == ORACLE_SLOT_CAP &&
          floor.current_slots == 0U && floor.play_slots == 0U);
    for (index = 0U; index < ORACLE_SLOT_CAP; ++index) {
        CHECK(slab_free(s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(reset_and_assert_pristine());
    return 1;
}

static int test_open_move_blocks_rollover(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    isaac_vita_room_entry_slab_decision decision;
    void *pointer;

    CHECK(clean_state(1U));
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY));
    CHECK(slab_malloc(&pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 24U, NULL, &decision, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE &&
          decision.floorlife_token ==
              (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY |
               ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT));
    CHECK(!isaac_vita_room_entry_slab_floor_lifetime_rollover_locked());
    CHECK(!floor_snapshot(&snapshot) && !snapshot.valid &&
          !snapshot.terminal && !snapshot.counter_saturated &&
          snapshot.epoch == 1U && snapshot.active == 1U &&
          snapshot.phase == ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY &&
          snapshot.live_slots == 1U && snapshot.unscoped_slots == 0U &&
          snapshot.prior_slots == 0U && snapshot.current_slots == 1U &&
          snapshot.level_init_slots == 0U &&
          snapshot.room_load_slots == 0U && snapshot.play_slots == 1U);
    CHECK(isaac_vita_room_entry_slab_realloc_cancel_locked(pointer, NULL));
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(reset_and_assert_pristine());
    return 1;
}

static int test_invalid_before_bootstrap_is_diagnostic_only(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    isaac_vita_room_entry_slab_decision decision;
    void *pointer;

    CHECK(clean_state(1U));
    CHECK(!isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT));
    CHECK(!floor_snapshot(&snapshot) && !snapshot.valid &&
          !snapshot.terminal && !snapshot.counter_saturated &&
          !snapshot.active && snapshot.epoch == 0U &&
          snapshot.live_slots == 0U);
    CHECK(!isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT));
    CHECK(slab_malloc(&pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
              NULL, &decision, NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.pointer == pointer && decision.floorlife_token == 0U);
    CHECK(!floor_snapshot(&snapshot) && snapshot.live_slots == 1U &&
          snapshot.unscoped_slots == 1U && snapshot.current_slots == 0U &&
          !snapshot.active && snapshot.epoch == 0U &&
          !snapshot.terminal && !snapshot.counter_saturated);
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(reset_and_assert_pristine());
    return 1;
}

static int test_cold_corruption_hooks(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    void *pointer;

    CHECK(clean_state(1U));
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD));
    CHECK(slab_malloc(&pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_phase_locked());
    CHECK(!isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_phase_locked());
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_current_locked());
    CHECK(!isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_current_locked());
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(floor_snapshot(&snapshot) && snapshot.valid &&
          snapshot.current_slots == 1U && snapshot.level_init_slots == 0U &&
          snapshot.room_load_slots == 1U && snapshot.play_slots == 0U);
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(reset_and_assert_pristine());
    return 1;
}

static int test_failed_publish_has_no_floor_ghost(uint32_t raw_limit)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    void *pointer = (void *)(uintptr_t)1U;

    CHECK(clean_state(raw_limit));
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY));
    isaac_vita_room_entry_slab_test_fail_next_page_publish_locked();
    CHECK(slab_malloc(&pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL &&
          pointer == NULL);
    CHECK(floor_snapshot(&snapshot) && snapshot.valid &&
          snapshot.terminal && !snapshot.counter_saturated &&
          snapshot.live_slots == 0U && snapshot.unscoped_slots == 0U &&
          snapshot.prior_slots == 0U && snapshot.current_slots == 0U &&
          snapshot.level_init_slots == 0U &&
          snapshot.room_load_slots == 0U && snapshot.play_slots == 0U);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    CHECK(reset_and_assert_pristine());
    return 1;
}

/* Must be the final case in this process: it deliberately terminalizes the
 * slab while 8,064 allocations remain live, so test reset correctly refuses
 * teardown.  The raw allocator publishes high, low, then middle pages.  The
 * injected failure removes the middle descriptor at index 1 of 3, forcing a
 * non-zero forget_page memmove of both lifetime side rows. */
static int test_failed_middle_publish_moves_rows(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot floor;
    isaac_vita_room_entry_slab_snapshot slab;
    const uint32_t slots = ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
    const uint32_t live = ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE * 2U;
    uint32_t index;
    void *failed = (void *)(uintptr_t)1U;

    CHECK(clean_state(3U));
    CHECK(slab_malloc(&s_pointers[0]) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT));
    for (index = 1U; index < slots; ++index) {
        CHECK(slab_malloc(&s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_rollover_locked());
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD));
    for (index = slots; index < live; ++index) {
        CHECK(slab_malloc(&s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    CHECK((uintptr_t)s_pointers[slots] < (uintptr_t)s_pointers[0]);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_forget_row_moves_locked() == 0U);
    isaac_vita_room_entry_slab_test_fail_next_page_publish_locked();
    CHECK(slab_malloc(&failed) == ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL &&
          failed == NULL);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_forget_row_moves_locked() == 1U);
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&slab) &&
          slab.terminal && slab.pages == 2U && slab.live_slots == live);
    CHECK(floor_snapshot(&floor) && floor.valid && floor.terminal &&
          !floor.counter_saturated && floor.live_slots == live &&
          floor.unscoped_slots == 1U && floor.prior_slots == slots - 1U &&
          floor.current_slots == slots && floor.level_init_slots == 0U &&
          floor.room_load_slots == slots && floor.play_slots == 0U &&
          floor.epoch == 2U &&
          floor.phase == ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD);
    CHECK(isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked());
    return 1;
}

static int test_saturation_is_diagnostic_only(void)
{
    isaac_vita_room_entry_slab_floor_lifetime_snapshot snapshot;
    void *pointer;

    CHECK(clean_state(1U));
    CHECK(isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
              ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT));
    CHECK(slab_malloc(&pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    isaac_vita_room_entry_slab_test_floor_lifetime_force_epoch_max_locked();
    CHECK(!isaac_vita_room_entry_slab_floor_lifetime_rollover_locked());
    CHECK(!floor_snapshot(&snapshot) && !snapshot.valid &&
          snapshot.counter_saturated && !snapshot.terminal &&
          snapshot.epoch == UINT32_MAX && snapshot.live_slots == 1U &&
          snapshot.prior_slots == 0U && snapshot.current_slots == 1U &&
          snapshot.level_init_slots == 1U &&
          snapshot.room_load_slots == 0U && snapshot.play_slots == 0U);
    /* Saturation did not clear CURRENT and allocator ownership is unchanged. */
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(!floor_snapshot(&snapshot) && snapshot.live_slots == 0U &&
          snapshot.current_slots == 0U && snapshot.level_init_slots == 0U &&
          snapshot.counter_saturated && !snapshot.terminal);
    CHECK(reset_and_assert_pristine());
    return 1;
}

int main(void)
{
    if (!test_epoch_phase_and_realloc() ||
        !test_three_page_phase_permutation(3U, 1) ||
        !test_full_page_bound() ||
        !test_open_move_blocks_rollover() ||
        !test_invalid_before_bootstrap_is_diagnostic_only() ||
        !test_cold_corruption_hooks() ||
        !test_failed_publish_has_no_floor_ghost(1U) ||
        !test_saturation_is_diagnostic_only())
        return 1;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    if (!test_three_page_phase_permutation(0U, 0) ||
        !test_failed_publish_has_no_floor_ghost(0U))
        return 1;
    if (!test_failed_middle_publish_moves_rows())
        return 1;
    puts("Vita RoomConfig floor-lifetime Hybrid192 oracle: PASS");
#else
    if (!test_failed_middle_publish_moves_rows())
        return 1;
    puts("Vita RoomConfig floor-lifetime Raw96 oracle: PASS");
#endif
    return 0;
}
