/* PS Vita trophies for Isaac achievements; see host_vita_trophies.h and
 * recomp/vita/TROPHIES.md (manager contract, log lines, device plan).
 *
 * Design: the trophy design of 2026-09-27 (recomp/vita/TROPHIES.md),
 * reworked 2026-10-09 for the device facts (two live contexts at most,
 * one setup dialog per context instance, comm id ISAC00000_0N): the
 * 2-context pool.
 * Every SDK call sits behind a static seam so the host oracle
 * (host_vita_trophies_oracle.c, ISAAC_VITA_TROPHIES_ORACLE) drives the
 * observer, both boot stages, the worker and every failure row without a
 * Vita.  No allocator and no stdio: state is static, receipts go through
 * KAGE_VITA_TROPHY_LOG (isaac_vita_log), the files this unit writes are
 * built by tiny formatters.
 *
 * Threads: the observer runs on the game thread and only reads guest
 * memory, ORs a bit into an atomic pending set and signals the semaphore;
 * Stage A runs on the main thread before the guest exists (no context),
 * Stage B on the same thread inside KAGE Initialize: it owns context
 * creation, setup dialogs and destruction.  The worker owns every handle /
 * unlock / state call, the unlock cache and the persistent pending store.
 * The slot protocol (trophy_enter / trophy_destroy) keeps a context from
 * being destroyed while the worker may be using it. */
#include "host_vita_trophies.h"

#if !defined(ISAAC_VITA_TROPHIES)
#error "host_vita_trophies.c must only be compiled with ISAAC_VITA_TROPHIES"
#endif

#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#if defined(ISAAC_VITA_TROPHIES_ORACLE)
# if !defined(GUEST_IMAGE_BASE)
#  define GUEST_IMAGE_BASE 0x98000000u
# endif
/* SDK stand-ins for the host build only, values copied from vitasdk
 * vita-headers (e66ebe9, 2026-09-26): psp2/sysmodule.h:64,
 * psp2common/defs.h:11, psp2/common_dialog.h:58,63,106-121,
 * psp2/kernel/cpu.h:19.  The Vita build includes the real headers. */
# define SCE_SYSMODULE_NP_TROPHY 0x0025
# define PSP2_SDK_VERSION 0x03570011
# define SCE_COMMON_DIALOG_STATUS_RUNNING 1
# define SCE_COMMON_DIALOG_RESULT_OK 0
# define SCE_KERNEL_CPU_MASK_USER_2 0x00040000
typedef struct SceCommonDialogParam {
    void *infobarParam;
    void *bgColor;
    void *dimmerColor;
    uint8_t reserved[60];
    uint32_t magic;
} SceCommonDialogParam;
#else
# if !defined(GUEST_IMAGE_BASE) || GUEST_IMAGE_BASE != 0x98000000u
#  error "Vita trophies read guest memory at GUEST_IMAGE_BASE 0x98000000"
# endif
# include <psp2/common_dialog.h>
# include <psp2/io/fcntl.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/threadmgr.h>
# include <psp2/sysmodule.h>
#endif

#ifndef KAGE_VITA_TROPHY_LOG
#define KAGE_VITA_TROPHY_LOG isaac_vita_log
#endif

/* ---- constants ------------------------------------------------------- */

/* PersistentGameData manager pointer: one store at 0090b973;
 * TryUnlock reads it at 00484cb9 `mov esi, [0xbfd680]` (VA; gen_all.py
 * VITA_TROPHY_UNLOCK_MANAGER_PTR_RVA).  PGD = mgr + 0x14. */
#define TROPHY_MGR_PTR_RVA       0x7fd680u
#define TROPHY_PGD_OFFSET        0x14u
/* 00484c8e `cmp byte ptr [edi + 1], 0` (ReadOnly). */
#define TROPHY_PGD_READONLY      0x1u
/* 00484ca3 `cmp byte ptr [ebx + edi + 0x38], 0` (unlocked[id]). */
#define TROPHY_PGD_UNLOCKED      0x38u
#define TROPHY_ID_MAX            637u
#define TROPHY_FIRST_LOGGED      4u

/* Save layout and checksum, read off the frozen PE
 * (magic 0xb573dc, CRC sub_0065b340/0065b4b0, reader 0x882112..0x88215d). */
#define TROPHY_SAVE_MAGIC        "ISAACNGSAVE09R  "
#define TROPHY_SAVE_MAGIC_BYTES  16u
#define TROPHY_SAVE_CRC_BEGIN    0x10u
#define TROPHY_SAVE_CHUNK        0x14u
#define TROPHY_SAVE_DATA         0x20u
#define TROPHY_SAVE_SLOTS        0x27eu
#define TROPHY_SAVE_HEAD_BYTES   (TROPHY_SAVE_DATA + TROPHY_SAVE_SLOTS)
#define TROPHY_CRC_POLY          0xEDB88320u
#define TROPHY_CRC_INIT          0xFEDCBA76u

/* Vita3K local/data/web/src/vita3k_SceNpTrophy.cpp:49 and :52; both seen
 * on the device (device run 2026-10-09). */
#define TROPHY_ERROR_ALREADY_UNLOCKED 0x8055160fu
#define TROPHY_ERROR_SETUP_REQUIRED   0x80551612u

/* Context pool, device run 2026-10-09 (FW 3.65, NoTrpDrm 1.1): the third
 * live sceNpTrophyCreateContext returns 0x80551606 (EXCEEDS_MAX),
 * sceNpTrophyDestroyContext frees the slot, and every new context instance
 * needs its own setup dialog (else 0x80551612 SETUP_REQUIRED). */
#define TROPHY_ERROR_EXCEEDS_MAX 0x80551606u
#define TROPHY_LIVE_MAX          2u

#define TROPHY_UNLOCK_GAP_US     100000u
/* Device run 2026-10-09: a first install took 241-245 frames (4.0 s), an
 * installed set 55-58 frames (0.92 s); an error dialog (C0-11133-9) stays
 * RUNNING until the user presses OK.  600 frames = 10 s at 60 Hz. */
#define TROPHY_SETUP_MAX_FRAMES  600u
#define TROPHY_SETUP_ABORT_FRAMES 300u
/* Boot delivery waits (Stage B, main thread): poll every 10 ms; give a set
 * up after 5 s without any worker progress (UnlockTrophy took ~0.48 s on
 * the device); wait at most 2 s for the worker to leave a retired set
 * before its context may be destroyed. */
#define TROPHY_WAIT_TICK_US      10000u
#define TROPHY_DRAIN_STALL_US    5000000u
#define TROPHY_RETIRE_WAIT_US    2000000u
#define TROPHY_SAVE_MAX_BYTES    (1024u * 1024u)
#define TROPHY_REQUEST_MAX_BYTES 16u
#define TROPHY_TEST_MAX_IDS      8u
#define TROPHY_TEST_MAX_BYTES    64u
#define TROPHY_SAVE_CHUNK_BYTES  4096u

/* Worker: the async logger's priority, core and fallback
 * (host_vita_log_async.h:32-39, host_vita_log_async.c:423-469). */
#define TROPHY_THREAD_PRIORITY   191
#define TROPHY_THREAD_FALLBACK_PRIORITY 0x10000100
#define TROPHY_THREAD_STACK      0x10000u
#define TROPHY_SEMA_MAX          1024

#define TROPHY_WORDS             20u /* 640 bits >= ids 0..637 */
#define TROPHY_NO_FILE           (-1)
#define TROPHY_READ_FAILED       (-2)

#define TROPHY_DATA_DIR ISAAC_VITA_NATIVE_DATA_ROOT
#define TROPHY_IMPORT_REQUEST TROPHY_DATA_DIR "/trophy-import.req"
#define TROPHY_IMPORT_RESULT  TROPHY_DATA_DIR "/trophy-import.res"
#define TROPHY_TEST_REQUEST   TROPHY_DATA_DIR "/trophy-test.req"
/* Persistent pending store: live unlocks whose set is not live this boot
 * (owner decision 2026-10-09: delivered at the next game start, never
 * through a dialog during gameplay).  Published like options.ini in
 * host_vita_render_scale.c:452-505: temp (O_EXCL) -> write -> sync -> close
 * -> old file aside -> temp into place -> aside removed. */
#define TROPHY_STORE_PATH     TROPHY_DATA_DIR "/trophy-pending.bin"
#define TROPHY_STORE_TMP      TROPHY_DATA_DIR "/trophy-pending.bin.tmp"
#define TROPHY_STORE_BAK      TROPHY_DATA_DIR "/trophy-pending.bin.bak"
/* Layout v1, little endian, 200 bytes: magic[8] "ISAACTRP", u32 version 1,
 * u32 id max 637, u32 pending[20] (bit i = achievement id i; bit 0 and bits
 * 638/639 are zero), u32 known (bit n = state[n] valid), u32 state[6][4]
 * (the set's last GetTrophyUnlockState flags, bit t = trophy t), u32
 * checksum over [0, 196) with the save checksum's table and update. */
#define TROPHY_STORE_MAGIC    "ISAACTRP"
#define TROPHY_STORE_VERSION  1u
#define TROPHY_STORE_PENDING  16u
#define TROPHY_STORE_KNOWN    96u
#define TROPHY_STORE_STATE    100u
#define TROPHY_STORE_CRC      196u
#define TROPHY_STORE_BYTES    200u
/* SCE_ERROR_ERRNO_ENOENT (host_vita_render_scale.c:29-30). */
#define TROPHY_SCE_ENOENT     0x80010002u
/* No rep_ prefix on the Vita (recomp/vita/manager_storage.h:7-10). */
#define TROPHY_SAVE_PREFIX \
    TROPHY_DATA_DIR "/Documents/My Games/Binding of Isaac Repentance/" \
    "persistentgamedata"

/* pending = an id from the persistent store, delivered at boot. */
enum { TROPHY_SRC_LIVE = 0, TROPHY_SRC_TEST, TROPHY_SRC_IMPORT,
       TROPHY_SRC_PENDING, TROPHY_SRC_COUNT };
static const char *const k_src_names[TROPHY_SRC_COUNT] = {
    "live", "test", "import", "pending",
};

/* Per-set slot, published by the main thread, read by the worker with
 * seq_cst (see trophy_enter): WAITING = not usable yet (never opened, or
 * opened and held until Stage B ends), OPEN = the worker may use the
 * context, FINAL = not live for the rest of this boot. */
enum { TROPHY_SLOT_WAITING = 0, TROPHY_SLOT_OPEN, TROPHY_SLOT_FINAL };

/* Mapping rule (the pack tool implements the same rule
 * and both tests pin the canonical-dump SHA-256). */
static const uint16_t k_set_first[ISAAC_VITA_TROPHY_SET_COUNT] = {
    1u, 90u, 179u, 277u, 404u, 521u,
};
static const uint16_t k_set_last[ISAAC_VITA_TROPHY_SET_COUNT] = {
    89u, 178u, 276u, 403u, 520u, 637u,
};
/* Daily Challenge achievements, not in any set. */
static const uint16_t k_excluded[3] = { 325u, 336u, 354u };

/* ---- NP types (vitasdk ships the stubs but no header) ------------------ */

/* Comm id {char data[9]; char term; uint8_t num; char dummy;} and the
 * 160-byte signature, as in public homebrew trophy code; NoTrpDrm accepts both.
 * data = ISAAC_VITA_TROPHY_COMM_BASE (9 characters, no NUL inside). */
typedef struct trophy_comm_id {
    char data[9];
    char term;
    uint8_t num;
    char dummy;
} trophy_comm_id;
_Static_assert(sizeof(trophy_comm_id) == 12u, "SceNpCommunicationId is 12 bytes");
_Static_assert(sizeof(ISAAC_VITA_TROPHY_COMM_BASE) == 10u,
               "the comm id base fills data[9]");

typedef struct trophy_comm_sign {
    uint8_t data[160];
} trophy_comm_sign;

/* SceNpTrophyFlagArray: SCE_NP_TROPHY_FLAG_SETSIZE 128 bits
 * (the SceNpTrophy SDK declaration). */
typedef struct trophy_flag_array {
    uint32_t flag_bits[4];
} trophy_flag_array;

/* As the SceNpTrophy SDK declares it. */
typedef struct trophy_dialog_param {
    uint32_t sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    uint32_t options;
    uint8_t reserved[128];
} trophy_dialog_param;

typedef struct trophy_dialog_result {
    int result;
    uint8_t reserved[128];
} trophy_dialog_result;

#if !defined(ISAAC_VITA_TROPHIES_ORACLE)
/* Prototypes as ref/mc3-trophies.c:35-43; NIDs in vita-headers
 * db/360/SceNpTrophy.yml:11-25 and db/360/SceCommonDialog.yml:76-80. */
int sceNpTrophyInit(void *opt);
int sceNpTrophyCreateContext(int *context, const void *commId,
                             const void *commSign, uint64_t options);
/* vita-headers db/360/SceNpTrophy.yml:14 (NID 0x56F5CBA5); returned 0 on
 * the device (device run 2026-10-09). */
int sceNpTrophyDestroyContext(int context);
int sceNpTrophyCreateHandle(int *handle);
int sceNpTrophyDestroyHandle(int handle);
int sceNpTrophyUnlockTrophy(int context, int handle, int trophyId,
                            int *platinumId);
int sceNpTrophyGetTrophyUnlockState(int context, int handle,
                                    trophy_flag_array *flags, int *count);
int sceNpTrophySetupDialogInit(trophy_dialog_param *param);
SceCommonDialogStatus sceNpTrophySetupDialogGetStatus(void);
int sceNpTrophySetupDialogGetResult(trophy_dialog_result *result);
int sceNpTrophySetupDialogAbort(void);
int sceNpTrophySetupDialogTerm(void);
#endif

/* ---- state -------------------------------------------------------------- */

struct trophy_counts {
    uint32_t unlocked;
    uint32_t already;
    uint32_t failed;
    uint32_t noset;
    uint32_t excluded;
};

static int s_booted;
static int s_setup_called;
static int s_feature_live;           /* Stage A: module + init + worker */
static int s_accepting;              /* observer gate (atomic) */
static int s_stage_b;                /* Stage B began (release/acquire) */
static int s_sema = -1;

/* Main thread only (Stage B): the context pool. */
static int s_context[ISAAC_VITA_TROPHY_SET_COUNT];
static uint8_t s_ctx_created[ISAAC_VITA_TROPHY_SET_COUNT];
static uint8_t s_held[ISAAC_VITA_TROPHY_SET_COUNT];
static uint32_t s_ctx_count;
static uint32_t s_dialogs;
static uint32_t s_timeouts;
static trophy_comm_id s_comm[ISAAC_VITA_TROPHY_SET_COUNT];

/* Shared main <-> worker, all seq_cst: s_slot (TROPHY_SLOT_*), the set
 * whose context the worker may be using (-1 none), per-set count of drain
 * passes in flight, and a progress counter for the boot waits. */
static int s_slot[ISAAC_VITA_TROPHY_SET_COUNT];
static int s_worker_set = -1;
static int s_busy[ISAAC_VITA_TROPHY_SET_COUNT];
static uint32_t s_progress;

/* Worker only. */
static uint8_t s_state_loaded[ISAAC_VITA_TROPHY_SET_COUNT];
static uint32_t s_state[ISAAC_VITA_TROPHY_SET_COUNT][4];
static const trophy_comm_sign k_comm_sign = {
    { 0xb9u, 0xddu, 0xe1u, 0x3bu, 0x01u, 0x00u },
};
/* _sceCommonDialogSetMagicNumber stores 0xC0D1A109 + &commonParam: the
 * parameter block must not move while a dialog runs, so it is static. */
static trophy_dialog_param s_dialog_param;
static trophy_dialog_result s_dialog_result;

static uint32_t s_pending_live[TROPHY_WORDS];
static uint32_t s_pending_test[TROPHY_WORDS];
static uint32_t s_pending_import[TROPHY_WORDS];
/* The store's ids as loaded at Stage A: delivered (src=pending) once. */
static uint32_t s_pending_store[TROPHY_WORDS];
static uint32_t s_readonly_logged[TROPHY_WORDS];
static uint32_t s_mismatch_count;
static uint32_t s_range_count;

/* The gameplay pair (Stage A): the sets that stay live during play; -1 =
 * none. */
static int s_pair[2] = { -1, -1 };

static struct trophy_counts s_counts[TROPHY_SRC_COUNT];
static int s_import_active;
static int s_import_reported;
static uint32_t s_import_slot;
static uint32_t s_import_saved;
static uint32_t s_import_mapped;
static int s_test_active;
static int s_test_reported;
static uint32_t s_test_ids;

/* Pending store: Stage A loads it before the worker may touch it (the
 * worker reads it only after Stage B's release store); afterwards the
 * worker alone reads, changes and publishes it. */
static uint32_t s_store[TROPHY_WORDS];
static uint32_t s_store_known;
static uint32_t s_store_state[ISAAC_VITA_TROPHY_SET_COUNT][4];
static int s_store_dirty;
static int s_store_on_disk;
static uint8_t s_store_bytes[TROPHY_STORE_BYTES + 1u];

static uint32_t s_crc_table[256];
static int s_crc_ready;
static uint8_t s_save_buffer[TROPHY_SAVE_CHUNK_BYTES];
static uint8_t s_save_head[TROPHY_SAVE_HEAD_BYTES];
static char s_save_path[] = TROPHY_SAVE_PREFIX "0.dat";
#define TROPHY_SAVE_DIGIT (sizeof(TROPHY_SAVE_PREFIX) - 1u)
static char s_result_text[256];

/* ---- seams --------------------------------------------------------------- */

#if defined(ISAAC_VITA_TROPHIES_ORACLE)
static uint8_t trophy_read8(uint32_t address)
{
    return isaac_vita_trophies_fake_read8(address);
}

static uint32_t trophy_read32(uint32_t address)
{
    return isaac_vita_trophies_fake_read32(address);
}

static int trophy_sysmodule_load(void)
{
    return isaac_vita_trophies_fake_sysmodule_load(SCE_SYSMODULE_NP_TROPHY);
}

static int trophy_np_init(void)
{
    return isaac_vita_trophies_fake_np_init();
}

static int trophy_create_context(int *context, const trophy_comm_id *comm,
                                 const trophy_comm_sign *sign)
{
    return isaac_vita_trophies_fake_create_context(context, comm, sign, 0u);
}

static int trophy_destroy_context(int context)
{
    return isaac_vita_trophies_fake_destroy_context(context);
}

static void trophy_wait_tick(void)
{
    isaac_vita_trophies_fake_wait_tick();
}

static int trophy_create_handle(int *handle)
{
    return isaac_vita_trophies_fake_create_handle(handle);
}

static int trophy_destroy_handle(int handle)
{
    return isaac_vita_trophies_fake_destroy_handle(handle);
}

static int trophy_unlock(int context, int handle, int trophy, int *platinum)
{
    return isaac_vita_trophies_fake_unlock(context, handle, trophy, platinum);
}

static int trophy_unlock_state(int context, int handle,
                               trophy_flag_array *flags, int *count)
{
    return isaac_vita_trophies_fake_unlock_state(context, handle,
                                                 flags->flag_bits, count);
}

static void trophy_dialog_magic(SceCommonDialogParam *param)
{
    param->magic = 0xC0D1A109u + (uint32_t)(uintptr_t)param;
}

static int trophy_setup_init(trophy_dialog_param *param)
{
    return isaac_vita_trophies_fake_setup_init(
        param->sdkVersion, param->context, param->options,
        param->commonParam.magic, &param->commonParam);
}

static int trophy_setup_status(void)
{
    return isaac_vita_trophies_fake_setup_status();
}

static int trophy_setup_result(trophy_dialog_result *result)
{
    return isaac_vita_trophies_fake_setup_result(&result->result);
}

static int trophy_setup_abort(void)
{
    return isaac_vita_trophies_fake_setup_abort();
}

static int trophy_setup_term(void)
{
    return isaac_vita_trophies_fake_setup_term();
}

static int trophy_sema_create(void)
{
    return isaac_vita_trophies_fake_sema_create("isaac_trophy", 0,
                                                TROPHY_SEMA_MAX);
}

static int trophy_sema_signal(void)
{
    return isaac_vita_trophies_fake_sema_signal(s_sema);
}

static int trophy_thread_create(int priority)
{
    return isaac_vita_trophies_fake_thread_create(
        "isaac_trophy", priority, TROPHY_THREAD_STACK,
        (uint32_t)SCE_KERNEL_CPU_MASK_USER_2);
}

static int trophy_thread_start(int thread)
{
    return isaac_vita_trophies_fake_thread_start(thread);
}

static void trophy_delay(uint32_t microseconds)
{
    isaac_vita_trophies_fake_delay(microseconds);
}

static uint64_t trophy_time_us(void)
{
    return isaac_vita_trophies_fake_time_us();
}

static int trophy_open_read(const char *path)
{
    return isaac_vita_trophies_fake_open_read(path);
}

static int trophy_read(int descriptor, void *buffer, uint32_t size)
{
    return isaac_vita_trophies_fake_read(descriptor, buffer, size);
}

static int64_t trophy_size(int descriptor)
{
    return isaac_vita_trophies_fake_size(descriptor);
}

static int trophy_close(int descriptor)
{
    return isaac_vita_trophies_fake_close(descriptor);
}

static int trophy_remove(const char *path)
{
    return isaac_vita_trophies_fake_remove(path);
}

static int trophy_write_file(const char *path, const void *data,
                             uint32_t size)
{
    return isaac_vita_trophies_fake_write_file(path, data, size);
}

static int trophy_write_new(const char *path, const void *data, uint32_t size)
{
    return isaac_vita_trophies_fake_write_new(path, data, size);
}

static int trophy_rename(const char *source, const char *destination)
{
    return isaac_vita_trophies_fake_rename(source, destination);
}
#else
/* Guest address == host address (guest.h); the same seam shape as
 * kage_vita_backdrop_watergfx.c:58-66, volatile because the game thread
 * owns these bytes. */
static uint8_t trophy_read8(uint32_t address)
{
    return *(const volatile uint8_t *)(uintptr_t)address;
}

static uint32_t trophy_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static int trophy_sysmodule_load(void)
{
    return sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
}

static int trophy_np_init(void)
{
    return sceNpTrophyInit(NULL);
}

static int trophy_create_context(int *context, const trophy_comm_id *comm,
                                 const trophy_comm_sign *sign)
{
    return sceNpTrophyCreateContext(context, comm, sign, 0u);
}

static int trophy_destroy_context(int context)
{
    return sceNpTrophyDestroyContext(context);
}

static void trophy_wait_tick(void)
{
    (void)sceKernelDelayThread(TROPHY_WAIT_TICK_US);
}

static int trophy_create_handle(int *handle)
{
    return sceNpTrophyCreateHandle(handle);
}

static int trophy_destroy_handle(int handle)
{
    return sceNpTrophyDestroyHandle(handle);
}

static int trophy_unlock(int context, int handle, int trophy, int *platinum)
{
    return sceNpTrophyUnlockTrophy(context, handle, trophy, platinum);
}

static int trophy_unlock_state(int context, int handle,
                               trophy_flag_array *flags, int *count)
{
    return sceNpTrophyGetTrophyUnlockState(context, handle, flags, count);
}

static void trophy_dialog_magic(SceCommonDialogParam *param)
{
    _sceCommonDialogSetMagicNumber(param);
}

static int trophy_setup_init(trophy_dialog_param *param)
{
    return sceNpTrophySetupDialogInit(param);
}

static int trophy_setup_status(void)
{
    return (int)sceNpTrophySetupDialogGetStatus();
}

static int trophy_setup_result(trophy_dialog_result *result)
{
    return sceNpTrophySetupDialogGetResult(result);
}

static int trophy_setup_abort(void)
{
    return sceNpTrophySetupDialogAbort();
}

static int trophy_setup_term(void)
{
    return sceNpTrophySetupDialogTerm();
}

static int trophy_sema_create(void)
{
    return sceKernelCreateSema("isaac_trophy", 0, 0, TROPHY_SEMA_MAX, NULL);
}

static int trophy_sema_signal(void)
{
    return sceKernelSignalSema(s_sema, 1);
}

static int trophy_worker_step(void);

static int trophy_thread_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    /* Blocks between batches; a failing wait (semaphore gone) ends the
     * thread instead of spinning. */
    while (sceKernelWaitSema(s_sema, 1, NULL) >= 0)
        (void)trophy_worker_step();
    return 0;
}

static int trophy_thread_create(int priority)
{
    return sceKernelCreateThread("isaac_trophy", trophy_thread_entry,
                                 priority, TROPHY_THREAD_STACK, 0,
                                 SCE_KERNEL_CPU_MASK_USER_2, NULL);
}

static int trophy_thread_start(int thread)
{
    return sceKernelStartThread(thread, 0, NULL);
}

static void trophy_delay(uint32_t microseconds)
{
    (void)sceKernelDelayThread(microseconds);
}

static uint64_t trophy_time_us(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static int trophy_open_read(const char *path)
{
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int trophy_read(int descriptor, void *buffer, uint32_t size)
{
    return (int)sceIoRead(descriptor, buffer, size);
}

static int64_t trophy_size(int descriptor)
{
    SceOff end = sceIoLseek(descriptor, 0, SCE_SEEK_END);
    SceOff back;

    if (end < 0)
        return (int64_t)end;
    back = sceIoLseek(descriptor, 0, SCE_SEEK_SET);
    return back < 0 ? (int64_t)back : (int64_t)end;
}

static int trophy_close(int descriptor)
{
    return sceIoClose(descriptor);
}

static int trophy_remove(const char *path)
{
    return sceIoRemove(path);
}

static int trophy_write_file(const char *path, const void *data,
                             uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t done = 0u;
    int rc;
    int descriptor = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                               0666);

    if (descriptor < 0)
        return descriptor;
    rc = 0;
    while (done < size) {
        int wrote = (int)sceIoWrite(descriptor, bytes + done, size - done);
        if (wrote <= 0) {
            rc = wrote < 0 ? wrote : -1;
            break;
        }
        done += (uint32_t)wrote;
    }
    {
        int closed = sceIoClose(descriptor);
        if (rc == 0 && closed < 0)
            rc = closed;
    }
    return rc;
}

/* O_EXCL create, write everything, sync, close (host_vita_render_scale.c
 * rs_publish:462-488); 0666 as host_vita_archive_receipt.c:38-42 (0600 is
 * EINVAL on hardware).  A short write is reported as rc -1. */
static int trophy_write_new(const char *path, const void *data, uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t done = 0u;
    int rc = 0;
    int descriptor = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL,
                               0666);

    if (descriptor < 0)
        return descriptor;
    while (done < size) {
        int wrote = (int)sceIoWrite(descriptor, bytes + done, size - done);
        if (wrote <= 0) {
            rc = wrote < 0 ? wrote : -1;
            break;
        }
        done += (uint32_t)wrote;
    }
    if (rc == 0)
        rc = sceIoSyncByFd(descriptor, 0);
    {
        int closed = sceIoClose(descriptor);
        if (rc >= 0 && closed < 0)
            rc = closed;
    }
    return rc < 0 ? rc : 0;
}

static int trophy_rename(const char *source, const char *destination)
{
    return sceIoRename(source, destination);
}
#endif

/* ---- helpers ------------------------------------------------------------- */

static uint32_t trophy_elapsed(uint64_t started)
{
    return (uint32_t)(trophy_time_us() - started);
}

static int trophy_is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static uint32_t trophy_bit(uint32_t id)
{
    return 1u << (id & 31u);
}

static uint32_t trophy_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

struct trophy_text {
    char *out;
    uint32_t length;
    uint32_t capacity;
};

static void trophy_text_str(struct trophy_text *text, const char *value)
{
    while (*value != '\0' && text->length + 1u < text->capacity)
        text->out[text->length++] = *value++;
    text->out[text->length] = '\0';
}

static void trophy_text_u32(struct trophy_text *text, uint32_t value)
{
    char digits[11];
    uint32_t count = 0u;

    do {
        digits[count++] = (char)('0' + (char)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    while (count != 0u && text->length + 1u < text->capacity)
        text->out[text->length++] = digits[--count];
    text->out[text->length] = '\0';
}

/* Reads at most capacity bytes; TROPHY_NO_FILE when the file cannot be
 * opened, TROPHY_READ_FAILED on a read error, else the byte count (capacity
 * itself means "longer than capacity - 1"). */
static int trophy_read_small(const char *path, char *buffer, uint32_t capacity)
{
    uint32_t total = 0u;
    int descriptor = trophy_open_read(path);

    if (descriptor < 0)
        return TROPHY_NO_FILE;
    while (total < capacity) {
        int got = trophy_read(descriptor, buffer + total, capacity - total);
        if (got < 0) {
            (void)trophy_close(descriptor);
            return TROPHY_READ_FAILED;
        }
        if (got == 0)
            break;
        if ((uint32_t)got > capacity - total)
            got = (int)(capacity - total);
        total += (uint32_t)got;
    }
    (void)trophy_close(descriptor);
    return (int)total;
}

/* ---- mapping and checksum ------------------------------------------------ */

int isaac_vita_trophies_map(uint32_t achievement_id, uint32_t *set,
                            uint32_t *trophy)
{
    uint32_t n;
    uint32_t i;

    if (achievement_id == 0u || achievement_id > TROPHY_ID_MAX)
        return 0;
    for (i = 0u; i < sizeof k_excluded / sizeof k_excluded[0]; ++i) {
        if (achievement_id == k_excluded[i])
            return 0;
    }
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        if (achievement_id >= k_set_first[n] &&
                achievement_id <= k_set_last[n]) {
            uint32_t number = achievement_id - k_set_first[n] + 1u;
            for (i = 0u; i < sizeof k_excluded / sizeof k_excluded[0]; ++i) {
                if (k_excluded[i] >= k_set_first[n] &&
                        k_excluded[i] < achievement_id)
                    --number;
            }
            *set = n;
            *trophy = number;
            return 1;
        }
    }
    return 0;
}

/* Transcribed from the frozen PE (sub_0065b340
 * 0x65b3b3..0x65b46a and 0x65b470..0x65b4a1): ONE logical shift, then
 * SEVEN arithmetic shifts; each step tests bit 0 of the value before its
 * shift. */
static void trophy_crc_build(void)
{
    uint32_t i;

    if (s_crc_ready)
        return;
    for (i = 0u; i < 256u; ++i) {
        uint32_t value = (i >> 1) ^ ((i & 1u) != 0u ? TROPHY_CRC_POLY : 0u);
        uint32_t step;

        for (step = 0u; step < 7u; ++step) {
            uint32_t low = value & 1u;
            value = (value >> 1) | (value & 0x80000000u);
            if (low != 0u)
                value ^= TROPHY_CRC_POLY;
        }
        s_crc_table[i] = value;
    }
    s_crc_ready = 1;
}

/* ebx = ~acc on entry, ~ebx on exit. */
static uint32_t trophy_crc_bytes(uint32_t ebx, const uint8_t *bytes,
                                 uint32_t size)
{
    uint32_t i;

    for (i = 0u; i < size; ++i)
        ebx = (ebx >> 8) ^ s_crc_table[(bytes[i] ^ ebx) & 0xffu];
    return ebx;
}

/* ---- pending store ------------------------------------------------------- */

static void trophy_put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t trophy_count_bits(const uint32_t *words, uint32_t count)
{
    uint32_t total = 0u;
    uint32_t i;

    for (i = 0u; i < count; ++i)
        total += (uint32_t)__builtin_popcount(words[i]);
    return total;
}

static void trophy_store_clear(void)
{
    uint32_t i;
    uint32_t n;

    for (i = 0u; i < TROPHY_WORDS; ++i)
        s_store[i] = 0u;
    s_store_known = 0u;
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        for (i = 0u; i < 4u; ++i)
            s_store_state[n][i] = 0u;
    }
}

/* s_store_bytes[0..TROPHY_STORE_BYTES) -> state; 1 when every field is
 * plausible, else 0 and nothing changed. */
static int trophy_store_parse(void)
{
    const uint8_t *b = s_store_bytes;
    uint32_t i;
    uint32_t n;

    for (i = 0u; i < 8u; ++i) {
        if (b[i] != (uint8_t)TROPHY_STORE_MAGIC[i])
            return 0;
    }
    trophy_crc_build();
    if (trophy_le32(b + 8u) != TROPHY_STORE_VERSION ||
            trophy_le32(b + 12u) != TROPHY_ID_MAX ||
            trophy_le32(b + TROPHY_STORE_CRC) !=
                ~trophy_crc_bytes(~TROPHY_CRC_INIT, b, TROPHY_STORE_CRC))
        return 0;
    /* bit 0 and bits 638..639 are never ids; known names six sets */
    if ((trophy_le32(b + TROPHY_STORE_PENDING) & 1u) != 0u ||
            (trophy_le32(b + TROPHY_STORE_PENDING + 4u * (TROPHY_WORDS - 1u)) &
             ~((1u << ((TROPHY_ID_MAX + 1u) & 31u)) - 1u)) != 0u ||
            (trophy_le32(b + TROPHY_STORE_KNOWN) &
             ~((1u << ISAAC_VITA_TROPHY_SET_COUNT) - 1u)) != 0u)
        return 0;
    for (i = 0u; i < TROPHY_WORDS; ++i)
        s_store[i] = trophy_le32(b + TROPHY_STORE_PENDING + 4u * i);
    s_store_known = trophy_le32(b + TROPHY_STORE_KNOWN);
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        for (i = 0u; i < 4u; ++i)
            s_store_state[n][i] =
                trophy_le32(b + TROPHY_STORE_STATE + 16u * n + 4u * i);
    }
    return 1;
}

/* Stage A (main thread, before the worker may touch the store).  Removes a
 * stale temp; a store stranded aside by an interrupted publish (no file,
 * an aside copy) is moved back first.  Missing or garbage = nothing
 * pending; garbage is replaced by the next publish. */
static void trophy_store_load(void)
{
    const char *status;
    int recovered = 0;
    int length;

    trophy_store_clear();
    s_store_dirty = 0;
    s_store_on_disk = 0;
    (void)trophy_remove(TROPHY_STORE_TMP);
    length = trophy_read_small(TROPHY_STORE_PATH, (char *)s_store_bytes,
                               (uint32_t)sizeof s_store_bytes);
    if (length == TROPHY_NO_FILE) {
        length = trophy_read_small(TROPHY_STORE_BAK, (char *)s_store_bytes,
                                   (uint32_t)sizeof s_store_bytes);
        if (length != TROPHY_NO_FILE) {
            /* If the move back fails the aside copy stays; the next
             * publish (no file in place) recreates the file. */
            recovered = 1;
            if (trophy_rename(TROPHY_STORE_BAK, TROPHY_STORE_PATH) >= 0)
                s_store_on_disk = 1;
        }
    } else {
        s_store_on_disk = 1;
        (void)trophy_remove(TROPHY_STORE_BAK);
    }
    if (length == TROPHY_NO_FILE)
        status = "absent";
    else if (length == TROPHY_READ_FAILED)
        status = "read-error";
    else if (length != (int)TROPHY_STORE_BYTES || !trophy_store_parse())
        status = "garbage";      /* parse changed nothing: still empty */
    else
        status = recovered ? "recovered" : "ok";
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: pending load status=%s ids=%u known=0x%02x", status,
        (unsigned)trophy_count_bits(s_store, TROPHY_WORDS),
        (unsigned)s_store_known);
}

/* Worker only.  Publishes the store; on any failure the previous file is
 * back in place and the store stays dirty (the next worker pass retries). */
static void trophy_store_save(void)
{
    const char *status = "ok";
    uint8_t *b = s_store_bytes;
    uint32_t i;
    uint32_t n;
    int rc;

    for (i = 0u; i < 8u; ++i)
        b[i] = (uint8_t)TROPHY_STORE_MAGIC[i];
    trophy_put32(b + 8u, TROPHY_STORE_VERSION);
    trophy_put32(b + 12u, TROPHY_ID_MAX);
    for (i = 0u; i < TROPHY_WORDS; ++i)
        trophy_put32(b + TROPHY_STORE_PENDING + 4u * i, s_store[i]);
    trophy_put32(b + TROPHY_STORE_KNOWN, s_store_known);
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        for (i = 0u; i < 4u; ++i)
            trophy_put32(b + TROPHY_STORE_STATE + 16u * n + 4u * i,
                         s_store_state[n][i]);
    }
    trophy_crc_build();
    trophy_put32(b + TROPHY_STORE_CRC,
                 ~trophy_crc_bytes(~TROPHY_CRC_INIT, b, TROPHY_STORE_CRC));
    (void)trophy_remove(TROPHY_STORE_TMP);
    rc = trophy_write_new(TROPHY_STORE_TMP, b, TROPHY_STORE_BYTES);
    if (rc < 0) {
        status = "write-error";
        (void)trophy_remove(TROPHY_STORE_TMP);
    } else {
        if (s_store_on_disk) {
            rc = trophy_rename(TROPHY_STORE_PATH, TROPHY_STORE_BAK);
            if (rc < 0 && (uint32_t)rc == TROPHY_SCE_ENOENT)
                rc = 0;      /* removed behind our back: nothing to keep */
        }
        if (rc >= 0) {
            rc = trophy_rename(TROPHY_STORE_TMP, TROPHY_STORE_PATH);
            if (rc < 0)
                (void)trophy_rename(TROPHY_STORE_BAK, TROPHY_STORE_PATH);
        }
        if (rc < 0) {
            status = "rename-error";
            (void)trophy_remove(TROPHY_STORE_TMP);
        } else {
            (void)trophy_remove(TROPHY_STORE_BAK);
            s_store_on_disk = 1;
            s_store_dirty = 0;
        }
    }
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: pending save ids=%u status=%s rc=0x%08x",
        (unsigned)trophy_count_bits(s_store, TROPHY_WORDS), status,
        (unsigned)(rc < 0 ? rc : 0));
}

/* ---- logging helpers ----------------------------------------------------- */

static void trophy_disable(const char *stage, int rc)
{
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: disabled stage=%s rc=0x%08x",
                         stage, (unsigned)rc);
    s_feature_live = 0;
    __atomic_store_n(&s_accepting, 0, __ATOMIC_RELEASE);
}

/* Skip lines for live, test and pending ids; import ids are only counted. */
static void trophy_skip(int src, uint32_t id, const char *reason)
{
    if (src == TROPHY_SRC_IMPORT)
        return;
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: skip src=%s id=%u reason=%s",
                         k_src_names[src], (unsigned)id, reason);
}

/* Log line + trophy-import.res (same text without "KAGE VITA ").  The
 * request is deleted only when asked and the remove succeeds. */
static void trophy_import_result(uint32_t slot, const char *status,
                                 uint32_t saved, uint32_t mapped,
                                 const struct trophy_counts *counts,
                                 int delete_request)
{
    struct trophy_text text;
    const char *request = "kept";

    if (delete_request && trophy_remove(TROPHY_IMPORT_REQUEST) >= 0)
        request = "deleted";
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY IMPORT: slot=%u status=%s saved=%u mapped=%u "
        "unlocked=%u already=%u failed=%u noset=%u request=%s",
        (unsigned)slot, status, (unsigned)saved, (unsigned)mapped,
        (unsigned)counts->unlocked, (unsigned)counts->already,
        (unsigned)counts->failed, (unsigned)counts->noset, request);
    text.out = s_result_text;
    text.length = 0u;
    text.capacity = (uint32_t)sizeof s_result_text;
    trophy_text_str(&text, "TROPHY IMPORT: slot=");
    trophy_text_u32(&text, slot);
    trophy_text_str(&text, " status=");
    trophy_text_str(&text, status);
    trophy_text_str(&text, " saved=");
    trophy_text_u32(&text, saved);
    trophy_text_str(&text, " mapped=");
    trophy_text_u32(&text, mapped);
    trophy_text_str(&text, " unlocked=");
    trophy_text_u32(&text, counts->unlocked);
    trophy_text_str(&text, " already=");
    trophy_text_u32(&text, counts->already);
    trophy_text_str(&text, " failed=");
    trophy_text_u32(&text, counts->failed);
    trophy_text_str(&text, " noset=");
    trophy_text_u32(&text, counts->noset);
    trophy_text_str(&text, " request=");
    trophy_text_str(&text, request);
    trophy_text_str(&text, "\n");
    (void)trophy_write_file(TROPHY_IMPORT_RESULT, s_result_text, text.length);
}

/* ---- observer (game thread) --------------------------------------------- */

void isaac_vita_trophies_try_unlock_observe(uint32_t pgd, uint32_t id,
                                            uint32_t return_rva)
{
    uint32_t mgr;
    uint32_t bit;

    if (!__atomic_load_n(&s_accepting, __ATOMIC_ACQUIRE))
        return;
    mgr = trophy_read32(GUEST_IMAGE_BASE + TROPHY_MGR_PTR_RVA);
    if (mgr == 0u || pgd != mgr + TROPHY_PGD_OFFSET) {
        /* Never read a guest byte through a foreign `this`. */
        uint32_t n = __atomic_add_fetch(&s_mismatch_count, 1u,
                                        __ATOMIC_RELAXED);
        if (n <= TROPHY_FIRST_LOGGED)
            KAGE_VITA_TROPHY_LOG(
                "KAGE VITA TROPHY: pgd-mismatch pgd=0x%08x mgr=0x%08x id=%u "
                "ra=0x%08x n=%u", (unsigned)pgd, (unsigned)mgr, (unsigned)id,
                (unsigned)return_rva, (unsigned)n);
        return;
    }
    if (id == 0u)
        return;
    if (id > TROPHY_ID_MAX) {
        uint32_t n = __atomic_add_fetch(&s_range_count, 1u, __ATOMIC_RELAXED);
        if (n <= TROPHY_FIRST_LOGGED)
            KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: id-range id=%u ra=0x%08x",
                                 (unsigned)id, (unsigned)return_rva);
        return;
    }
    if (trophy_read8(pgd + TROPHY_PGD_UNLOCKED + id) != 0u)
        return;
    bit = trophy_bit(id);
    if (trophy_read8(pgd + TROPHY_PGD_READONLY) != 0u) {
        uint32_t before = __atomic_fetch_or(&s_readonly_logged[id >> 5], bit,
                                            __ATOMIC_RELAXED);
        if ((before & bit) == 0u)
            KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: readonly id=%u ra=0x%08x",
                                 (unsigned)id, (unsigned)return_rva);
        return;
    }
    (void)__atomic_fetch_or(&s_pending_live[id >> 5], bit, __ATOMIC_RELEASE);
    (void)trophy_sema_signal();
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: event id=%u ra=0x%08x",
                         (unsigned)id, (unsigned)return_rva);
}

/* ---- Stage A: requests --------------------------------------------------- */

/* "<ws>*[1-3]<ws>*", at most TROPHY_REQUEST_MAX_BYTES bytes -> slot, else 0. */
static uint32_t trophy_parse_slot(const char *text, int length)
{
    int i = 0;
    uint32_t slot;

    if (length <= 0 || length > (int)TROPHY_REQUEST_MAX_BYTES)
        return 0u;
    while (i < length && trophy_is_space(text[i]))
        ++i;
    if (i >= length || text[i] < '1' || text[i] > '3')
        return 0u;
    slot = (uint32_t)(text[i] - '0');
    ++i;
    while (i < length && trophy_is_space(text[i]))
        ++i;
    return i == length ? slot : 0u;
}

/* Streams the save through the checksum with one 4 KiB buffer, keeps the
 * first 0x20 + 0x27e bytes.  NULL on success (bits/saved/mapped filled),
 * else the import status word. */
static const char *trophy_read_save(uint32_t slot, uint32_t bits[TROPHY_WORDS],
                                    uint32_t *saved, uint32_t *mapped)
{
    int descriptor;
    int64_t size;
    uint32_t total;
    uint32_t position = 0u;
    uint32_t ebx;
    uint32_t stored = 0u;
    uint32_t i;

    *saved = 0u;
    *mapped = 0u;
    for (i = 0u; i < TROPHY_WORDS; ++i)
        bits[i] = 0u;
    for (i = 0u; i < TROPHY_SAVE_HEAD_BYTES; ++i)
        s_save_head[i] = 0u;
    trophy_crc_build();
    s_save_path[TROPHY_SAVE_DIGIT] = (char)('0' + (char)slot);
    descriptor = trophy_open_read(s_save_path);
    if (descriptor < 0)
        return "no-save";
    size = trophy_size(descriptor);
    if (size < 0) {
        (void)trophy_close(descriptor);
        return "read-error";
    }
    if (size > (int64_t)TROPHY_SAVE_MAX_BYTES) {
        (void)trophy_close(descriptor);
        return "too-large";
    }
    total = (uint32_t)size;
    ebx = ~TROPHY_CRC_INIT;
    while (position < total) {
        uint32_t want = total - position;
        int got;

        if (want > TROPHY_SAVE_CHUNK_BYTES)
            want = TROPHY_SAVE_CHUNK_BYTES;
        got = trophy_read(descriptor, s_save_buffer, want);
        if (got <= 0) {
            (void)trophy_close(descriptor);
            return "read-error";
        }
        if ((uint32_t)got > want)
            got = (int)want;
        for (i = 0u; i < (uint32_t)got; ++i) {
            uint32_t at = position + i;
            uint8_t byte = s_save_buffer[i];

            if (at < TROPHY_SAVE_HEAD_BYTES)
                s_save_head[at] = byte;
            if (total >= TROPHY_SAVE_CHUNK && at >= TROPHY_SAVE_CRC_BEGIN &&
                    at < total - 4u)
                ebx = trophy_crc_bytes(ebx, &byte, 1u);
            if (total >= 4u && at >= total - 4u)
                stored |= (uint32_t)byte << (8u * (at - (total - 4u)));
        }
        position += (uint32_t)got;
    }
    (void)trophy_close(descriptor);
    if (total < TROPHY_SAVE_MAGIC_BYTES)
        return "bad-magic";
    for (i = 0u; i < TROPHY_SAVE_MAGIC_BYTES; ++i) {
        if (s_save_head[i] != (uint8_t)TROPHY_SAVE_MAGIC[i])
            return "bad-magic";
    }
    if (total < TROPHY_SAVE_CHUNK || ~ebx != stored)
        return "bad-checksum";
    if (total < TROPHY_SAVE_DATA + 4u ||
            trophy_le32(s_save_head + TROPHY_SAVE_CHUNK) != 1u)
        return "bad-chunk";
    {
        uint32_t data_bytes = trophy_le32(s_save_head + TROPHY_SAVE_CHUNK + 4u);
        uint32_t count = trophy_le32(s_save_head + TROPHY_SAVE_CHUNK + 8u);

        if (count > TROPHY_SAVE_SLOTS)
            count = TROPHY_SAVE_SLOTS;
        if (total < TROPHY_SAVE_DATA + count + 4u)
            return "bad-chunk";
        /* Reader 0x882142..0x88215d: i < count && i < data_bytes, unlocked
         * iff the byte as signed is > 0 (setg). */
        for (i = 1u; i < count && i < data_bytes; ++i) {
            uint32_t set;
            uint32_t trophy;

            if ((int8_t)s_save_head[TROPHY_SAVE_DATA + i] <= 0)
                continue;
            ++*saved;
            if (isaac_vita_trophies_map(i, &set, &trophy)) {
                ++*mapped;
                bits[i >> 5] |= trophy_bit(i);
            }
        }
    }
    return NULL;
}

static void trophy_import_boot(int available)
{
    static const struct trophy_counts zero;
    char request[TROPHY_REQUEST_MAX_BYTES + 1u];
    uint32_t bits[TROPHY_WORDS];
    uint32_t slot;
    uint32_t saved;
    uint32_t mapped;
    uint32_t i;
    const char *status;
    int length = trophy_read_small(TROPHY_IMPORT_REQUEST, request,
                                   (uint32_t)sizeof request);

    if (length == TROPHY_NO_FILE)
        return;
    slot = trophy_parse_slot(request, length);
    if (!available) {
        trophy_import_result(slot, "unavailable", 0u, 0u, &zero, 0);
        return;
    }
    if (slot == 0u) {
        trophy_import_result(0u, "bad-request", 0u, 0u, &zero, 1);
        return;
    }
    status = trophy_read_save(slot, bits, &saved, &mapped);
    if (status != NULL) {
        trophy_import_result(slot, status, 0u, 0u, &zero, 1);
        return;
    }
    s_import_slot = slot;
    s_import_saved = saved;
    s_import_mapped = mapped;
    s_import_active = 1;
    for (i = 0u; i < TROPHY_WORDS; ++i)
        (void)__atomic_fetch_or(&s_pending_import[i], bits[i],
                                __ATOMIC_RELEASE);
}

/* The gameplay pair: the two lowest sets that still have a locked trophy.
 * A trophy is unlocked when its id is set in any of persistentgamedata1..3
 * (read with the import parser) or in the set's last known Vita state (the
 * pending store); the game cannot newly unlock anything else.  No readable
 * save: sets 0 and 1.  Stage A, main thread, after trophy_store_load(). */
static void trophy_choose_pair(void)
{
    uint32_t unlocked[TROPHY_WORDS];
    uint32_t bits[TROPHY_WORDS];
    uint32_t locked[ISAAC_VITA_TROPHY_SET_COUNT];
    uint32_t saves = 0u;
    uint32_t chosen = 0u;
    uint32_t slot;
    uint32_t id;
    uint32_t n;
    const char *rule;

    for (n = 0u; n < TROPHY_WORDS; ++n)
        unlocked[n] = 0u;
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n)
        locked[n] = 0u;
    for (slot = 1u; slot <= 3u; ++slot) {
        uint32_t saved;
        uint32_t mapped;

        if (trophy_read_save(slot, bits, &saved, &mapped) != NULL)
            continue;
        ++saves;
        for (n = 0u; n < TROPHY_WORDS; ++n)
            unlocked[n] |= bits[n];
    }
    for (id = 1u; id <= TROPHY_ID_MAX; ++id) {
        uint32_t set;
        uint32_t trophy;

        if (!isaac_vita_trophies_map(id, &set, &trophy) ||
                (unlocked[id >> 5] & trophy_bit(id)) != 0u)
            continue;
        if ((s_store_known & (1u << set)) != 0u &&
                (s_store_state[set][trophy >> 5] & trophy_bit(trophy)) != 0u)
            continue;
        ++locked[set];
    }
    s_pair[0] = -1;
    s_pair[1] = -1;
    if (saves == 0u) {
        rule = "fallback";
        s_pair[0] = 0;
        s_pair[1] = 1;
    } else {
        for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT && chosen < 2u; ++n) {
            if (locked[n] != 0u)
                s_pair[chosen++] = (int)n;
        }
        rule = chosen == 0u ? "complete" : "locked";
    }
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: pair a=%d b=%d rule=%s saves=%u known=0x%02x "
        "locked=%u,%u,%u,%u,%u,%u", s_pair[0], s_pair[1], rule,
        (unsigned)saves, (unsigned)s_store_known, (unsigned)locked[0],
        (unsigned)locked[1], (unsigned)locked[2], (unsigned)locked[3],
        (unsigned)locked[4], (unsigned)locked[5]);
}

#if defined(ISAAC_VITA_TROPHY_TEST_REQUEST)
/* Diagnostic: up to 8 decimal ids 1..637 separated by whitespace, at most
 * 64 bytes.  Anything else queues nothing; the file is then deleted. */
static void trophy_test_boot(void)
{
    char text[TROPHY_TEST_MAX_BYTES + 1u];
    uint32_t bits[TROPHY_WORDS];
    uint32_t ids = 0u;
    uint32_t distinct = 0u;
    uint32_t i;
    int position = 0;
    int valid = 1;
    int length = trophy_read_small(TROPHY_TEST_REQUEST, text,
                                   (uint32_t)sizeof text);

    if (length == TROPHY_NO_FILE)
        return;
    for (i = 0u; i < TROPHY_WORDS; ++i)
        bits[i] = 0u;
    if (length < 0 || length > (int)TROPHY_TEST_MAX_BYTES)
        valid = 0;
    while (valid && position < length) {
        uint32_t value = 0u;
        int digits = 0;

        if (trophy_is_space(text[position])) {
            ++position;
            continue;
        }
        while (position < length && text[position] >= '0' &&
               text[position] <= '9') {
            if (++digits > 3)
                break;
            value = value * 10u + (uint32_t)(text[position] - '0');
            ++position;
        }
        if (digits == 0 || digits > 3 || value == 0u || value > TROPHY_ID_MAX ||
                (position < length && !trophy_is_space(text[position])) ||
                ++ids > TROPHY_TEST_MAX_IDS) {
            valid = 0;
            break;
        }
        if ((bits[value >> 5] & trophy_bit(value)) == 0u)
            ++distinct;
        bits[value >> 5] |= trophy_bit(value);
    }
    if (!valid || distinct == 0u) {
        KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY TEST: queued ids=%u", 0u);
        (void)trophy_remove(TROPHY_TEST_REQUEST);
        return;
    }
    s_test_ids = distinct;
    s_test_active = 1;
    for (i = 0u; i < TROPHY_WORDS; ++i)
        (void)__atomic_fetch_or(&s_pending_test[i], bits[i], __ATOMIC_RELEASE);
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY TEST: queued ids=%u",
                         (unsigned)distinct);
}
#endif

/* ---- Stage A ------------------------------------------------------------- */

/* No context is created here: at most two may be live (device run
 * 2026-10-09) and each needs its setup dialog, which only Stage B can
 * show.  Stage B opens them. */
void isaac_vita_trophies_boot(void)
{
    int module_rc;
    int init_rc;
    int thread;
    int priority;
    int rc;
    uint32_t i;

    if (s_booted)
        return;
    s_booted = 1;
    module_rc = trophy_sysmodule_load();
    if (module_rc < 0) {
        KAGE_VITA_TROPHY_LOG(
            "KAGE VITA TROPHY: boot module=0x%08x init=- title=ISAACR001 "
            "sets=6", (unsigned)module_rc);
        trophy_disable("module", module_rc);
        trophy_import_boot(0);
        return;
    }
    init_rc = trophy_np_init();
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: boot module=0x%08x init=0x%08x title=ISAACR001 "
        "sets=6", (unsigned)module_rc, (unsigned)init_rc);
    if (init_rc < 0) {
        trophy_disable("init", init_rc);
        trophy_import_boot(0);
        return;
    }
    rc = trophy_sema_create();
    if (rc < 0) {
        trophy_disable("thread", rc);
        trophy_import_boot(0);
        return;
    }
    s_sema = rc;
    priority = TROPHY_THREAD_PRIORITY;
    thread = trophy_thread_create(priority);
    if (thread < 0) {
        /* Lowest user priority refused: the process default, as the async
         * logger does (host_vita_log_async.c:463-469). */
        priority = TROPHY_THREAD_FALLBACK_PRIORITY;
        thread = trophy_thread_create(priority);
    }
    rc = thread < 0 ? thread : trophy_thread_start(thread);
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: worker rc=0x%08x prio=%u stack=65536 "
        "affinity=0x%08x", (unsigned)(rc < 0 ? rc : 0), (unsigned)priority,
        (unsigned)SCE_KERNEL_CPU_MASK_USER_2);
    if (rc < 0) {
        trophy_disable("thread", rc);
        trophy_import_boot(0);
        return;
    }
    s_feature_live = 1;
    for (i = 0u; i < ISAAC_VITA_TROPHY_SET_COUNT; ++i) {
        s_context[i] = -1;
        s_slot[i] = TROPHY_SLOT_WAITING;
    }
    trophy_store_load();
    for (i = 0u; i < TROPHY_WORDS; ++i)
        s_pending_store[i] = s_store[i];
    trophy_choose_pair();
    trophy_import_boot(1);
#if defined(ISAAC_VITA_TROPHY_TEST_REQUEST)
    trophy_test_boot();
#endif
    __atomic_store_n(&s_accepting, 1, __ATOMIC_RELEASE);
}

/* ---- set ranges ----------------------------------------------------------- */

/* Bits of word w that belong to set n's id range [first, last] (excluded
 * ids inside the range included: they resolve as `excluded`). */
static uint32_t trophy_range_mask(uint32_t set, uint32_t w)
{
    uint32_t lo = w * 32u;
    uint32_t first = k_set_first[set];
    uint32_t last = k_set_last[set];
    uint32_t a;
    uint32_t b;
    uint32_t high;

    if (lo + 31u < first || lo > last)
        return 0u;
    a = first > lo ? first - lo : 0u;
    b = last < lo + 31u ? last - lo : 31u;
    high = b == 31u ? 0xffffffffu : ((1u << (b + 1u)) - 1u);
    return high & ~((1u << a) - 1u);
}

static uint32_t trophy_queued(uint32_t set)
{
    uint32_t total = 0u;
    uint32_t w;

    for (w = k_set_first[set] >> 5; w <= (uint32_t)(k_set_last[set] >> 5); ++w) {
        uint32_t mask = trophy_range_mask(set, w);
        uint32_t bits =
            __atomic_load_n(&s_pending_live[w], __ATOMIC_SEQ_CST) |
            __atomic_load_n(&s_pending_store[w], __ATOMIC_SEQ_CST) |
            __atomic_load_n(&s_pending_test[w], __ATOMIC_SEQ_CST) |
            __atomic_load_n(&s_pending_import[w], __ATOMIC_SEQ_CST);

        total += (uint32_t)__builtin_popcount(bits & mask);
    }
    return total;
}

/* ---- Stage B ------------------------------------------------------------- */

enum { TROPHY_SETUP_OK = 0, TROPHY_SETUP_FAILED, TROPHY_SETUP_TIMEOUT };

static int trophy_setup_one(uint32_t set, isaac_vita_trophies_present_fn present)
{
    static const trophy_dialog_param zero_param;
    static const trophy_dialog_result zero_result;
    uint64_t started = trophy_time_us();
    uint32_t frames = 0u;
    int timed_out = 0;
    int status;
    int result;
    int result_rc;
    int init_rc;
    int live;
    const char *word;

    ++s_dialogs;
    s_dialog_param = zero_param;
    trophy_dialog_magic(&s_dialog_param.commonParam);
    s_dialog_param.sdkVersion = PSP2_SDK_VERSION;
    s_dialog_param.context = s_context[set];
    s_dialog_param.options = 0u;
    init_rc = trophy_setup_init(&s_dialog_param);
    if (init_rc < 0) {
        KAGE_VITA_TROPHY_LOG(
            "KAGE VITA TROPHY: setup set=%u status=%s init=0x%08x result=%d "
            "frames=%u us=%u", (unsigned)set, "init-failed",
            (unsigned)init_rc, 0, 0u, (unsigned)trophy_elapsed(started));
        return TROPHY_SETUP_FAILED;
    }
    for (;;) {
        status = trophy_setup_status();
        if (status != SCE_COMMON_DIALOG_STATUS_RUNNING ||
                frames >= TROPHY_SETUP_MAX_FRAMES)
            break;
        present();
        ++frames;
    }
    if (status == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        uint32_t extra = 0u;

        timed_out = 1;
        (void)trophy_setup_abort();
        for (;;) {
            status = trophy_setup_status();
            if (status != SCE_COMMON_DIALOG_STATUS_RUNNING ||
                    extra >= TROPHY_SETUP_ABORT_FRAMES)
                break;
            present();
            ++extra;
        }
        frames += extra;
    }
    s_dialog_result = zero_result;
    result_rc = trophy_setup_result(&s_dialog_result);
    result = result_rc < 0 ? result_rc : s_dialog_result.result;
    (void)trophy_setup_term();
    live = !timed_out && result_rc >= 0 &&
           s_dialog_result.result == SCE_COMMON_DIALOG_RESULT_OK;
    word = live ? "ok" : (timed_out ? "timeout" : "result-failed");
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: setup set=%u status=%s init=0x%08x result=%d "
        "frames=%u us=%u", (unsigned)set, word, (unsigned)init_rc, result,
        (unsigned)frames, (unsigned)trophy_elapsed(started));
    if (live)
        return TROPHY_SETUP_OK;
    return timed_out ? TROPHY_SETUP_TIMEOUT : TROPHY_SETUP_FAILED;
}

static void trophy_setup_skipped(uint32_t set)
{
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: setup set=%u status=%s init=0x%08x result=%d "
        "frames=%u us=%u", (unsigned)set, "skipped", 0u, 0, 0u, 0u);
}

static int trophy_open_context(uint32_t set)
{
    trophy_comm_id *comm = &s_comm[set];
    uint32_t i;
    int rc;

    for (i = 0u; i < sizeof comm->data; ++i)
        comm->data[i] = ISAAC_VITA_TROPHY_COMM_BASE[i];
    comm->term = '\0';
    comm->num = (uint8_t)set;
    comm->dummy = '\0';
    s_context[set] = -1;
    rc = trophy_create_context(&s_context[set], comm, &k_comm_sign);
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: ctx set=%u comm=ISAC00000_%02u rc=0x%08x ctx=%d",
        (unsigned)set, (unsigned)set, (unsigned)rc, s_context[set]);
    if (rc < 0)
        return 0;
    s_ctx_created[set] = 1u;
    ++s_ctx_count;
    return 1;
}

/* Marks the set FINAL, waits (bounded) until the worker is not inside a
 * call on it, then destroys the context and frees the pool slot.  The
 * worker publishes s_worker_set before it reads the slot (trophy_enter),
 * both seq_cst, so once FINAL is stored and s_worker_set != set is seen,
 * no later NP call on this context can start.  If the worker stays inside
 * a call past the bound, the context is kept (never destroyed under it). */
static void trophy_destroy(uint32_t set, const char *reason)
{
    uint64_t started = trophy_time_us();
    int rc;

    __atomic_store_n(&s_slot[set], TROPHY_SLOT_FINAL, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_worker_set, __ATOMIC_SEQ_CST) == (int)set) {
        if (trophy_elapsed(started) >= TROPHY_RETIRE_WAIT_US) {
            KAGE_VITA_TROPHY_LOG(
                "KAGE VITA TROPHY: destroy set=%u rc=0x%08x reason=%s",
                (unsigned)set, 0u, "busy");
            return;
        }
        trophy_wait_tick();
    }
    rc = trophy_destroy_context(s_context[set]);
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: destroy set=%u rc=0x%08x reason=%s",
                         (unsigned)set, (unsigned)rc, reason);
    s_ctx_created[set] = 0u;
    --s_ctx_count;
}

/* Waits until the worker has resolved every queued id of the set, giving
 * up after TROPHY_DRAIN_STALL_US without progress.  Returns 1 = drained.
 * The worker raises s_busy[set] before it takes bits and lowers it after
 * resolving them (seq_cst), so "nothing queued, then not busy" means done. */
static int trophy_wait_drained(uint32_t set)
{
    uint64_t started = trophy_time_us();
    uint64_t changed = started;
    uint32_t progress = __atomic_load_n(&s_progress, __ATOMIC_SEQ_CST);
    uint32_t left;
    int drained;

    for (;;) {
        uint32_t now;

        left = trophy_queued(set);
        if (left == 0u &&
                __atomic_load_n(&s_busy[set], __ATOMIC_SEQ_CST) == 0) {
            drained = 1;
            break;
        }
        if (trophy_time_us() - changed >= TROPHY_DRAIN_STALL_US) {
            drained = 0;
            break;
        }
        trophy_wait_tick();
        now = __atomic_load_n(&s_progress, __ATOMIC_SEQ_CST);
        if (now != progress) {
            progress = now;
            changed = trophy_time_us();
        }
    }
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: deliver set=%u status=%s left=%u "
                         "us=%u", (unsigned)set,
                         drained ? "drained" : "stalled", (unsigned)left,
                         (unsigned)trophy_elapsed(started));
    return drained;
}

/* One set of the boot work list.  A delivery set (queued ids, not in the
 * pair) is opened, handed to the worker, waited for and destroyed before
 * the next set is opened, so no UnlockTrophy runs during a dialog and at
 * most two contexts exist.  A pair set is opened and held: the worker gets
 * it when Stage B ends.  After the first dialog timeout no further dialog
 * is shown this boot (the remaining sets keep their ids pending). */
static void trophy_boot_set(uint32_t set, isaac_vita_trophies_present_fn present,
                            int pair)
{
    int setup;

    if (s_timeouts != 0u || s_ctx_count >= TROPHY_LIVE_MAX) {
        trophy_setup_skipped(set);
        return;
    }
    if (!trophy_open_context(set))
        return;
    setup = trophy_setup_one(set, present);
    if (setup != TROPHY_SETUP_OK) {
        if (setup == TROPHY_SETUP_TIMEOUT)
            ++s_timeouts;
        trophy_destroy(set, "setup");
        return;
    }
    if (pair) {
        s_held[set] = 1u;
        return;
    }
    __atomic_store_n(&s_slot[set], TROPHY_SLOT_OPEN, __ATOMIC_SEQ_CST);
    (void)trophy_sema_signal();
    trophy_destroy(set, trophy_wait_drained(set) ? "delivered" : "stalled");
}

static int trophy_in_pair(uint32_t set)
{
    return s_pair[0] == (int)set || s_pair[1] == (int)set;
}

void isaac_vita_trophies_setup(isaac_vita_trophies_present_fn present)
{
    uint64_t started;
    uint32_t pair_size = 0u;
    uint32_t live = 0u;
    uint32_t n;

    if (s_setup_called)
        return;
    s_setup_called = 1;
    if (!s_feature_live)
        return;
    started = trophy_time_us();
    __atomic_store_n(&s_stage_b, 1, __ATOMIC_RELEASE);
    for (n = 0u; n < 2u; ++n)
        pair_size += s_pair[n] >= 0;
    if (present != NULL) {
        /* (a) sets with queued ids that are not in the pair, in set order;
         * (b) the pair, which also delivers its own queued ids in play. */
        for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
            if (!trophy_in_pair(n) && trophy_queued(n) != 0u)
                trophy_boot_set(n, present, 0);
        }
        for (n = 0u; n < 2u; ++n) {
            if (s_pair[n] >= 0)
                trophy_boot_set((uint32_t)s_pair[n], present, 1);
        }
    }
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        if (s_held[n]) {
            __atomic_store_n(&s_slot[n], TROPHY_SLOT_OPEN, __ATOMIC_SEQ_CST);
            ++live;
        } else if (__atomic_load_n(&s_slot[n], __ATOMIC_SEQ_CST) ==
                   TROPHY_SLOT_WAITING) {
            __atomic_store_n(&s_slot[n], TROPHY_SLOT_FINAL, __ATOMIC_SEQ_CST);
        }
    }
    KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: setup done live=%u/%u dialogs=%u "
                         "timeouts=%u us=%u", (unsigned)live,
                         (unsigned)pair_size, (unsigned)s_dialogs,
                         (unsigned)s_timeouts,
                         (unsigned)trophy_elapsed(started));
    (void)trophy_sema_signal();
}

/* ---- worker -------------------------------------------------------------- */

/* Publishes "the worker may be inside a call on this set" and then reads
 * the slot (both seq_cst; trophy_destroy does the mirror image).  Every NP
 * call on a context is made between trophy_enter() returning 1 and the
 * next trophy_enter()/trophy_leave(). */
static int trophy_enter(uint32_t set)
{
    __atomic_store_n(&s_worker_set, (int)set, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&s_slot[set], __ATOMIC_SEQ_CST) == TROPHY_SLOT_OPEN;
}

static void trophy_leave(void)
{
    __atomic_store_n(&s_worker_set, -1, __ATOMIC_SEQ_CST);
}

static void trophy_load_state(uint32_t set)
{
    trophy_flag_array flags = { { 0u, 0u, 0u, 0u } };
    int handle = -1;
    int count = 0;
    int rc;
    uint32_t i;

    rc = trophy_create_handle(&handle);
    if (rc >= 0) {
        rc = trophy_unlock_state(s_context[set], handle, &flags, &count);
        (void)trophy_destroy_handle(handle);
    }
    for (i = 0u; i < 4u; ++i)
        s_state[set][i] = rc >= 0 ? flags.flag_bits[i] : 0u;
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: state set=%u rc=0x%08x count=%d "
        "bits=%08x%08x%08x%08x", (unsigned)set, (unsigned)rc, count,
        (unsigned)s_state[set][0], (unsigned)s_state[set][1],
        (unsigned)s_state[set][2], (unsigned)s_state[set][3]);
    if (rc < 0)
        return;
    /* The pair choice of the next boot uses this state. */
    for (i = 0u; i < 4u; ++i) {
        if (s_store_state[set][i] != s_state[set][i]) {
            s_store_state[set][i] = s_state[set][i];
            s_store_dirty = 1;
        }
    }
    if ((s_store_known & (1u << set)) == 0u) {
        s_store_known |= 1u << set;
        s_store_dirty = 1;
    }
}

/* The Vita has the trophy: drop the id from the store, remember the bit. */
static void trophy_delivered(uint32_t id, uint32_t set, uint32_t trophy)
{
    uint32_t bit = trophy_bit(trophy);

    s_state[set][trophy >> 5] |= bit;
    if ((s_store_known & (1u << set)) != 0u &&
            (s_store_state[set][trophy >> 5] & bit) == 0u) {
        s_store_state[set][trophy >> 5] |= bit;
        s_store_dirty = 1;
    }
    if ((s_store[id >> 5] & trophy_bit(id)) != 0u) {
        s_store[id >> 5] &= ~trophy_bit(id);
        s_store_dirty = 1;
    }
}

/* The id cannot reach the Vita now.  A live unlock is kept in the store
 * and delivered at the next game start; a store id simply stays there; an
 * import or test id counts as noset (a partial import keeps its request). */
static void trophy_undelivered(uint32_t id, uint32_t set, uint32_t trophy,
                               int src)
{
    if (src == TROPHY_SRC_LIVE) {
        if ((s_store[id >> 5] & trophy_bit(id)) == 0u) {
            s_store[id >> 5] |= trophy_bit(id);
            s_store_dirty = 1;
        }
        KAGE_VITA_TROPHY_LOG(
            "KAGE VITA TROPHY: pending id=%u set=%u trophy=%03u",
            (unsigned)id, (unsigned)set, (unsigned)trophy);
        return;
    }
    ++s_counts[src].noset;
    trophy_skip(src, id, "noset");
}

static void trophy_resolve(uint32_t id, int src)
{
    struct trophy_counts *counts = &s_counts[src];
    uint32_t set;
    uint32_t trophy;
    uint64_t started;
    int handle = -1;
    int platinum = -1;
    int called = 0;
    int rc;

    if (!isaac_vita_trophies_map(id, &set, &trophy)) {
        ++counts->excluded;
        trophy_skip(src, id, "excluded");
        return;
    }
    if (!trophy_enter(set)) {
        trophy_leave();
        trophy_undelivered(id, set, trophy, src);
        return;
    }
    if ((s_state[set][trophy >> 5] & trophy_bit(trophy)) != 0u) {
        /* GetTrophyUnlockState said unlocked: the Vita has it. */
        trophy_leave();
        ++counts->already;
        trophy_delivered(id, set, trophy);
        trophy_skip(src, id, "already");
        return;
    }
    started = trophy_time_us();
    rc = trophy_create_handle(&handle);
    if (rc >= 0) {
        rc = trophy_unlock(s_context[set], handle, (int)trophy, &platinum);
        called = 1;
        (void)trophy_destroy_handle(handle);
    }
    trophy_leave();
    KAGE_VITA_TROPHY_LOG(
        "KAGE VITA TROPHY: unlock src=%s id=%u set=%u trophy=%03u rc=0x%08x "
        "plat=%d us=%u", k_src_names[src], (unsigned)id, (unsigned)set,
        (unsigned)trophy, (unsigned)rc, platinum,
        (unsigned)trophy_elapsed(started));
    if (rc == 0) {
        ++counts->unlocked;
        trophy_delivered(id, set, trophy);
    } else if ((uint32_t)rc == TROPHY_ERROR_ALREADY_UNLOCKED) {
        ++counts->already;
        trophy_delivered(id, set, trophy);
    } else if ((uint32_t)rc == TROPHY_ERROR_SETUP_REQUIRED) {
        __atomic_store_n(&s_slot[set], TROPHY_SLOT_FINAL, __ATOMIC_SEQ_CST);
        KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: set-dead set=%u rc=0x%08x",
                             (unsigned)set, (unsigned)rc);
        trophy_undelivered(id, set, trophy, src);
    } else {
        ++counts->failed;
        /* a live unlock is retried at the next start; import and test
         * ids are retried by their request */
        if (src == TROPHY_SRC_LIVE)
            trophy_undelivered(id, set, trophy, src);
    }
    if (platinum >= 0)
        KAGE_VITA_TROPHY_LOG("KAGE VITA TROPHY: platinum set=%u trophy=%d",
                             (unsigned)set, platinum);
    /* Pacing: one SceShell notification per unlock. */
    if (called)
        trophy_delay(TROPHY_UNLOCK_GAP_US);
}

static int trophy_drain_live_all(void);

/* Takes and resolves every queued id of one source in one set.  Import
 * ids re-drain the live queue in between, so a live unlock never waits
 * behind an import backlog. */
static int trophy_drain_set(uint32_t words[TROPHY_WORDS], uint32_t set,
                            int src)
{
    int resolved = 0;
    uint32_t w;

    (void)__atomic_add_fetch(&s_busy[set], 1, __ATOMIC_SEQ_CST);
    for (w = k_set_first[set] >> 5; w <= (uint32_t)(k_set_last[set] >> 5); ++w) {
        uint32_t mask = trophy_range_mask(set, w);
        uint32_t bits = __atomic_fetch_and(&words[w], ~mask,
                                           __ATOMIC_SEQ_CST) & mask;

        while (bits != 0u) {
            uint32_t b = (uint32_t)__builtin_ctz(bits);

            bits &= bits - 1u;
            trophy_resolve(w * 32u + b, src);
            ++resolved;
            (void)__atomic_add_fetch(&s_progress, 1u, __ATOMIC_SEQ_CST);
            if (src == TROPHY_SRC_IMPORT)
                resolved += trophy_drain_live_all();
        }
    }
    (void)__atomic_sub_fetch(&s_busy[set], 1, __ATOMIC_SEQ_CST);
    return resolved;
}

static int trophy_drain_live_all(void)
{
    int resolved = 0;
    uint32_t n;

    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        if (__atomic_load_n(&s_slot[n], __ATOMIC_SEQ_CST) != TROPHY_SLOT_WAITING)
            resolved += trophy_drain_set(s_pending_live, n, TROPHY_SRC_LIVE);
    }
    return resolved;
}

static int trophy_all_zero(uint32_t words[TROPHY_WORDS])
{
    uint32_t w;

    for (w = 0u; w < TROPHY_WORDS; ++w) {
        if (__atomic_load_n(&words[w], __ATOMIC_SEQ_CST) != 0u)
            return 0;
    }
    return 1;
}

/* One pass: every set the main thread has released (OPEN) or closed for
 * this boot (FINAL; its ids resolve as not deliverable).  Sets still
 * WAITING keep their queued ids until Stage B decides about them. */
static int trophy_worker_step(void)
{
    int resolved = 0;
    uint32_t n;

    if (!__atomic_load_n(&s_stage_b, __ATOMIC_ACQUIRE))
        return 0;
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        int slot = __atomic_load_n(&s_slot[n], __ATOMIC_SEQ_CST);

        if (slot == TROPHY_SLOT_WAITING)
            continue;
        if (slot == TROPHY_SLOT_OPEN && !s_state_loaded[n]) {
            s_state_loaded[n] = 1u;
            if (trophy_enter(n))
                trophy_load_state(n);
            trophy_leave();
            (void)__atomic_add_fetch(&s_progress, 1u, __ATOMIC_SEQ_CST);
        }
        resolved += trophy_drain_set(s_pending_live, n, TROPHY_SRC_LIVE);
        resolved += trophy_drain_set(s_pending_store, n, TROPHY_SRC_PENDING);
        resolved += trophy_drain_set(s_pending_test, n, TROPHY_SRC_TEST);
        resolved += trophy_drain_set(s_pending_import, n, TROPHY_SRC_IMPORT);
    }
    if (s_test_active && !s_test_reported && trophy_all_zero(s_pending_test)) {
        const struct trophy_counts *counts = &s_counts[TROPHY_SRC_TEST];

        s_test_reported = 1;
        KAGE_VITA_TROPHY_LOG(
            "KAGE VITA TROPHY TEST: done ids=%u unlocked=%u already=%u "
            "failed=%u", (unsigned)s_test_ids, (unsigned)counts->unlocked,
            (unsigned)counts->already, (unsigned)counts->failed);
        (void)trophy_remove(TROPHY_TEST_REQUEST);
    }
    if (s_import_active && !s_import_reported &&
            trophy_all_zero(s_pending_import)) {
        const struct trophy_counts *counts = &s_counts[TROPHY_SRC_IMPORT];
        int done = counts->failed == 0u && counts->noset == 0u;

        s_import_reported = 1;
        trophy_import_result(s_import_slot, done ? "done" : "partial",
                             s_import_saved, s_import_mapped, counts, done);
    }
    if (s_store_dirty)
        trophy_store_save();
    return resolved;
}

/* ---- oracle-only entry points ------------------------------------------- */

#if defined(ISAAC_VITA_TROPHIES_ORACLE)
int isaac_vita_trophies_worker_step(void)
{
    return trophy_worker_step();
}

void isaac_vita_trophies_oracle_reset(void)
{
    static const struct trophy_counts zero;
    uint32_t i;
    uint32_t n;

    s_booted = 0;
    s_setup_called = 0;
    s_feature_live = 0;
    s_accepting = 0;
    s_stage_b = 0;
    s_sema = -1;
    s_ctx_count = 0u;
    s_dialogs = 0u;
    s_timeouts = 0u;
    s_worker_set = -1;
    s_progress = 0u;
    for (n = 0u; n < ISAAC_VITA_TROPHY_SET_COUNT; ++n) {
        s_context[n] = 0;
        s_ctx_created[n] = 0u;
        s_held[n] = 0u;
        s_slot[n] = TROPHY_SLOT_WAITING;
        s_busy[n] = 0;
        s_state_loaded[n] = 0u;
        for (i = 0u; i < 4u; ++i)
            s_state[n][i] = 0u;
    }
    for (i = 0u; i < TROPHY_WORDS; ++i) {
        s_pending_live[i] = 0u;
        s_pending_test[i] = 0u;
        s_pending_import[i] = 0u;
        s_pending_store[i] = 0u;
        s_readonly_logged[i] = 0u;
    }
    s_mismatch_count = 0u;
    s_range_count = 0u;
    for (i = 0u; i < TROPHY_SRC_COUNT; ++i)
        s_counts[i] = zero;
    s_import_active = 0;
    s_import_reported = 0;
    s_import_slot = 0u;
    s_import_saved = 0u;
    s_import_mapped = 0u;
    s_test_active = 0;
    s_test_reported = 0;
    s_test_ids = 0u;
    trophy_store_clear();
    s_store_dirty = 0;
    s_store_on_disk = 0;
    s_pair[0] = -1;
    s_pair[1] = -1;
}

int isaac_vita_trophies_oracle_pair(uint32_t index)
{
    return s_pair[index];
}

void isaac_vita_trophies_oracle_worker_inside(int set)
{
    __atomic_store_n(&s_worker_set, set, __ATOMIC_SEQ_CST);
}

uint32_t isaac_vita_trophies_oracle_contexts(void)
{
    return s_ctx_count;
}

void isaac_vita_trophies_oracle_store_mark(uint32_t id)
{
    s_store[id >> 5] |= trophy_bit(id);
    s_store_dirty = 1;
}

int isaac_vita_trophies_oracle_store_has(uint32_t id)
{
    return (s_store[id >> 5] & trophy_bit(id)) != 0u;
}

uint32_t isaac_vita_trophies_oracle_store_count(void)
{
    return trophy_count_bits(s_store, TROPHY_WORDS);
}

void isaac_vita_trophies_oracle_store_set_state(uint32_t set,
                                                const uint32_t bits[4])
{
    uint32_t i;

    for (i = 0u; i < 4u; ++i)
        s_store_state[set][i] = bits[i];
    s_store_known |= 1u << set;
    s_store_dirty = 1;
}

uint32_t isaac_vita_trophies_oracle_store_known(void)
{
    return s_store_known;
}

uint32_t isaac_vita_trophies_oracle_store_state_word(uint32_t set,
                                                     uint32_t word)
{
    return s_store_state[set][word];
}

int isaac_vita_trophies_oracle_store_dirty(void)
{
    return s_store_dirty;
}

void isaac_vita_trophies_oracle_store_load(void)
{
    trophy_store_load();
}

void isaac_vita_trophies_oracle_store_save(void)
{
    trophy_store_save();
}

uint32_t isaac_vita_trophies_oracle_checksum(const void *data, uint32_t size)
{
    trophy_crc_build();
    return ~trophy_crc_bytes(~TROPHY_CRC_INIT, (const uint8_t *)data, size);
}
#endif
