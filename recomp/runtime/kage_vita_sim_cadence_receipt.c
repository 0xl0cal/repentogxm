#include "kage_vita_sim_cadence_receipt.h"

#include <stdint.h>
#include <string.h>

#if defined(ISAAC_KAGE_VITA_SIM_CADENCE_ORACLE)
int kage_vita_sim_cadence_oracle_printf(const char *format, ...);
# define KAGE_VITA_SIM_CADENCE_LOG kage_vita_sim_cadence_oracle_printf
#else
# include <psp2/kernel/clib.h>
# define KAGE_VITA_SIM_CADENCE_LOG sceClibPrintf
#endif

#ifndef ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID
# define ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID "sim-cadence:unstamped"
#endif

typedef struct KageVitaSimCadenceState {
    uint32_t update_count;
    uint32_t game_pointer;
    uint32_t game_frame;
    uint32_t have_game_sample;
    uint32_t report_count;
    uint32_t prior_present_count;
    uint32_t prior_elapsed_ms;
    uint32_t prior_update_count;
    uint32_t prior_game_pointer;
    uint32_t prior_game_frame;
    uint32_t prior_have_game_sample;
} KageVitaSimCadenceState;

static KageVitaSimCadenceState s_cadence;

void kage_vita_sim_cadence_reset(void)
{
    memset(&s_cadence, 0, sizeof s_cadence);
}

void kage_vita_sim_cadence_note_game_update(
    uint32_t game_pointer, uint32_t game_frame)
{
    ++s_cadence.update_count;
    s_cadence.game_pointer = game_pointer;
    s_cadence.game_frame = game_frame;
    s_cadence.have_game_sample = game_pointer != 0u;
}

void kage_vita_sim_cadence_report_present_heartbeat(
    uint32_t present_count, uint32_t elapsed_ms)
{
    uint32_t window_valid = 0u;
    uint32_t present_delta = 0u;
    uint32_t elapsed_delta = 0u;
    uint32_t update_delta = 0u;
    uint32_t ups_milli = 0u;
    uint32_t frame_delta = 0u;
    uint32_t frame_delta_valid = 0u;

    if (s_cadence.report_count != 0u &&
            present_count >= s_cadence.prior_present_count &&
            elapsed_ms >= s_cadence.prior_elapsed_ms) {
        window_valid = 1u;
        present_delta = present_count - s_cadence.prior_present_count;
        elapsed_delta = elapsed_ms - s_cadence.prior_elapsed_ms;
        update_delta = s_cadence.update_count -
            s_cadence.prior_update_count;
        if (elapsed_delta != 0u) {
            ups_milli = (uint32_t)(
                ((uint64_t)update_delta * UINT64_C(1000000)) /
                elapsed_delta);
        }
        if (s_cadence.prior_have_game_sample != 0u &&
                s_cadence.have_game_sample != 0u &&
                s_cadence.game_pointer ==
                    s_cadence.prior_game_pointer &&
                s_cadence.game_frame >= s_cadence.prior_game_frame) {
            frame_delta = s_cadence.game_frame -
                s_cadence.prior_game_frame;
            frame_delta_valid = 1u;
        }
    }

    ++s_cadence.report_count;
    KAGE_VITA_SIM_CADENCE_LOG(
        "[kage-vita] sim cadence bid=%.48s seq=%u present=%u "
        "elapsed_ms=%u win(v,p,ms,u,ups_milli)=%u,%u,%u,%u,%u "
        "total_u=%u game=0x%08x frame(prev,now,delta,valid)="
        "%u,%u,%u,%u\n",
        ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID,
        (unsigned)s_cadence.report_count,
        (unsigned)present_count, (unsigned)elapsed_ms,
        (unsigned)window_valid, (unsigned)present_delta,
        (unsigned)elapsed_delta, (unsigned)update_delta,
        (unsigned)ups_milli, (unsigned)s_cadence.update_count,
        (unsigned)s_cadence.game_pointer,
        (unsigned)s_cadence.prior_game_frame,
        (unsigned)s_cadence.game_frame, (unsigned)frame_delta,
        (unsigned)frame_delta_valid);

    s_cadence.prior_present_count = present_count;
    s_cadence.prior_elapsed_ms = elapsed_ms;
    s_cadence.prior_update_count = s_cadence.update_count;
    s_cadence.prior_game_pointer = s_cadence.game_pointer;
    s_cadence.prior_game_frame = s_cadence.game_frame;
    s_cadence.prior_have_game_sample = s_cadence.have_game_sample;
}
