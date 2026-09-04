#include "host_vita_save_reader_guest.h"

#include <stddef.h>
#include <stdint.h>

#include "host_vita_audio_cooperative.h"
#include "host_vita_import_id.h"
#include "host_vita_memory.h"

#if !defined(ISAAC_VITA_SAVE_READER_FASTPATH) || \
    !ISAAC_VITA_SAVE_READER_FASTPATH
#error The guest Save Reader shim must only be compiled for the native fast path
#endif

enum {
    ISAAC_SAVE_READER_OBJECT_BYTES = 0x24U,
    ISAAC_SAVE_READER_BUFFER_OFFSET = 0x10U,
    ISAAC_SAVE_READER_CURSOR_OFFSET = 0x18U,
    ISAAC_SAVE_READER_END_OFFSET = 0x1cU,
    ISAAC_SAVE_READER_EOF_OFFSET = 0x21U,
    ISAAC_SAVE_READER_VTABLE_RVA = 0x00746d3cU,
    ISAAC_SAVE_READER_ENTRY_BYTES = 16U,
    ISAAC_SAVE_READER_PUSH_BYTES = 28U,
    ISAAC_SAVE_READER_VISIBLE_STACK_BYTES =
        ISAAC_SAVE_READER_PUSH_BYTES + ISAAC_SAVE_READER_ENTRY_BYTES,
    ISAAC_SAVE_READER_MEMCPY_RETURN_RVA = 0x0025ba89U,
    ISAAC_SAVE_READER_MEMCPY_FUNCTION_COVERAGE_ID = 12570U,
    ISAAC_SAVE_READER_MEMCPY_IMPORT_COVERAGE_ID = 281U,
    ISAAC_SAVE_READER_MEMCPY_LOCAL_INDEX = 3U
};

static int save_reader_range_nonwrapping(uint32_t address, uint32_t size)
{
    return size == 0U ||
           (address != 0U && address <= UINT32_MAX - (size - 1U));
}

static int save_reader_ranges_overlap(
    uint32_t first, uint32_t first_size,
    uint32_t second, uint32_t second_size)
{
    const uint64_t first_end = (uint64_t)first + first_size;
    const uint64_t second_end = (uint64_t)second + second_size;

    return first_size != 0U && second_size != 0U &&
           (uint64_t)first < second_end &&
           (uint64_t)second < first_end;
}

int isaac_vita_save_reader_guest_try(CPU *__restrict c)
{
    uint32_t entry_esp;
    uint32_t stack_low;
    uint32_t reader;
    uint32_t destination;
    uint32_t element_size;
    uint32_t element_count;
    uint32_t requested;
    uint32_t available;
    uint32_t copied;
    uint32_t buffer;
    uint32_t cursor;
    uint32_t end;
    uint32_t source;
    uint32_t new_cursor;

    if (c == NULL || c->esp < ISAAC_SAVE_READER_PUSH_BYTES)
        return 0;
    entry_esp = c->esp;
    stack_low = entry_esp - ISAAC_SAVE_READER_PUSH_BYTES;
    if (!guest_stack_contains(
            c, stack_low, ISAAC_SAVE_READER_VISIBLE_STACK_BYTES))
        return 0;

    reader = c->ecx;
    if (!save_reader_range_nonwrapping(
            reader, ISAAC_SAVE_READER_OBJECT_BYTES) ||
            save_reader_ranges_overlap(
                stack_low, ISAAC_SAVE_READER_VISIBLE_STACK_BYTES,
                reader, ISAAC_SAVE_READER_OBJECT_BYTES) ||
            ld32(reader) !=
                (uint32_t)(GUEST_IMAGE_BASE + ISAAC_SAVE_READER_VTABLE_RVA))
        return 0;

    destination = ld32(entry_esp + 4U);
    element_size = ld32(entry_esp + 8U);
    element_count = ld32(entry_esp + 12U);
    /* Two-operand IMUL keeps this same low 32-bit product; its overflow flags
     * are dead in the frozen body. */
    requested = element_size * element_count;
    buffer = ld32(reader + ISAAC_SAVE_READER_BUFFER_OFFSET);
    cursor = ld32(reader + ISAAC_SAVE_READER_CURSOR_OFFSET);
    end = ld32(reader + ISAAC_SAVE_READER_END_OFFSET);
    if (cursor > end || buffer > UINT32_MAX - cursor)
        return 0;
    available = end - cursor;
    copied = requested < available ? requested : available;
    source = buffer + cursor;

    if (!save_reader_range_nonwrapping(destination, copied) ||
            !save_reader_range_nonwrapping(source, copied) ||
            save_reader_ranges_overlap(
                destination, copied, source, copied) ||
            save_reader_ranges_overlap(
                destination, copied,
                reader, ISAAC_SAVE_READER_OBJECT_BYTES) ||
            save_reader_ranges_overlap(
                source, copied,
                reader, ISAAC_SAVE_READER_OBJECT_BYTES) ||
            save_reader_ranges_overlap(
                destination, copied,
                stack_low, ISAAC_SAVE_READER_VISIBLE_STACK_BYTES) ||
            save_reader_ranges_overlap(
                source, copied,
                stack_low, ISAAC_SAVE_READER_VISIBLE_STACK_BYTES))
        return 0;

    /* Reproduce every visible PUSH store below the returned ESP.  Calling the
     * already-oracled direct memcpy family entry retains its range/fault ABI,
     * import count and cdecl return while skipping only lookup/translation. */
    st32(entry_esp - 4U, c->ebp);
    st32(entry_esp - 8U, c->esi);
    st32(entry_esp - 12U, c->edi);
    st32(entry_esp - 16U, copied);
    st32(entry_esp - 20U, source);
    st32(entry_esp - 24U, destination);
    st32(entry_esp - 28U, ISAAC_SAVE_READER_MEMCPY_RETURN_RVA);
    /* Exact register image at the nested memcpy boundary.  It is observable
     * if the import faults before the normal POP restoration below. */
    c->ebp = entry_esp - 4U;
    c->esi = copied;
    c->edi = reader;
    c->eax = source;
    if (!guest_stack_set_generated(c, stack_low))
        return 1;
    GUEST_STACK_CALLSITE_BARRIER();

    guest_coverage_function(
        ISAAC_SAVE_READER_MEMCPY_FUNCTION_COVERAGE_ID);
    GUEST_PHASE_PROFILE_NOTE_CALL();
    guest_coverage_import(ISAAC_SAVE_READER_MEMCPY_IMPORT_COVERAGE_ID);
    if (!isaac_vita_memory_import_indexed(
            c, ISAAC_SAVE_READER_MEMCPY_LOCAL_INDEX,
            &g_host_import_calls)) {
        guest_fault(c,
                    (uint32_t)(GUEST_IMAGE_BASE +
                               ISAAC_VITA_MEMORY_MEMCPY_IAT_RVA),
                    ISAAC_VITA_MEMORY_MEMCPY_NAME);
        return 1;
    }
    if (c->fault != NULL)
        return 1;

    new_cursor = cursor + copied;
    st32(reader + ISAAC_SAVE_READER_CURSOR_OFFSET, new_cursor);
    if (!guest_stack_adjust_generated(c, 12U))
        return 1;
    c->ecx = new_cursor;
    c->eax = copied;
    c->ecx = (c->ecx & UINT32_C(0xffffff00)) |
             (new_cursor >= end ? 1U : 0U);
    st8(reader + ISAAC_SAVE_READER_EOF_OFFSET, (uint8_t)c->ecx);
    c->edi = gpop_generated(c);
    c->esi = gpop_generated(c);
    c->ebp = gpop_generated(c);
    (void)gpop_generated(c);
    if (!guest_stack_adjust_generated(c, 12U))
        return 1;
    GUEST_STACK_CALLSITE_BARRIER();
    /* Save parsing can hold the main loop longer than the music stream's
     * queued PCM.  This exact Save Reader boundary is an owner-thread safe
     * point; the save-specific helper is throttled, recursion-guarded and a
     * no-op when audio is disabled or inactive. */
    isaac_vita_audio_cooperative_save_poll(c);
    return 1;
}
