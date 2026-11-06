/* PS Vita trophies for Isaac achievements (ISAAC_VITA_TROPHIES).
 *
 * The game unlocks achievements 1..637 through
 * PersistentGameData::TryUnlock(id) (sub_00484c80).  gen_all.py emits a
 * knob-fenced observer at its entry (both translated copies: the owner and
 * the tail-jump copy inside sub_00485ab0); the observer replicates the
 * body's pure guards and queues every predicted fresh unlock.  A worker
 * thread mirrors each one as a trophy in six local NoTrpDrm trophy sets
 * ISAC00000_00..ISAC00000_05 (no PSN).  The translated body always runs
 * after the observer, so guest behaviour is the same with the knob on or
 * off.
 *
 * Device run 2026-10-09: at most two contexts may be live, and every new
 * context needs its own setup dialog, which can only be shown at Stage B.
 * So two sets (the gameplay pair) stay live during play; an unlock for any
 * other set is kept in ux0:/data/isaacr001/trophy-pending.bin and
 * delivered at the next game start.
 *
 *   Stage A  isaac_vita_trophies_boot()     entry_vita.c, before vitaGL and
 *            before guest_register_all(): NP module, sceNpTrophyInit,
 *            semaphore, worker thread, the pending store, the gameplay
 *            pair (from the saves), import/test requests.  No context.
 *   Stage B  isaac_vita_trophies_setup(fn)  kage_vita_backend.c cold path,
 *            right after vitaGL init: each set with queued trophies is
 *            opened (one setup dialog pumped by fn = vglSwapBuffers(GL_TRUE),
 *            bounded), delivered by the worker (bounded wait) and destroyed;
 *            then the pair is opened and handed to the worker.
 *   observer isaac_vita_trophies_try_unlock_observe()   game thread, never
 *            waits: atomic OR into a 640-bit pending set + one semaphore
 *            signal.
 *
 * Any failure turns off only what it touches (one set, or the feature);
 * nothing asserts, nothing loops without a bound, the game never waits on
 * trophies.  Manager contract (ux0:/data/isaacr001/trophy-import.req /
 * trophy-import.res), log lines and the device test plan:
 * recomp/vita/TROPHIES.md.
 *
 * Declares nothing without ISAAC_VITA_TROPHIES; CMake attaches the
 * definition per source (the generated owner, this unit, entry_vita.c,
 * kage_vita_backend.c) only when the option is ON. */
#ifndef HOST_VITA_TROPHIES_H
#define HOST_VITA_TROPHIES_H

#if defined(ISAAC_VITA_TROPHIES)

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* == CMake ISAAC_TITLE_ID (recomp/test_vita_trophies.py checks it). */
#define ISAAC_VITA_TROPHY_TITLE_ID        "ISAACR001"
/* NP communication id base of the six sets (<base>_00 .. <base>_05), ==
 * CMake ISAAC_VITA_TROPHY_COMM_BASE.  Device run 2026-10-09 (FW 3.65,
 * NoTrpDrm 1.1): SceShell refuses ISAACR001_0N at the setup dialog with
 * C0-11133-9 while ISAC00000_0N (4 letters + 5 digits, the NPWR##### shape)
 * installs and works.  The title id stays ISAACR001 (data paths, logs). */
#define ISAAC_VITA_TROPHY_COMM_BASE       "ISAC00000"
#define ISAAC_VITA_TROPHY_SET_COUNT       6u
#define ISAAC_VITA_TROPHY_ACHIEVEMENT_MAX 637u

typedef void (*isaac_vita_trophies_present_fn)(void);

/* Stage A, entry_vita.c (main thread, before vitaGL and the guest). */
void isaac_vita_trophies_boot(void);
/* Stage B, kage_vita_backend.c (GL thread, no scene open); idempotent. */
void isaac_vita_trophies_setup(isaac_vita_trophies_present_fn present);
/* The generated TryUnlock entry seam (gen_all.py
 * render_vita_trophy_unlock_seam): pgd = ECX, id = [esp+4], return_rva =
 * [esp] (a bare RVA). */
void isaac_vita_trophies_try_unlock_observe(uint32_t pgd, uint32_t id,
                                            uint32_t return_rva);
/* Achievement id -> (set 0..5, trophy 1..125); 1 = mapped, 0 for 0, 325,
 * 336, 354 and > 637.  Trophy 000 (the platinum) never comes out. */
int isaac_vita_trophies_map(uint32_t achievement_id, uint32_t *set,
                            uint32_t *trophy);

#if defined(ISAAC_VITA_TROPHIES_ORACLE)
/* One drain pass of the worker (the Vita thread loops wait-sema + step);
 * returns the number of ids it resolved. */
int isaac_vita_trophies_worker_step(void);
/* Back to the pre-boot state (static flags, bitmaps, counters). */
void isaac_vita_trophies_oracle_reset(void);
/* The save checksum over data[0..size) (the same table and update the
 * streaming reader uses). */
uint32_t isaac_vita_trophies_oracle_checksum(const void *data, uint32_t size);
/* The gameplay pair chosen at Stage A (index 0 or 1; -1 = none). */
int isaac_vita_trophies_oracle_pair(uint32_t index);
/* Pretends the worker is inside an NP call on `set` (-1 = none). */
void isaac_vita_trophies_oracle_worker_inside(int set);
/* Contexts the runtime believes it holds. */
uint32_t isaac_vita_trophies_oracle_contexts(void);
/* The persistent pending store (trophy-pending.bin) driven directly. */
void isaac_vita_trophies_oracle_store_mark(uint32_t id);
int isaac_vita_trophies_oracle_store_has(uint32_t id);
uint32_t isaac_vita_trophies_oracle_store_count(void);
void isaac_vita_trophies_oracle_store_set_state(uint32_t set,
                                                const uint32_t bits[4]);
uint32_t isaac_vita_trophies_oracle_store_known(void);
uint32_t isaac_vita_trophies_oracle_store_state_word(uint32_t set,
                                                     uint32_t word);
int isaac_vita_trophies_oracle_store_dirty(void);
void isaac_vita_trophies_oracle_store_load(void);
void isaac_vita_trophies_oracle_store_save(void);

/* Host oracle seams (host_vita_trophies_oracle.c defines them) in place of
 * every SDK call.  SCE conventions: 0 / a descriptor / a handle >= 0 on
 * success, a negative SCE error otherwise. */
uint8_t isaac_vita_trophies_fake_read8(uint32_t address);
uint32_t isaac_vita_trophies_fake_read32(uint32_t address);
int isaac_vita_trophies_fake_sysmodule_load(uint32_t module);
int isaac_vita_trophies_fake_np_init(void);
int isaac_vita_trophies_fake_create_context(int *context, const void *comm_id,
                                            const void *signature,
                                            uint64_t options);
int isaac_vita_trophies_fake_destroy_context(int context);
/* One 10 ms poll of a Stage B wait (the oracle runs the worker here). */
void isaac_vita_trophies_fake_wait_tick(void);
int isaac_vita_trophies_fake_create_handle(int *handle);
int isaac_vita_trophies_fake_destroy_handle(int handle);
int isaac_vita_trophies_fake_unlock(int context, int handle, int trophy,
                                    int *platinum);
int isaac_vita_trophies_fake_unlock_state(int context, int handle,
                                          uint32_t flags[4], int *count);
int isaac_vita_trophies_fake_setup_init(uint32_t sdk_version, int context,
                                        uint32_t options, uint32_t magic,
                                        const void *common_param);
int isaac_vita_trophies_fake_setup_status(void);
int isaac_vita_trophies_fake_setup_result(int *result);
int isaac_vita_trophies_fake_setup_abort(void);
int isaac_vita_trophies_fake_setup_term(void);
int isaac_vita_trophies_fake_sema_create(const char *name, int initial,
                                         int maximum);
int isaac_vita_trophies_fake_sema_signal(int sema);
int isaac_vita_trophies_fake_thread_create(const char *name, int priority,
                                           uint32_t stack, uint32_t affinity);
int isaac_vita_trophies_fake_thread_start(int thread);
void isaac_vita_trophies_fake_delay(uint32_t microseconds);
uint64_t isaac_vita_trophies_fake_time_us(void);
int isaac_vita_trophies_fake_open_read(const char *path);
int isaac_vita_trophies_fake_read(int descriptor, void *buffer, uint32_t size);
int64_t isaac_vita_trophies_fake_size(int descriptor);
int isaac_vita_trophies_fake_close(int descriptor);
int isaac_vita_trophies_fake_remove(const char *path);
int isaac_vita_trophies_fake_write_file(const char *path, const void *data,
                                        uint32_t size);
/* O_EXCL create + write + sync + close (fails if the file exists). */
int isaac_vita_trophies_fake_write_new(const char *path, const void *data,
                                       uint32_t size);
/* Fails if the destination exists (the publish protocol moves it aside). */
int isaac_vita_trophies_fake_rename(const char *source,
                                    const char *destination);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ISAAC_VITA_TROPHIES */

#endif /* HOST_VITA_TROPHIES_H */
