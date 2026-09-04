#include <stdint.h>
#include <stdio.h>

#include "guest.h"
#include "kage_vita_preloop_phase.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita pre-loop phase oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

extern uint32_t kage_vita_preloop_phase_oracle_real_calls[
    KAGE_VITA_PRELOOP_PHASE_COUNT];
extern uint32_t kage_vita_preloop_phase_oracle_failure;
extern uint32_t kage_vita_preloop_phase_oracle_inner_glfw_calls;

void sub_0059fa30(CPU *__restrict c);
void sub_00598c80(CPU *__restrict c);
void sub_0050b240(CPU *__restrict c);
void sub_00563880(CPU *__restrict c);
void sub_004ab7a0(CPU *__restrict c);
void sub_005aeb00(CPU *__restrict c);
void sub_005c2fd0(CPU *__restrict c);
void sub_005ce7d0(CPU *__restrict c);

static uint32_t s_reentered;
static uint32_t s_reset_during_platform;
static uint32_t s_call_archive_from_platform;
static uint32_t s_reset_during_archive;

void kage_vita_preloop_phase_oracle_reenter(CPU *__restrict c)
{
    if (!s_reentered) {
        s_reentered = 1u;
        sub_0050b240(c);
    }
}

void kage_vita_preloop_phase_oracle_call_archive(CPU *__restrict c)
{
    sub_00563880(c);
}

void kage_vita_preloop_phase_oracle_platform_body(CPU *__restrict c)
{
    kage_vita_preloop_phase_snapshot snapshot;

    if (s_call_archive_from_platform) {
        s_call_archive_from_platform = 0u;
        sub_00563880(c);
    }
    if (!s_reset_during_platform)
        return;
    s_reset_during_platform = 0u;
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    if (snapshot.active_mask !=
            (1u << KAGE_VITA_PRELOOP_PLATFORM_INIT) ||
        snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].current_depth != 1u)
        kage_vita_preloop_phase_oracle_failure = 1u;
    kage_vita_preloop_phase_reset();
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    if (snapshot.active_mask !=
            (1u << KAGE_VITA_PRELOOP_PLATFORM_INIT) ||
        snapshot.total_enter_count != 1u ||
        snapshot.total_exit_count != 0u ||
        snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].current_depth != 1u ||
        snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].max_depth != 1u)
        kage_vita_preloop_phase_oracle_failure = 1u;
}

void kage_vita_preloop_phase_oracle_archive_body(void)
{
    kage_vita_preloop_phase_snapshot snapshot;
    uint32_t expected_mask =
        (1u << KAGE_VITA_PRELOOP_PLATFORM_INIT) |
        (1u << KAGE_VITA_PRELOOP_LOAD_ARCHIVE);

    if (!s_reset_during_archive)
        return;
    s_reset_during_archive = 0u;
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    if (snapshot.active_mask != expected_mask ||
        snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].current_depth != 1u ||
        snapshot.slots[KAGE_VITA_PRELOOP_LOAD_ARCHIVE].current_depth != 1u)
        kage_vita_preloop_phase_oracle_failure = 1u;
    kage_vita_preloop_phase_reset();
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    if (snapshot.active_mask != expected_mask ||
        snapshot.packed_depths != 0x00001010u ||
        snapshot.total_enter_count != 2u ||
        snapshot.total_exit_count != 0u)
        kage_vita_preloop_phase_oracle_failure = 1u;
}

int main(void)
{
    CPU cpu = {0};
    kage_vita_preloop_phase_snapshot snapshot;
    unsigned slot;

    kage_vita_preloop_phase_get_snapshot(NULL);
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    CHECK(snapshot.active_mask == 0u);
    CHECK(snapshot.packed_depths == 0u);
    CHECK(snapshot.total_enter_count == 0u);
    CHECK(snapshot.total_exit_count == 0u);

    sub_0059fa30(&cpu);
    sub_00598c80(&cpu);
    sub_0050b240(&cpu);
    sub_004ab7a0(&cpu);
    sub_005aeb00(&cpu);
    sub_005c2fd0(&cpu);
    sub_005ce7d0(&cpu);

    kage_vita_preloop_phase_get_snapshot(&snapshot);
    CHECK(kage_vita_preloop_phase_oracle_failure == 0u);
    CHECK(kage_vita_preloop_phase_oracle_inner_glfw_calls == 1u);
    CHECK(snapshot.active_mask == 0u);
    CHECK(snapshot.packed_depths == 0u);
    CHECK(snapshot.total_enter_count == 9u);
    CHECK(snapshot.total_exit_count == 9u);
    for (slot = 0u; slot < KAGE_VITA_PRELOOP_PHASE_COUNT; ++slot) {
        uint32_t expected =
            slot == KAGE_VITA_PRELOOP_ISAAC_STARTUP ? 2u : 1u;

        CHECK(kage_vita_preloop_phase_oracle_real_calls[slot] == expected);
        CHECK(snapshot.slots[slot].enter_count == expected);
        CHECK(snapshot.slots[slot].exit_count == expected);
        CHECK(snapshot.slots[slot].current_depth == 0u);
        CHECK(snapshot.slots[slot].max_depth == expected);
    }

    kage_vita_preloop_phase_reset();
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    CHECK(snapshot.active_mask == 0u);
    CHECK(snapshot.packed_depths == 0u);
    CHECK(snapshot.total_enter_count == 0u);
    CHECK(snapshot.total_exit_count == 0u);
    for (slot = 0u; slot < KAGE_VITA_PRELOOP_PHASE_COUNT; ++slot) {
        CHECK(snapshot.slots[slot].enter_count == 0u);
        CHECK(snapshot.slots[slot].exit_count == 0u);
        CHECK(snapshot.slots[slot].current_depth == 0u);
        CHECK(snapshot.slots[slot].max_depth == 0u);
    }
    sub_0059fa30(&cpu);
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    CHECK(snapshot.total_enter_count == 1u);
    CHECK(snapshot.total_exit_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_GLFW_ERROR].max_depth == 1u);
    CHECK(kage_vita_preloop_phase_oracle_real_calls[
        KAGE_VITA_PRELOOP_GLFW_ERROR] == 2u);
    CHECK(kage_vita_preloop_phase_oracle_inner_glfw_calls == 2u);

    kage_vita_preloop_phase_reset();
    s_reset_during_platform = 1u;
    sub_00598c80(&cpu);
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    CHECK(kage_vita_preloop_phase_oracle_failure == 0u);
    CHECK(s_reset_during_platform == 0u);
    CHECK(snapshot.active_mask == 0u);
    CHECK(snapshot.packed_depths == 0u);
    CHECK(snapshot.total_enter_count == 1u);
    CHECK(snapshot.total_exit_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].enter_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].exit_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].current_depth == 0u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].max_depth == 1u);

    kage_vita_preloop_phase_reset();
    s_call_archive_from_platform = 1u;
    s_reset_during_archive = 1u;
    sub_00598c80(&cpu);
    kage_vita_preloop_phase_get_snapshot(&snapshot);
    CHECK(kage_vita_preloop_phase_oracle_failure == 0u);
    CHECK(s_call_archive_from_platform == 0u);
    CHECK(s_reset_during_archive == 0u);
    CHECK(snapshot.active_mask == 0u);
    CHECK(snapshot.packed_depths == 0u);
    CHECK(snapshot.total_enter_count == 2u);
    CHECK(snapshot.total_exit_count == 2u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].enter_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].exit_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_LOAD_ARCHIVE].enter_count == 1u);
    CHECK(snapshot.slots[KAGE_VITA_PRELOOP_LOAD_ARCHIVE].exit_count == 1u);

    puts("Vita pre-loop linker-wrap phase oracle: PASS");
    return 0;
}
