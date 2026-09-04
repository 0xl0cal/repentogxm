/* Source oracle for the Vita MSVC x86 single-inheritance RTTI adapter.
 *
 * The softfp gate compiles and links this program but deliberately does not
 * execute it: only Vita3K or hardware is a Vita runtime oracle. */
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_rtti.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita RTTI oracle requires a 32-bit identity-mapped target
#endif

enum {
    TEST_IMAGE_SIZE = 1024U,
    TEST_VFTABLE_LOCATOR_SLOT = 0x40U,
    TEST_COL = 0x80U,
    TEST_CHD = 0xa0U,
    TEST_BASE_ARRAY = 0xc0U,
    TEST_TARGET_BCD = 0xe0U,
    TEST_SOURCE_BCD = 0x100U,
    TEST_TARGET_TD = 0x180U,
    TEST_TARGET_ALIAS_TD = 0x1c0U,
    TEST_SOURCE_TD = 0x200U,
    TEST_MISSING_TD = 0x240U
};

_Static_assert(ISAAC_VITA_RTTI_IAT_RVA == 0x00606464U &&
                   ISAAC_VITA_RTTI_THUNK_RVA == 0x005ec34cU &&
                   ISAAC_VITA_RTTI_IMPORT_COUNT == 1U &&
                   ISAAC_VITA_RTTI_CDECL_ARGUMENT_COUNT == 5U,
               "__RTDynamicCast import/ABI identity drifted");
_Static_assert(ISAAC_VITA_RTTI_PHYSICAL_CALL_COUNT == 662U &&
                   ISAAC_VITA_RTTI_PHYSICAL_CALL_FNV64 ==
                       UINT64_C(0x6acddf0a2f8c8863),
               "__RTDynamicCast complete PE callsite census drifted");
_Static_assert(ISAAC_VITA_RTTI_PC_COVERAGE_IMPORT_ID == 272U &&
                   ISAAC_VITA_RTTI_PC_COVERAGE_HIT_COUNT == 1U,
               "__RTDynamicCast normal-PC coverage identity drifted");

_Alignas(8) static uint8_t s_image[TEST_IMAGE_SIZE];
_Alignas(8) static uint32_t s_object[8];
_Alignas(8) static uint32_t s_frame[16];

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint32_t image_address(uint32_t offset)
{
    return pointer32(s_image) + offset;
}

int guest_image_contains(uint32_t address, uint32_t size)
{
    uint32_t base = pointer32(s_image);
    uint32_t offset = address - base;

    return offset <= TEST_IMAGE_SIZE &&
           size <= TEST_IMAGE_SIZE - offset;
}

void guest_check(uint32_t address, uint32_t size, int write)
{
    (void)address;
    (void)size;
    (void)write;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    if (!c->fault) {
        c->fault = what;
        c->fault_addr = address;
    }
}

static void image_write32(uint32_t offset, uint32_t value)
{
    memcpy(s_image + offset, &value, sizeof value);
}

static void fixture_reset(void)
{
    static const char target_name[] = ".?AVTarget@@";
    static const char source_name[] = ".?AVSource@@";
    static const char missing_name[] = ".?AVMissing@@";
    uint32_t target_bcd = image_address(TEST_TARGET_BCD);
    uint32_t source_bcd = image_address(TEST_SOURCE_BCD);

    memset(s_image, 0, sizeof s_image);
    memset(s_object, 0, sizeof s_object);

    /* vfptr[-1] -> CompleteObjectLocator.  The function slot itself is never
     * called by __RTDynamicCast and stays zero in this synthetic fixture. */
    image_write32(TEST_VFTABLE_LOCATOR_SLOT, image_address(TEST_COL));

    /* x86 CompleteObjectLocator: signature, offset, cdOffset,
     * TypeDescriptor*, ClassHierarchyDescriptor*. */
    image_write32(TEST_COL + 0U, 0U);
    image_write32(TEST_COL + 4U, 0U);
    image_write32(TEST_COL + 8U, 0U);
    image_write32(TEST_COL + 12U, image_address(TEST_TARGET_TD));
    image_write32(TEST_COL + 16U, image_address(TEST_CHD));

    /* x86 ClassHierarchyDescriptor: signature, attributes, base count,
     * BaseClassDescriptor**. */
    image_write32(TEST_CHD + 0U, 0U);
    image_write32(TEST_CHD + 4U, 0U);
    image_write32(TEST_CHD + 8U, 2U);
    image_write32(TEST_CHD + 12U, image_address(TEST_BASE_ARRAY));
    image_write32(TEST_BASE_ARRAY + 0U, target_bcd);
    image_write32(TEST_BASE_ARRAY + 4U, source_bcd);

    /* x86 BaseClassDescriptor: TD*, contained bases, PMD mdisp/pdisp/vdisp,
     * attributes.  This exact six-dword order is copied from host_win32. */
    image_write32(TEST_TARGET_BCD + 0U, image_address(TEST_TARGET_TD));
    image_write32(TEST_TARGET_BCD + 4U, 1U);
    image_write32(TEST_TARGET_BCD + 8U, 0U);
    image_write32(TEST_TARGET_BCD + 12U, UINT32_MAX);
    image_write32(TEST_TARGET_BCD + 16U, 0U);
    image_write32(TEST_TARGET_BCD + 20U, 0U);
    image_write32(TEST_SOURCE_BCD + 0U, image_address(TEST_SOURCE_TD));
    image_write32(TEST_SOURCE_BCD + 4U, 0U);
    image_write32(TEST_SOURCE_BCD + 8U, 0U);
    image_write32(TEST_SOURCE_BCD + 12U, UINT32_MAX);
    image_write32(TEST_SOURCE_BCD + 16U, 0U);
    image_write32(TEST_SOURCE_BCD + 20U, 0U);

    memcpy(s_image + TEST_TARGET_TD + 8U,
           target_name, sizeof target_name);
    memcpy(s_image + TEST_TARGET_ALIAS_TD + 8U,
           target_name, sizeof target_name);
    memcpy(s_image + TEST_SOURCE_TD + 8U,
           source_name, sizeof source_name);
    memcpy(s_image + TEST_MISSING_TD + 8U,
           missing_name, sizeof missing_name);

    s_object[0] = image_address(TEST_VFTABLE_LOCATOR_SLOT + 4U);
}

static uint32_t prepare_call(CPU *c, const uint32_t arguments[5])
{
    uint32_t index;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[4] = 0x0badc0deU;
    for (index = 0U; index < ISAAC_VITA_RTTI_CDECL_ARGUMENT_COUNT; ++index)
        s_frame[5U + index] = arguments[index];
    c->eax = 0xa5a5a5a5U;
    c->ecx = 0x11111111U;
    c->edx = 0x22222222U;
    c->ebx = 0x33333333U;
    c->ebp = 0x44444444U;
    c->esi = 0x55555555U;
    c->edi = 0x66666666U;
    c->esp = pointer32(&s_frame[4]);
    return c->esp;
}

static int preserved_nonreturn_registers(const CPU *c)
{
    return c->ecx == 0x11111111U && c->edx == 0x22222222U &&
           c->ebx == 0x33333333U && c->ebp == 0x44444444U &&
           c->esi == 0x55555555U && c->edi == 0x66666666U;
}

static int call_case(uint32_t input, uint32_t source, uint32_t target,
                     uint32_t is_reference, uint32_t expected,
                     int expect_fault, unsigned *calls)
{
    uint32_t arguments[5] = { input, 0U, source, target, is_reference };
    uint8_t image_before[sizeof s_image];
    uint32_t object_before[sizeof s_object / sizeof s_object[0]];
    CPU c;
    uint32_t esp;
    unsigned before = *calls;

    memcpy(image_before, s_image, sizeof image_before);
    memcpy(object_before, s_object, sizeof object_before);
    esp = prepare_call(&c, arguments);
    if (!isaac_vita_rtti_import_counted(
            &c, ISAAC_VITA_RTTI_IMPORT_NAME, calls) ||
        *calls != before + 1U || c.eax != expected ||
        !preserved_nonreturn_registers(&c) ||
        memcmp(image_before, s_image, sizeof image_before) != 0 ||
        memcmp(object_before, s_object, sizeof object_before) != 0)
        return 0;
    if (expect_fault)
        return c.fault != NULL && c.esp == esp;
    return c.fault == NULL && c.esp == esp + 4U &&
           s_frame[4] == 0x0badc0deU;
}

static int check_null_short_circuit(unsigned *calls)
{
    fixture_reset();
    return call_case(0U, 0xdead0001U, 0xdead0002U, 0U, 0U, 0, calls);
}

static int check_pointer_and_name_matches(unsigned *calls)
{
    uint32_t object = pointer32(s_object);

    fixture_reset();
    if (!call_case(object, image_address(TEST_SOURCE_TD),
                   image_address(TEST_TARGET_TD), 0U, object, 0, calls))
        return 0;
    return call_case(object, image_address(TEST_SOURCE_TD),
                     image_address(TEST_TARGET_ALIAS_TD),
                     0U, object, 0, calls);
}

static int check_failures(unsigned *calls)
{
    uint32_t object = pointer32(s_object);

    fixture_reset();
    if (!call_case(object, image_address(TEST_SOURCE_TD),
                   image_address(TEST_MISSING_TD), 0U, 0U, 0, calls))
        return 0;

    /* A private/protected source base is a clean pointer-cast failure. */
    image_write32(TEST_SOURCE_BCD + 20U, 0x04U);
    if (!call_case(object, image_address(TEST_SOURCE_TD),
                   image_address(TEST_TARGET_TD), 0U, 0U, 0, calls))
        return 0;

    /* Reference failure and any MI/VI hierarchy remain deliberately loud.
     * Both also prove that counted dispatch increments before metadata work. */
    fixture_reset();
    if (!call_case(object, image_address(TEST_SOURCE_TD),
                   image_address(TEST_MISSING_TD), 1U, 0U, 1, calls))
        return 0;
    fixture_reset();
    image_write32(TEST_CHD + 4U, 0x01U);
    return call_case(object, image_address(TEST_SOURCE_TD),
                     image_address(TEST_TARGET_TD), 0U,
                     0xa5a5a5a5U, 1, calls);
}

static int check_negative_dispatch(unsigned *calls)
{
    static const char other[] = "VCRUNTIME140.dll!memchr";
    uint32_t arguments[5] = { 0U, 0U, 0U, 0U, 0U };
    CPU c;
    uint32_t esp = prepare_call(&c, arguments);
    unsigned before = *calls;

    return isaac_vita_rtti_import_counted(&c, other, calls) == 0 &&
           *calls == before && c.esp == esp && c.eax == 0xa5a5a5a5U &&
           c.fault == NULL;
}

int main(void)
{
    unsigned calls = 0U;

    if (strcmp(ISAAC_VITA_RTTI_IMPORT_NAME,
               "VCRUNTIME140.dll!__RTDynamicCast") != 0)
        return 1;
    if (!check_negative_dispatch(&calls))
        return 2;
    if (!check_null_short_circuit(&calls))
        return 3;
    if (!check_pointer_and_name_matches(&calls))
        return 4;
    if (!check_failures(&calls))
        return 5;
    return calls == 7U ? 0 : 6;
}
