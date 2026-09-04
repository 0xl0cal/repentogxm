#include "host_vita_save_checksum_guest.h"

#include <stddef.h>
#include <stdint.h>

#include "host_vita_save_checksum_native.h"

#if !defined(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH) || \
    !ISAAC_VITA_SAVE_CHECKSUM_FASTPATH
#error The guest checksum shim must only be compiled for the native fast path
#endif

enum {
    ISAAC_SAVE_CHECKSUM_TABLE_RVA = 0x008034f0U,
    ISAAC_SAVE_CHECKSUM_STACK_BYTES = 12U,
    ISAAC_SAVE_CHECKSUM_FRAME_BYTES = 16U,
    ISAAC_SAVE_CHECKSUM_STACK_TOUCH_BYTES =
        ISAAC_SAVE_CHECKSUM_FRAME_BYTES + ISAAC_SAVE_CHECKSUM_STACK_BYTES
};

static int checksum_range_nonwrapping(uint32_t address, uint32_t size)
{
    return size == 0U ||
           (address != 0U && address <= UINT32_MAX - (size - 1U));
}

static int checksum_ranges_overlap(
    uint32_t first, uint32_t first_size,
    uint32_t second, uint32_t second_size)
{
    const uint64_t first_end = (uint64_t)first + first_size;
    const uint64_t second_end = (uint64_t)second + second_size;

    return first_size != 0U && second_size != 0U &&
           (uint64_t)first < second_end &&
           (uint64_t)second < first_end;
}

int isaac_vita_save_checksum_guest_try(CPU *__restrict c)
{
    uint32_t state_address;
    uint32_t data_address;
    uint32_t size;
    uint32_t mode;
    uint32_t stack_touch_address;
    uint32_t table_address;
    uint8_t *state;
    const uint8_t *data;
    uint8_t *table;

    if (c == NULL || c->esp < ISAAC_SAVE_CHECKSUM_FRAME_BYTES ||
            !guest_stack_contains(c, c->esp,
                                  ISAAC_SAVE_CHECKSUM_STACK_BYTES) ||
            !guest_stack_contains(
                c, c->esp - ISAAC_SAVE_CHECKSUM_FRAME_BYTES,
                ISAAC_SAVE_CHECKSUM_FRAME_BYTES))
        return 0;

    stack_touch_address = c->esp - ISAAC_SAVE_CHECKSUM_FRAME_BYTES;
    state_address = c->ecx;
    data_address = ld32(c->esp + 4U);
    size = ld32(c->esp + 8U);
    if (!checksum_range_nonwrapping(
            state_address, ISAAC_VITA_SAVE_CHECKSUM_STATE_BYTES) ||
            !checksum_range_nonwrapping(data_address, size))
        return 0;

    table_address =
        (uint32_t)GUEST_IMAGE_BASE + ISAAC_SAVE_CHECKSUM_TABLE_RVA;
    if (!checksum_range_nonwrapping(
            table_address, ISAAC_VITA_SAVE_CHECKSUM_TABLE_BYTES) ||
            checksum_ranges_overlap(
                stack_touch_address,
                ISAAC_SAVE_CHECKSUM_STACK_TOUCH_BYTES,
                state_address, ISAAC_VITA_SAVE_CHECKSUM_STATE_BYTES) ||
            checksum_ranges_overlap(
                stack_touch_address,
                ISAAC_SAVE_CHECKSUM_STACK_TOUCH_BYTES,
                data_address, size) ||
            checksum_ranges_overlap(
                stack_touch_address,
                ISAAC_SAVE_CHECKSUM_STACK_TOUCH_BYTES,
                table_address, ISAAC_VITA_SAVE_CHECKSUM_TABLE_BYTES) ||
            checksum_ranges_overlap(
                state_address, ISAAC_VITA_SAVE_CHECKSUM_STATE_BYTES,
                data_address, size) ||
            checksum_ranges_overlap(
                state_address, ISAAC_VITA_SAVE_CHECKSUM_STATE_BYTES,
                table_address, ISAAC_VITA_SAVE_CHECKSUM_TABLE_BYTES) ||
            checksum_ranges_overlap(
                data_address, size,
                table_address, ISAAC_VITA_SAVE_CHECKSUM_TABLE_BYTES))
        return 0;

    state = (uint8_t *)(uintptr_t)state_address;
    data = (const uint8_t *)(uintptr_t)data_address;
    mode = (uint32_t)state[12] |
           ((uint32_t)state[13] << 8) |
           ((uint32_t)state[14] << 16) |
           ((uint32_t)state[15] << 24);
    if (mode > 1U || (mode == 0U && size != 0U && state[4] > 3U))
        return 0;

    /* Preserve the four prologue PUSH stores and stack high-water accounting.
     * The corresponding POPs restore registers but deliberately leave these
     * saved words below the returned ESP. */
    st32(c->esp - 4U, c->ebp);
    st32(c->esp - 8U, c->ebx);
    st32(c->esp - 12U, c->esi);
    st32(c->esp - 16U, c->edi);
    guest_stack_note_low(c, stack_touch_address);

    table = (uint8_t *)(uintptr_t)table_address;
    isaac_vita_save_checksum_native_feed(state, data, size, table);

    /* The frozen body has two exits and both are RET 8.  Its 16-byte saved
     * frame plus the 12-byte return/argument range were checked before any
     * state mutation. */
    (void)gpop_generated(c);
    if (!guest_stack_adjust_generated(c, 8U))
        return 1;
    GUEST_STACK_CALLSITE_BARRIER();
    return 1;
}
