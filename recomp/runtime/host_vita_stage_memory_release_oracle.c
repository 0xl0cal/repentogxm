/* Release-path oracle for stage/floor lifetime transitions.  It deliberately
 * does not provide mallinfo or isaac_vita_log: any diagnostic call retained in
 * this configuration must fail at compile/link time. */
#include <stdint.h>
#include <stdio.h>

#define ISAAC_VITA_STAGE_MEMORY_RELEASE_ORACLE 1
#define ISAAC_VITA_HEAP_TESTING 1
#define ISAAC_VITA_HEAP_RANGE_LEASE 1

#include "host_vita_heap.c"

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
#error Release stage-memory oracle unexpectedly enabled diagnostics
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "stage-memory release oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    (void)address;
    if (c)
        c->fault = what ? what : "stage-memory release oracle fault";
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)c;
    (void)pc;
    (void)kind;
    (void)address;
    (void)size;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)c;
    (void)pc;
    return 0;
}

int main(void)
{
    static const vita_heap_floor_lifetime_state floor_initial =
        VITA_HEAP_FLOOR_LIFETIME_STATE_INITIALIZER;

    s_floor_lifetime = floor_initial;
    isaac_vita_stage_memory_test_reset();

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 1U, 2U, UINT32_MAX);
    CHECK(s_stage_memory.active == 1U);
    CHECK(s_floor_lifetime.protocol_valid == 1U);
    CHECK(s_floor_lifetime.epoch == 1U);
    CHECK(s_floor_lifetime.bootstrap_count == 1U);
    CHECK(s_floor_lifetime.phase ==
          ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT);

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 5U, 3U, UINT32_MAX);
    CHECK(s_stage_memory.room_open == 1U);
    CHECK(s_floor_lifetime.phase ==
          ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD);

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 5U, 3U, UINT32_MAX);
    CHECK(s_stage_memory.room_after_unload == 1U);
    CHECK(s_floor_lifetime.phase ==
          ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD);

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 5U, 3U, 1U);
    CHECK(s_stage_memory.room_open == 0U);
    CHECK(s_floor_lifetime.phase ==
          ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT);

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 1U, 2U, UINT32_MAX);
    CHECK(s_stage_memory.active == 0U);
    CHECK(s_floor_lifetime.phase ==
          ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY);
    CHECK(s_floor_lifetime.protocol_valid == 1U);
    CHECK(s_stage_memory.sequence == 5U);
    return 0;
}
