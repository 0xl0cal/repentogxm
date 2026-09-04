#ifndef KAGE_VITA_CONTINUE_PROFILE_H
#define KAGE_VITA_CONTINUE_PROFILE_H

#include <stdint.h>

/* Frozen-PE aggregate boundaries for the two blocking actions reached from
 * the save menu.  Values are part of the generated-code/native ABI and are
 * intentionally explicit so the codegen oracle can reject drift. */
enum IsaacVitaContinueProfileEvent {
    ISAAC_VITA_CONTINUE_FILE_BEGIN = 1,
    ISAAC_VITA_CONTINUE_PERSISTENT_BEGIN = 2,
    ISAAC_VITA_CONTINUE_PERSISTENT_END = 3,
    ISAAC_VITA_CONTINUE_GAMESTATE_LOAD_BEGIN = 4,
    ISAAC_VITA_CONTINUE_GAMESTATE_LOAD_END = 5,
    ISAAC_VITA_CONTINUE_GAMESTATE_READ_BEGIN = 6,
    ISAAC_VITA_CONTINUE_GAMESTATE_READ_END = 7,
    ISAAC_VITA_CONTINUE_FILE_END = 8,
    ISAAC_VITA_CONTINUE_CANDIDATE_BEGIN = 9,
    ISAAC_VITA_CONTINUE_RESTORE_BEGIN = 10,
    ISAAC_VITA_CONTINUE_RESTORE_END = 11,
    ISAAC_VITA_CONTINUE_CANDIDATE_END = 12,
    ISAAC_VITA_CONTINUE_PLAYER_CREATE_BEGIN = 13,
    ISAAC_VITA_CONTINUE_PLAYER_CREATE_END = 14,
    ISAAC_VITA_CONTINUE_ITEMPOOL_INIT_BEGIN = 15,
    ISAAC_VITA_CONTINUE_ITEMPOOL_INIT_END = 16,
    ISAAC_VITA_CONTINUE_SFX_LOAD_BEGIN = 17,
    ISAAC_VITA_CONTINUE_SFX_LOAD_END = 18,
    ISAAC_VITA_CONTINUE_ITEMPOOL_RESTORE_BEGIN = 19,
    ISAAC_VITA_CONTINUE_ITEMPOOL_RESTORE_END = 20,
    ISAAC_VITA_CONTINUE_PLAYERS_PRE_BEGIN = 21,
    ISAAC_VITA_CONTINUE_PLAYERS_PRE_END = 22,
    ISAAC_VITA_CONTINUE_LEVEL_RESTORE_BEGIN = 23,
    ISAAC_VITA_CONTINUE_LEVEL_RESTORE_END = 24,
    ISAAC_VITA_CONTINUE_PLAYERS_POST_BEGIN = 25,
    ISAAC_VITA_CONTINUE_PLAYERS_POST_END = 26,
    ISAAC_VITA_CONTINUE_ROOM_LOAD_BEGIN = 27,
    ISAAC_VITA_CONTINUE_ROOM_LOAD_END = 28,
    ISAAC_VITA_CONTINUE_GUEST_LOG_BEGIN = 29,
    ISAAC_VITA_CONTINUE_GUEST_LOG_WRITE_BEGIN = 30,
    ISAAC_VITA_CONTINUE_GUEST_LOG_WRITE_END = 31,
    ISAAC_VITA_CONTINUE_GUEST_LOG_FLUSH_BEGIN = 32,
    ISAAC_VITA_CONTINUE_GUEST_LOG_FLUSH_END = 33,
    ISAAC_VITA_CONTINUE_GUEST_LOG_END = 34
};

/* Disabled production builds compile the generated hook calls out entirely.
 * Enabled builds still have no per-frame or per-I/O hook. */
void isaac_vita_continue_profile_note(uint32_t event, uint32_t value);

/* The durable native logger is timed as one aggregate bucket.  platform.c
 * calls these only when ISAAC_VITA_CONTINUE_PROFILE is explicitly enabled,
 * so the normal logger gains no calls or branches. */
void isaac_vita_continue_profile_log_begin(void);
void isaac_vita_continue_profile_log_end(void);

#endif
