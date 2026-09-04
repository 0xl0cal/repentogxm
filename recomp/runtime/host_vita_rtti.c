/* Vita implementation of the one PC-proven MSVC x86 RTTI import.
 *
 * This is the portable single-inheritance algorithm from host_win32.c.  It
 * reads the relocated guest metadata directly; it never asks native ARM C++
 * RTTI to interpret x86 TypeDescriptor/ClassHierarchyDescriptor records. */
#include <stdint.h>
#include <string.h>

#include "host_vita_rtti.h"
#include "host_vita_import_id.h"

enum {
    VITA_RTTI_MAX_BASES = 512U,
    VITA_RTTI_MAX_NAME = 4096U,
    VITA_RTTI_CHD_MULTINH = 0x01U,
    VITA_RTTI_CHD_VIRTINH = 0x02U,
    VITA_RTTI_CHD_AMBIGUOUS = 0x04U,
    VITA_RTTI_BCD_PRIVORPROTBASE = 0x04U,
    VITA_RTTI_BCD_KNOWN = 0x7fU
};

typedef struct vita_rtti_bcd {
    uint32_t type_descriptor;
    uint32_t contained_bases;
    int32_t mdisp;
    int32_t pdisp;
    int32_t vdisp;
    uint32_t attributes;
} vita_rtti_bcd;

_Static_assert(sizeof(vita_rtti_bcd) == 24U,
               "x86 RTTI BaseClassDescriptor layout drifted");
_Static_assert(ISAAC_VITA_RTTI_IMPORT_COUNT == 1U &&
                   ISAAC_VITA_RTTI_CDECL_ARGUMENT_COUNT == 5U &&
                   ISAAC_VITA_RTTI_PHYSICAL_CALL_COUNT == 662U &&
                   ISAAC_VITA_RTTI_PHYSICAL_CALL_FNV64 ==
                       UINT64_C(0x6acddf0a2f8c8863),
               "frozen __RTDynamicCast import census drifted");

static uint32_t vita_rtti_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_rtti_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

static int vita_rtti_image_range(CPU *__restrict c, uint32_t address,
                                 uint32_t size, const char *what)
{
    if (!guest_image_contains(address, size)) {
        guest_fault(c, address, what);
        return 0;
    }
    return 1;
}

static int vita_rtti_read_bcd(CPU *__restrict c, uint32_t address,
                              uint32_t hierarchy_count,
                              vita_rtti_bcd *out)
{
    if (!vita_rtti_image_range(c, address, 24U,
                               "__RTDynamicCast BCD leaves guest image"))
        return 0;
    out->type_descriptor = ld32(address);
    out->contained_bases = ld32(address + 4U);
    out->mdisp = (int32_t)ld32(address + 8U);
    out->pdisp = (int32_t)ld32(address + 12U);
    out->vdisp = (int32_t)ld32(address + 16U);
    out->attributes = ld32(address + 20U);
    if (!guest_image_contains(out->type_descriptor, 9U) ||
        out->contained_bases >= hierarchy_count ||
        out->mdisp < 0 || out->pdisp != -1 || out->vdisp < 0 ||
        (out->attributes & ~VITA_RTTI_BCD_KNOWN) != 0U) {
        guest_fault(c, address,
                    "__RTDynamicCast invalid SI base descriptor");
        return 0;
    }
    return 1;
}

static int vita_rtti_names_equal(CPU *__restrict c, uint32_t left,
                                 uint32_t right, int *ok)
{
    uint32_t index;

    for (index = 0U; index < VITA_RTTI_MAX_NAME; ++index) {
        uint32_t left_at = left + 8U + index;
        uint32_t right_at = right + 8U + index;
        uint8_t a, b;

        if (!guest_image_contains(left_at, 1U) ||
            !guest_image_contains(right_at, 1U)) {
            guest_fault(c, !guest_image_contains(left_at, 1U)
                             ? left_at : right_at,
                        "__RTDynamicCast type name leaves guest image");
            *ok = 0;
            return 0;
        }
        a = ld8(left_at);
        b = ld8(right_at);
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
    guest_fault(c, left, "__RTDynamicCast unterminated type name");
    *ok = 0;
    return 0;
}

/* MSVC x86 `void *__cdecl __RTDynamicCast(void *inptr, long vf_delta,
 * TypeDescriptor *source, TypeDescriptor *target, int is_reference)`.
 *
 * The complete frozen surface is single-inheritance.  Multiple/virtual
 * inheritance remains loud instead of silently applying the smaller SI
 * algorithm to an unproved metadata shape. */
static void vita_rtti_RTDynamicCast(CPU *__restrict c)
{
    uint32_t input = vita_rtti_arg(c, 0U);
    uint32_t vf_delta = vita_rtti_arg(c, 1U);
    uint32_t source_type = vita_rtti_arg(c, 2U);
    uint32_t target_type = vita_rtti_arg(c, 3U);
    uint32_t is_reference = vita_rtti_arg(c, 4U);
    uint32_t vfptr, locator, hierarchy, base_array, base_count;
    uint32_t locator_offset, construction_offset, complete;
    uint32_t pass, i, j, target_address = 0U;
    int64_t complete_calc;
    vita_rtti_bcd target_bcd;
    int match_ok = 1;

    (void)vf_delta; /* MSVC's SI path does not use the vfptr displacement. */
    if (!input) {
        c->eax = 0U;
        vita_rtti_cdecl_return(c);
        return;
    }

    /* The object pointer itself may belong to the guest heap.  Once its
     * compiler-written vfptr is loaded, every RTTI pointer must stay inside
     * the mapped PE image. */
    vfptr = ld32(input);
    if (vfptr < 4U ||
        !vita_rtti_image_range(c, vfptr - 4U, 4U,
                               "__RTDynamicCast vfptr leaves guest image"))
        return;
    locator = ld32(vfptr - 4U);
    if (!vita_rtti_image_range(c, locator, 20U,
                               "__RTDynamicCast COL leaves guest image"))
        return;
    locator_offset = ld32(locator + 4U);
    construction_offset = ld32(locator + 8U);
    if (ld32(locator) != 0U || locator_offset > 0x100000U ||
        construction_offset > 0x100000U || (locator_offset & 3U) != 0U ||
        (construction_offset & 3U) != 0U) {
        guest_fault(c, locator, "__RTDynamicCast invalid x86 COL");
        return;
    }
    complete_calc = (int64_t)(uint64_t)input - (int64_t)locator_offset;
    if (construction_offset) {
        if (input < construction_offset) {
            guest_fault(c, input,
                        "__RTDynamicCast construction offset wraps");
            return;
        }
        complete_calc -= (int32_t)ld32(input - construction_offset);
    }
    if (complete_calc < 0 || complete_calc > UINT32_MAX) {
        guest_fault(c, input, "__RTDynamicCast complete object wraps");
        return;
    }
    complete = (uint32_t)complete_calc;

    hierarchy = ld32(locator + 16U);
    if (!vita_rtti_image_range(c, hierarchy, 16U,
                               "__RTDynamicCast CHD leaves guest image"))
        return;
    if (ld32(hierarchy) != 0U ||
        (ld32(hierarchy + 4U) &
         ~(VITA_RTTI_CHD_MULTINH | VITA_RTTI_CHD_VIRTINH |
           VITA_RTTI_CHD_AMBIGUOUS)) != 0U) {
        guest_fault(c, hierarchy,
                    "__RTDynamicCast invalid class hierarchy");
        return;
    }
    if (ld32(hierarchy + 4U) != 0U) {
        guest_fault(c, hierarchy,
                    "__RTDynamicCast MI/VI hierarchy is not implemented");
        return;
    }
    base_count = ld32(hierarchy + 8U);
    base_array = ld32(hierarchy + 12U);
    if (!base_count || base_count > VITA_RTTI_MAX_BASES ||
        !vita_rtti_image_range(c, base_array, base_count * 4U,
                               "__RTDynamicCast base array leaves guest image"))
        return;
    if (!vita_rtti_image_range(c, source_type, 9U,
                               "__RTDynamicCast source type leaves guest image") ||
        !vita_rtti_image_range(c, target_type, 9U,
                               "__RTDynamicCast target type leaves guest image"))
        return;

    /* Match pointer identity first across the whole hierarchy.  Only if no
     * target pointer exists do we repeat by decorated name, matching MSVC's
     * cross-image fallback ordering. */
    for (pass = 0U; pass < 2U; ++pass) {
        for (i = 0U; i < base_count; ++i) {
            uint32_t address = ld32(base_array + i * 4U);
            vita_rtti_bcd candidate;
            int target_match;

            if (!vita_rtti_read_bcd(c, address, base_count, &candidate))
                return;
            target_match = pass == 0U
                ? candidate.type_descriptor == target_type
                : vita_rtti_names_equal(c, candidate.type_descriptor,
                                        target_type, &match_ok);
            if (!match_ok)
                return;
            if (!target_match)
                continue;

            target_address = address;
            target_bcd = candidate;
            for (j = i + 1U; j < base_count; ++j) {
                uint32_t source_address = ld32(base_array + j * 4U);
                vita_rtti_bcd source_bcd;
                int source_match;

                if (!vita_rtti_read_bcd(c, source_address, base_count,
                                        &source_bcd))
                    return;
                if (source_bcd.attributes &
                    VITA_RTTI_BCD_PRIVORPROTBASE)
                    goto cast_failed;
                source_match = pass == 0U
                    ? source_bcd.type_descriptor == source_type
                    : vita_rtti_names_equal(c, source_bcd.type_descriptor,
                                            source_type, &match_ok);
                if (!match_ok)
                    return;
                if (source_match)
                    goto cast_succeeded;
            }
            goto cast_failed;
        }
    }

cast_failed:
    c->eax = 0U;
    if (is_reference) {
        guest_fault(c, target_type,
                    "__RTDynamicCast reference failure needs guest bad_cast");
        return;
    }
    vita_rtti_cdecl_return(c);
    return;

cast_succeeded:
    (void)target_address;
    complete_calc = (int64_t)(uint64_t)complete + target_bcd.mdisp;
    if (complete_calc < 0 || complete_calc > UINT32_MAX) {
        guest_fault(c, complete, "__RTDynamicCast result pointer wraps");
        return;
    }
    c->eax = (uint32_t)complete_calc;
    vita_rtti_cdecl_return(c);
}

const char *isaac_vita_rtti_import_name(uint32_t index)
{
    return index == 0U ? ISAAC_VITA_RTTI_IMPORT_NAME : NULL;
}

int isaac_vita_rtti_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    if (index >= ISAAC_VITA_RTTI_IMPORT_COUNT)
        return 0;
    /* Production guest_fault may longjmp. */
    if (call_count)
        ++*call_count;
    vita_rtti_RTDynamicCast(c);
    return 1;
}

static int vita_rtti_dispatch(CPU *__restrict c, const char *name,
                              unsigned *call_count)
{
    if (!name || strcmp(name, ISAAC_VITA_RTTI_IMPORT_NAME) != 0)
        return 0;

    return isaac_vita_rtti_import_indexed(c, 0U, call_count);
}

int isaac_vita_rtti_import(CPU *__restrict c, const char *name)
{
    return vita_rtti_dispatch(c, name, NULL);
}

int isaac_vita_rtti_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    return vita_rtti_dispatch(c, name, call_count);
}
