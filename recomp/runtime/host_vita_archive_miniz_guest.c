#include "host_vita_archive_miniz_guest.h"

#include <stdint.h>

#include "host_vita_archive_miniz_native.h"
#include "host_vita_heap.h"

#if !defined(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH) || \
    !ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH
#error The archive MiniZ guest shim must only be compiled when enabled
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error The archive MiniZ guest shim requires exact requested-size heap leases
#endif

enum {
    ARCHIVE_MINIZ_RETURN_RVA = 0x0059c3f8U,
    ARCHIVED_FILE_INPUT_OFFSET = 0x1cU,
    ARCHIVED_FILE_OUTPUT_OFFSET = 0x81cU,
    ARCHIVED_FILE_NATIVE_RANGE = 0xc00U,
    INPUT_SIZE_FRAME_OFFSET = 0x90U,
    OUTPUT_SIZE_FRAME_OFFSET = 0x98U,
    LEASE_COUNT = 2U
};

static int release_leases(uint32_t tokens[LEASE_COUNT])
{
    unsigned index;
    int ok = 1;

    for (index = LEASE_COUNT; index != 0U; --index) {
        if (tokens[index - 1U] != 0U) {
            ok = isaac_vita_guest_heap_lease_release(
                tokens[index - 1U]) && ok;
        }
    }
    return ok;
}

static int release_for_fallback(
    CPU *__restrict c, uint32_t tokens[LEASE_COUNT], uint32_t fault_address)
{
    if (release_leases(tokens))
        return 0;
    guest_fault(c, fault_address,
                "native archive MiniZ lease release failed");
    (void)gpop_generated(c);
    return 1;
}

int isaac_vita_archive_miniz_guest_try(CPU *__restrict c)
{
    uint32_t tokens[LEASE_COUNT] = { 0U, 0U };
    uint32_t return_rva;
    uint32_t state;
    uint32_t input;
    uint32_t input_size_address;
    uint32_t output_start;
    uint32_t output_next;
    uint32_t output_size_address;
    uint32_t flags;
    uint32_t input_size;
    uint32_t output_size;
    uint32_t object;
    int status;

    if (c == NULL || !guest_stack_contains(c, c->esp, 24U))
        return 0;
    return_rva = ld32(c->esp);
    if (return_rva != ARCHIVE_MINIZ_RETURN_RVA)
        return 0;

    state = c->ecx;
    input = c->edx;
    input_size_address = ld32(c->esp + 4U);
    output_start = ld32(c->esp + 8U);
    output_next = ld32(c->esp + 12U);
    output_size_address = ld32(c->esp + 16U);
    flags = ld32(c->esp + 20U);

    if (c->ebp < INPUT_SIZE_FRAME_OFFSET ||
            input_size_address != c->ebp - INPUT_SIZE_FRAME_OFFSET ||
            c->ebp < OUTPUT_SIZE_FRAME_OFFSET ||
            output_size_address != c->ebp - OUTPUT_SIZE_FRAME_OFFSET ||
            !guest_stack_contains(c, input_size_address, 4U) ||
            !guest_stack_contains(c, output_size_address, 4U) ||
            state == 0U || input < ARCHIVED_FILE_INPUT_OFFSET ||
            c->edi != input - ARCHIVED_FILE_INPUT_OFFSET ||
            c->edi > UINT32_MAX - ARCHIVED_FILE_OUTPUT_OFFSET ||
            output_start != c->edi + ARCHIVED_FILE_OUTPUT_OFFSET ||
            output_next != output_start || c->ebx != output_start ||
            input > UINT32_MAX - ARCHIVED_FILE_NATIVE_RANGE ||
            flags > 2U || (flags & ~2U) != 0U) {
        return 0;
    }

    input_size = ld32(input_size_address);
    output_size = ld32(output_size_address);
    if (input_size > ISAAC_VITA_ARCHIVE_MINIZ_INPUT_MAX ||
            output_size != ISAAC_VITA_ARCHIVE_MINIZ_OUTPUT_BYTES)
        return 0;

    object = c->edi;
    tokens[0] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)state,
        (const void *)(uintptr_t)state,
        ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES);
    if (tokens[0] == 0U)
        return 0;
    tokens[1] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)object,
        (const void *)(uintptr_t)input,
        ARCHIVED_FILE_NATIVE_RANGE);
    if (tokens[1] == 0U)
        return release_for_fallback(c, tokens, object);

    status = isaac_vita_archive_miniz_native(
        (void *)(uintptr_t)state,
        (const uint8_t *)(uintptr_t)input,
        (uint32_t *)(uintptr_t)input_size_address,
        (uint8_t *)(uintptr_t)output_start,
        (uint8_t *)(uintptr_t)output_next,
        (uint32_t *)(uintptr_t)output_size_address,
        flags);
    c->eax = (uint32_t)status;

    if (!release_leases(tokens)) {
        guest_fault(c, object,
                    "native archive MiniZ lease release failed");
        (void)gpop_generated(c);
        return 1;
    }
    /* The translated ArchivedFile caller owns all five stacked arguments. */
    (void)gpop_generated(c);
    return 1;
}
