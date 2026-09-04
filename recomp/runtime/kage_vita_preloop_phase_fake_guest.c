#include <stdint.h>

#include "guest.h"
#include "kage_vita_preloop_phase.h"

uint32_t kage_vita_preloop_phase_oracle_real_calls[
    KAGE_VITA_PRELOOP_PHASE_COUNT];
uint32_t kage_vita_preloop_phase_oracle_failure;
uint32_t kage_vita_preloop_phase_oracle_inner_glfw_calls;

void kage_vita_preloop_phase_oracle_reenter(CPU *__restrict c);
void kage_vita_preloop_phase_oracle_call_archive(CPU *__restrict c);
void kage_vita_preloop_phase_oracle_platform_body(CPU *__restrict c);
void kage_vita_preloop_phase_oracle_archive_body(void);

static void check_active(unsigned slot, uint32_t required_mask,
                         uint32_t required_depth)
{
    kage_vita_preloop_phase_snapshot snapshot;

    kage_vita_preloop_phase_get_snapshot(&snapshot);
    if ((snapshot.active_mask & required_mask) != required_mask ||
        snapshot.slots[slot].current_depth != required_depth)
        kage_vita_preloop_phase_oracle_failure = 1u;
    ++kage_vita_preloop_phase_oracle_real_calls[slot];
}

/* Mirror the production shape: this inner target and sub_0059fa30 share one
 * TU, so GNU ld --wrap cannot be trusted to rewrite their direct call. */
__attribute__((noinline))
void sub_0059b7a0(CPU *__restrict c)
{
    kage_vita_preloop_phase_snapshot snapshot;

    (void)c;
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    if ((snapshot.active_mask &
            (1u << KAGE_VITA_PRELOOP_GLFW_ERROR)) == 0u ||
        snapshot.slots[KAGE_VITA_PRELOOP_GLFW_ERROR].current_depth != 1u)
        kage_vita_preloop_phase_oracle_failure = 1u;
    ++kage_vita_preloop_phase_oracle_inner_glfw_calls;
}

void sub_0059fa30(CPU *__restrict c)
{
    check_active(
        KAGE_VITA_PRELOOP_GLFW_ERROR,
        1u << KAGE_VITA_PRELOOP_GLFW_ERROR, 1u);
    sub_0059b7a0(c);
}

void sub_00598c80(CPU *__restrict c)
{
    (void)c;
    check_active(
        KAGE_VITA_PRELOOP_PLATFORM_INIT,
        1u << KAGE_VITA_PRELOOP_PLATFORM_INIT, 1u);
    kage_vita_preloop_phase_oracle_platform_body(c);
}

void sub_0050b240(CPU *__restrict c)
{
    kage_vita_preloop_phase_snapshot snapshot;
    uint32_t calls = kage_vita_preloop_phase_oracle_real_calls[
        KAGE_VITA_PRELOOP_ISAAC_STARTUP];
    uint32_t required_mask =
        1u << KAGE_VITA_PRELOOP_ISAAC_STARTUP;
    uint32_t depth = calls == 0u ? 1u : 2u;

    check_active(
        KAGE_VITA_PRELOOP_ISAAC_STARTUP, required_mask, depth);
    if (calls == 0u) {
        kage_vita_preloop_phase_oracle_reenter(c);
        kage_vita_preloop_phase_oracle_call_archive(c);
        kage_vita_preloop_phase_get_snapshot(&snapshot);
        if (snapshot.active_mask != required_mask ||
            snapshot.slots[KAGE_VITA_PRELOOP_ISAAC_STARTUP].
                current_depth != 1u)
            kage_vita_preloop_phase_oracle_failure = 1u;
    }
}

void sub_00563880(CPU *__restrict c)
{
    (void)c;
    check_active(
        KAGE_VITA_PRELOOP_LOAD_ARCHIVE,
        1u << KAGE_VITA_PRELOOP_LOAD_ARCHIVE, 1u);
    kage_vita_preloop_phase_oracle_archive_body();
}

void sub_004ab7a0(CPU *__restrict c)
{
    (void)c;
    check_active(
        KAGE_VITA_PRELOOP_MANAGER_CONSTRUCT,
        1u << KAGE_VITA_PRELOOP_MANAGER_CONSTRUCT, 1u);
}

void sub_005aeb00(CPU *__restrict c)
{
    (void)c;
    check_active(
        KAGE_VITA_PRELOOP_MINIZ_STATE,
        1u << KAGE_VITA_PRELOOP_MINIZ_STATE, 1u);
}

void sub_005c2fd0(CPU *__restrict c)
{
    (void)c;
    check_active(
        KAGE_VITA_PRELOOP_INFLATE_STATE,
        1u << KAGE_VITA_PRELOOP_INFLATE_STATE, 1u);
}

void sub_005ce7d0(CPU *__restrict c)
{
    (void)c;
    check_active(
        KAGE_VITA_PRELOOP_INFLATE_BLOCKS,
        1u << KAGE_VITA_PRELOOP_INFLATE_BLOCKS, 1u);
}
