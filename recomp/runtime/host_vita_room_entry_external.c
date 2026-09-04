/* Lazy USER_RW backing for the optional RoomConfig Entry hybrid slab tier. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_room_entry_external.h"

typedef struct room_external_chunk {
    SceUID uid;
    unsigned char *base;
    uintptr_t request_end;
    uintptr_t usable_end;
    uintptr_t retained_end;
    uint32_t issued_pages;
} room_external_chunk;

typedef struct room_external_storage {
    isaac_vita_room_entry_external_state state;
    isaac_vita_heap_overflow_forbidden_ranges forbidden;
    isaac_vita_heap_overflow_range raw_request;
    room_external_chunk chunks[ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS];
    room_external_chunk orphan;
    isaac_vita_room_entry_external_ticket pending_ticket;
    uint32_t chunk_count;
    uint32_t issued_pages;
    uint32_t pending;
    uint32_t allocation_attempts;
    uint32_t out_of_memory;
    uint32_t post_uid_failures;
    uint32_t rollback_free_attempts;
    uint32_t rollback_free_failures;
    uint32_t reset_free_attempts;
    uint32_t reset_free_failures;
    uint32_t counter_saturated;
    isaac_vita_room_entry_external_fault terminal_fault;
    isaac_vita_room_entry_external_syscall last_syscall;
    int32_t last_syscall_result;
} room_external_storage;

static room_external_storage s_external = {
    .state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_UNINITIALIZED,
    .orphan = {
        .uid = (SceUID)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID
    }
};

_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES ==
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES *
                       ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK,
               "external chunk/page layout drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES ==
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES + 0x1000U,
               "external actual request receipt drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES ==
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES + 0x1000U,
               "external conservative retained receipt drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS *
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK ==
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_ISSUED_PAGES,
               "external compensation bound drifted");

static int external_range_valid(
    const isaac_vita_heap_overflow_range *range)
{
    return range && range->begin < range->end;
}

static int external_forbidden_valid(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden)
{
    return forbidden &&
        external_range_valid(&forbidden->fixed_image) &&
        external_range_valid(&forbidden->guest_stack) &&
        external_range_valid(&forbidden->retained_newlib);
}

static int external_overlap(
    uintptr_t begin, uintptr_t end,
    const isaac_vita_heap_overflow_range *range)
{
    return begin < range->end && range->begin < end;
}

static int external_manual_overlap(
    uintptr_t begin, uintptr_t end,
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden)
{
    return external_overlap(begin, end, &forbidden->fixed_image) ||
        external_overlap(begin, end, &forbidden->guest_stack) ||
        external_overlap(begin, end, &forbidden->retained_newlib);
}

static void external_chunk_clear(room_external_chunk *chunk)
{
    memset(chunk, 0, sizeof *chunk);
    chunk->uid = (SceUID)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID;
}

static void *external_page_address(const room_external_chunk *chunk,
                                   uint32_t page_index)
{
    return (void *)((uintptr_t)chunk->base +
        (uintptr_t)page_index *
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES);
}

static void external_pending_clear(void)
{
    memset(&s_external.pending_ticket, 0,
           sizeof s_external.pending_ticket);
    s_external.pending = 0U;
}

static int external_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        ++*counter;
        return 1;
    }
    s_external.counter_saturated = 1U;
    if (s_external.terminal_fault ==
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
        s_external.terminal_fault =
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_COUNTER;
    if (s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY;
    return 0;
}

static isaac_vita_room_entry_external_result external_terminal(
    isaac_vita_room_entry_external_fault fault)
{
    if (s_external.terminal_fault ==
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
        s_external.terminal_fault = fault;
    if (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED)
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY;
    return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL;
}

static int external_ticket_equal(
    const isaac_vita_room_entry_external_ticket *first,
    const isaac_vita_room_entry_external_ticket *second)
{
    return first && second && first->page == second->page &&
        first->chunk_index == second->chunk_index &&
        first->page_index == second->page_index &&
        first->new_chunk == second->new_chunk;
}

static isaac_vita_room_entry_external_fault external_manual_fault(
    uintptr_t begin, uintptr_t end, int retained)
{
    if (external_overlap(begin, end, &s_external.forbidden.fixed_image))
        return retained ?
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_FIXED :
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_FIXED;
    if (external_overlap(begin, end, &s_external.forbidden.guest_stack))
        return retained ?
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_STACK :
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_STACK;
    if (external_overlap(begin, end, &s_external.forbidden.retained_newlib))
        return retained ?
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_NEWLIB :
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_NEWLIB;
    return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE;
}

static isaac_vita_room_entry_external_fault
external_actual_owned_fault(uintptr_t begin, uintptr_t end)
{
    uint32_t index;

    if (external_overlap(begin, end, &s_external.raw_request))
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_RAW;
    if (s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_STATE;
    for (index = 0U; index < s_external.chunk_count; ++index) {
        isaac_vita_heap_overflow_range request;

        request.begin = (uintptr_t)s_external.chunks[index].base;
        request.end = s_external.chunks[index].request_end;
        if (external_overlap(begin, end, &request))
            return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_PRIOR;
    }
    return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE;
}

static int external_uid_owned(SceUID uid)
{
    uint32_t index;

    if (s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS)
        return 1;
    if (s_external.orphan.uid == uid)
        return 1;
    for (index = 0U; index < s_external.chunk_count; ++index) {
        if (s_external.chunks[index].uid == uid)
            return 1;
    }
    return 0;
}

static isaac_vita_room_entry_external_result
external_fail_after_uid(SceUID uid, void *base,
                        uintptr_t request_end, uintptr_t usable_end,
                        uintptr_t retained_end,
                        isaac_vita_room_entry_external_fault fault)
{
    room_external_chunk receipt;
    int free_result;

    external_chunk_clear(&receipt);
    receipt.uid = uid;
    receipt.base = (unsigned char *)base;
    receipt.request_end = request_end;
    receipt.usable_end = usable_end;
    receipt.retained_end = retained_end;
    if (s_external.terminal_fault ==
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
        s_external.terminal_fault = fault;
    s_external.last_syscall =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ROLLBACK_FREE;
    free_result = sceKernelFreeMemBlock(uid);
    s_external.last_syscall_result = free_result;
    (void)external_increment(&s_external.post_uid_failures);
    (void)external_increment(&s_external.rollback_free_attempts);
    if (free_result < 0) {
        (void)external_increment(&s_external.rollback_free_failures);
        s_external.orphan = receipt;
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED;
    }
    else {
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY;
    }
    return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL;
}

static int external_release_new_empty_chunk(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    room_external_chunk *chunk;
    int free_result;

    if (!ticket || !ticket->new_chunk ||
        s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        ticket->chunk_index >= s_external.chunk_count ||
        ticket->chunk_index + 1U != s_external.chunk_count)
        return 1;
    chunk = &s_external.chunks[ticket->chunk_index];
    if (chunk->issued_pages)
        return 0;
    s_external.last_syscall =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ROLLBACK_FREE;
    free_result = sceKernelFreeMemBlock(chunk->uid);
    s_external.last_syscall_result = free_result;
    (void)external_increment(&s_external.rollback_free_attempts);
    if (free_result < 0) {
        (void)external_increment(&s_external.rollback_free_failures);
        if (s_external.terminal_fault ==
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
            s_external.terminal_fault =
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION;
        s_external.orphan = *chunk;
        external_chunk_clear(chunk);
        --s_external.chunk_count;
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED;
        return 0;
    }
    external_chunk_clear(chunk);
    --s_external.chunk_count;
    return 1;
}

static int external_storage_valid(void);

int isaac_vita_room_entry_external_init(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden,
    const isaac_vita_heap_overflow_range *raw_request)
{
    if (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_UNINITIALIZED)
        return 0;
    if (!external_forbidden_valid(forbidden) ||
        !external_range_valid(raw_request) ||
        external_manual_overlap(raw_request->begin, raw_request->end,
                                forbidden)) {
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAILED;
        s_external.terminal_fault =
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_INIT;
        return 0;
    }
    s_external.forbidden = *forbidden;
    s_external.raw_request = *raw_request;
    external_chunk_clear(&s_external.orphan);
    s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY;
    return 1;
}

isaac_vita_room_entry_external_result
isaac_vita_room_entry_external_reserve_page(
    int allow_new_chunk, isaac_vita_room_entry_external_ticket *ticket_out)
{
    room_external_chunk *chunk;
    isaac_vita_room_entry_external_ticket ticket;
    SceUID uid;
    int get_base_result;
    void *base = NULL;
    uintptr_t begin;
    uintptr_t request_end;
    uintptr_t usable_end;
    uintptr_t retained_end;
    isaac_vita_room_entry_external_fault fault;

    if (ticket_out)
        memset(ticket_out, 0, sizeof *ticket_out);
    if (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL;
    if (!ticket_out || s_external.pending || !external_storage_valid())
        return external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_STATE);
    if (s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS)
        return external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_STATE);
    if (s_external.chunk_count) {
        chunk = &s_external.chunks[s_external.chunk_count - 1U];
        if (chunk->issued_pages <
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK) {
            ticket.chunk_index = s_external.chunk_count - 1U;
            ticket.page_index = chunk->issued_pages;
            ticket.page = external_page_address(chunk, ticket.page_index);
            ticket.new_chunk = 0U;
            s_external.pending_ticket = ticket;
            s_external.pending = 1U;
            *ticket_out = ticket;
            return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS;
        }
    }
    if (!allow_new_chunk)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_EMPTY;
    if (s_external.chunk_count >=
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS)
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_LIMIT;
    if (!external_increment(&s_external.allocation_attempts))
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL;
    s_external.last_syscall =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ALLOC;
    uid = sceKernelAllocMemBlock(
        "isaac_room_entries", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        (SceSize)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES, NULL);
    s_external.last_syscall_result = (int32_t)uid;
    if (uid < 0) {
        if (!external_increment(&s_external.out_of_memory))
            return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL;
        return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_OUT_OF_MEMORY;
    }
    if (external_uid_owned(uid))
        return external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_DUPLICATE_UID);
    s_external.last_syscall =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_GET_BASE;
    get_base_result = sceKernelGetMemBlockBase(uid, &base);
    s_external.last_syscall_result = get_base_result;
    if (get_base_result < 0)
        return external_fail_after_uid(
            uid, base, 0U, 0U, 0U,
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_GET_BASE);
    if (!base)
        return external_fail_after_uid(
            uid, base, 0U, 0U, 0U,
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NULL_BASE);
    begin = (uintptr_t)base;
    if ((begin & 0xfffU) != 0U)
        return external_fail_after_uid(
            uid, base, 0U, 0U, 0U,
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ALIGNMENT);
    if (begin > UINTPTR_MAX -
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES)
        return external_fail_after_uid(
            uid, base, 0U, 0U, 0U,
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_WRAP);
    request_end = begin + ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES;
    usable_end = begin + ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES;
    if (begin > UINTPTR_MAX -
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES)
        return external_fail_after_uid(
            uid, base, request_end, usable_end, 0U,
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_WRAP);
    retained_end = begin + ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES;
    fault = external_manual_fault(begin, request_end, 0);
    if (fault == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
        fault = external_actual_owned_fault(begin, request_end);
    if (fault == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
        fault = external_manual_fault(begin, retained_end, 1);
    if (fault != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE)
        return external_fail_after_uid(
            uid, base, request_end, usable_end, retained_end, fault);

    chunk = &s_external.chunks[s_external.chunk_count];
    external_chunk_clear(chunk);
    chunk->uid = uid;
    chunk->base = (unsigned char *)base;
    chunk->request_end = request_end;
    chunk->usable_end = usable_end;
    chunk->retained_end = retained_end;
    ticket.chunk_index = s_external.chunk_count;
    ticket.page_index = 0U;
    ticket.page = base;
    ticket.new_chunk = 1U;
    ++s_external.chunk_count;
    s_external.pending_ticket = ticket;
    s_external.pending = 1U;
    *ticket_out = ticket;
    return ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS;
}

int isaac_vita_room_entry_external_commit_page(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    room_external_chunk *chunk;

    if (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        return 0;
    if (!external_storage_valid() || !s_external.pending ||
        !external_ticket_equal(ticket, &s_external.pending_ticket) ||
        s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        ticket->chunk_index >= s_external.chunk_count ||
        s_external.issued_pages >=
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_ISSUED_PAGES) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    chunk = &s_external.chunks[ticket->chunk_index];
    if (ticket->page_index != chunk->issued_pages ||
        chunk->issued_pages >=
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    ++chunk->issued_pages;
    ++s_external.issued_pages;
    external_pending_clear();
    if (!external_storage_valid()) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    return 1;
}

int isaac_vita_room_entry_external_cancel_page(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    int released;

    if (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        return 0;
    if (!external_storage_valid() || !s_external.pending ||
        !external_ticket_equal(ticket, &s_external.pending_ticket)) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    external_pending_clear();
    released = external_release_new_empty_chunk(ticket);
    if (!released || s_external.state !=
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        return 0;
    if (!external_storage_valid()) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    return 1;
}

int isaac_vita_room_entry_external_unissue_page(
    const isaac_vita_room_entry_external_ticket *ticket)
{
    room_external_chunk *chunk;
    int released;

    if (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        return 0;
    if (!external_storage_valid() || !ticket || s_external.pending ||
        s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        ticket->chunk_index + 1U != s_external.chunk_count ||
        ticket->chunk_index >= s_external.chunk_count ||
        !s_external.issued_pages) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    chunk = &s_external.chunks[ticket->chunk_index];
    if (!chunk->issued_pages ||
        ticket->page_index + 1U != chunk->issued_pages ||
        ticket->new_chunk != (ticket->page_index == 0U) ||
        ticket->page != external_page_address(chunk, ticket->page_index)) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    --chunk->issued_pages;
    --s_external.issued_pages;
    released = external_release_new_empty_chunk(ticket);
    if (!released || s_external.state !=
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        return 0;
    if (!external_storage_valid()) {
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION);
        return 0;
    }
    return 1;
}

int isaac_vita_room_entry_external_page_matches(
    uint32_t chunk_index, uint32_t page_index, const void *page)
{
    room_external_chunk *chunk;

    if (!page ||
        s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        chunk_index >= s_external.chunk_count)
        return 0;
    chunk = &s_external.chunks[chunk_index];
    return page_index < chunk->issued_pages &&
        page_index < ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK &&
        page == external_page_address(chunk, page_index);
}

static int external_chunk_valid(uint32_t index,
                                const room_external_chunk *chunk)
{
    uintptr_t begin;
    uint32_t prior_index;

    if (!chunk || chunk->uid < 0 || !chunk->base ||
        chunk->issued_pages >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK)
        return 0;
    begin = (uintptr_t)chunk->base;
    if ((begin & 0xfffU) != 0U ||
        begin > UINTPTR_MAX -
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES ||
        chunk->request_end != begin +
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES ||
        chunk->usable_end != begin +
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES ||
        chunk->retained_end != begin +
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES ||
        external_manual_overlap(begin, chunk->request_end,
                                &s_external.forbidden) ||
        external_manual_overlap(begin, chunk->retained_end,
                                &s_external.forbidden) ||
        external_overlap(begin, chunk->request_end,
                         &s_external.raw_request))
        return 0;
    for (prior_index = 0U; prior_index < index; ++prior_index) {
        isaac_vita_heap_overflow_range prior;

        if (s_external.chunks[prior_index].uid == chunk->uid)
            return 0;
        prior.begin = (uintptr_t)s_external.chunks[prior_index].base;
        prior.end = s_external.chunks[prior_index].request_end;
        if (external_overlap(begin, chunk->request_end, &prior))
            return 0;
    }
    return 1;
}

static int external_storage_valid(void)
{
    uint32_t issued = 0U;
    uint32_t index;

    /* Check every attacker-controlled bound before indexing fixed storage. */
    if ((unsigned)s_external.state >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED ||
        s_external.chunk_count >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        s_external.chunk_count +
                (s_external.orphan.uid >= 0 ? 1U : 0U) >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS ||
        s_external.issued_pages >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_ISSUED_PAGES ||
        s_external.pending > 1U ||
        s_external.counter_saturated > 1U ||
        (unsigned)s_external.terminal_fault >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION ||
        (unsigned)s_external.last_syscall >
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_RESET_FREE ||
        s_external.reset_free_failures > s_external.reset_free_attempts ||
        s_external.rollback_free_failures >
            s_external.rollback_free_attempts ||
        s_external.post_uid_failures >
            s_external.rollback_free_attempts ||
        s_external.post_uid_failures >
            s_external.allocation_attempts ||
        s_external.out_of_memory > s_external.allocation_attempts ||
        (uint64_t)s_external.out_of_memory +
                s_external.post_uid_failures >
            s_external.allocation_attempts)
        return 0;
    if (s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_UNINITIALIZED ||
        s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAILED) {
        return !s_external.chunk_count && !s_external.issued_pages &&
            !s_external.pending &&
            s_external.orphan.uid ==
                (SceUID)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID &&
            !s_external.orphan.base && !s_external.orphan.request_end &&
            !s_external.orphan.usable_end &&
            !s_external.orphan.retained_end &&
            !s_external.orphan.issued_pages;
    }
    if (!external_forbidden_valid(&s_external.forbidden) ||
        !external_range_valid(&s_external.raw_request) ||
        external_manual_overlap(s_external.raw_request.begin,
                                s_external.raw_request.end,
                                &s_external.forbidden) ||
        (s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED) !=
            (s_external.orphan.uid >= 0) ||
        (s_external.orphan.uid >= 0 &&
         s_external.orphan.issued_pages != 0U) ||
        (s_external.orphan.uid < 0 &&
         (s_external.orphan.uid !=
              (SceUID)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID ||
          s_external.orphan.base || s_external.orphan.request_end ||
          s_external.orphan.usable_end ||
          s_external.orphan.retained_end ||
          s_external.orphan.issued_pages)) ||
        (s_external.counter_saturated &&
         s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY) ||
        (s_external.state != ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY &&
         s_external.pending))
        return 0;
    if (s_external.orphan.uid >= 0) {
        for (index = 0U; index < s_external.chunk_count; ++index) {
            if (s_external.chunks[index].uid == s_external.orphan.uid)
                return 0;
        }
    }
    for (index = 0U; index < s_external.chunk_count; ++index) {
        const room_external_chunk *chunk = &s_external.chunks[index];

        if (!external_chunk_valid(index, chunk))
            return 0;
        /* A zero-page chunk exists only as a pending newly-allocated receipt. */
        if (!chunk->issued_pages) {
            if (!s_external.pending ||
                index + 1U != s_external.chunk_count ||
                !s_external.pending_ticket.new_chunk)
                return 0;
        }
        if (index + 1U < s_external.chunk_count &&
            chunk->issued_pages !=
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK)
            return 0;
        issued += chunk->issued_pages;
    }
    if (issued != s_external.issued_pages)
        return 0;
    if (s_external.pending) {
        const isaac_vita_room_entry_external_ticket *ticket =
            &s_external.pending_ticket;
        const room_external_chunk *chunk;

        if (!s_external.chunk_count ||
            ticket->chunk_index + 1U != s_external.chunk_count)
            return 0;
        chunk = &s_external.chunks[ticket->chunk_index];
        if (chunk->uid < 0 || !chunk->base ||
            ticket->page_index != chunk->issued_pages ||
            ticket->page_index >=
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK ||
            ticket->page !=
                external_page_address(chunk, ticket->page_index) ||
            ticket->new_chunk != (chunk->issued_pages == 0U) ||
            !external_chunk_valid(ticket->chunk_index, chunk))
            return 0;
    }
    return 1;
}

int isaac_vita_room_entry_external_snapshot_get(
    isaac_vita_room_entry_external_snapshot *snapshot_out)
{
    if (!snapshot_out)
        return 0;
    memset(snapshot_out, 0, sizeof *snapshot_out);
    if (!external_storage_valid())
        return 0;
    snapshot_out->state = s_external.state;
    snapshot_out->chunks = s_external.chunk_count;
    snapshot_out->issued_pages = s_external.issued_pages;
    snapshot_out->pending = s_external.pending;
    snapshot_out->allocation_attempts = s_external.allocation_attempts;
    snapshot_out->out_of_memory = s_external.out_of_memory;
    snapshot_out->post_uid_failures = s_external.post_uid_failures;
    snapshot_out->rollback_free_attempts =
        s_external.rollback_free_attempts;
    snapshot_out->rollback_free_failures =
        s_external.rollback_free_failures;
    snapshot_out->reset_free_attempts = s_external.reset_free_attempts;
    snapshot_out->reset_free_failures = s_external.reset_free_failures;
    snapshot_out->counter_saturated = s_external.counter_saturated;
    snapshot_out->terminal_fault = s_external.terminal_fault;
    snapshot_out->last_syscall = s_external.last_syscall;
    snapshot_out->last_syscall_result = s_external.last_syscall_result;
    snapshot_out->requested_bytes =
        (size_t)s_external.chunk_count *
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES;
    snapshot_out->usable_bytes =
        (size_t)s_external.chunk_count *
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES;
    snapshot_out->retained_bytes =
        (size_t)s_external.chunk_count *
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES;
    snapshot_out->orphan_uid = (int32_t)s_external.orphan.uid;
    if (s_external.orphan.uid >= 0) {
        snapshot_out->orphan_requested_bytes =
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES;
        snapshot_out->orphan_retained_bytes =
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES;
    }
    return 1;
}

#ifdef ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING
static int external_test_receipt_equal(
    const isaac_vita_room_entry_external_test_receipt *first,
    const isaac_vita_room_entry_external_test_receipt *second)
{
    return first && second && first->uid == second->uid &&
        first->base == second->base &&
        first->chunk_index == second->chunk_index &&
        first->issued_pages == second->issued_pages &&
        first->orphan == second->orphan;
}

int isaac_vita_room_entry_external_test_newest_receipt(
    isaac_vita_room_entry_external_test_receipt *receipt_out)
{
    room_external_chunk *chunk;

    if (!receipt_out)
        return -1;
    memset(receipt_out, 0, sizeof *receipt_out);
    receipt_out->uid = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID;
    if (!external_storage_valid() || s_external.pending)
        return -1;
    if (s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY)
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY;
    if (s_external.orphan.uid >= 0) {
        receipt_out->uid = (int32_t)s_external.orphan.uid;
        receipt_out->base = s_external.orphan.base;
        receipt_out->chunk_index = UINT32_MAX;
        receipt_out->orphan = 1U;
        return 1;
    }
    if (!s_external.chunk_count)
        return 0;
    chunk = &s_external.chunks[s_external.chunk_count - 1U];
    receipt_out->uid = (int32_t)chunk->uid;
    receipt_out->base = chunk->base;
    receipt_out->chunk_index = s_external.chunk_count - 1U;
    receipt_out->issued_pages = chunk->issued_pages;
    return 1;
}

int isaac_vita_room_entry_external_test_release_newest(
    const isaac_vita_room_entry_external_test_receipt *receipt)
{
    isaac_vita_room_entry_external_test_receipt current;
    room_external_chunk *chunk;
    int newest;
    int free_result;

    newest = isaac_vita_room_entry_external_test_newest_receipt(&current);
    if (newest != 1 || !external_test_receipt_equal(receipt, &current))
        return -1;
    chunk = NULL;
    if (!current.orphan) {
        if (!s_external.chunk_count ||
            current.chunk_index + 1U != s_external.chunk_count)
            return -1;
        chunk = &s_external.chunks[current.chunk_index];
        if (chunk->issued_pages != current.issued_pages ||
            s_external.issued_pages < current.issued_pages)
            return -1;
    }
    s_external.last_syscall =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_RESET_FREE;
    free_result = sceKernelFreeMemBlock((SceUID)current.uid);
    s_external.last_syscall_result = free_result;
    (void)external_increment(&s_external.reset_free_attempts);
    if (free_result < 0) {
        (void)external_increment(&s_external.reset_free_failures);
        return 0;
    }
    if (current.orphan) {
        external_chunk_clear(&s_external.orphan);
        if (s_external.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED)
            s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY;
    }
    else {
        s_external.issued_pages -= current.issued_pages;
        external_chunk_clear(chunk);
        --s_external.chunk_count;
    }
    if (!external_storage_valid())
        (void)external_terminal(
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_STATE);
    return 1;
}

int isaac_vita_room_entry_external_test_finish_reset(void)
{
    if (!external_storage_valid() || s_external.pending ||
        s_external.chunk_count || s_external.issued_pages ||
        s_external.orphan.uid >= 0)
        return 0;
    memset(&s_external, 0, sizeof s_external);
    s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_UNINITIALIZED;
    external_chunk_clear(&s_external.orphan);
    return 1;
}

void isaac_vita_room_entry_external_test_corrupt_chunk_count(void)
{
    s_external.chunk_count =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS + 1U;
}

void isaac_vita_room_entry_external_test_corrupt_receipt_total(void)
{
    if (s_external.chunk_count ==
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS &&
        s_external.orphan.uid < 0) {
        external_chunk_clear(&s_external.orphan);
        s_external.orphan.uid = INT32_MAX;
        s_external.state = ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED;
    }
}

void isaac_vita_room_entry_external_test_saturate_rollback_counter(void)
{
    s_external.rollback_free_attempts = UINT32_MAX;
}
#endif
