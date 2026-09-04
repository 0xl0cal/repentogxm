/* Loud high-level KAGE boundary for Vita before a native backend exists.
 *
 * These nine symbols replace the generated public roots just as the PC
 * manual backend does, but none may report fake graphics/audio success. */
#include "manual_kage.h"
#include "guest_coverage_generated.h"

int guest_kage_pc_handoff_ready(void)
{
    return 0;
}

int guest_kage_vsync_policy_allows(uint32_t refresh_hz)
{
    uint32_t remainder;
    uint32_t distance;
    if (refresh_hz <= 1U)
        return 0;
    remainder = refresh_hz % 60U;
    distance = remainder <= 30U ? remainder : 60U - remainder;
    return distance <= 2U;
}

void sub_00481280(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SET_VSYNC_ID);
    guest_fault(c, GUEST_KAGE_VSYNC_RVA, GUEST_KAGE_VSYNC_FAULT);
}

void sub_00560c60(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_ID);
    guest_fault(c, GUEST_KAGE_INITIALIZE_RVA, GUEST_KAGE_INITIALIZE_FAULT);
}

void sub_00560e30(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SHUTDOWN_ID);
    guest_fault(c, 0x00560e30U,
                "KAGE graphics backend unavailable on Vita: Shutdown");
}

void sub_00560eb0(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_PRESENT_ID);
    guest_fault(c, 0x00560eb0U,
                "KAGE graphics backend unavailable on Vita: Present");
}

void sub_00560f20(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_WIDTH_ID);
    guest_fault(c, 0x00560f20U,
                "KAGE graphics backend unavailable on Vita: width");
}

void sub_00560f90(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_HEIGHT_ID);
    guest_fault(c, 0x00560f90U,
                "KAGE graphics backend unavailable on Vita: height");
}

void sub_00561830(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_RENDER_DISPLAY_ID);
    guest_fault(c, GUEST_KAGE_RENDER_DISPLAY_RVA,
                GUEST_KAGE_RENDER_DISPLAY_FAULT);
}

void sub_0056dd70(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SOUND_INITIALIZE_ID);
    guest_fault(c, GUEST_KAGE_SOUND_INITIALIZE_RVA,
                GUEST_KAGE_SOUND_INITIALIZE_FAULT);
}

void sub_005700e0(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GL_PROVIDER_RESOLVER_ID);
    /* Without a ready platform backend, retain libepoxy's exact behaviour. */
    guest_original_005700e0(c);
}
