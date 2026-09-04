/* Host oracle for the aggregate FILE/Continue profiler and optional overlay. */
#include "kage_vita_continue_profile.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORACLE_WIDTH 960u
#define ORACLE_HEIGHT 544u
#define ORACLE_PIXELS (ORACLE_WIDTH * ORACLE_HEIGHT)
#define ORACLE_LOGS 8u
#define ORACLE_LOG_SIZE 1024u
#define ORACLE_REDZONE 0x6a17c35du

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "continue-profile oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint64_t s_now;
static int s_thread = 7;
static int s_loading_active;
static char s_logs[ORACLE_LOGS][ORACLE_LOG_SIZE];
static unsigned s_log_count;
static void (*s_callback)(void *);
static void (*s_last_callback)(void *);
static unsigned s_callback_sets;
static unsigned s_callback_clears;
static unsigned s_queue_finishes;
static unsigned s_swaps;
static uint32_t s_framebuffer[ORACLE_PIXELS + 2u];

int sceKernelGetThreadId(void)
{
    return s_thread;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    return s_now;
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    int result;

    if (s_log_count >= ORACLE_LOGS)
        return -1;
    va_start(arguments, format);
    result = vsnprintf(s_logs[s_log_count], ORACLE_LOG_SIZE,
                       format, arguments);
    va_end(arguments);
    if (result >= 0)
        ++s_log_count;
    return result;
}

int kage_vita_loading_active(void)
{
    return s_loading_active;
}

void vglSetDisplayCallback(void (*callback)(void *framebuffer))
{
    if (callback) {
        s_callback = callback;
        s_last_callback = callback;
        ++s_callback_sets;
    } else {
        s_callback = NULL;
        ++s_callback_clears;
    }
}

void vglSwapBuffers(uint8_t has_common_dialog)
{
    (void)has_common_dialog;
    ++s_swaps;
    if (s_callback) {
        s_framebuffer[0] = ORACLE_REDZONE;
        s_framebuffer[ORACLE_PIXELS + 1u] = ORACLE_REDZONE;
        memset(&s_framebuffer[1], 0,
               ORACLE_PIXELS * sizeof(s_framebuffer[0]));
        s_callback(&s_framebuffer[1]);
        if (s_framebuffer[0] != ORACLE_REDZONE ||
            s_framebuffer[ORACLE_PIXELS + 1u] != ORACLE_REDZONE) {
            fprintf(stderr, "continue overlay crossed framebuffer redzone\n");
            exit(2);
        }
    }
}

int sceGxmDisplayQueueFinish(void)
{
    if (s_callback != NULL) {
        fprintf(stderr, "continue overlay drained before callback removal\n");
        exit(3);
    }
    ++s_queue_finishes;
    return 0;
}

static void note(uint64_t now, uint32_t event, uint32_t value)
{
    s_now = now;
    isaac_vita_continue_profile_note(event, value);
}

static int log_has(unsigned index, const char *needle)
{
    return index < s_log_count && strstr(s_logs[index], needle) != NULL;
}

static int verify_stale_callback(void)
{
    unsigned index;

    CHECK(s_last_callback != NULL);
    for (index = 0u; index < ORACLE_PIXELS + 2u; ++index)
        s_framebuffer[index] = ORACLE_REDZONE;
    s_last_callback(&s_framebuffer[1]);
    for (index = 0u; index < ORACLE_PIXELS + 2u; ++index)
        CHECK(s_framebuffer[index] == ORACLE_REDZONE);
    return 0;
}

static int run_file_profile(void)
{
    note(100u, ISAAC_VITA_CONTINUE_FILE_BEGIN, 0u);
    note(110u, ISAAC_VITA_CONTINUE_PERSISTENT_BEGIN, 0u);
    s_now = 210u;
    isaac_vita_continue_profile_log_begin();
    s_now = 230u;
    isaac_vita_continue_profile_log_end();
    s_thread = 8;
    s_now = 240u;
    isaac_vita_continue_profile_log_begin();
    s_now = 250u;
    isaac_vita_continue_profile_log_end();
    note(260u, ISAAC_VITA_CONTINUE_GUEST_LOG_BEGIN, 0u);
    note(270u, ISAAC_VITA_CONTINUE_GUEST_LOG_END, 0u);
    s_thread = 7;
    note(410u, ISAAC_VITA_CONTINUE_PERSISTENT_END, 0u);
    note(450u, ISAAC_VITA_CONTINUE_GAMESTATE_LOAD_BEGIN, 0u);
    note(500u, ISAAC_VITA_CONTINUE_GUEST_LOG_BEGIN, 0u);
    note(510u, ISAAC_VITA_CONTINUE_GUEST_LOG_WRITE_BEGIN, 64u);
    note(530u, ISAAC_VITA_CONTINUE_GUEST_LOG_WRITE_END, 0u);
    note(540u, ISAAC_VITA_CONTINUE_GUEST_LOG_FLUSH_BEGIN, 0u);
    note(590u, ISAAC_VITA_CONTINUE_GUEST_LOG_FLUSH_END, 0u);
    note(600u, ISAAC_VITA_CONTINUE_GUEST_LOG_END, 0u);
    note(620u, ISAAC_VITA_CONTINUE_GAMESTATE_READ_BEGIN, 0u);
    note(920u, ISAAC_VITA_CONTINUE_GAMESTATE_READ_END, 0u);
    note(1050u, ISAAC_VITA_CONTINUE_GAMESTATE_LOAD_END, 0u);
    note(1100u, ISAAC_VITA_CONTINUE_FILE_END, 0u);

    CHECK(s_log_count == 1u);
    CHECK(log_has(0u, "file total=1000 persistent=300 loadstate=600"));
    CHECK(log_has(0u, "read=300 load_other=300 top_other=100"));
    CHECK(log_has(0u, "nlog=1/20 glog=1/100"));
    CHECK(log_has(0u, "write=1/64/20 flush=1/50"));
    CHECK(log_has(0u, "overlay_swaps=4 faults=0x00"));
    CHECK(s_callback_sets == 1u && s_callback_clears == 1u);
    CHECK(s_queue_finishes == 1u && s_swaps == 4u);
    return verify_stale_callback();
}

static int run_continue_profile(void)
{
    /* A new-game candidate must vanish without a report or display owner. */
    note(1200u, ISAAC_VITA_CONTINUE_CANDIDATE_BEGIN, 0u);
    note(1300u, ISAAC_VITA_CONTINUE_CANDIDATE_END, 0u);
    CHECK(s_log_count == 1u && s_callback_sets == 1u);

    note(2000u, ISAAC_VITA_CONTINUE_CANDIDATE_BEGIN, 0u);
    note(2010u, ISAAC_VITA_CONTINUE_RESTORE_BEGIN, 0u);
    note(2020u, ISAAC_VITA_CONTINUE_PLAYER_CREATE_BEGIN, 0u);
    note(2120u, ISAAC_VITA_CONTINUE_PLAYER_CREATE_END, 0u);
    note(2130u, ISAAC_VITA_CONTINUE_ITEMPOOL_INIT_BEGIN, 0u);
    note(2140u, ISAAC_VITA_CONTINUE_SFX_LOAD_BEGIN, 0u);
    note(2170u, ISAAC_VITA_CONTINUE_SFX_LOAD_END, 0u);
    note(2180u, ISAAC_VITA_CONTINUE_SFX_LOAD_BEGIN, 0u);
    note(2230u, ISAAC_VITA_CONTINUE_SFX_LOAD_END, 0u);
    note(2240u, ISAAC_VITA_CONTINUE_ITEMPOOL_INIT_END, 0u);
    note(2250u, ISAAC_VITA_CONTINUE_ITEMPOOL_RESTORE_BEGIN, 0u);
    note(2350u, ISAAC_VITA_CONTINUE_ITEMPOOL_RESTORE_END, 0u);
    note(2360u, ISAAC_VITA_CONTINUE_PLAYERS_PRE_BEGIN, 0u);
    note(2460u, ISAAC_VITA_CONTINUE_PLAYERS_PRE_END, 0u);
    note(2470u, ISAAC_VITA_CONTINUE_LEVEL_RESTORE_BEGIN, 0u);
    note(2480u, ISAAC_VITA_CONTINUE_ROOM_LOAD_BEGIN, 0u);
    note(2580u, ISAAC_VITA_CONTINUE_ROOM_LOAD_END, 1u);
    note(2590u, ISAAC_VITA_CONTINUE_ROOM_LOAD_BEGIN, 7u);
    note(2790u, ISAAC_VITA_CONTINUE_ROOM_LOAD_END, 1u);
    note(2800u, ISAAC_VITA_CONTINUE_ROOM_LOAD_BEGIN, 13u);
    note(2850u, ISAAC_VITA_CONTINUE_ROOM_LOAD_END, 1u);
    note(2870u, ISAAC_VITA_CONTINUE_LEVEL_RESTORE_END, 0u);
    note(2880u, ISAAC_VITA_CONTINUE_PLAYERS_POST_BEGIN, 0u);
    note(2980u, ISAAC_VITA_CONTINUE_PLAYERS_POST_END, 0u);
    note(3100u, ISAAC_VITA_CONTINUE_RESTORE_END, 0u);
    note(3150u, ISAAC_VITA_CONTINUE_CANDIDATE_END, 0u);

    CHECK(s_log_count == 2u);
    CHECK(log_has(1u, "run total=1150 restore=1090 outer_other=60"));
    CHECK(log_has(1u, "player_create=100 pool_init=110 sfx=2/80"));
    CHECK(log_has(1u, "pool_restore=100 players=100/100 level=400"));
    CHECK(log_has(1u, "rooms=3/350 restore_other=180"));
    CHECK(log_has(1u, "room4=0:100,7:200,13:50,4294967295:0"));
    CHECK(log_has(1u, "overlay_swaps=12 faults=0x00"));
    CHECK(s_callback_sets == 2u && s_callback_clears == 2u);
    CHECK(s_queue_finishes == 2u && s_swaps == 16u);
    return verify_stale_callback();
}

static int run_loading_owner_conflict(void)
{
    s_loading_active = 1;
    note(4000u, ISAAC_VITA_CONTINUE_FILE_BEGIN, 0u);
    note(4010u, ISAAC_VITA_CONTINUE_FILE_END, 0u);
    s_loading_active = 0;
    CHECK(s_log_count == 3u);
    CHECK(log_has(2u, "file total=10"));
    CHECK(log_has(2u, "overlay_swaps=0 faults=0x40"));
    CHECK(s_callback_sets == 2u && s_callback_clears == 2u);
    CHECK(s_queue_finishes == 2u && s_swaps == 16u);
    return 0;
}

int main(void)
{
    CHECK(run_file_profile() == 0);
    CHECK(run_continue_profile() == 0);
    CHECK(run_loading_owner_conflict() == 0);
    puts("continue-profile native oracle: PASS; file=1 run=1 new=discarded "
         "foreign=ignored overlay=2x lifecycle+redzone+stale "
         "conflict=closed");
    return 0;
}
