#include "kage_vita_sim_cadence_receipt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_CAPACITY 8192u

static char s_log[LOG_CAPACITY];
static size_t s_log_size;
static unsigned s_checks;

#define CHECK(condition)                                                     \
    do {                                                                     \
        ++s_checks;                                                          \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int kage_vita_sim_cadence_oracle_printf(const char *format, ...)
{
    va_list args;
    int written;

    if (s_log_size >= sizeof s_log)
        return -1;
    va_start(args, format);
    written = vsnprintf(
        s_log + s_log_size, sizeof s_log - s_log_size, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof s_log - s_log_size)
        return -1;
    s_log_size += (size_t)written;
    return written;
}

static unsigned count_lines(void)
{
    unsigned lines = 0u;
    size_t index;

    for (index = 0u; index < s_log_size; ++index) {
        if (s_log[index] == '\n')
            ++lines;
    }
    return lines;
}

static void note_range(
    uint32_t game_pointer, uint32_t first_frame, uint32_t count)
{
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        kage_vita_sim_cadence_note_game_update(
            game_pointer, first_frame + index);
    }
}

int main(void)
{
    kage_vita_sim_cadence_reset();
    kage_vita_sim_cadence_report_present_heartbeat(1u, 100u);
    CHECK(strstr(s_log,
        "bid=sim:oracle seq=1 present=1 elapsed_ms=100 "
        "win(v,p,ms,u,ups_milli)=0,0,0,0,0 total_u=0") != NULL);

    note_range(0x91234000u, 100u, 60u);
    kage_vita_sim_cadence_report_present_heartbeat(121u, 2100u);
    CHECK(strstr(s_log,
        "seq=2 present=121 elapsed_ms=2100 "
        "win(v,p,ms,u,ups_milli)=1,120,2000,60,30000 total_u=60") != NULL);
    CHECK(strstr(s_log,
        "game=0x91234000 frame(prev,now,delta,valid)=0,159,0,0") != NULL);

    note_range(0x91234000u, 160u, 60u);
    kage_vita_sim_cadence_report_present_heartbeat(241u, 4100u);
    CHECK(strstr(s_log,
        "seq=3 present=241 elapsed_ms=4100 "
        "win(v,p,ms,u,ups_milli)=1,120,2000,60,30000 total_u=120") != NULL);
    CHECK(strstr(s_log,
        "game=0x91234000 frame(prev,now,delta,valid)=159,219,60,1") != NULL);

    note_range(0x92345000u, 0u, 60u);
    kage_vita_sim_cadence_report_present_heartbeat(361u, 6100u);
    CHECK(strstr(s_log,
        "seq=4 present=361 elapsed_ms=6100 "
        "win(v,p,ms,u,ups_milli)=1,120,2000,60,30000 total_u=180") != NULL);
    CHECK(strstr(s_log,
        "game=0x92345000 frame(prev,now,delta,valid)=219,59,0,0") != NULL);

    kage_vita_sim_cadence_report_present_heartbeat(481u, 8100u);
    CHECK(strstr(s_log,
        "seq=5 present=481 elapsed_ms=8100 "
        "win(v,p,ms,u,ups_milli)=1,120,2000,0,0 total_u=180") != NULL);
    CHECK(strstr(s_log,
        "game=0x92345000 frame(prev,now,delta,valid)=59,59,0,1") != NULL);

    /* A regressed clock/present baseline is explicit invalid evidence, never
     * unsigned arithmetic that could look like an overspeed sample. */
    kage_vita_sim_cadence_report_present_heartbeat(470u, 8000u);
    CHECK(strstr(s_log,
        "seq=6 present=470 elapsed_ms=8000 "
        "win(v,p,ms,u,ups_milli)=0,0,0,0,0") != NULL);
    CHECK(count_lines() == 6u);

    kage_vita_sim_cadence_reset();
    s_log_size = 0u;
    s_log[0] = '\0';
    kage_vita_sim_cadence_report_present_heartbeat(600u, 9000u);
    CHECK(strstr(s_log,
        "seq=1 present=600 elapsed_ms=9000 "
        "win(v,p,ms,u,ups_milli)=0,0,0,0,0 total_u=0") != NULL);
    CHECK(count_lines() == 1u);

    printf("sim cadence receipt oracle: PASS (%u checks)\n", s_checks);
    return 0;
}
