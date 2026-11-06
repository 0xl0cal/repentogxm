/* ISAAC_VITA_LAUNCH_ARGS_FILE (default OFF, bench/profile knob): extra guest
 * command-line options from ISAAC_VITA_NATIVE_DATA_ROOT "/launch-args.txt".
 *
 * The guest's main() walks argv[1..argc) for --luadebug, --luaheapsize=,
 * --set-stage=, --set-stage-type= and --load-room= (isaac-ng.exe 0x90b26d..
 * 0x90be31); --set-stage starts a run directly and skips the menus
 * (0x90be37 sets the manager's start-game fields).  The Vita CRT shim hands
 * out argv = { "isaac-ng.exe" }; with this knob it appends the whitespace
 * separated tokens of the file.  Absent file = unchanged argv.  No quoting,
 * '#' starts a comment that runs to the end of the line.
 *
 * Save slot.  The game keeps the save slot in
 * Manager+0x10.  Isaac.LoadModData/SaveModData/HasModData/RemoveModData
 * build "<mod data dir>/save%d.dat" from it, mapping 0 to 1
 * (0040d7d0..0040d7de, guest_0122.c:8064-8074 of the int3-off corpus), and
 * the "RNG Start Seed ... [New, %d]" game-start lines print it.  Manager::Init
 * (sub_004ad610) clears it, 004ad7fa mov dword ptr [edi + 0x10], 0
 * (guest_0143.c:16893), and only LoadPersistentGameData (sub_004b4500,
 * 004b4768/004b4781 mov [reg + 0x10], ebx, guest_0144.c:14145/14182) sets it,
 * which only the save menus call.  A --set-stage boot never visits the menus,
 * so the slot stays 0 = File 1's mod data.  The host-only token
 * "--vita-save-slot=N" (N = 1..3) makes the CRT fopen owner store N into
 * Manager+0x10 at the first guest fopen after Manager::Init stored the Game
 * pointer (004ad841 mov dword ptr [0x987fd65c], eax: 0x47 bytes after the
 * clear, no branch in between), i.e. before any mod script runs, and put it
 * back (loudly, event=restored) if a later fopen finds it changed.  The token
 * never reaches the guest argv.  Fail-safe: a file with --set-stage= and no
 * slot token, a slot outside 1..3 or two slot tokens is rejected as a whole
 * (status=no-slot / bad-slot, guest argv unchanged = normal menu boot,
 * nothing stored). */
#ifndef ISAAC_VITA_LAUNCH_ARGS_H
#define ISAAC_VITA_LAUNCH_ARGS_H

#include <stdint.h>

#include "platform.h"

#define ISAAC_VITA_LAUNCH_ARGS_PATH ISAAC_VITA_NATIVE_DATA_ROOT "/launch-args.txt"
#define ISAAC_VITA_LAUNCH_ARGS_MAX 8u
#define ISAAC_VITA_LAUNCH_ARGS_BYTES 512u

#define ISAAC_VITA_LAUNCH_ARGS_SLOT_TOKEN "--vita-save-slot="
#define ISAAC_VITA_LAUNCH_ARGS_SET_STAGE_TOKEN "--set-stage="
/* RVA of the Manager pointer: 0050b973 mov dword ptr [0x987fd680], eax
 * (guest_0154.c:5481) with the image at 0x98000000. */
#define ISAAC_VITA_LAUNCH_ARGS_MANAGER_RVA 0x007FD680u
/* RVA of the Game pointer: 004ad841 mov dword ptr [0x987fd65c], eax
 * (Manager::Init, after the slot clear; GFUEL_G_GAME in
 * host_vita_gfuel_seed_native.c is the same word). */
#define ISAAC_VITA_LAUNCH_ARGS_GAME_RVA 0x007FD65Cu
/* 0040d7d5 mov eax, dword ptr [eax + 0x10] (guest_0122.c:8066). */
#define ISAAC_VITA_LAUNCH_ARGS_MANAGER_SLOT 0x10u
/* event=restored receipts after the first one are counted, not logged. */
#define ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX 8u

#ifdef __cplusplus
extern "C" {
#endif

/* Splits text[0..length) in place (NUL-terminates every token) and stores at
 * most cap token pointers in out.  Returns the number of tokens found, which
 * may exceed cap (the caller rejects that). */
unsigned isaac_vita_launch_args_split(char *text, unsigned length,
                                      char **out, unsigned cap);

/* Reads ISAAC_VITA_LAUNCH_ARGS_PATH into static storage and stores up to
 * ISAAC_VITA_LAUNCH_ARGS_MAX pointers into it in out (the guest tokens; a
 * slot token is consumed, not stored).  Returns the count (0 when absent,
 * unreadable, larger than ISAAC_VITA_LAUNCH_ARGS_BYTES, holding more than
 * ISAAC_VITA_LAUNCH_ARGS_MAX tokens, or rejected by the slot rule: all or
 * nothing).  Logs one "ISAAC VITA LAUNCH ARGS:" receipt with the resolved
 * command line; argv0 is only used for that receipt. */
unsigned isaac_vita_launch_args_load(const char *argv0, char **out);

/* The slot of an accepted --vita-save-slot= token (1..3), 0 = none. */
unsigned isaac_vita_launch_args_save_slot(void);

/* Called by the CRT fopen owner with the guest addresses of the Manager and
 * Game pointers and the native path being opened.  Without a slot: returns 0
 * and reads nothing.  Until both pointers are non-zero: counts the call
 * (waited=) and returns 0.  The first time both are: stores the slot at
 * Manager+0x10, logs "ISAAC VITA LAUNCH SAVE SLOT: event=applied ..." and
 * returns 1.  Later: when Manager+0x10 no longer holds the slot, stores it
 * again, logs event=restored (the first ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX
 * times) and returns 1; otherwise returns 0 and writes nothing. */
int isaac_vita_launch_args_slot_apply(uint32_t manager_global,
                                      uint32_t game_global,
                                      const char *trigger);

#if defined(ISAAC_VITA_LAUNCH_ARGS_ORACLE)
int isaac_vita_launch_args_oracle_open(const char *path);
int isaac_vita_launch_args_oracle_read(int descriptor, void *buffer,
                                       unsigned size);
int isaac_vita_launch_args_oracle_close(int descriptor);
uint32_t isaac_vita_launch_args_oracle_ld32(uint32_t address);
void isaac_vita_launch_args_oracle_st32(uint32_t address, uint32_t value);
/* Forget the parsed slot and the applied flag (one oracle case per launch). */
void isaac_vita_launch_args_oracle_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
