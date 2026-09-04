#include "host_vita_save_read32_guest.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(ISAAC_VITA_SAVE_READ32_FUSED) || \
    !ISAAC_VITA_SAVE_READ32_FUSED
#error The fused Save Reader read32 shim must only be compiled when enabled
#endif

/* These are deliberately the generated owners, not duplicated native
 * algorithms.  Their own guarded fast paths or exact translated fallbacks
 * remain responsible for all reader/checksum semantics and coverage. */
extern void sub_0025ba60(CPU *__restrict c);
extern void sub_0025b340(CPU *__restrict c);

enum {
    ISAAC_SAVE_READ32_WRAPPER_BYTES = 0x1cU,
    ISAAC_SAVE_READ32_READER_BYTES = 0x24U,
    ISAAC_SAVE_READ32_READER_VTABLE_RVA = 0x00746d3cU,
    ISAAC_SAVE_READ32_DEEPEST_PUSH_BYTES = 56U,
    ISAAC_SAVE_READ32_VISIBLE_STACK_BYTES = 64U,
    ISAAC_SAVE_READ32_READER_RETURN_RVA = 0x0052eaa6U,
    ISAAC_SAVE_READ32_CHECKSUM_RETURN_RVA = 0x0052eab9U
};

static int save_read32_range_nonwrapping(uint32_t address, uint32_t size)
{
    return size == 0U ||
           (address != 0U && address <= UINT32_MAX - (size - 1U));
}

static int save_read32_ranges_overlap(
    uint32_t first, uint32_t first_size,
    uint32_t second, uint32_t second_size)
{
    const uint64_t first_end = (uint64_t)first + first_size;
    const uint64_t second_end = (uint64_t)second + second_size;

    return first_size != 0U && second_size != 0U &&
           (uint64_t)first < second_end &&
           (uint64_t)second < first_end;
}

int isaac_vita_save_read32_guest_try(CPU *__restrict c)
{
    uint32_t entry_esp;
    uint32_t stack_low;
    uint32_t wrapper;
    uint32_t reader;
    uint32_t destination;

    if (c == NULL || c->esp < ISAAC_SAVE_READ32_DEEPEST_PUSH_BYTES)
        return 0;
    entry_esp = c->esp;
    stack_low = entry_esp - ISAAC_SAVE_READ32_DEEPEST_PUSH_BYTES;
    if (!guest_stack_contains(
            c, stack_low, ISAAC_SAVE_READ32_VISIBLE_STACK_BYTES))
        return 0;

    wrapper = c->ecx;
    if (!save_read32_range_nonwrapping(
            wrapper, ISAAC_SAVE_READ32_WRAPPER_BYTES) ||
            save_read32_ranges_overlap(
                stack_low, ISAAC_SAVE_READ32_VISIBLE_STACK_BYTES,
                wrapper, ISAAC_SAVE_READ32_WRAPPER_BYTES))
        return 0;
    reader = ld32(wrapper);
    if (!save_read32_range_nonwrapping(
            reader, ISAAC_SAVE_READ32_READER_BYTES) ||
            save_read32_ranges_overlap(
                stack_low, ISAAC_SAVE_READ32_VISIBLE_STACK_BYTES,
                reader, ISAAC_SAVE_READ32_READER_BYTES) ||
            ld32(reader) !=
                (uint32_t)(GUEST_IMAGE_BASE +
                           ISAAC_SAVE_READ32_READER_VTABLE_RVA))
        return 0;

    /* Exact translated wrapper sequence through the indirect reader call. */
    gpush_generated(c, c->ebp); GUEST_STACK_CALLSITE_BARRIER();
    c->ebp = c->esp;
    gpush_generated(c, c->esi); GUEST_STACK_CALLSITE_BARRIER();
    c->esi = ld32(c->ebp + 8U);
    destination = c->esi;
    gpush_generated(c, c->edi); GUEST_STACK_CALLSITE_BARRIER();
    c->edi = wrapper;
    gpush_generated(c, 1U); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, 4U); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, destination); GUEST_STACK_CALLSITE_BARRIER();
    c->ecx = reader;
    c->eax = ld32(reader);
    gpush_generated(c, ISAAC_SAVE_READ32_READER_RETURN_RVA);
    GUEST_STACK_CALLSITE_BARRIER();
    /* The original edge is indirect.  Preserve its profile census while
     * skipping only the already-proved dynamic lookup/dispatch. */
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_LOOKUP();
#if defined(ISAAC_VITA_PHASE_PROFILE)
    guest_phase_profile_note_lookup_cache_hit();
#endif
    sub_0025ba60(c);
    if (c->fault != NULL)
        return 1;

    c->eax = ld32(c->esi);
    c->ecx = c->edi + 0x0cU;
    st32(c->ebp + 8U, c->eax);
    c->eax = c->ebp + 8U;
    gpush_generated(c, 4U); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, c->eax); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, ISAAC_SAVE_READ32_CHECKSUM_RETURN_RVA);
    GUEST_STACK_CALLSITE_BARRIER();
    sub_0025b340(c);
    if (c->fault != NULL)
        return 1;

    c->edi = gpop_generated(c);
    c->esi = gpop_generated(c);
    c->ebp = gpop_generated(c);
    (void)gpop_generated(c);
    if (!guest_stack_adjust_generated(c, 4U))
        return 1;
    GUEST_STACK_CALLSITE_BARRIER();
    return 1;
}
