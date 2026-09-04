#ifndef KAGE_VITA_SIM_CADENCE_RECEIPT_H
#define KAGE_VITA_SIM_CADENCE_RECEIPT_H

#include <stdint.h>

/* Optional, single-threaded evidence for the rate of the game's real
 * Game::Update calls.  The generated entry seam calls note_game_update once
 * and performs no clock read, log write, atomic operation or pacing change.
 * The existing present-heartbeat boundary owns the infrequent report. */
void kage_vita_sim_cadence_reset(void);
void kage_vita_sim_cadence_note_game_update(
    uint32_t game_pointer, uint32_t game_frame);
void kage_vita_sim_cadence_report_present_heartbeat(
    uint32_t present_count, uint32_t elapsed_ms);

#endif
