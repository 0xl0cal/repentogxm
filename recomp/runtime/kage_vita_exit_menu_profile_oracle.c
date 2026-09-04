/* Host state/order/saturation/bounds oracle for Exit -> menu attribution. */
#include "kage_vita_exit_menu_profile.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORACLE_LOGS 24u
#define ORACLE_LOG_SIZE 512u
#define ORACLE_MAX_LINE 383u

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Exit-menu oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint64_t s_now;
static int s_thread = 7;
static unsigned s_clock_calls;
static char s_logs[ORACLE_LOGS][ORACLE_LOG_SIZE];
static unsigned s_log_count;
static unsigned s_long_lines;

int sceKernelGetThreadId(void)
{
    return s_thread;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++s_clock_calls;
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
    if (result >= 0) {
        if ((unsigned)result > ORACLE_MAX_LINE)
            ++s_long_lines;
        ++s_log_count;
    }
    return result;
}

static void reset(void)
{
    isaac_vita_exit_menu_profile_oracle_reset();
    s_now = 0u;
    s_thread = 7;
    s_clock_calls = 0u;
    memset(s_logs, 0, sizeof s_logs);
    s_log_count = 0u;
    s_long_lines = 0u;
}

static void note(uint64_t now, uint32_t event, uint32_t value)
{
    s_now = now;
    isaac_vita_exit_menu_profile_note(event, value);
}

static void fread_begin(uint64_t now, uint32_t requested)
{
    s_now = now;
    isaac_vita_exit_menu_profile_fread_begin(requested);
}

static void fread_end(uint64_t now, uint32_t returned)
{
    s_now = now;
    isaac_vita_exit_menu_profile_fread_end(returned);
}

static int log_has(unsigned index, const char *needle)
{
    return index < s_log_count && strstr(s_logs[index], needle) != NULL;
}

static void record(uint64_t base, uint32_t slot,
                   uint32_t requested, uint32_t returned)
{
    note(base, ISAAC_VITA_EXIT_MENU_PERSISTENT_BEGIN, slot);
    note(base + 10u, ISAAC_VITA_EXIT_MENU_PERSISTENT_END, 0u);
    note(base + 20u, ISAAC_VITA_EXIT_MENU_GAMESTATE_BEGIN, slot);
    note(base + 30u, ISAAC_VITA_EXIT_MENU_READ_BEGIN, 0u);
    fread_begin(base + 40u, requested);
    fread_end(base + 50u, returned);
    note(base + 60u, ISAAC_VITA_EXIT_MENU_CHECKSUM_BEGIN, 50u);
    note(base + 70u, ISAAC_VITA_EXIT_MENU_CHECKSUM_END, 0u);
    note(base + 100u, ISAAC_VITA_EXIT_MENU_READ_END, 0u);
    note(base + 120u, ISAAC_VITA_EXIT_MENU_GAMESTATE_END, 0u);
}

static int run_normal(void)
{
    unsigned before;
    unsigned index;
    unsigned record_index;
    char rebuild_needle[32];
    static const uint32_t slots[4] = {1u, 2u, 3u, 1u};

    reset();
    /* A normal per-frame candidate which never takes SetSaveSlot vanishes. */
    note(9u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0u);
    note(10u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 0u);
    note(20u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 0u && s_clock_calls == 0u);

    /* A live Game without the helper's work byte must not read the clock. */
    note(21u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0x1234u);
    note(22u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 0u);
    note(23u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 0u && s_clock_calls == 0u);

    /* A rejected cross-thread token is consumed and cannot go stale. */
    note(30u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0x1234u);
    s_thread = 8;
    note(31u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    s_thread = 7;
    note(32u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 0u);
    note(33u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
    note(34u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 0u && s_clock_calls == 0u);

    /* Constructor promotion alone is not enough: a candidate which performs
     * no PersistentData/GameState work is discarded without a report. */
    note(40u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0x1234u);
    note(41u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    note(42u, ISAAC_VITA_EXIT_MENU_CTOR_BEGIN, 0u);
    note(43u, ISAAC_VITA_EXIT_MENU_CTOR_END, 0u);
    note(44u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 0u && s_clock_calls == 4u);

    note(99u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0x1234u);
    note(100u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    /* The physical gameplay Exit uses Manager's second menu-helper call.  It
     * can reach the constructor without first taking the pre-constructor
     * SetSaveSlot edge, so the exact constructor edge must also promote. */
    note(130u, ISAAC_VITA_EXIT_MENU_CTOR_BEGIN, 0u);
    note(140u, ISAAC_VITA_EXIT_MENU_CTOR_END, 0u);
    note(150u, ISAAC_VITA_EXIT_MENU_INIT_BEGIN, 0u);
    record(160u, 1u, 100u, 90u);
    record(300u, 2u, 200u, 180u);
    record(440u, 3u, 300u, 270u);
    record(580u, 1u, 400u, 360u);

    /* Same callback shapes from a foreign thread must not affect counts. */
    s_thread = 8;
    fread_begin(705u, 999u);
    fread_end(706u, 999u);
    note(707u, ISAAC_VITA_EXIT_MENU_CHECKSUM_BEGIN, 999u);
    note(708u, ISAAC_VITA_EXIT_MENU_CHECKSUM_END, 0u);
    s_thread = 7;

    note(720u, ISAAC_VITA_EXIT_MENU_INIT_END, 0u);
    note(730u, ISAAC_VITA_EXIT_MENU_POST_BEGIN, 0u);
    note(750u, ISAAC_VITA_EXIT_MENU_POST_END, 0u);
    note(800u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);

    CHECK(s_log_count == 5u && s_long_lines == 0u);
    CHECK(log_has(0u, "rebuild=1 gs#1 slot=1 pd=10 gs=100 read=70"));
    CHECK(log_has(0u, "load_other=30 read_other=50"));
    CHECK(log_has(0u, "fread=1/100/90/10/10"));
    CHECK(log_has(0u, "checksum=1/50/10/10"));
    CHECK(log_has(1u, "gs#2 slot=2"));
    CHECK(log_has(2u, "gs#3 slot=3"));
    CHECK(log_has(3u, "gs#4 slot=1"));
    CHECK(log_has(4u, "total=700 set=0 ctor=10 init=570 post=20"));
    CHECK(log_has(4u, "other=100 records=4+0 faults=0x00"));

    /* Four complete 5-line rebuilds are retained; this also proves that all
     * four per-GameState records reset between rebuilds. */
    for (index = 0u; index < 3u; ++index) {
        uint64_t base = 900u + index * 1000u;
        unsigned line = 5u + index * 5u;
        note(base - 1u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0x1234u);
        note(base, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
        note(base + 10u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
        note(base + 20u, ISAAC_VITA_EXIT_MENU_SET_SAVE_END, 0u);
        for (record_index = 0u; record_index < 4u; ++record_index)
            record(base + 40u + record_index * 140u,
                   slots[record_index], 10u, 9u);
        note(base + 620u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
        CHECK(log_has(line, "gs#1 slot=1"));
        CHECK(log_has(line + 1u, "gs#2 slot=2"));
        CHECK(log_has(line + 2u, "gs#3 slot=3"));
        CHECK(log_has(line + 3u, "gs#4 slot=1"));
        (void)snprintf(rebuild_needle, sizeof rebuild_needle,
                       "rebuild=%u", index + 2u);
        CHECK(log_has(line + 4u, rebuild_needle));
        CHECK(log_has(line + 4u, "records=4+0 faults=0x00"));
    }
    CHECK(s_log_count == 20u && s_long_lines == 0u);
    before = s_log_count;
    note(3999u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 0x1234u);
    note(4000u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    note(4010u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
    note(4020u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == before);
    return 0;
}

static int run_order_fault(void)
{
    reset();
    note(99u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 1u);
    note(100u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    note(110u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
    note(111u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
    note(120u, ISAAC_VITA_EXIT_MENU_SET_SAVE_END, 0u);
    note(121u, ISAAC_VITA_EXIT_MENU_PERSISTENT_BEGIN, 1u);
    note(122u, ISAAC_VITA_EXIT_MENU_PERSISTENT_END, 0u);
    note(130u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 2u && s_long_lines == 0u);
    CHECK(log_has(1u, "faults=0x01"));
    return 0;
}

static int run_saturation_and_max(void)
{
    reset();
    note(0u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 1u);
    note(1u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    note(2u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
    note(3u, ISAAC_VITA_EXIT_MENU_SET_SAVE_END, 0u);
    note(4u, ISAAC_VITA_EXIT_MENU_GAMESTATE_BEGIN, 9u);
    note(5u, ISAAC_VITA_EXIT_MENU_READ_BEGIN, 0u);
    fread_begin(10u, UINT32_MAX);
    fread_end(15u, UINT32_MAX);
    fread_begin(20u, UINT32_MAX);
    fread_end(28u, UINT32_MAX);
    note(30u, ISAAC_VITA_EXIT_MENU_READ_END, 0u);
    note(40u, ISAAC_VITA_EXIT_MENU_GAMESTATE_END, 0u);
    note((uint64_t)UINT32_MAX + 100u,
         ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 2u && s_long_lines == 0u);
    CHECK(log_has(0u, "fread=2/4294967295/4294967295/13/8"));
    CHECK(log_has(1u, "total=4294967295"));
    CHECK(log_has(1u, "faults=0x04"));
    return 0;
}

static int run_record_bound(void)
{
    unsigned index;

    reset();
    note(99u, ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT, 1u);
    note(100u, ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN, 1u);
    note(101u, ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN, 0u);
    note(102u, ISAAC_VITA_EXIT_MENU_SET_SAVE_END, 0u);
    for (index = 0u; index < 4u; ++index) {
        note(110u + index * 10u,
             ISAAC_VITA_EXIT_MENU_PERSISTENT_BEGIN, index + 1u);
        note(115u + index * 10u,
             ISAAC_VITA_EXIT_MENU_PERSISTENT_END, 0u);
    }
    note(155u, ISAAC_VITA_EXIT_MENU_PERSISTENT_BEGIN, 5u);
    note(170u, ISAAC_VITA_EXIT_MENU_CANDIDATE_END, 0u);
    CHECK(s_log_count == 5u && s_long_lines == 0u);
    CHECK(log_has(4u, "records=4+1 faults=0x10"));
    return 0;
}

int main(void)
{
    CHECK(run_normal() == 0);
    CHECK(run_order_fault() == 0);
    CHECK(run_saturation_and_max() == 0);
    CHECK(run_record_bound() == 0);
    puts("Exit-menu native oracle: PASS; qualified no-idle-clock "
         "state/order/foreign/stale slot-order=1,2,3,1 saturation/max "
         "and <=4x5 logs <384");
    return 0;
}
