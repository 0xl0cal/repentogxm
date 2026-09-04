#include "host_vita_png_unfilter_guest.h"

#include <stddef.h>
#include <stdint.h>

#include "host_vita_heap.h"
#include "host_vita_png_unfilter_native.h"
#include "kage_vita_png_decode_profile.h"

#if !ISAAC_VITA_PNG_NATIVE_UNFILTER
#error The guest PNG shim must only be compiled for the opt-in native experiment
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error The guest PNG shim requires exact requested-size heap leases
#endif

enum {
    ISAAC_PNG_FILTER_NONE = 0U,
    ISAAC_PNG_FILTER_SUB = 1U,
    ISAAC_PNG_FILTER_UP = 2U,
    ISAAC_PNG_FILTER_AVG = 3U,
    ISAAC_PNG_FILTER_PAETH = 4U,
    ISAAC_PNG_ROW_INFO_OFFSET = 0xf8U,
    ISAAC_PNG_CALL_RETURN_RVA = 0x005b17fcU
};

static void png_native_note(uint32_t outcome)
{
#if defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    kage_vita_png_profile_native_result(outcome);
#else
    (void)outcome;
#endif
}

static uint32_t png_load_u32le(uint32_t address)
{
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;

    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int png_range_nonwrapping(uint32_t address, uint32_t size)
{
    return address != 0U && size != 0U &&
           address <= UINT32_MAX - (size - 1U);
}

static int png_ranges_overlap(
    uint32_t first, uint32_t first_size,
    uint32_t second, uint32_t second_size)
{
    const uint64_t first_end = (uint64_t)first + first_size;
    const uint64_t second_end = (uint64_t)second + second_size;

    return (uint64_t)first < second_end &&
           (uint64_t)second < first_end;
}

static int png_release_leases(
    uint32_t png_token, uint32_t row_token, uint32_t previous_token)
{
    int ok = 1;

    if (previous_token != 0U)
        ok = isaac_vita_guest_heap_lease_release(previous_token) && ok;
    if (row_token != 0U)
        ok = isaac_vita_guest_heap_lease_release(row_token) && ok;
    if (png_token != 0U)
        ok = isaac_vita_guest_heap_lease_release(png_token) && ok;
    return ok;
}

int isaac_vita_png_unfilter_guest_try(CPU *__restrict c)
{
    uint32_t png_token = 0U;
    uint32_t row_token = 0U;
    uint32_t previous_token = 0U;
    uint32_t return_rva;
    uint32_t row_info;
    uint32_t row;
    uint32_t previous_row;
    uint32_t filter;
    uint32_t rowbytes;
    uint32_t pixel_depth;
    uint32_t bpp;
    uint32_t outcome = KAGE_VITA_PNG_NATIVE_REJECT_ABI;
    int handled;

    if (c == NULL || !guest_stack_contains(c, c->esp, 16U)) {
        png_native_note(KAGE_VITA_PNG_NATIVE_REJECT_STACK);
        return 0;
    }

    return_rva = png_load_u32le(c->esp);
    row = png_load_u32le(c->esp + 4U);
    previous_row = png_load_u32le(c->esp + 8U);
    filter = png_load_u32le(c->esp + 12U);
    row_info = c->edx;
    if (return_rva != ISAAC_PNG_CALL_RETURN_RVA || c->ecx == 0U ||
            c->ecx > UINT32_MAX - ISAAC_PNG_ROW_INFO_OFFSET ||
            row_info != c->ecx + ISAAC_PNG_ROW_INFO_OFFSET) {
        png_native_note(outcome);
        return 0;
    }
    if (filter > ISAAC_PNG_FILTER_PAETH) {
        png_native_note(KAGE_VITA_PNG_NATIVE_REJECT_FILTER);
        return 0;
    }

    png_token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)c->ecx,
        (const void *)(uintptr_t)row_info,
        ISAAC_VITA_PNG_ROW_INFO_BYTES);
    if (png_token == 0U) {
        png_native_note(KAGE_VITA_PNG_NATIVE_REJECT_RANGE);
        return 0;
    }

    rowbytes = png_load_u32le(row_info + 4U);
    pixel_depth = (uint32_t)*(const uint8_t *)(uintptr_t)(row_info + 11U);
    bpp = (pixel_depth + 7U) >> 3;
    if (rowbytes == 0U || bpp == 0U || bpp > 8U ||
            ((filter == ISAAC_PNG_FILTER_SUB ||
              filter == ISAAC_PNG_FILTER_AVG ||
              filter == ISAAC_PNG_FILTER_PAETH) && rowbytes < bpp)) {
        outcome = KAGE_VITA_PNG_NATIVE_REJECT_METADATA;
        goto fallback;
    }
    if (!png_range_nonwrapping(row, rowbytes)) {
        outcome = KAGE_VITA_PNG_NATIVE_REJECT_RANGE;
        goto fallback;
    }
    if (filter >= ISAAC_PNG_FILTER_UP &&
            !png_range_nonwrapping(previous_row, rowbytes)) {
        outcome = KAGE_VITA_PNG_NATIVE_REJECT_RANGE;
        goto fallback;
    }
    if (png_ranges_overlap(row_info, ISAAC_VITA_PNG_ROW_INFO_BYTES,
                           row, rowbytes) ||
            png_ranges_overlap(c->esp, 16U, row_info,
                               ISAAC_VITA_PNG_ROW_INFO_BYTES) ||
            png_ranges_overlap(c->esp, 16U, row, rowbytes) ||
            (filter >= ISAAC_PNG_FILTER_UP &&
             (png_ranges_overlap(
                  row_info, ISAAC_VITA_PNG_ROW_INFO_BYTES,
                  previous_row, rowbytes) ||
              png_ranges_overlap(c->esp, 16U, previous_row, rowbytes) ||
              png_ranges_overlap(row, rowbytes, previous_row, rowbytes)))) {
        outcome = KAGE_VITA_PNG_NATIVE_REJECT_ALIAS;
        goto fallback;
    }

    row_token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)(row - 1U),
        (const void *)(uintptr_t)row, rowbytes);
    if (row_token == 0U) {
        outcome = KAGE_VITA_PNG_NATIVE_REJECT_RANGE;
        goto fallback;
    }
    if (filter >= ISAAC_PNG_FILTER_UP) {
        previous_token = isaac_vita_guest_heap_lease_exact_range(
            (const void *)(uintptr_t)(previous_row - 1U),
            (const void *)(uintptr_t)previous_row, rowbytes);
        if (previous_token == 0U) {
            outcome = KAGE_VITA_PNG_NATIVE_REJECT_RANGE;
            goto fallback;
        }
    }

    handled = isaac_vita_png_unfilter_try(
        (const void *)(uintptr_t)row_info,
        ISAAC_VITA_PNG_ROW_INFO_BYTES,
        (uint8_t *)(uintptr_t)row, rowbytes,
        (const uint8_t *)(uintptr_t)previous_row,
        filter >= ISAAC_PNG_FILTER_UP ? rowbytes : 0U,
        filter);
    if (!png_release_leases(png_token, row_token, previous_token)) {
        png_native_note(KAGE_VITA_PNG_NATIVE_REJECT_RELEASE);
        guest_fault(c, row, "PNG native unfilter lease release failed");
        /* Production guest_fault longjmps or aborts.  A returning test stub
         * must still never enter the original body with uncertain leases or
         * after a possibly-mutated native row. */
        (void)gpop_generated(c);
        return 1;
    }
    if (handled != ISAAC_VITA_PNG_UNFILTER_HANDLED) {
        png_native_note(KAGE_VITA_PNG_NATIVE_REJECT_HELPER);
        return 0;
    }

    png_native_note(KAGE_VITA_PNG_NATIVE_HANDLED);
    (void)gpop_generated(c);
    return 1;

fallback:
    if (!png_release_leases(png_token, row_token, previous_token)) {
        png_native_note(KAGE_VITA_PNG_NATIVE_REJECT_RELEASE);
        guest_fault(c, row_info, "PNG native unfilter lease release failed");
        (void)gpop_generated(c);
        return 1;
    }
    png_native_note(outcome);
    return 0;
}
