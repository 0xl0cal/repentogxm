#ifndef KAGE_VITA_EXIT_MENU_PROFILE_H
#define KAGE_VITA_EXIT_MENU_PROFILE_H

#include <stdint.h>

/* Frozen-PE boundaries for bounded gameplay -> menu rebuild attribution.
 * Values are shared with generated code and pinned by the codegen oracle. */
enum IsaacVitaExitMenuProfileEvent {
    ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN = 1,
    ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN = 2,
    ISAAC_VITA_EXIT_MENU_SET_SAVE_END = 3,
    ISAAC_VITA_EXIT_MENU_CTOR_BEGIN = 4,
    ISAAC_VITA_EXIT_MENU_CTOR_END = 5,
    ISAAC_VITA_EXIT_MENU_INIT_BEGIN = 6,
    ISAAC_VITA_EXIT_MENU_INIT_END = 7,
    ISAAC_VITA_EXIT_MENU_POST_BEGIN = 8,
    ISAAC_VITA_EXIT_MENU_POST_END = 9,
    ISAAC_VITA_EXIT_MENU_PERSISTENT_BEGIN = 10,
    ISAAC_VITA_EXIT_MENU_PERSISTENT_END = 11,
    ISAAC_VITA_EXIT_MENU_GAMESTATE_BEGIN = 12,
    ISAAC_VITA_EXIT_MENU_GAMESTATE_END = 13,
    ISAAC_VITA_EXIT_MENU_READ_BEGIN = 14,
    ISAAC_VITA_EXIT_MENU_READ_END = 15,
    ISAAC_VITA_EXIT_MENU_CHECKSUM_BEGIN = 16,
    ISAAC_VITA_EXIT_MENU_CHECKSUM_END = 17,
    ISAAC_VITA_EXIT_MENU_CANDIDATE_END = 18,
    ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT = 19
};

/* Disabled production builds compile every generated call site out. */
void isaac_vita_exit_menu_profile_note(uint32_t event, uint32_t value);

/* The CRT owner calls these immediately around its existing native fread.
 * The producer ignores calls unless the owner thread is inside sub_00524320. */
void isaac_vita_exit_menu_profile_fread_begin(uint32_t requested_bytes);
void isaac_vita_exit_menu_profile_fread_end(uint32_t returned_bytes);

#if defined(ISAAC_VITA_EXIT_MENU_PROFILE_ORACLE)
void isaac_vita_exit_menu_profile_oracle_reset(void);
#endif

#endif
