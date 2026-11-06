/* Host oracle for host_vita_trophies.c
 * (ISAAC_VITA_TROPHIES=1 ISAAC_VITA_TROPHIES_ORACLE=1, optionally
 * ISAAC_VITA_TROPHY_TEST_REQUEST=1; recomp/test_vita_trophies.py builds
 * both variants with clang and gcc).
 *
 * Every SDK seam is faked here: NP calls with scripted return codes, setup
 * dialog status sequences and results, unlock-state bits, the semaphore,
 * the thread, the delay and the clock, sceIo over an in-memory file table
 * with failure injection, a guest-memory window (the manager pointer and
 * one PersistentGameData) and log capture.
 *
 * The NP fake models device run 2026-10-09: at most two live contexts
 * (the third CreateContext -> 0x80551606), DestroyContext frees the slot,
 * an instance answers 0x80551612 until a dialog on that instance finished
 * with result OK; it also counts calls on destroyed contexts and NP calls
 * made while a dialog runs (the fake present runs the worker, as the real
 * thread runs during a dialog).  check_pool_invariants() holds every boot
 * to those rules.  The clock advances by the measured costs (UnlockTrophy
 * 0.48 s, a vblank per presented frame, the pacing, the 10 ms polls), so
 * case_model prints Stage B times.
 *
 * Cases: the mapping boundaries and the whole table; the observer (not
 * accepting, mgr 0, pgd mismatch first 4 logged then counted, id 0, id 638,
 * already set, ReadOnly once per id, fresh, duplicate before the drain,
 * nested events during the live and the import drains); the failure rows
 * (module, init, a pair context refused, every context refused,
 * semaphore, thread with and without the priority fallback, thread start,
 * setup init failure, result not OK, GetResult error, timeout with Abort
 * and no further dialog that boot, every set dead after Stage B); the
 * worker (the unlock-state cache, ALREADY, SETUP_REQUIRED on a live set,
 * other errors kept pending and retried, platinum, trophy 0 never passed,
 * pacing, CreateHandle failure, live ids of non-live sets queued in the
 * store); the pending store (round trip, garbage, missing, recovery,
 * atomic replace with injected failures); the gameplay pair (fallback,
 * union of saves, last known Vita state, one set, none); boot delivery
 * across four sets, bits cleared only on success, bounded waits (stalled
 * worker, worker inside a call); an import spanning four sets completing
 * in one boot; the import request (synthetic saves built with this file's
 * own copy of the reference CRC, request spellings, every error status,
 * signed-byte semantics, excluded ids, delete/keep, the .res text, an
 * idempotent rerun, partial results); the test request with and without
 * ISAAC_VITA_TROPHY_TEST_REQUEST.
 *
 *   --dump-map FILE      canonical mapping dump (the mapping rule), LF
 *   --save FILE...       run each file through the real import path as
 *                        slot 1; prints the import line and the checksum */
#if !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "host_vita_trophies.h"
#include "platform.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ISAAC_VITA_TROPHIES_ORACLE)
#error "the trophy oracle needs ISAAC_VITA_TROPHIES_ORACLE"
#endif

#if !defined(GUEST_IMAGE_BASE)
#define GUEST_IMAGE_BASE 0x98000000u
#endif

/* Copied from host_vita_trophies.c (TROPHY_MGR_PTR_RVA, gen_all.py
 * VITA_TROPHY_UNLOCK_MANAGER_PTR_RVA) and the brief (A.2). */
#define ORACLE_MGR_ADDRESS (GUEST_IMAGE_BASE + 0x7fd680u)
#define ORACLE_MGR_VALUE   0x99000000u
#define ORACLE_PGD         (ORACLE_MGR_VALUE + 0x14u)
#define ORACLE_UNLOCKED    0x38u
#define ORACLE_RA          0x0032b4cdu
#define ORACLE_ENOENT      ((int)0x80010002u)
#define ORACLE_EIO         ((int)0x80010005u)
#define ORACLE_ALREADY     ((int)0x8055160fu)
#define ORACLE_SETUP_REQ   ((int)0x80551612u)
#define ORACLE_NP_UNKNOWN  ((int)0x80551600u)
#define ORACLE_TRP_MISSING ((int)0x80551619u)
#define ORACLE_SDK_VERSION 0x03570011u
#define ORACLE_DIALOG_MAGIC 0xC0D1A109u

#define ORACLE_ROOT ISAAC_VITA_NATIVE_DATA_ROOT
#define ORACLE_SAVE_DIR ORACLE_ROOT "/Documents/My Games/Binding of Isaac Repentance/"

static unsigned s_checks;
static unsigned s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void check_u(unsigned long got, unsigned long expected, const char *what)
{
    ++s_checks;
    if (got != expected) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s: got %lu expected %lu\n", what, got,
                expected);
    }
}

static void check_str(const char *got, const char *expected, const char *what)
{
    ++s_checks;
    if (strcmp(got, expected) != 0) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s:\n  got      \"%s\"\n  expected \"%s\"\n",
                what, got, expected);
    }
}

/* ---- log capture ---------------------------------------------------------- */

#define LOG_LINES 4096u
#define LOG_WIDTH 256u
static char s_log[LOG_LINES][LOG_WIDTH];
static unsigned s_log_count;
static unsigned s_log_overflow;

void isaac_vita_log(const char *format, ...)
{
    va_list args;

    if (s_log_count >= LOG_LINES) {
        ++s_log_overflow;
        return;
    }
    va_start(args, format);
    vsnprintf(s_log[s_log_count], LOG_WIDTH, format, args);
    va_end(args);
    ++s_log_count;
}

static unsigned log_count(const char *line)
{
    unsigned i;
    unsigned n = 0u;

    for (i = 0u; i < s_log_count; ++i)
        n += strcmp(s_log[i], line) == 0;
    return n;
}

static unsigned log_count_prefix(const char *prefix)
{
    unsigned i;
    unsigned n = 0u;
    size_t length = strlen(prefix);

    for (i = 0u; i < s_log_count; ++i)
        n += strncmp(s_log[i], prefix, length) == 0;
    return n;
}

static int log_index_prefix(const char *prefix)
{
    unsigned i;
    size_t length = strlen(prefix);

    for (i = 0u; i < s_log_count; ++i) {
        if (strncmp(s_log[i], prefix, length) == 0)
            return (int)i;
    }
    return -1;
}

static void expect_line(const char *line)
{
    char what[LOG_WIDTH + 32u];

    snprintf(what, sizeof what, "log line once: %s", line);
    check_u(log_count(line), 1u, what);
}

static void expect_prefix(const char *prefix, unsigned count)
{
    char what[LOG_WIDTH + 32u];

    snprintf(what, sizeof what, "log prefix count: %s", prefix);
    check_u(log_count_prefix(prefix), count, what);
}

static void dump_log(void)
{
    unsigned i;

    for (i = 0u; i < s_log_count; ++i)
        fprintf(stderr, "  log[%u] %s\n", i, s_log[i]);
}

/* ---- guest memory --------------------------------------------------------- */

static uint32_t s_mgr_value;
static uint8_t s_pgd[ORACLE_UNLOCKED + 640u];
static unsigned s_guest_reads;
static unsigned s_foreign_reads;

uint8_t isaac_vita_trophies_fake_read8(uint32_t address)
{
    ++s_guest_reads;
    if (address >= ORACLE_PGD && address - ORACLE_PGD < sizeof s_pgd)
        return s_pgd[address - ORACLE_PGD];
    ++s_foreign_reads;
    return 0xeeu;
}

uint32_t isaac_vita_trophies_fake_read32(uint32_t address)
{
    ++s_guest_reads;
    if (address == ORACLE_MGR_ADDRESS)
        return s_mgr_value;
    ++s_foreign_reads;
    return 0xeeeeeeeeu;
}

/* ---- NP, dialog, kernel fakes ---------------------------------------------- */

struct dialog_script {
    int init_rc;
    int running_frames;  /* -1 = RUNNING forever */
    int abort_frames;    /* RUNNING frames after Abort, -1 = forever */
    int result_rc;
    int result;
};

static int s_module_rc;
static uint32_t s_module_id;
static unsigned s_module_calls;
static int s_init_rc;
static unsigned s_init_calls;
static int s_ctx_rc[6];
static unsigned s_ctx_calls;
static uint8_t s_ctx_comm[6][12];
static int s_ctx_sign_ok[6];
static int s_ctx_options_ok;
static int s_handle_rc;
static int s_handle_next;
static int s_handles_open;
static unsigned s_handle_creates;
static int s_unlock_rc[6][128];
static int s_unlock_plat[6][128];
static unsigned s_unlock_calls;
static int s_unlock_set[2048];
static int s_unlock_trophy[2048];
static int s_unlock_got[2048];
static int s_unlock_bad;
static void (*s_unlock_hook)(int set, int trophy);
static int s_state_rc[6];
static uint32_t s_state_flags[6][4];
static int s_state_count[6];
static unsigned s_state_calls;
static struct dialog_script s_dialog[6];
static int s_dialog_active;
static int s_dialog_left;
static unsigned s_dialog_inits[6];
static unsigned s_dialog_aborts[6];
static unsigned s_dialog_results[6];
static unsigned s_dialog_terms[6];
static unsigned s_dialog_presents[6];
static int s_dialog_param_bad;
static const void *s_dialog_param_address;
static int s_sema_rc;
static unsigned s_sema_creates;
static unsigned s_sema_signals;
static int s_sema_args_ok;
static int s_thread_rc[2];
static unsigned s_thread_calls;
static int s_thread_priority[2];
static int s_thread_args_ok;
static int s_thread_start_rc;
static unsigned s_thread_starts;
static unsigned s_delays;
static unsigned s_delay_bad;
static uint64_t s_clock;
static char s_trace[16384];
static unsigned s_trace_length;

static void trace(char event)
{
    if (s_trace_length + 1u < sizeof s_trace) {
        s_trace[s_trace_length++] = event;
        s_trace[s_trace_length] = '\0';
    }
}

/* Context instances, modelling device run 2026-10-09: at most two live
 * (the third CreateContext -> 0x80551606), DestroyContext frees the slot,
 * and an instance answers 0x80551612 (SETUP_REQUIRED) to state / unlock
 * calls until a setup dialog on that very instance finished with result
 * OK.  Handles are 200 + instance index; a destroyed instance stays in the
 * table so a later use of it is caught. */
#define ORACLE_EXCEEDS  ((int)0x80551606u)
#define ORACLE_CTX_BASE 200
#define ORACLE_CTX_MAX  64u
struct fake_ctx {
    int set;
    int live;
    int setup_ok;
};
static struct fake_ctx s_ctxs[ORACLE_CTX_MAX];
static unsigned s_ctx_instances;
static unsigned s_ctx_live;
static unsigned s_ctx_max_live;
static unsigned s_ctx_creates[6];
static unsigned s_ctx_destroys[6];
static unsigned s_ctx_exceeds;
static unsigned s_comm_bad;
static unsigned s_destroy_bad;
static unsigned s_use_bad;
static unsigned s_overlap_bad;
static unsigned s_setup_required;
static int s_dialog_instance;
static int s_dialog_aborted;
static int s_tick_stuck;
static unsigned s_ticks;

static int instance_of(int context)
{
    int index = context - ORACLE_CTX_BASE;

    return (index >= 0 && (unsigned)index < s_ctx_instances) ? index : -1;
}

/* The set of a live context; a destroyed or unknown one is a use error. */
static int set_of_context(int context)
{
    int index = instance_of(context);

    if (index < 0 || !s_ctxs[index].live) {
        ++s_use_bad;
        return -1;
    }
    return s_ctxs[index].set;
}

static unsigned live_contexts_of(int set)
{
    unsigned i;
    unsigned n = 0u;

    for (i = 0u; i < s_ctx_instances; ++i)
        n += s_ctxs[i].live && s_ctxs[i].set == set;
    return n;
}

int isaac_vita_trophies_fake_sysmodule_load(uint32_t module)
{
    ++s_module_calls;
    s_module_id = module;
    return s_module_rc;
}

int isaac_vita_trophies_fake_np_init(void)
{
    ++s_init_calls;
    return s_init_rc;
}

int isaac_vita_trophies_fake_create_context(int *context, const void *comm_id,
                                            const void *signature,
                                            uint64_t options)
{
    const uint8_t *comm = (const uint8_t *)comm_id;
    const uint8_t *sign = (const uint8_t *)signature;
    static const uint8_t head[6] = { 0xb9u, 0xddu, 0xe1u, 0x3bu, 0x01u, 0x00u };
    unsigned n;
    unsigned i;
    unsigned index;
    int ok = 1;

    ++s_ctx_calls;
    n = comm[10];
    if (memcmp(comm, "ISAC00000", 9u) != 0 || comm[9] != 0u || n >= 6u ||
            comm[11] != 0u) {
        ++s_comm_bad;
        return ORACLE_NP_UNKNOWN;
    }
    memcpy(s_ctx_comm[n], comm, 12u);
    for (i = 0u; i < 160u; ++i)
        ok &= sign[i] == (i < 6u ? head[i] : 0u);
    s_ctx_sign_ok[n] = ok;
    s_ctx_options_ok &= options == 0u;
    if (s_ctx_rc[n] < 0)
        return s_ctx_rc[n];
    if (s_ctx_live >= 2u) {
        ++s_ctx_exceeds;
        return ORACLE_EXCEEDS;
    }
    if (s_ctx_instances >= ORACLE_CTX_MAX)
        return ORACLE_NP_UNKNOWN;
    index = s_ctx_instances++;
    s_ctxs[index].set = (int)n;
    s_ctxs[index].live = 1;
    s_ctxs[index].setup_ok = 0;
    ++s_ctx_live;
    ++s_ctx_creates[n];
    if (s_ctx_live > s_ctx_max_live)
        s_ctx_max_live = s_ctx_live;
    *context = ORACLE_CTX_BASE + (int)index;
    return 0;
}

int isaac_vita_trophies_fake_destroy_context(int context)
{
    int index = instance_of(context);

    if (index < 0 || !s_ctxs[index].live || s_dialog_instance == index) {
        ++s_destroy_bad;
        return ORACLE_NP_UNKNOWN;
    }
    s_ctxs[index].live = 0;
    --s_ctx_live;
    ++s_ctx_destroys[s_ctxs[index].set];
    return 0;
}

void isaac_vita_trophies_fake_wait_tick(void)
{
    ++s_ticks;
    s_clock += 10000u;
    if (!s_tick_stuck)
        (void)isaac_vita_trophies_worker_step();
}

int isaac_vita_trophies_fake_create_handle(int *handle)
{
    ++s_handle_creates;
    trace('H');
    if (s_handle_rc < 0)
        return s_handle_rc;
    *handle = s_handle_next++;
    ++s_handles_open;
    return 0;
}

int isaac_vita_trophies_fake_destroy_handle(int handle)
{
    (void)handle;
    trace('X');
    --s_handles_open;
    return 0;
}

int isaac_vita_trophies_fake_unlock(int context, int handle, int trophy,
                                    int *platinum)
{
    int set = set_of_context(context);
    unsigned n;

    (void)handle;
    if (s_dialog_instance >= 0)
        ++s_overlap_bad;
    if (set < 0 || trophy <= 0 || trophy >= 128) {
        ++s_unlock_bad;
        return ORACLE_NP_UNKNOWN;
    }
    n = s_unlock_calls++;
    trace('U');
    s_clock += 480000u;      /* UnlockTrophy ~0.48 s, device run 2026-10-09 */
    if (n < 2048u) {
        s_unlock_set[n] = set;
        s_unlock_trophy[n] = trophy;
        s_unlock_got[n] = ORACLE_SETUP_REQ;
    }
    if (!s_ctxs[instance_of(context)].setup_ok) {
        ++s_setup_required;
        return ORACLE_SETUP_REQ;
    }
    if (s_unlock_hook != NULL)
        s_unlock_hook(set, trophy);
    *platinum = s_unlock_plat[set][trophy];
    if (n < 2048u)
        s_unlock_got[n] = s_unlock_rc[set][trophy];
    return s_unlock_rc[set][trophy];
}

int isaac_vita_trophies_fake_unlock_state(int context, int handle,
                                          uint32_t flags[4], int *count)
{
    int set = set_of_context(context);
    unsigned i;

    (void)handle;
    ++s_state_calls;
    trace('S');
    if (s_dialog_instance >= 0)
        ++s_overlap_bad;
    if (set < 0)
        return ORACLE_NP_UNKNOWN;
    if (!s_ctxs[instance_of(context)].setup_ok) {
        ++s_setup_required;
        return ORACLE_SETUP_REQ;
    }
    if (s_state_rc[set] < 0)
        return s_state_rc[set];
    for (i = 0u; i < 4u; ++i)
        flags[i] = s_state_flags[set][i];
    *count = s_state_count[set];
    return 0;
}

int isaac_vita_trophies_fake_setup_init(uint32_t sdk_version, int context,
                                        uint32_t options, uint32_t magic,
                                        const void *common_param)
{
    int index = instance_of(context);
    int set;

    if (index < 0 || !s_ctxs[index].live || s_dialog_instance >= 0) {
        ++s_dialog_param_bad;
        return ORACLE_NP_UNKNOWN;
    }
    set = s_ctxs[index].set;
    ++s_dialog_inits[set];
    if (sdk_version != ORACLE_SDK_VERSION || options != 0u ||
            magic != ORACLE_DIALOG_MAGIC + (uint32_t)(uintptr_t)common_param)
        ++s_dialog_param_bad;
    if (s_dialog_param_address == NULL)
        s_dialog_param_address = common_param;
    else if (s_dialog_param_address != common_param)
        ++s_dialog_param_bad;
    if (s_dialog[set].init_rc < 0)
        return s_dialog[set].init_rc;
    s_dialog_active = set;
    s_dialog_instance = index;
    s_dialog_aborted = 0;
    s_dialog_left = s_dialog[set].running_frames;
    return 0;
}

int isaac_vita_trophies_fake_setup_status(void)
{
    if (s_dialog_active < 0)
        return 0;
    return s_dialog_left != 0 ? 1 : 2;
}

static void fake_present(void)
{
    if (s_dialog_active < 0) {
        ++s_dialog_param_bad;
        return;
    }
    ++s_dialog_presents[s_dialog_active];
    s_clock += 16667u;       /* one vblank */
    if (s_dialog_left > 0)
        --s_dialog_left;
    /* The real worker thread runs while the main thread pumps a dialog:
     * any NP call it makes now is counted as an overlap. */
    if (!s_tick_stuck)
        (void)isaac_vita_trophies_worker_step();
}

int isaac_vita_trophies_fake_setup_abort(void)
{
    if (s_dialog_active < 0)
        return ORACLE_NP_UNKNOWN;
    ++s_dialog_aborts[s_dialog_active];
    s_dialog_aborted = 1;
    s_dialog_left = s_dialog[s_dialog_active].abort_frames;
    return 0;
}

int isaac_vita_trophies_fake_setup_result(int *result)
{
    if (s_dialog_active < 0)
        return ORACLE_NP_UNKNOWN;
    ++s_dialog_results[s_dialog_active];
    if (s_dialog[s_dialog_active].result_rc < 0)
        return s_dialog[s_dialog_active].result_rc;
    *result = s_dialog[s_dialog_active].result;
    return 0;
}

int isaac_vita_trophies_fake_setup_term(void)
{
    if (s_dialog_active < 0)
        return ORACLE_NP_UNKNOWN;
    ++s_dialog_terms[s_dialog_active];
    /* The instance is set up only by a dialog that finished by itself with
     * result OK. */
    if (!s_dialog_aborted && s_dialog_left == 0 &&
            s_dialog[s_dialog_active].result_rc >= 0 &&
            s_dialog[s_dialog_active].result == 0)
        s_ctxs[s_dialog_instance].setup_ok = 1;
    s_dialog_active = -1;
    s_dialog_instance = -1;
    return 0;
}

int isaac_vita_trophies_fake_sema_create(const char *name, int initial,
                                         int maximum)
{
    ++s_sema_creates;
    s_sema_args_ok = strcmp(name, "isaac_trophy") == 0 && initial == 0 &&
                     maximum == 1024;
    return s_sema_rc;
}

int isaac_vita_trophies_fake_sema_signal(int sema)
{
    if (sema == s_sema_rc && sema >= 0)
        ++s_sema_signals;
    return 0;
}

int isaac_vita_trophies_fake_thread_create(const char *name, int priority,
                                           uint32_t stack, uint32_t affinity)
{
    unsigned n = s_thread_calls++;

    s_thread_args_ok = strcmp(name, "isaac_trophy") == 0 &&
                       stack == 0x10000u && affinity == 0x00040000u;
    if (n >= 2u)
        return ORACLE_NP_UNKNOWN;
    s_thread_priority[n] = priority;
    return s_thread_rc[n];
}

int isaac_vita_trophies_fake_thread_start(int thread)
{
    (void)thread;
    ++s_thread_starts;
    return s_thread_start_rc;
}

void isaac_vita_trophies_fake_delay(uint32_t microseconds)
{
    ++s_delays;
    trace('D');
    s_clock += microseconds;
    if (microseconds != 100000u)
        ++s_delay_bad;
}

uint64_t isaac_vita_trophies_fake_time_us(void)
{
    s_clock += 10u;
    return s_clock;
}

/* ---- in-memory files ------------------------------------------------------- */

enum { F_IMPORT_REQ = 0, F_IMPORT_RES, F_TEST_REQ, F_SAVE1, F_SAVE2, F_SAVE3,
       F_STORE, F_STORE_TMP, F_STORE_BAK, F_COUNT };
static const char *const k_paths[F_COUNT] = {
    ORACLE_ROOT "/trophy-import.req",
    ORACLE_ROOT "/trophy-import.res",
    ORACLE_ROOT "/trophy-test.req",
    ORACLE_SAVE_DIR "persistentgamedata1.dat",
    ORACLE_SAVE_DIR "persistentgamedata2.dat",
    ORACLE_SAVE_DIR "persistentgamedata3.dat",
    ORACLE_ROOT "/trophy-pending.bin",
    ORACLE_ROOT "/trophy-pending.bin.tmp",
    ORACLE_ROOT "/trophy-pending.bin.bak",
};
#define ORACLE_EEXIST      ((int)0x80010011u)
#define ORACLE_STORE_BYTES 200u

#define BIG_SAVE_BYTES (1024u * 1024u + 64u)
static uint8_t s_big_save[BIG_SAVE_BYTES];
static uint8_t s_small_data[F_COUNT][16384];

struct fake_file {
    int exists;
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    int64_t size_override; /* >= 0 replaces the size; < -1 is a size error */
    int open_rc;
    int read_rc;
    int64_t eof_at;        /* >= 0: reads stop delivering at this offset */
    int remove_rc;
    int write_rc;
    int rename_rc;         /* < 0: renaming this file (as the source) fails */
    unsigned opens;
    unsigned writes;
    unsigned renames;
};
static struct fake_file s_files[F_COUNT];
static unsigned s_unknown_opens;
static unsigned s_open_descriptors;

struct fake_descriptor {
    int file;
    uint32_t position;
    int open;
};
static struct fake_descriptor s_fds[8];

static int file_of(const char *path)
{
    int i;

    for (i = 0; i < F_COUNT; ++i) {
        if (strcmp(path, k_paths[i]) == 0)
            return i;
    }
    return -1;
}

int isaac_vita_trophies_fake_open_read(const char *path)
{
    int file = file_of(path);
    int fd;

    if (file < 0) {
        ++s_unknown_opens;
        return ORACLE_ENOENT;
    }
    ++s_files[file].opens;
    if (s_files[file].open_rc < 0)
        return s_files[file].open_rc;
    if (!s_files[file].exists)
        return ORACLE_ENOENT;
    for (fd = 0; fd < 8; ++fd) {
        if (!s_fds[fd].open) {
            s_fds[fd].open = 1;
            s_fds[fd].file = file;
            s_fds[fd].position = 0u;
            ++s_open_descriptors;
            return fd + 3;
        }
    }
    return ORACLE_EIO;
}

int isaac_vita_trophies_fake_read(int descriptor, void *buffer, uint32_t size)
{
    struct fake_descriptor *fd;
    struct fake_file *file;
    uint32_t end;
    uint32_t count;

    if (descriptor < 3 || descriptor >= 11 || !s_fds[descriptor - 3].open)
        return ORACLE_EIO;
    fd = &s_fds[descriptor - 3];
    file = &s_files[fd->file];
    if (file->read_rc < 0)
        return file->read_rc;
    end = file->size;
    if (file->eof_at >= 0 && (uint32_t)file->eof_at < end)
        end = (uint32_t)file->eof_at;
    if (fd->position >= end)
        return 0;
    count = end - fd->position;
    if (count > size)
        count = size;
    memcpy(buffer, file->data + fd->position, count);
    fd->position += count;
    return (int)count;
}

int64_t isaac_vita_trophies_fake_size(int descriptor)
{
    struct fake_file *file;

    if (descriptor < 3 || descriptor >= 11 || !s_fds[descriptor - 3].open)
        return ORACLE_EIO;
    file = &s_files[s_fds[descriptor - 3].file];
    if (file->size_override < -1)
        return file->size_override;
    if (file->size_override >= 0)
        return file->size_override;
    return (int64_t)file->size;
}

int isaac_vita_trophies_fake_close(int descriptor)
{
    if (descriptor < 3 || descriptor >= 11 || !s_fds[descriptor - 3].open)
        return ORACLE_EIO;
    s_fds[descriptor - 3].open = 0;
    --s_open_descriptors;
    return 0;
}

int isaac_vita_trophies_fake_remove(const char *path)
{
    int file = file_of(path);

    if (file < 0)
        return ORACLE_ENOENT;
    if (s_files[file].remove_rc < 0)
        return s_files[file].remove_rc;
    if (!s_files[file].exists)
        return ORACLE_ENOENT;
    s_files[file].exists = 0;
    return 0;
}

int isaac_vita_trophies_fake_write_file(const char *path, const void *data,
                                        uint32_t size)
{
    int file = file_of(path);

    if (file < 0)
        return ORACLE_EIO;
    ++s_files[file].writes;
    if (s_files[file].write_rc < 0)
        return s_files[file].write_rc;
    if (size > s_files[file].capacity)
        return ORACLE_EIO;
    memcpy(s_files[file].data, data, size);
    s_files[file].size = size;
    s_files[file].exists = 1;
    return 0;
}

int isaac_vita_trophies_fake_write_new(const char *path, const void *data,
                                       uint32_t size)
{
    int file = file_of(path);

    if (file < 0)
        return ORACLE_EIO;
    ++s_files[file].writes;
    if (s_files[file].exists)
        return ORACLE_EEXIST;
    if (s_files[file].write_rc < 0) {
        /* a partly written temp is left behind, as on a real card */
        memcpy(s_files[file].data, data, size / 2u);
        s_files[file].size = size / 2u;
        s_files[file].exists = 1;
        return s_files[file].write_rc;
    }
    if (size > s_files[file].capacity)
        return ORACLE_EIO;
    memcpy(s_files[file].data, data, size);
    s_files[file].size = size;
    s_files[file].exists = 1;
    return 0;
}

int isaac_vita_trophies_fake_rename(const char *source, const char *destination)
{
    int from = file_of(source);
    int to = file_of(destination);

    if (from < 0 || to < 0)
        return ORACLE_EIO;
    ++s_files[from].renames;
    if (s_files[from].rename_rc < 0)
        return s_files[from].rename_rc;
    if (!s_files[from].exists)
        return ORACLE_ENOENT;
    if (s_files[to].exists)
        return ORACLE_EEXIST;
    memcpy(s_files[to].data, s_files[from].data, s_files[from].size);
    s_files[to].size = s_files[from].size;
    s_files[to].exists = 1;
    s_files[from].exists = 0;
    return 0;
}

static void put_file(int file, const void *data, uint32_t size)
{
    memcpy(s_files[file].data, data, size);
    s_files[file].size = size;
    s_files[file].exists = 1;
}

static void put_text(int file, const char *text)
{
    put_file(file, text, (uint32_t)strlen(text));
}

static const char *file_text(int file)
{
    static char text[16385];
    uint32_t size = s_files[file].size;

    if (!s_files[file].exists)
        return "<absent>";
    if (size > 16384u)
        size = 16384u;
    memcpy(text, s_files[file].data, size);
    text[size] = '\0';
    return text;
}

/* ---- reset ---------------------------------------------------------------- */

static void fake_defaults(void)
{
    int i;

    s_log_count = 0u;
    s_log_overflow = 0u;
    s_mgr_value = ORACLE_MGR_VALUE;
    memset(s_pgd, 0, sizeof s_pgd);
    s_guest_reads = 0u;
    s_foreign_reads = 0u;
    s_module_rc = 0;
    s_module_id = 0u;
    s_module_calls = 0u;
    s_init_rc = 0;
    s_init_calls = 0u;
    memset(s_ctx_rc, 0, sizeof s_ctx_rc);
    s_ctx_calls = 0u;
    memset(s_ctx_comm, 0, sizeof s_ctx_comm);
    memset(s_ctx_sign_ok, 0, sizeof s_ctx_sign_ok);
    s_ctx_options_ok = 1;
    s_handle_rc = 0;
    s_handle_next = 1;
    s_handles_open = 0;
    s_handle_creates = 0u;
    memset(s_unlock_rc, 0, sizeof s_unlock_rc);
    for (i = 0; i < 6; ++i) {
        int t;

        for (t = 0; t < 128; ++t)
            s_unlock_plat[i][t] = -1;
        s_dialog[i].init_rc = 0;
        s_dialog[i].running_frames = 3;
        s_dialog[i].abort_frames = 0;
        s_dialog[i].result_rc = 0;
        s_dialog[i].result = 0;
    }
    s_unlock_calls = 0u;
    s_unlock_bad = 0;
    s_unlock_hook = NULL;
    memset(s_state_rc, 0, sizeof s_state_rc);
    memset(s_state_flags, 0, sizeof s_state_flags);
    memset(s_state_count, 0, sizeof s_state_count);
    s_state_calls = 0u;
    s_dialog_active = -1;
    s_dialog_left = 0;
    memset(s_dialog_inits, 0, sizeof s_dialog_inits);
    memset(s_dialog_aborts, 0, sizeof s_dialog_aborts);
    memset(s_dialog_results, 0, sizeof s_dialog_results);
    memset(s_dialog_terms, 0, sizeof s_dialog_terms);
    memset(s_dialog_presents, 0, sizeof s_dialog_presents);
    s_dialog_param_bad = 0;
    s_dialog_param_address = NULL;
    s_sema_rc = 7;
    s_sema_creates = 0u;
    s_sema_signals = 0u;
    s_sema_args_ok = 0;
    s_thread_rc[0] = 11;
    s_thread_rc[1] = 12;
    s_thread_calls = 0u;
    s_thread_priority[0] = 0;
    s_thread_priority[1] = 0;
    s_thread_args_ok = 0;
    s_thread_start_rc = 0;
    s_thread_starts = 0u;
    s_delays = 0u;
    s_delay_bad = 0u;
    s_clock = 0u;
    s_trace_length = 0u;
    s_trace[0] = '\0';
    for (i = 0; i < F_COUNT; ++i) {
        memset(&s_files[i], 0, sizeof s_files[i]);
        s_files[i].data = i == F_SAVE1 ? s_big_save : s_small_data[i];
        s_files[i].capacity = i == F_SAVE1 ? BIG_SAVE_BYTES : 16384u;
        s_files[i].size_override = -1;
        s_files[i].eof_at = -1;
    }
    s_unknown_opens = 0u;
    s_open_descriptors = 0u;
    memset(s_fds, 0, sizeof s_fds);
    memset(s_ctxs, 0, sizeof s_ctxs);
    s_ctx_instances = 0u;
    s_ctx_live = 0u;
    s_ctx_max_live = 0u;
    memset(s_ctx_creates, 0, sizeof s_ctx_creates);
    memset(s_ctx_destroys, 0, sizeof s_ctx_destroys);
    s_ctx_exceeds = 0u;
    s_comm_bad = 0u;
    s_destroy_bad = 0u;
    s_use_bad = 0u;
    s_overlap_bad = 0u;
    s_setup_required = 0u;
    s_dialog_instance = -1;
    s_dialog_aborted = 0;
    s_tick_stuck = 0;
    s_ticks = 0u;
}

static void reset_all(void)
{
    isaac_vita_trophies_oracle_reset();
    fake_defaults();
}

/* Reset only the runtime (a new boot: a new process, so no context
 * survives) while the "device" keeps its files, its trophy state and its
 * scripts. */
static void reboot_runtime(void)
{
    isaac_vita_trophies_oracle_reset();
    s_log_count = 0u;
    s_unlock_calls = 0u;
    s_ctx_calls = 0u;
    s_thread_calls = 0u;
    s_dialog_param_address = NULL;
    s_trace_length = 0u;
    s_trace[0] = '\0';
    memset(s_ctxs, 0, sizeof s_ctxs);
    s_ctx_instances = 0u;
    s_ctx_live = 0u;
    s_ctx_max_live = 0u;
    memset(s_ctx_creates, 0, sizeof s_ctx_creates);
    memset(s_ctx_destroys, 0, sizeof s_ctx_destroys);
    memset(s_dialog_inits, 0, sizeof s_dialog_inits);
    s_clock = 0u;
    s_ticks = 0u;
}

/* Invariants every boot must keep, whatever the scripts: never more than
 * two live contexts, never a call on a destroyed context, never a context
 * used without its own dialog, never an unlock during a dialog, every
 * handle destroyed, nothing destroyed twice. */
static void check_pool_invariants(const char *where)
{
    char what[128];

    snprintf(what, sizeof what, "%s: <= 2 live contexts", where);
    check(s_ctx_max_live <= 2u && s_ctx_exceeds == 0u, what);
    snprintf(what, sizeof what, "%s: no call on a dead context", where);
    check_u(s_use_bad + s_destroy_bad, 0u, what);
    snprintf(what, sizeof what, "%s: no context used before its dialog", where);
    check_u(s_setup_required, 0u, what);
    snprintf(what, sizeof what, "%s: no NP call during a dialog", where);
    check_u(s_overlap_bad, 0u, what);
    snprintf(what, sizeof what, "%s: handles balanced, comm ids valid", where);
    check(s_handles_open == 0 && s_comm_bad == 0u, what);
}

static void boot_and_setup(void)
{
    isaac_vita_trophies_boot();
    isaac_vita_trophies_setup(fake_present);
}

/* What the fake device remembers: every unlocked (set, trophy) since the
 * last call becomes a GetTrophyUnlockState bit. */
static void persist_unlocks(void)
{
    unsigned i;

    for (i = 0u; i < s_unlock_calls && i < 2048u; ++i) {
        int set = s_unlock_set[i];
        int trophy = s_unlock_trophy[i];

        if (s_unlock_got[i] == 0 || s_unlock_got[i] == ORACLE_ALREADY)
            s_state_flags[set][trophy >> 5] |= 1u << (trophy & 31);
    }
}

/* Successful UnlockTrophy calls (rc 0) of one set. */
static unsigned unlocked_in(int set)
{
    unsigned i;
    unsigned n = 0u;

    for (i = 0u; i < s_unlock_calls && i < 2048u; ++i)
        n += s_unlock_set[i] == set && s_unlock_got[i] == 0;
    return n;
}

static void observe(uint32_t id)
{
    isaac_vita_trophies_try_unlock_observe(ORACLE_PGD, id, ORACLE_RA);
}

static int unlock_called(int set, int trophy)
{
    unsigned i;
    int n = 0;

    for (i = 0u; i < s_unlock_calls && i < 2048u; ++i)
        n += s_unlock_set[i] == set && s_unlock_trophy[i] == trophy;
    return n;
}

/* Every 'U' is followed by its destroy and then the pacing delay. */
static int pacing_ok(void)
{
    unsigned i;

    for (i = 0u; i < s_trace_length; ++i) {
        if (s_trace[i] == 'U' &&
                (i + 2u >= s_trace_length || s_trace[i + 1u] != 'X' ||
                 s_trace[i + 2u] != 'D'))
            return 0;
        if (s_trace[i] == 'D' && (i < 2u || s_trace[i - 2u] != 'U'))
            return 0;
    }
    return 1;
}

/* ---- reference save builder (own copy of the checksum steps) ---------- */

static uint32_t s_ref_table[256];

static void ref_table_build(void)
{
    uint32_t i;

    for (i = 0u; i < 256u; ++i) {
        uint32_t v = (i >> 1) ^ ((i & 1u) ? 0xEDB88320u : 0u);
        int k;

        for (k = 0; k < 7; ++k) {
            uint32_t sar = (v >> 1) | (v & 0x80000000u);
            v = sar ^ ((v & 1u) ? 0xEDB88320u : 0u);
        }
        s_ref_table[i] = v;
    }
}

static uint32_t ref_checksum(const uint8_t *data, uint32_t size)
{
    uint32_t ebx = ~0xFEDCBA76u;
    uint32_t i;

    for (i = 0u; i < size; ++i)
        ebx = (ebx >> 8) ^ s_ref_table[(data[i] ^ ebx) & 0xffu];
    return ~ebx;
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

/* magic, u32 @0x10, chunk 1 {id, 0x27e, count, 0x27e bytes}, chunk 2
 * {2, 0x7c0, 0x1f0} + 64 filler bytes, CRC LE over [0x10, size-4). */
static uint32_t build_save(uint8_t *out, const uint8_t bytes[0x27e],
                           uint32_t chunk_id, uint32_t count)
{
    uint32_t size = 0u;
    uint32_t i;

    memcpy(out, "ISAACNGSAVE09R  ", 16u);
    put32(out + 0x10, 0x00012345u);
    put32(out + 0x14, chunk_id);
    put32(out + 0x18, 0x27eu);
    put32(out + 0x1c, count);
    memcpy(out + 0x20, bytes, 0x27eu);
    size = 0x20u + 0x27eu;
    put32(out + size, 2u);
    put32(out + size + 4u, 0x7c0u);
    put32(out + size + 8u, 0x1f0u);
    size += 12u;
    for (i = 0u; i < 64u; ++i)
        out[size + i] = (uint8_t)(i * 7u + 3u);
    size += 64u;
    put32(out + size, ref_checksum(out + 0x10, size - 0x10u));
    return size + 4u;
}

static void reseal(uint8_t *out, uint32_t size)
{
    put32(out + size - 4u, ref_checksum(out + 0x10, size - 0x14u));
}

/* ids 1 (0x01), 2 (0x7f), 31, 325, 336, 354, 637 unlocked; 3 (0x80), 4
 * (0xff) and slot 0 are not ids: saved 7, mapped 4 (1, 2, 31, 637). */
static uint32_t standard_save(uint8_t *out)
{
    uint8_t bytes[0x27e];

    memset(bytes, 0, sizeof bytes);
    bytes[0] = 0x01u;
    bytes[1] = 0x01u;
    bytes[2] = 0x7fu;
    bytes[3] = 0x80u;
    bytes[4] = 0xffu;
    bytes[31] = 0x01u;
    bytes[325] = 0x01u;
    bytes[336] = 0x01u;
    bytes[354] = 0x01u;
    bytes[637] = 0x01u;
    return build_save(out, bytes, 1u, 0x27eu);
}

/* ---- cases: mapping --------------------------------------------------------- */

static void case_map(void)
{
    static const struct {
        uint32_t id;
        int mapped;
        uint32_t set;
        uint32_t trophy;
    } k_cases[] = {
        { 0u, 0, 0u, 0u },     { 1u, 1, 0u, 1u },     { 89u, 1, 0u, 89u },
        { 90u, 1, 1u, 1u },    { 276u, 1, 2u, 98u },  { 277u, 1, 3u, 1u },
        { 324u, 1, 3u, 48u },  { 325u, 0, 0u, 0u },   { 326u, 1, 3u, 49u },
        { 336u, 0, 0u, 0u },   { 337u, 1, 3u, 59u },  { 354u, 0, 0u, 0u },
        { 355u, 1, 3u, 76u },  { 403u, 1, 3u, 124u }, { 404u, 1, 4u, 1u },
        { 520u, 1, 4u, 117u }, { 521u, 1, 5u, 1u },   { 637u, 1, 5u, 117u },
        { 638u, 0, 0u, 0u },   { 0xffffffffu, 0, 0u, 0u },
    };
    static const uint32_t k_sizes[6] = { 89u, 89u, 98u, 124u, 117u, 117u };
    uint32_t seen[6][128];
    uint32_t count[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
    unsigned i;
    uint32_t id;
    char what[96];

    for (i = 0u; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        uint32_t set = 99u;
        uint32_t trophy = 99u;
        int mapped = isaac_vita_trophies_map(k_cases[i].id, &set, &trophy);

        snprintf(what, sizeof what, "map(%u)", (unsigned)k_cases[i].id);
        check_u((unsigned long)mapped, (unsigned long)k_cases[i].mapped, what);
        if (k_cases[i].mapped) {
            check_u(set, k_cases[i].set, what);
            check_u(trophy, k_cases[i].trophy, what);
        } else {
            check(set == 99u && trophy == 99u, "unmapped id leaves outputs");
        }
    }
    memset(seen, 0, sizeof seen);
    for (id = 1u; id <= 637u; ++id) {
        uint32_t set;
        uint32_t trophy;

        if (!isaac_vita_trophies_map(id, &set, &trophy))
            continue;
        if (set >= 6u || trophy == 0u || trophy >= 128u) {
            check(0, "map produced an impossible pair");
            continue;
        }
        ++seen[set][trophy];
        ++count[set];
    }
    for (i = 0u; i < 6u; ++i) {
        uint32_t t;
        int contiguous = 1;

        snprintf(what, sizeof what, "set %u size", i);
        check_u(count[i], k_sizes[i], what);
        for (t = 1u; t <= k_sizes[i]; ++t)
            contiguous &= seen[i][t] == 1u;
        check(contiguous && seen[i][0] == 0u, "set trophies 1..n exactly once");
    }
}

/* Written in binary mode so the dump is LF-only on every host. */
static int dump_map(const char *path)
{
    FILE *out = fopen(path, "wb");
    uint32_t id;

    if (out == NULL)
        return 1;
    for (id = 1u; id <= 637u; ++id) {
        uint32_t set;
        uint32_t trophy;

        if (isaac_vita_trophies_map(id, &set, &trophy))
            fprintf(out, "%u %u %03u\n", (unsigned)id, (unsigned)set,
                    (unsigned)trophy);
        else
            fprintf(out, "%u - -\n", (unsigned)id);
    }
    return fclose(out) != 0;
}

/* ---- cases: boot / setup / failure rows ------------------------------------- */

static void expect_comm(unsigned n)
{
    static const char k_base[9] = { 'I', 'S', 'A', 'C', '0', '0', '0', '0',
                                    '0' };

    check(memcmp(s_ctx_comm[n], k_base, 9u) == 0 && s_ctx_comm[n][9] == 0u &&
          s_ctx_comm[n][10] == n && s_ctx_comm[n][11] == 0u, "comm id bytes");
    check(s_ctx_sign_ok[n], "comm signature bytes");
}

static void case_boot_ok(void)
{
    char line[LOG_WIDTH];
    unsigned n;

    reset_all();
    observe(31u);
    check_u(s_guest_reads, 0u, "observer before boot reads nothing");
    check_u(s_log_count, 0u, "observer before boot logs nothing");
    isaac_vita_trophies_boot();
    check_u(s_module_id, 0x25u, "SCE_SYSMODULE_NP_TROPHY");
    expect_line("KAGE VITA TROPHY: boot module=0x00000000 init=0x00000000 "
                "title=ISAACR001 sets=6");
    /* Stage A creates no context (device run 2026-10-09: two at most, each
     * needs its own dialog, which only Stage B can show). */
    check_u(s_ctx_calls, 0u, "Stage A creates no context");
    expect_prefix("KAGE VITA TROPHY: ctx ", 0u);
    check(s_sema_args_ok && s_sema_creates == 1u, "semaphore name/initial/max");
    check(s_thread_args_ok && s_thread_calls == 1u && s_thread_starts == 1u &&
          s_thread_priority[0] == 191, "thread name/prio/stack/affinity");
    expect_line("KAGE VITA TROPHY: worker rc=0x00000000 prio=191 stack=65536 "
                "affinity=0x00040000");
    expect_line("KAGE VITA TROPHY: pending load status=absent ids=0 "
                "known=0x00");
    expect_line("KAGE VITA TROPHY: pair a=0 b=1 rule=fallback saves=0 "
                "known=0x00 locked=89,89,98,124,117,117");
    /* Events before Stage B queue; the worker does nothing yet. */
    observe(31u);
    expect_line("KAGE VITA TROPHY: event id=31 ra=0x0032b4cd");
    check_u(s_sema_signals, 1u, "event signals the worker");
    check_u((unsigned long)isaac_vita_trophies_worker_step(), 0u,
            "worker before setup");
    check_u(s_unlock_calls + s_state_calls + s_handle_creates, 0u,
            "no NP call before setup");
    isaac_vita_trophies_setup(fake_present);
    for (n = 0u; n < 2u; ++n) {
        snprintf(line, sizeof line, "KAGE VITA TROPHY: ctx set=%u "
                 "comm=ISAC00000_%02u rc=0x00000000 ctx=%u", n, n, 200u + n);
        expect_line(line);
        expect_comm(n);
        snprintf(line, sizeof line, "KAGE VITA TROPHY: setup set=%u status=ok "
                 "init=0x00000000 result=0 frames=3 us=", n);
        expect_prefix(line, 1u);
        check_u(s_dialog_presents[n], 3u, "presents per dialog");
        check_u(s_dialog_terms[n], 1u, "Term per dialog");
        check_u(s_dialog_aborts[n], 0u, "no Abort");
    }
    check(s_ctx_calls == 2u && s_dialog_inits[2] + s_dialog_inits[3] +
          s_dialog_inits[4] + s_dialog_inits[5] == 0u,
          "only the pair is opened when nothing else is queued");
    check(s_ctx_options_ok, "CreateContext options 0");
    check_u((unsigned long)s_dialog_param_bad, 0u,
            "dialog param: sdk, options, magic, static address");
    expect_prefix("KAGE VITA TROPHY: setup done live=2/2 dialogs=2 "
                  "timeouts=0 us=", 1u);
    expect_prefix("KAGE VITA TROPHY: destroy", 0u);
    check_u(s_ctx_live, 2u, "the pair stays live");
    check_u(s_sema_signals, 2u, "setup signals the worker");
    isaac_vita_trophies_setup(fake_present);
    check_u(s_dialog_inits[0], 1u, "setup is idempotent");
    check((unsigned long)isaac_vita_trophies_worker_step() == 1u,
          "worker resolves the queued event");
    for (n = 0u; n < 2u; ++n) {
        snprintf(line, sizeof line, "KAGE VITA TROPHY: state set=%u "
                 "rc=0x00000000 count=0 bits=00000000000000000000000000000000",
                 n);
        expect_line(line);
    }
    expect_prefix("KAGE VITA TROPHY: state set=", 2u);
    expect_prefix("KAGE VITA TROPHY: unlock src=live id=31 set=0 trophy=031 "
                  "rc=0x00000000 plat=-1 us=", 1u);
    check(pacing_ok() && s_delays == 1u && s_delay_bad == 0u, "pacing");
    /* the state of both sets is remembered for the next pair choice */
    expect_line("KAGE VITA TROPHY: pending save ids=0 status=ok rc=0x00000000");
    check(isaac_vita_trophies_oracle_store_known() == 0x03u &&
          isaac_vita_trophies_oracle_store_state_word(0u, 0u) == 1u << 31,
          "known state: sets 0 and 1, trophy 031 of set 0");
    check_u(s_foreign_reads, 0u, "no foreign guest read");
    check_pool_invariants("boot ok");
    isaac_vita_trophies_boot();
    check_u(s_module_calls, 1u, "boot is idempotent");
}

static void case_failure_rows(void)
{
    /* module */
    reset_all();
    s_module_rc = (int)0x80010001u;
    put_text(F_IMPORT_REQ, "1");
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: boot module=0x80010001 init=- "
                "title=ISAACR001 sets=6");
    expect_line("KAGE VITA TROPHY: disabled stage=module rc=0x80010001");
    check(s_init_calls == 0u && s_ctx_calls == 0u && s_thread_calls == 0u &&
          s_sema_creates == 0u, "module failure stops the boot");
    check(s_dialog_inits[0] == 0u, "Stage B no-op when disabled");
    expect_prefix("KAGE VITA TROPHY: pending", 0u);
    expect_prefix("KAGE VITA TROPHY: pair", 0u);
    expect_line("KAGE VITA TROPHY IMPORT: slot=1 status=unavailable saved=0 "
                "mapped=0 unlocked=0 already=0 failed=0 noset=0 request=kept");
    check_str(file_text(F_IMPORT_RES),
              "TROPHY IMPORT: slot=1 status=unavailable saved=0 mapped=0 "
              "unlocked=0 already=0 failed=0 noset=0 request=kept\n",
              "unavailable .res");
    check(s_files[F_IMPORT_REQ].exists, "unavailable keeps the request");
    check_u(s_files[F_SAVE1].opens, 0u, "unavailable never reads the save");
    check_u(s_files[F_STORE].opens, 0u, "unavailable never reads the store");
    observe(31u);
    check_u(s_guest_reads, 0u, "observer inert when disabled");
    check_u((unsigned long)isaac_vita_trophies_worker_step(), 0u,
            "worker idle when disabled");

    /* init */
    reset_all();
    s_init_rc = (int)0x80551601u;
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: boot module=0x00000000 init=0x80551601 "
                "title=ISAACR001 sets=6");
    expect_line("KAGE VITA TROPHY: disabled stage=init rc=0x80551601");
    check(s_ctx_calls == 0u && s_dialog_inits[0] == 0u, "init failure");
    observe(31u);
    check_u(s_guest_reads, 0u, "observer inert after init failure");

    /* a pair context refused: the other one lives, its ids go pending */
    reset_all();
    s_ctx_rc[1] = ORACLE_TRP_MISSING;
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: ctx set=1 comm=ISAC00000_01 rc=0x80551619 "
                "ctx=-1");
    check_u(s_dialog_inits[1], 0u, "no dialog without a context");
    expect_prefix("KAGE VITA TROPHY: setup done live=1/2 dialogs=1 "
                  "timeouts=0 us=", 1u);
    expect_prefix("KAGE VITA TROPHY: disabled", 0u);
    observe(90u);
    observe(1u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: pending id=90 set=1 trophy=001");
    check(unlock_called(1, 1) == 0 && unlock_called(0, 1) == 1,
          "dead set queued, live set unlocked");
    check(isaac_vita_trophies_oracle_store_has(90u) &&
          !isaac_vita_trophies_oracle_store_has(1u), "store: 90 only");
    expect_line("KAGE VITA TROPHY: pending save ids=1 status=ok rc=0x00000000");
    expect_prefix("KAGE VITA TROPHY: state set=1 ", 0u);
    check_pool_invariants("pair context refused");

    /* every context refused: still accepting, everything pending */
    reset_all();
    s_ctx_rc[0] = ORACLE_TRP_MISSING;
    s_ctx_rc[1] = ORACLE_TRP_MISSING;
    boot_and_setup();
    expect_prefix("KAGE VITA TROPHY: ctx set=", 2u);
    expect_prefix("KAGE VITA TROPHY: setup done live=0/2 dialogs=0 "
                  "timeouts=0 us=", 1u);
    expect_prefix("KAGE VITA TROPHY: disabled", 0u);
    observe(31u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: pending id=31 set=0 trophy=031");
    check_u(s_unlock_calls, 0u, "no context: no unlock");

    /* semaphore */
    reset_all();
    s_sema_rc = (int)0x80020001u;
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: disabled stage=thread rc=0x80020001");
    check(s_thread_calls == 0u && s_dialog_inits[0] == 0u, "semaphore failure");

    /* thread refused at 191 and at the fallback */
    reset_all();
    s_thread_rc[0] = (int)0x80020003u;
    s_thread_rc[1] = (int)0x80020003u;
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: worker rc=0x80020003 prio=268435712 "
                "stack=65536 affinity=0x00040000");
    expect_line("KAGE VITA TROPHY: disabled stage=thread rc=0x80020003");
    check(s_thread_starts == 0u && s_dialog_inits[0] == 0u, "thread failure");
    observe(31u);
    check_u(s_guest_reads, 0u, "observer inert after thread failure");

    /* fallback priority accepted */
    reset_all();
    s_thread_rc[0] = (int)0x80020003u;
    boot_and_setup();
    check(s_thread_priority[0] == 191 && s_thread_priority[1] == 0x10000100,
          "fallback priority order");
    expect_line("KAGE VITA TROPHY: worker rc=0x00000000 prio=268435712 "
                "stack=65536 affinity=0x00040000");
    expect_prefix("KAGE VITA TROPHY: setup done live=2/2", 1u);

    /* thread start */
    reset_all();
    s_thread_start_rc = (int)0x80020004u;
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: worker rc=0x80020004 prio=191 stack=65536 "
                "affinity=0x00040000");
    expect_line("KAGE VITA TROPHY: disabled stage=thread rc=0x80020004");

    /* setup: init failure and result not OK -> context destroyed at once */
    reset_all();
    s_dialog[0].init_rc = (int)0x80020403u;
    s_dialog[1].result = 1;
    boot_and_setup();
    expect_prefix("KAGE VITA TROPHY: setup set=0 status=init-failed "
                  "init=0x80020403 result=0 frames=0 us=", 1u);
    check(s_dialog_results[0] == 0u && s_dialog_terms[0] == 0u,
          "no GetResult/Term after a failed init");
    expect_prefix("KAGE VITA TROPHY: setup set=1 status=result-failed "
                  "init=0x00000000 result=1 frames=3 us=", 1u);
    expect_line("KAGE VITA TROPHY: destroy set=0 rc=0x00000000 reason=setup");
    expect_line("KAGE VITA TROPHY: destroy set=1 rc=0x00000000 reason=setup");
    check(s_ctx_live == 0u && s_ctx_destroys[0] == 1u && s_ctx_destroys[1] == 1u,
          "failed dialogs free their pool slots");
    expect_prefix("KAGE VITA TROPHY: setup done live=0/2 dialogs=2 "
                  "timeouts=0 us=", 1u);
    observe(2u);
    check(s_guest_reads != 0u, "observer still accepting with no live set");
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: pending id=2 set=0 trophy=002");
    check_pool_invariants("dialog failures");

    /* GetResult error */
    reset_all();
    s_dialog[0].result_rc = (int)0x80020404u;
    boot_and_setup();
    {
        char line[LOG_WIDTH];

        snprintf(line, sizeof line, "KAGE VITA TROPHY: setup set=0 "
                 "status=result-failed init=0x00000000 result=%d frames=3 us=",
                 (int)0x80020404u);
        expect_prefix(line, 1u);
    }
    expect_prefix("KAGE VITA TROPHY: setup set=1 status=ok", 1u);
    expect_prefix("KAGE VITA TROPHY: setup done live=1/2 dialogs=2", 1u);

    /* timeout: 600 frames, Abort, at most 300 more; then no further dialog
     * this boot (the next set is skipped) */
    reset_all();
    s_dialog[0].running_frames = -1;
    s_dialog[0].abort_frames = 5;
    boot_and_setup();
    expect_prefix("KAGE VITA TROPHY: setup set=0 status=timeout "
                  "init=0x00000000 result=0 frames=605 us=", 1u);
    check(s_dialog_aborts[0] == 1u && s_dialog_terms[0] == 1u,
          "Abort then Term after a timeout");
    expect_line("KAGE VITA TROPHY: destroy set=0 rc=0x00000000 reason=setup");
    expect_line("KAGE VITA TROPHY: setup set=1 status=skipped init=0x00000000 "
                "result=0 frames=0 us=0");
    check(s_dialog_inits[1] == 0u && s_ctx_creates[1] == 0u,
          "after a timeout no further context or dialog");
    expect_prefix("KAGE VITA TROPHY: setup done live=0/2 dialogs=1 "
                  "timeouts=1 us=", 1u);
    reset_all();
    s_dialog[1].running_frames = -1;
    s_dialog[1].abort_frames = -1;
    boot_and_setup();
    expect_prefix("KAGE VITA TROPHY: setup set=1 status=timeout "
                  "init=0x00000000 result=0 frames=900 us=", 1u);
    expect_prefix("KAGE VITA TROPHY: setup set=0 status=ok", 1u);
    expect_prefix("KAGE VITA TROPHY: setup done live=1/2 dialogs=2 "
                  "timeouts=1 us=", 1u);
    check_pool_invariants("timeouts");

    /* every set dead after Stage B: the import resolves as noset */
    reset_all();
    s_dialog[0].init_rc = (int)0x80020403u;
    s_dialog[1].init_rc = (int)0x80020403u;
    s_dialog[5].init_rc = (int)0x80020403u;
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1\n");
    boot_and_setup();
    expect_prefix("KAGE VITA TROPHY: setup set=", 3u);
    expect_prefix("KAGE VITA TROPHY: setup done live=0/2 dialogs=3 "
                  "timeouts=0 us=", 1u);
    expect_prefix("KAGE VITA TROPHY: disabled", 0u);
    (void)isaac_vita_trophies_worker_step();
    check_u(s_unlock_calls + s_state_calls, 0u, "no NP call on dead sets");
    expect_line("KAGE VITA TROPHY IMPORT: slot=1 status=partial saved=7 "
                "mapped=4 unlocked=0 already=0 failed=0 noset=4 request=kept");
    check(s_files[F_IMPORT_REQ].exists, "partial keeps the request");
    expect_prefix("KAGE VITA TROPHY: skip src=import", 0u);
    check_pool_invariants("every set dead");
}

/* ---- cases: observer ------------------------------------------------------- */

static uint32_t s_nested_id;
static int s_nested_set;
static int s_nested_trophy;

static void nested_hook(int set, int trophy)
{
    if (set == s_nested_set && trophy == s_nested_trophy && s_nested_id) {
        uint32_t id = s_nested_id;

        s_nested_id = 0u;
        observe(id);
    }
}

static void case_observer(void)
{
    char line[LOG_WIDTH];
    unsigned n;
    unsigned signals;

    reset_all();
    boot_and_setup();
    s_log_count = 0u;
    /* mgr 0 */
    s_mgr_value = 0u;
    s_guest_reads = 0u;
    observe(5u);
    expect_line("KAGE VITA TROPHY: pgd-mismatch pgd=0x99000014 mgr=0x00000000 "
                "id=5 ra=0x0032b4cd n=1");
    check_u(s_guest_reads, 1u, "mgr 0: only the manager pointer read");
    s_mgr_value = ORACLE_MGR_VALUE;
    /* foreign this: first 4 logged, then counted */
    for (n = 0u; n < 6u; ++n)
        isaac_vita_trophies_try_unlock_observe(0x12345678u, 7u, 0x00484a90u);
    for (n = 2u; n <= 4u; ++n) {
        snprintf(line, sizeof line, "KAGE VITA TROPHY: pgd-mismatch "
                 "pgd=0x12345678 mgr=0x99000000 id=7 ra=0x00484a90 n=%u", n);
        expect_line(line);
    }
    expect_prefix("KAGE VITA TROPHY: pgd-mismatch", 4u);
    check_u(s_foreign_reads, 0u, "no byte read through a foreign this");
    /* id 0 and id 638 */
    s_log_count = 0u;
    observe(0u);
    check_u(s_log_count, 0u, "id 0 silent");
    for (n = 0u; n < 6u; ++n)
        observe(638u);
    check_u(log_count("KAGE VITA TROPHY: id-range id=638 ra=0x0032b4cd"), 4u,
            "id-range first 4");
    /* already unlocked in the game */
    s_log_count = 0u;
    s_pgd[ORACLE_UNLOCKED + 5u] = 1u;
    signals = s_sema_signals;
    observe(5u);
    check(s_log_count == 0u && s_sema_signals == signals, "already: silent");
    /* ReadOnly: logged once per id, nothing queued */
    s_pgd[1] = 1u;
    observe(31u);
    observe(31u);
    observe(32u);
    expect_line("KAGE VITA TROPHY: readonly id=31 ra=0x0032b4cd");
    expect_line("KAGE VITA TROPHY: readonly id=32 ra=0x0032b4cd");
    check_u(s_sema_signals, signals, "ReadOnly queues nothing");
    /* ReadOnly but already: silent (already is checked first) */
    s_log_count = 0u;
    observe(5u);
    check_u(s_log_count, 0u, "ReadOnly + already: silent");
    s_pgd[1] = 0u;
    /* fresh + duplicate before the drain: one unlock */
    observe(31u);
    observe(31u);
    check_u(log_count("KAGE VITA TROPHY: event id=31 ra=0x0032b4cd"), 2u,
            "two events");
    check_u(s_sema_signals, signals + 2u, "each event signals");
    check((unsigned long)isaac_vita_trophies_worker_step() == 1u &&
          unlock_called(0, 31) == 1, "duplicate coalesced");
    /* nested event during the live drain: queued, next pass */
    s_nested_id = 33u;
    s_nested_set = 0;
    s_nested_trophy = 32;
    s_unlock_hook = nested_hook;
    observe(32u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: event id=33 ra=0x0032b4cd");
    check(unlock_called(0, 32) == 1 && unlock_called(0, 33) == 0,
          "nested event waits for the next pass");
    (void)isaac_vita_trophies_worker_step();
    check_u((unsigned long)unlock_called(0, 33), 1u, "nested event resolved");
    s_unlock_hook = NULL;
    /* excluded id from the game (Daily) */
    observe(325u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: skip src=live id=325 reason=excluded");
    check_u(s_foreign_reads, 0u, "no foreign guest read");
}

/* ---- cases: worker --------------------------------------------------------- */

static void case_worker(void)
{
    unsigned calls;

    /* no save: the pair is sets 0 and 1 */
    reset_all();
    s_state_flags[0][0] = 1u << 12;      /* trophy 012 of set 0 */
    s_state_count[0] = 1;
    s_state_rc[1] = ORACLE_NP_UNKNOWN;
    s_unlock_rc[0][7] = ORACLE_ALREADY;
    s_unlock_rc[0][9] = ORACLE_NP_UNKNOWN;
    s_unlock_plat[1][5] = 0;
    boot_and_setup();
    observe(12u);
    observe(7u);
    observe(9u);
    observe(94u);
    observe(179u);
    observe(404u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: state set=0 rc=0x00000000 count=1 "
                "bits=00001000000000000000000000000000");
    expect_line("KAGE VITA TROPHY: state set=1 rc=0x80551600 count=0 "
                "bits=00000000000000000000000000000000");
    expect_line("KAGE VITA TROPHY: skip src=live id=12 reason=already");
    check_u((unsigned long)unlock_called(0, 12), 0u, "cached bit: no call");
    expect_prefix("KAGE VITA TROPHY: unlock src=live id=7 set=0 trophy=007 "
                  "rc=0x8055160f plat=-1 us=", 1u);
    expect_prefix("KAGE VITA TROPHY: unlock src=live id=9 set=0 trophy=009 "
                  "rc=0x80551600 plat=-1 us=", 1u);
    expect_line("KAGE VITA TROPHY: pending id=9 set=0 trophy=009");
    expect_prefix("KAGE VITA TROPHY: unlock src=live id=94 set=1 trophy=005 "
                  "rc=0x00000000 plat=0 us=", 1u);
    expect_line("KAGE VITA TROPHY: platinum set=1 trophy=0");
    /* sets 2 and 4 are not live: queued for the next start, no call */
    expect_line("KAGE VITA TROPHY: pending id=179 set=2 trophy=001");
    expect_line("KAGE VITA TROPHY: pending id=404 set=4 trophy=001");
    check(unlock_called(2, 1) + unlock_called(4, 1) == 0,
          "no unlock for a set that is not live");
    expect_line("KAGE VITA TROPHY: pending save ids=3 status=ok rc=0x00000000");
    check(isaac_vita_trophies_oracle_store_has(9u) &&
          isaac_vita_trophies_oracle_store_has(179u) &&
          isaac_vita_trophies_oracle_store_has(404u) &&
          !isaac_vita_trophies_oracle_store_has(7u) &&
          !isaac_vita_trophies_oracle_store_has(94u),
          "store: the failure and the two non-live ids");
    calls = s_unlock_calls;
    check_u(calls, 3u, "three unlock calls");
    /* a live set refusing with SETUP_REQUIRED is dead from then on */
    s_unlock_rc[1][1] = ORACLE_SETUP_REQ;
    observe(90u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: set-dead set=1 rc=0x80551612");
    expect_line("KAGE VITA TROPHY: pending id=90 set=1 trophy=001");
    observe(91u);
    calls = s_unlock_calls;
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: pending id=91 set=1 trophy=002");
    check_u(s_unlock_calls, calls, "dead set: no further call");
    /* the failure retries when the event recurs; success clears the bit */
    s_unlock_rc[0][9] = 0;
    observe(9u);
    observe(7u);
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: skip src=live id=7 reason=already");
    expect_prefix("KAGE VITA TROPHY: unlock src=live id=9 set=0 trophy=009 "
                  "rc=0x00000000", 1u);
    check(!isaac_vita_trophies_oracle_store_has(9u), "success clears the bit");
    check_u(s_unlock_calls, calls + 1u, "only the retry calls UnlockTrophy");
    check(pacing_ok() && s_delays == s_unlock_calls && s_delay_bad == 0u,
          "pacing delay after each UnlockTrophy");
    check(s_unlock_bad == 0, "trophy 0 never passed");
    /* CreateHandle failing: no UnlockTrophy, counted as failed, pending */
    s_handle_rc = (int)0x80551603u;
    observe(10u);
    calls = s_unlock_calls;
    (void)isaac_vita_trophies_worker_step();
    expect_prefix("KAGE VITA TROPHY: unlock src=live id=10 set=0 trophy=010 "
                  "rc=0x80551603 plat=-1 us=", 1u);
    expect_line("KAGE VITA TROPHY: pending id=10 set=0 trophy=010");
    check(s_unlock_calls == calls && pacing_ok(),
          "no UnlockTrophy and no delay without a handle");
    check_pool_invariants("worker");
}

/* ---- cases: import ----------------------------------------------------------- */

static void import_expect(const char *line, int request_exists)
{
    char res[LOG_WIDTH];

    expect_line(line);
    snprintf(res, sizeof res, "%s\n", line + strlen("KAGE VITA "));
    check_str(file_text(F_IMPORT_RES), res, ".res text == log line");
    check(s_files[F_IMPORT_REQ].exists == request_exists,
          request_exists ? "request kept" : "request deleted");
    check_u(s_open_descriptors, 0u, "every descriptor closed");
}

static void import_case(const char *request, int expect_final,
                        const char *line)
{
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    (void)request;
    import_expect(line, !expect_final);
}

static void case_import(void)
{
    uint8_t bytes[0x27e];
    uint32_t size;
    unsigned i;
    unsigned n;
    static const char *const k_bad_requests[] = {
        "0", "4", "12", "a", "", "1                ", "1 2", ("\x01" "1"),
    };

    ref_table_build();
    /* good save, "1": signed bytes, excluded ids, counts, .res, delete */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    observe(50u);                       /* before boot: ignored */
    isaac_vita_trophies_boot();
    isaac_vita_trophies_setup(fake_present);
    observe(50u);                       /* live, queued before the drain */
    s_nested_id = 60u;                  /* live event during the import */
    s_nested_set = 0;
    s_nested_trophy = 1;
    s_unlock_hook = nested_hook;
    (void)isaac_vita_trophies_worker_step();
    s_unlock_hook = NULL;
    /* 637 (set 5, not in the pair 0/1) was delivered during Stage B; then
     * the pair: live first, live re-drained between import ids */
    check(s_unlock_calls == 6u && s_unlock_set[0] == 5 &&
          s_unlock_trophy[0] == 117 && s_unlock_set[1] == 0 &&
          s_unlock_trophy[1] == 50 && s_unlock_trophy[2] == 1 &&
          s_unlock_trophy[3] == 60 && s_unlock_trophy[4] == 2 &&
          s_unlock_trophy[5] == 31,
          "delivery set at boot, then live first, live re-drained between "
          "import ids, import in order");
    expect_line("KAGE VITA TROPHY: destroy set=5 rc=0x00000000 "
                "reason=delivered");
    expect_prefix("KAGE VITA TROPHY: unlock src=import id=1 set=0 trophy=001 "
                  "rc=0x00000000", 1u);
    import_expect("KAGE VITA TROPHY IMPORT: slot=1 status=done saved=7 "
                  "mapped=4 unlocked=4 already=0 failed=0 noset=0 "
                  "request=deleted", 0);
    expect_prefix("KAGE VITA TROPHY: skip src=import", 0u);
    /* idempotent rerun: same request, the device remembers */
    persist_unlocks();
    put_text(F_IMPORT_REQ, "1");
    reboot_runtime();
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    check_u(s_unlock_calls, 0u, "rerun calls nothing");
    import_expect("KAGE VITA TROPHY IMPORT: slot=1 status=done saved=7 "
                  "mapped=4 unlocked=0 already=4 failed=0 noset=0 "
                  "request=deleted", 0);
    expect_prefix("KAGE VITA TROPHY: skip src=import", 0u);
    /* the import line is written once; later passes add nothing */
    (void)isaac_vita_trophies_worker_step();
    expect_prefix("KAGE VITA TROPHY IMPORT:", 1u);

    /* partial: one failure, one dead set -> request kept */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    s_unlock_rc[0][2] = ORACLE_NP_UNKNOWN;
    s_ctx_rc[5] = ORACLE_TRP_MISSING;
    import_case("1", 0, "KAGE VITA TROPHY IMPORT: slot=1 status=partial "
                "saved=7 mapped=4 unlocked=2 already=0 failed=1 noset=1 "
                "request=kept");

    /* spellings: "2\n" and "  3 " */
    reset_all();
    s_files[F_SAVE2].size = standard_save(s_files[F_SAVE2].data);
    s_files[F_SAVE2].exists = 1;
    put_text(F_IMPORT_REQ, "2\n");
    import_case("2\n", 1, "KAGE VITA TROPHY IMPORT: slot=2 status=done "
                "saved=7 mapped=4 unlocked=4 already=0 failed=0 noset=0 "
                "request=deleted");
    /* Stage A reads slots 1..3 once each for the gameplay pair. */
    check(s_files[F_SAVE1].opens == 1u && s_files[F_SAVE2].opens == 2u,
          "slot 2: the import reads only its own save");
    reset_all();
    s_files[F_SAVE3].size = standard_save(s_files[F_SAVE3].data);
    s_files[F_SAVE3].exists = 1;
    put_text(F_IMPORT_REQ, "  3 \r\n\t");
    import_case("  3 ", 1, "KAGE VITA TROPHY IMPORT: slot=3 status=done "
                "saved=7 mapped=4 unlocked=4 already=0 failed=0 noset=0 "
                "request=deleted");

    /* bad requests: final, slot 0, the save is never read */
    for (i = 0u; i < sizeof k_bad_requests / sizeof k_bad_requests[0]; ++i) {
        reset_all();
        s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
        s_files[F_SAVE1].exists = 1;
        put_text(F_IMPORT_REQ, k_bad_requests[i]);
        import_case(k_bad_requests[i], 1,
                    "KAGE VITA TROPHY IMPORT: slot=0 status=bad-request "
                    "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                    "request=deleted");
        check_u(s_files[F_SAVE1].opens, 1u,
                "bad request: the save is read by the pair choice only");
    }
    check_u((unsigned long)strlen(k_bad_requests[5]), 17u, "17-byte request");
    /* request read error: bad-request */
    reset_all();
    put_text(F_IMPORT_REQ, "1");
    s_files[F_IMPORT_REQ].read_rc = ORACLE_EIO;
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=0 status=bad-request "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* no request: nothing written */
    reset_all();
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    check(!s_files[F_IMPORT_RES].exists && log_count_prefix(
          "KAGE VITA TROPHY IMPORT:") == 0u, "no request, no result");

    /* missing save */
    reset_all();
    put_text(F_IMPORT_REQ, "2");
    import_case("2", 1, "KAGE VITA TROPHY IMPORT: slot=2 status=no-save "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* bad magic */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].data[13] = '8';
    reseal(s_files[F_SAVE1].data, s_files[F_SAVE1].size);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=bad-magic "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* bad checksum */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].data[0x20 + 100] ^= 0x01u;
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=bad-checksum "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* chunk id 2 */
    reset_all();
    memset(bytes, 0, sizeof bytes);
    bytes[1] = 1u;
    s_files[F_SAVE1].size = build_save(s_files[F_SAVE1].data, bytes, 2u,
                                       0x27eu);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=bad-chunk "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* truncated: valid CRC, too short for 0x20 + count + 4 */
    reset_all();
    size = standard_save(s_files[F_SAVE1].data);
    (void)size;
    s_files[F_SAVE1].size = 0x100u;
    reseal(s_files[F_SAVE1].data, 0x100u);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=bad-chunk "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* tiny files */
    reset_all();
    put_file(F_SAVE1, "ISAACNGSAVE09R", 14u);
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=bad-magic "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    reset_all();
    put_file(F_SAVE1, "ISAACNGSAVE09R  \x01\x02", 18u);
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=bad-checksum "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* too large (size only; never read) */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].exists = 1;
    s_files[F_SAVE1].size_override = (int64_t)(1024u * 1024u + 1u);
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=too-large "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* exactly 1 MiB is allowed (and then fails its checksum) */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    memset(s_files[F_SAVE1].data + s_files[F_SAVE1].size, 0,
           1024u * 1024u - s_files[F_SAVE1].size);
    s_files[F_SAVE1].size = 1024u * 1024u;
    reseal(s_files[F_SAVE1].data, s_files[F_SAVE1].size);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=done saved=7 "
                "mapped=4 unlocked=4 already=0 failed=0 noset=0 "
                "request=deleted");
    /* read errors: read rc, size rc, early EOF */
    for (n = 0u; n < 3u; ++n) {
        reset_all();
        s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
        s_files[F_SAVE1].exists = 1;
        if (n == 0u)
            s_files[F_SAVE1].read_rc = ORACLE_EIO;
        else if (n == 1u)
            s_files[F_SAVE1].size_override = (int64_t)ORACLE_EIO;
        else
            s_files[F_SAVE1].eof_at = 0x200;
        put_text(F_IMPORT_REQ, "1");
        import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 "
                    "status=read-error saved=0 mapped=0 unlocked=0 already=0 "
                    "failed=0 noset=0 request=deleted");
    }
    /* open error other than ENOENT: no-save */
    reset_all();
    s_files[F_SAVE1].size = standard_save(s_files[F_SAVE1].data);
    s_files[F_SAVE1].exists = 1;
    s_files[F_SAVE1].open_rc = ORACLE_EIO;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=no-save "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=deleted");
    /* a final error whose remove fails reports request=kept */
    reset_all();
    put_text(F_IMPORT_REQ, "9");
    s_files[F_IMPORT_REQ].remove_rc = ORACLE_EIO;
    import_case("9", 0, "KAGE VITA TROPHY IMPORT: slot=0 status=bad-request "
                "saved=0 mapped=0 unlocked=0 already=0 failed=0 noset=0 "
                "request=kept");
    /* count field: clamped to 0x27e; data_bytes bounds the loop */
    reset_all();
    memset(bytes, 0, sizeof bytes);
    bytes[10] = 1u;
    bytes[600] = 1u;
    size = build_save(s_files[F_SAVE1].data, bytes, 1u, 0xffffffffu);
    put32(s_files[F_SAVE1].data + 0x18, 0x100u);  /* data_bytes 256 */
    reseal(s_files[F_SAVE1].data, size);
    s_files[F_SAVE1].size = size;
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=done saved=1 "
                "mapped=1 unlocked=1 already=0 failed=0 noset=0 "
                "request=deleted");

    /* everything: 637 ids -> 634 unlocks, each trophy of each set once */
    reset_all();
    memset(bytes, 0x01, sizeof bytes);
    s_files[F_SAVE1].size = build_save(s_files[F_SAVE1].data, bytes, 1u,
                                       0x27eu);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    import_case("1", 1, "KAGE VITA TROPHY IMPORT: slot=1 status=done "
                "saved=637 mapped=634 unlocked=634 already=0 failed=0 "
                "noset=0 request=deleted");
    {
        static const int k_sizes[6] = { 89, 89, 98, 124, 117, 117 };
        int set;
        int ok = s_unlock_calls == 634u && s_unlock_bad == 0;

        for (set = 0; set < 6; ++set) {
            int t;

            for (t = 1; t <= k_sizes[set]; ++t)
                ok &= unlock_called(set, t) == 1;
            ok &= unlock_called(set, k_sizes[set] + 1) == 0;
        }
        check(ok, "full import: every trophy once, never 000");
        check(pacing_ok() && s_delays == 634u, "full import pacing");
    }
    check_u(s_log_overflow, 0u, "log capture large enough");
}

/* ---- cases: pending store ------------------------------------------------------ */

/* Own copy of the v1 layout (host_vita_trophies.c TROPHY_STORE_*): magic
 * "ISAACTRP", version 1, id max 637, pending[20], known, state[6][4], the
 * save checksum over [0, 196). */
static void store_image(uint8_t out[ORACLE_STORE_BYTES], const uint32_t *ids,
                        unsigned count, uint32_t known,
                        uint32_t state[6][4])
{
    uint32_t pending[20];
    unsigned i;
    unsigned n;

    memset(pending, 0, sizeof pending);
    for (i = 0u; i < count; ++i)
        pending[ids[i] >> 5] |= 1u << (ids[i] & 31u);
    memcpy(out, "ISAACTRP", 8u);
    put32(out + 8, 1u);
    put32(out + 12, 637u);
    for (i = 0u; i < 20u; ++i)
        put32(out + 16 + 4u * i, pending[i]);
    put32(out + 96, known);
    for (n = 0u; n < 6u; ++n) {
        for (i = 0u; i < 4u; ++i)
            put32(out + 100 + 16u * n + 4u * i, state != NULL ? state[n][i] : 0u);
    }
    put32(out + 196, ref_checksum(out, 196u));
}

static int store_file_is(const uint32_t *ids, unsigned count, uint32_t known,
                         uint32_t state[6][4])
{
    uint8_t expected[ORACLE_STORE_BYTES];

    store_image(expected, ids, count, known, state);
    return s_files[F_STORE].exists &&
           s_files[F_STORE].size == ORACLE_STORE_BYTES &&
           memcmp(s_files[F_STORE].data, expected, ORACLE_STORE_BYTES) == 0;
}

static void store_reboot_load(void)
{
    isaac_vita_trophies_oracle_reset();
    s_log_count = 0u;
    isaac_vita_trophies_oracle_store_load();
}

static void case_store(void)
{
    static const uint32_t k_three[3] = { 1u, 31u, 637u };
    static const uint32_t k_four[4] = { 1u, 2u, 31u, 637u };
    static const uint32_t k_five[5] = { 1u, 2u, 3u, 31u, 637u };
    uint32_t state[6][4];
    uint32_t bits[4] = { 0x00000006u, 0u, 0x80000000u, 0x1u };
    uint8_t image[ORACLE_STORE_BYTES];
    unsigned i;
    char what[96];

    ref_table_build();
    memset(state, 0, sizeof state);
    memcpy(state[2], bits, sizeof bits);

    /* missing file: nothing pending */
    reset_all();
    isaac_vita_trophies_oracle_store_load();
    expect_line("KAGE VITA TROPHY: pending load status=absent ids=0 "
                "known=0x00");
    check_u(isaac_vita_trophies_oracle_store_count(), 0u, "absent: empty");
    /* first publish: temp written, moved into place, nothing left over */
    for (i = 0u; i < 3u; ++i)
        isaac_vita_trophies_oracle_store_mark(k_three[i]);
    isaac_vita_trophies_oracle_store_set_state(2u, bits);
    isaac_vita_trophies_oracle_store_save();
    expect_line("KAGE VITA TROPHY: pending save ids=3 status=ok "
                "rc=0x00000000");
    check(store_file_is(k_three, 3u, 0x04u, state), "store bytes == layout v1");
    check(!s_files[F_STORE_TMP].exists && !s_files[F_STORE_BAK].exists,
          "no temp or aside file after a publish");
    check(s_files[F_STORE].writes == 0u && s_files[F_STORE_TMP].writes == 1u,
          "the store is only ever written through its temp");
    check_u((unsigned long)isaac_vita_trophies_oracle_store_dirty(), 0u,
            "clean after a publish");
    /* round trip */
    store_reboot_load();
    expect_line("KAGE VITA TROPHY: pending load status=ok ids=3 known=0x04");
    check(isaac_vita_trophies_oracle_store_has(1u) &&
          isaac_vita_trophies_oracle_store_has(31u) &&
          isaac_vita_trophies_oracle_store_has(637u) &&
          !isaac_vita_trophies_oracle_store_has(2u), "round trip: bits");
    check(isaac_vita_trophies_oracle_store_state_word(2u, 0u) == 6u &&
          isaac_vita_trophies_oracle_store_state_word(2u, 2u) == 0x80000000u &&
          isaac_vita_trophies_oracle_store_state_word(2u, 3u) == 1u &&
          isaac_vita_trophies_oracle_store_known() == 0x04u,
          "round trip: state");
    /* atomic replace of an existing file */
    isaac_vita_trophies_oracle_store_mark(2u);
    isaac_vita_trophies_oracle_store_save();
    check(store_file_is(k_four, 4u, 0x04u, state) &&
          s_files[F_STORE].renames == 1u && !s_files[F_STORE_BAK].exists &&
          !s_files[F_STORE_TMP].exists, "replace: aside, temp in place");
    /* temp write fails: the old file stays, the store stays dirty */
    s_files[F_STORE_TMP].write_rc = ORACLE_EIO;
    isaac_vita_trophies_oracle_store_mark(3u);
    s_log_count = 0u;
    isaac_vita_trophies_oracle_store_save();
    expect_line("KAGE VITA TROPHY: pending save ids=5 status=write-error "
                "rc=0x80010005");
    check(store_file_is(k_four, 4u, 0x04u, state) &&
          !s_files[F_STORE_TMP].exists &&
          isaac_vita_trophies_oracle_store_dirty(),
          "write error: previous file intact, temp removed, still dirty");
    s_files[F_STORE_TMP].write_rc = 0;
    isaac_vita_trophies_oracle_store_save();
    check(store_file_is(k_five, 5u, 0x04u, state), "retry publishes");
    /* temp -> file rename fails: the aside copy goes back */
    s_files[F_STORE_TMP].rename_rc = ORACLE_EIO;
    isaac_vita_trophies_oracle_store_mark(4u);
    s_log_count = 0u;
    isaac_vita_trophies_oracle_store_save();
    expect_line("KAGE VITA TROPHY: pending save ids=6 status=rename-error "
                "rc=0x80010005");
    check(store_file_is(k_five, 5u, 0x04u, state) &&
          !s_files[F_STORE_BAK].exists && !s_files[F_STORE_TMP].exists,
          "rename error: previous file back in place");
    s_files[F_STORE_TMP].rename_rc = 0;
    /* moving the old file aside fails: untouched */
    s_files[F_STORE].rename_rc = ORACLE_EIO;
    s_log_count = 0u;
    isaac_vita_trophies_oracle_store_save();
    expect_line("KAGE VITA TROPHY: pending save ids=6 status=rename-error "
                "rc=0x80010005");
    check(store_file_is(k_five, 5u, 0x04u, state) &&
          !s_files[F_STORE_TMP].exists, "aside error: file untouched");
    s_files[F_STORE].rename_rc = 0;
    /* interrupted publish: file aside, stale temp -> recovered */
    memcpy(s_files[F_STORE_BAK].data, s_files[F_STORE].data,
           ORACLE_STORE_BYTES);
    s_files[F_STORE_BAK].size = ORACLE_STORE_BYTES;
    s_files[F_STORE_BAK].exists = 1;
    s_files[F_STORE].exists = 0;
    put_text(F_STORE_TMP, "half a temp");
    store_reboot_load();
    expect_line("KAGE VITA TROPHY: pending load status=recovered ids=5 "
                "known=0x04");
    check(store_file_is(k_five, 5u, 0x04u, state) &&
          !s_files[F_STORE_BAK].exists && !s_files[F_STORE_TMP].exists,
          "recovered: aside copy back in place, temp removed");
    /* a stale aside copy next to a good file is dropped */
    put_text(F_STORE_BAK, "stale");
    store_reboot_load();
    expect_line("KAGE VITA TROPHY: pending load status=ok ids=5 known=0x04");
    check(!s_files[F_STORE_BAK].exists, "stale aside copy removed");
    /* read error */
    s_files[F_STORE].read_rc = ORACLE_EIO;
    store_reboot_load();
    expect_line("KAGE VITA TROPHY: pending load status=read-error ids=0 "
                "known=0x00");
    s_files[F_STORE].read_rc = 0;
    /* garbage: every field checked, nothing taken from a bad file */
    for (i = 0u; i < 11u; ++i) {
        uint32_t size = ORACLE_STORE_BYTES;

        store_image(image, k_five, 5u, 0x04u, state);
        switch (i) {
        case 0: size = 199u; break;
        case 1: size = 201u; break;
        case 2: size = 0u; break;
        case 3: image[3] ^= 0x20u; break;                  /* magic */
        case 4: put32(image + 8, 2u); break;               /* version */
        case 5: put32(image + 12, 638u); break;            /* id max */
        case 6: image[196] ^= 1u; break;                   /* checksum */
        case 7: image[16] |= 1u; break;                    /* id 0 */
        case 8: image[16 + 4 * 19 + 3] |= 0x40u; break;    /* id 638 */
        case 9: put32(image + 96, 0x44u); break;           /* known 6 */
        default: image[100] ^= 1u; break;                  /* state, no reseal */
        }
        if (i >= 4u && i <= 9u && i != 6u)
            put32(image + 196, ref_checksum(image, 196u));
        reset_all();
        memset(s_files[F_STORE].data, 0, 256u);
        memcpy(s_files[F_STORE].data, image, ORACLE_STORE_BYTES);
        s_files[F_STORE].size = size;
        s_files[F_STORE].exists = 1;
        isaac_vita_trophies_oracle_store_load();
        snprintf(what, sizeof what, "garbage store #%u rejected", i);
        check_u(log_count("KAGE VITA TROPHY: pending load status=garbage "
                          "ids=0 known=0x00"), 1u, what);
        check_u(isaac_vita_trophies_oracle_store_count() +
                isaac_vita_trophies_oracle_store_known(), 0u, what);
    }
    /* the next publish replaces garbage with a valid file */
    isaac_vita_trophies_oracle_store_mark(31u);
    isaac_vita_trophies_oracle_store_save();
    {
        static const uint32_t k_one[1] = { 31u };

        check(store_file_is(k_one, 1u, 0u, NULL), "garbage replaced");
    }
    check_u(s_unknown_opens, 0u, "store: only known paths opened");
    check_u(s_open_descriptors, 0u, "store: every descriptor closed");
}

/* ---- cases: gameplay pair ----------------------------------------------------- */

static void put_save_ids(int file, uint32_t first, uint32_t last)
{
    uint8_t bytes[0x27e];
    uint32_t id;

    memset(bytes, 0, sizeof bytes);
    for (id = first; id <= last; ++id)
        bytes[id] = 1u;
    s_files[file].size = build_save(s_files[file].data, bytes, 1u, 0x27eu);
    s_files[file].exists = 1;
}

static void pair_boot(void)
{
    isaac_vita_trophies_boot();
}

static void case_pair(void)
{
    uint32_t bits[4];

    ref_table_build();
    /* no save: sets 0 and 1 */
    reset_all();
    pair_boot();
    expect_line("KAGE VITA TROPHY: pair a=0 b=1 rule=fallback saves=0 "
                "known=0x00 locked=89,89,98,124,117,117");
    /* set 0 complete across two saves (union), slot 3 corrupt (ignored) */
    reset_all();
    put_save_ids(F_SAVE1, 1u, 45u);
    put_save_ids(F_SAVE2, 46u, 89u);
    put_save_ids(F_SAVE3, 90u, 100u);
    s_files[F_SAVE3].data[0x30] ^= 1u;
    pair_boot();
    expect_line("KAGE VITA TROPHY: pair a=1 b=2 rule=locked saves=2 "
                "known=0x00 locked=0,89,98,124,117,117");
    check(isaac_vita_trophies_oracle_pair(0u) == 1 &&
          isaac_vita_trophies_oracle_pair(1u) == 2, "pair state");
    /* set 1 lacks one id in the saves, but the last Vita state has it */
    reset_all();
    put_save_ids(F_SAVE1, 1u, 177u);          /* 178 = set 1 trophy 089 */
    memset(bits, 0, sizeof bits);
    bits[89u >> 5] = 1u << (89u & 31u);
    isaac_vita_trophies_oracle_store_set_state(1u, bits);
    isaac_vita_trophies_oracle_store_save();
    isaac_vita_trophies_oracle_reset();
    s_log_count = 0u;
    pair_boot();
    expect_line("KAGE VITA TROPHY: pair a=2 b=3 rule=locked saves=1 "
                "known=0x02 locked=0,0,98,124,117,117");
    /* the same state is not trusted for a set it does not name */
    reset_all();
    put_save_ids(F_SAVE1, 1u, 177u);
    pair_boot();
    expect_line("KAGE VITA TROPHY: pair a=1 b=2 rule=locked saves=1 "
                "known=0x00 locked=0,1,98,124,117,117");
    /* one set left: one live set; none left: no live set */
    reset_all();
    put_save_ids(F_SAVE1, 1u, 520u);
    pair_boot();
    expect_line("KAGE VITA TROPHY: pair a=5 b=-1 rule=locked saves=1 "
                "known=0x00 locked=0,0,0,0,0,117");
    reset_all();
    put_save_ids(F_SAVE2, 1u, 637u);
    pair_boot();
    expect_line("KAGE VITA TROPHY: pair a=-1 b=-1 rule=complete saves=1 "
                "known=0x00 locked=0,0,0,0,0,0");
    /* feature off at Stage A: no pair line, no save read */
    reset_all();
    s_module_rc = (int)0x80010001u;
    put_save_ids(F_SAVE1, 1u, 10u);
    pair_boot();
    expect_prefix("KAGE VITA TROPHY: pair", 0u);
    check_u(s_files[F_SAVE1].opens, 0u, "feature off: saves not read");
    check_u(s_open_descriptors, 0u, "pair: every descriptor closed");
}

/* ---- cases: boot delivery of the pending store ------------------------------- */

static void store_with(const uint32_t *ids, unsigned count)
{
    unsigned i;

    for (i = 0u; i < count; ++i)
        isaac_vita_trophies_oracle_store_mark(ids[i]);
    isaac_vita_trophies_oracle_store_save();
}

static int log_before(const char *first, const char *second)
{
    int a = log_index_prefix(first);
    int b = log_index_prefix(second);

    return a >= 0 && b >= 0 && a < b;
}

static void case_pending_delivery(void)
{
    static const uint32_t k_ids[5] = { 1u, 179u, 300u, 404u, 521u };
    char line[LOG_WIDTH];
    unsigned n;

    /* five pending ids in five sets, no save (pair 0, 1): sets 2..5 are
     * delivered one at a time at boot, then the pair opens */
    reset_all();
    store_with(k_ids, 5u);
    reboot_runtime();
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: pending load status=ok ids=5 known=0x00");
    for (n = 2u; n <= 5u; ++n) {
        snprintf(line, sizeof line, "KAGE VITA TROPHY: deliver set=%u "
                 "status=drained left=0 us=", n);
        expect_prefix(line, 1u);
        snprintf(line, sizeof line, "KAGE VITA TROPHY: destroy set=%u "
                 "rc=0x00000000 reason=delivered", n);
        expect_line(line);
        check(s_ctx_creates[n] == 1u && s_ctx_destroys[n] == 1u,
              "delivery set opened and destroyed once");
    }
    check(log_before("KAGE VITA TROPHY: destroy set=2",
                     "KAGE VITA TROPHY: ctx set=3") &&
          log_before("KAGE VITA TROPHY: destroy set=5",
                     "KAGE VITA TROPHY: ctx set=0") &&
          log_before("KAGE VITA TROPHY: ctx set=0",
                     "KAGE VITA TROPHY: ctx set=1"),
          "order: delivery sets one by one, then the pair");
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=179 set=2 "
                  "trophy=001 rc=0x00000000", 1u);
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=300 set=3 "
                  "trophy=024 rc=0x00000000", 1u);
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=404 set=4 "
                  "trophy=001 rc=0x00000000", 1u);
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=521 set=5 "
                  "trophy=001 rc=0x00000000", 1u);
    expect_prefix("KAGE VITA TROPHY: setup done live=2/2 dialogs=6 "
                  "timeouts=0 us=", 1u);
    check(unlock_called(0, 1) == 0, "pair ids wait for the end of Stage B");
    (void)isaac_vita_trophies_worker_step();
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=1 set=0 trophy=001 "
                  "rc=0x00000000", 1u);
    check_u(isaac_vita_trophies_oracle_store_count(), 0u, "store drained");
    {
        /* the file says: nothing pending, the state of all six sets known */
        isaac_vita_trophies_oracle_reset();
        isaac_vita_trophies_oracle_store_load();
        check(isaac_vita_trophies_oracle_store_count() == 0u &&
              isaac_vita_trophies_oracle_store_known() == 0x3fu,
              "reloaded: empty, six sets known");
    }
    check_pool_invariants("pending delivery");

    /* bits are cleared only on success: a failure, a dead set (SETUP
     * REQUIRED), a dialog timeout (no further dialog) keep theirs */
    reset_all();
    store_with(k_ids, 5u);
    reboot_runtime();
    s_unlock_rc[3][24] = ORACLE_NP_UNKNOWN;        /* id 300 */
    s_unlock_rc[4][1] = ORACLE_SETUP_REQ;          /* id 404 */
    s_dialog[5].running_frames = -1;               /* set 5: timeout */
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=179 set=2 "
                  "trophy=001 rc=0x00000000", 1u);
    expect_prefix("KAGE VITA TROPHY: unlock src=pending id=300 set=3 "
                  "trophy=024 rc=0x80551600", 1u);
    expect_line("KAGE VITA TROPHY: set-dead set=4 rc=0x80551612");
    expect_prefix("KAGE VITA TROPHY: setup set=5 status=timeout", 1u);
    expect_line("KAGE VITA TROPHY: setup set=0 status=skipped init=0x00000000 "
                "result=0 frames=0 us=0");
    expect_line("KAGE VITA TROPHY: setup set=1 status=skipped init=0x00000000 "
                "result=0 frames=0 us=0");
    expect_prefix("KAGE VITA TROPHY: setup done live=0/2 dialogs=4 "
                  "timeouts=1 us=", 1u);
    expect_line("KAGE VITA TROPHY: skip src=pending id=521 reason=noset");
    expect_line("KAGE VITA TROPHY: skip src=pending id=1 reason=noset");
    check(!isaac_vita_trophies_oracle_store_has(179u) &&
          isaac_vita_trophies_oracle_store_has(300u) &&
          isaac_vita_trophies_oracle_store_has(404u) &&
          isaac_vita_trophies_oracle_store_has(521u) &&
          isaac_vita_trophies_oracle_store_has(1u),
          "only the delivered id left the store");
    {
        static const uint32_t k_left[4] = { 1u, 300u, 404u, 521u };
        uint32_t state[6][4];

        memset(state, 0, sizeof state);
        state[2][0] = 1u << 1;                   /* 179 = set 2 trophy 001 */
        /* set 4 answered its state before refusing the unlock */
        check(store_file_is(k_left, 4u, 0x1cu, state),
              "file: four ids left, sets 2, 3 and 4 known");
    }
    check_pool_invariants("pending kept");

    /* a pending id the Vita already reports unlocked (GetTrophyUnlockState)
     * leaves the store without an UnlockTrophy call */
    reset_all();
    store_with(k_ids + 2, 1u);                     /* id 300 */
    reboot_runtime();
    s_state_flags[3][0] = 1u << 24;
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: skip src=pending id=300 reason=already");
    check(unlock_called(3, 24) == 0 &&
          !isaac_vita_trophies_oracle_store_has(300u),
          "already on the Vita: cleared without a call");
    check_pool_invariants("pending already");
}

/* ---- cases: pool, bounded waits ------------------------------------------------ */

static void case_pool(void)
{
    static const uint32_t k_set2[1] = { 180u };
    uint64_t before;

    /* the worker never runs during Stage B: the wait gives up after 5 s
     * without progress, the set is destroyed, the pair still opens */
    reset_all();
    store_with(k_set2, 1u);
    reboot_runtime();
    s_tick_stuck = 1;
    isaac_vita_trophies_boot();
    before = s_clock;
    isaac_vita_trophies_setup(fake_present);
    expect_prefix("KAGE VITA TROPHY: deliver set=2 status=stalled left=1 us=",
                  1u);
    expect_line("KAGE VITA TROPHY: destroy set=2 rc=0x00000000 reason=stalled");
    expect_prefix("KAGE VITA TROPHY: setup done live=2/2 dialogs=3", 1u);
    check(s_clock - before >= 5000000u && s_clock - before < 6000000u,
          "a stalled delivery costs the stall bound, not more");
    s_tick_stuck = 0;
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY: skip src=pending id=180 reason=noset");
    check(isaac_vita_trophies_oracle_store_has(180u), "stalled id stays");
    check_pool_invariants("stalled");

    /* the worker stays inside a call on the set: never destroyed under it;
     * the pool then has room for one pair set only */
    reset_all();
    store_with(k_set2, 1u);
    reboot_runtime();
    s_tick_stuck = 1;
    isaac_vita_trophies_boot();
    isaac_vita_trophies_oracle_worker_inside(2);
    before = s_clock;
    isaac_vita_trophies_setup(fake_present);
    expect_line("KAGE VITA TROPHY: destroy set=2 rc=0x00000000 reason=busy");
    check_u(s_ctx_destroys[2], 0u, "busy set not destroyed");
    expect_line("KAGE VITA TROPHY: setup set=1 status=skipped init=0x00000000 "
                "result=0 frames=0 us=0");
    expect_prefix("KAGE VITA TROPHY: setup done live=1/2 dialogs=2", 1u);
    check(s_clock - before >= 7000000u && s_clock - before < 8000000u,
          "stall bound + retire bound, no more");
    check_u(isaac_vita_trophies_oracle_contexts(), 2u,
            "the busy context is still counted");
    isaac_vita_trophies_oracle_worker_inside(-1);
    s_tick_stuck = 0;
    check_pool_invariants("busy");
}

/* ---- cases: import spanning four sets -------------------------------------------- */

static void case_import_four_sets(void)
{
    uint8_t bytes[0x27e];
    uint32_t id;

    /* slot 1: ids 1..10 (set 0), 200, 201 (set 2), 300 (set 3), 450..452
     * (set 4): the pair is 0, 1; sets 2, 3 and 4 are delivered at boot and
     * set 0 right after; one boot completes the import */
    reset_all();
    memset(bytes, 0, sizeof bytes);
    for (id = 1u; id <= 10u; ++id)
        bytes[id] = 1u;
    bytes[200] = bytes[201] = bytes[300] = 1u;
    bytes[450] = bytes[451] = bytes[452] = 1u;
    s_files[F_SAVE1].size = build_save(s_files[F_SAVE1].data, bytes, 1u,
                                       0x27eu);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    boot_and_setup();
    expect_line("KAGE VITA TROPHY: pair a=0 b=1 rule=locked saves=1 "
                "known=0x00 locked=79,89,96,123,114,117");
    check(unlocked_in(2) == 2u && unlocked_in(3) == 1u && unlocked_in(4) == 3u,
          "delivery sets unlocked during Stage B");
    expect_prefix("KAGE VITA TROPHY IMPORT:", 0u);
    (void)isaac_vita_trophies_worker_step();
    check_u(unlocked_in(0), 10u, "pair set 0 unlocked after Stage B");
    import_expect("KAGE VITA TROPHY IMPORT: slot=1 status=done saved=16 "
                  "mapped=16 unlocked=16 already=0 failed=0 noset=0 "
                  "request=deleted", 0);
    check(s_ctx_destroys[2] == 1u && s_ctx_destroys[3] == 1u &&
          s_ctx_destroys[4] == 1u && live_contexts_of(0) == 1u &&
          live_contexts_of(1) == 1u, "end state: exactly the pair is live");
    check(pacing_ok() && s_delays == 16u, "import pacing");
    check_pool_invariants("import across four sets");
}

/* ---- modelled boot times ----------------------------------------------------- */

/* Stage B with the device's measured costs (device run 2026-10-09): dialog
 * 56 frames for an installed set, 245 for a first install, UnlockTrophy
 * 0.48 s, the 100 ms pacing, 16.7 ms per presented frame, 10 ms polls. */
static void model_line(const char *name, uint64_t us)
{
    printf("host_vita_trophies_oracle: model %s stage_b_ms=%u dialogs=%u "
           "unlocks=%u\n", name, (unsigned)(us / 1000u),
           s_dialog_inits[0] + s_dialog_inits[1] + s_dialog_inits[2] +
           s_dialog_inits[3] + s_dialog_inits[4] + s_dialog_inits[5],
           s_unlock_calls);
}

static uint64_t model_stage_b(int frames)
{
    uint64_t before;
    int n;

    for (n = 0; n < 6; ++n)
        s_dialog[n].running_frames = frames;
    isaac_vita_trophies_boot();
    before = s_clock;
    isaac_vita_trophies_setup(fake_present);
    return s_clock - before;
}

static void case_model(int print)
{
    uint8_t bytes[0x27e];
    uint32_t id;
    uint64_t us;

    reset_all();
    us = model_stage_b(56);
    if (print)
        model_line("normal-installed-pair", us);
    check(us < 2500000u, "normal boot: two installed dialogs < 2.5 s");
    reset_all();
    us = model_stage_b(245);
    if (print)
        model_line("first-install-pair", us);
    check(us < 9000000u, "first install of the pair < 9 s");
    /* an import of 100 ids in sets 2, 3, 4 (+ 30 in pair set 0) */
    reset_all();
    memset(bytes, 0, sizeof bytes);
    for (id = 1u; id <= 30u; ++id)
        bytes[id] = 1u;
    for (id = 180u; id <= 213u; ++id)
        bytes[id] = 1u;
    for (id = 280u; id <= 312u; ++id)
        bytes[id] = 1u;
    for (id = 410u; id <= 442u; ++id)
        bytes[id] = 1u;
    s_files[F_SAVE1].size = build_save(s_files[F_SAVE1].data, bytes, 1u,
                                       0x27eu);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    us = model_stage_b(56);
    if (print)
        model_line("import-100-ids-3-delivery-sets-installed", us);
    reset_all();
    s_files[F_SAVE1].size = build_save(s_files[F_SAVE1].data, bytes, 1u,
                                       0x27eu);
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    us = model_stage_b(245);
    if (print)
        model_line("import-100-ids-3-delivery-sets-first-install", us);
    check_pool_invariants("model");
}

/* ---- cases: test request ------------------------------------------------------ */

static void case_test_request(void)
{
#if defined(ISAAC_VITA_TROPHY_TEST_REQUEST)
    static char s_long[66];
    static const char *const k_bad[] = {
        "0", "638", "1 x", "1,2", "1 2 3 4 5 6 7 8 9", "abc", "1000", s_long,
    };
    unsigned i;

    /* "1" + 64 spaces: valid content, one byte over the 64-byte cap. */
    memset(s_long, ' ', 65u);
    s_long[0] = '1';
    s_long[65] = '\0';
    reset_all();
    s_state_flags[0][1] = 1u << 0;       /* trophy 032 already on the device */
    put_text(F_TEST_REQ, " 1 31\n32 325 1\r\n");
    boot_and_setup();
    expect_line("KAGE VITA TROPHY TEST: queued ids=4");
    check(s_files[F_TEST_REQ].exists, "test request kept until processed");
    (void)isaac_vita_trophies_worker_step();
    expect_prefix("KAGE VITA TROPHY: unlock src=test id=1 set=0 trophy=001 "
                  "rc=0x00000000", 1u);
    expect_prefix("KAGE VITA TROPHY: unlock src=test id=31 set=0 trophy=031 "
                  "rc=0x00000000", 1u);
    expect_line("KAGE VITA TROPHY: skip src=test id=32 reason=already");
    expect_line("KAGE VITA TROPHY: skip src=test id=325 reason=excluded");
    expect_line("KAGE VITA TROPHY TEST: done ids=4 unlocked=2 already=1 "
                "failed=0");
    check(!s_files[F_TEST_REQ].exists, "test request deleted after the run");
    (void)isaac_vita_trophies_worker_step();
    expect_prefix("KAGE VITA TROPHY TEST: done", 1u);
    for (i = 0u; i < sizeof k_bad / sizeof k_bad[0]; ++i) {
        reset_all();
        put_text(F_TEST_REQ, k_bad[i]);
        boot_and_setup();
        (void)isaac_vita_trophies_worker_step();
        expect_line("KAGE VITA TROPHY TEST: queued ids=0");
        expect_prefix("KAGE VITA TROPHY TEST: done", 0u);
        check(!s_files[F_TEST_REQ].exists && s_unlock_calls == 0u,
              "bad test request: nothing queued, file deleted");
    }
    check_u((unsigned long)strlen(k_bad[7]), 65u, "65-byte test request");
    /* a test id outside the pair goes through boot delivery */
    reset_all();
    put_text(F_TEST_REQ, "200");
    boot_and_setup();
    expect_prefix("KAGE VITA TROPHY: unlock src=test id=200 set=2 trophy=022 "
                  "rc=0x00000000", 1u);
    expect_line("KAGE VITA TROPHY: destroy set=2 rc=0x00000000 "
                "reason=delivered");
    (void)isaac_vita_trophies_worker_step();
    expect_line("KAGE VITA TROPHY TEST: done ids=1 unlocked=1 already=0 "
                "failed=0");
    check(!s_files[F_TEST_REQ].exists, "test request deleted");
    check_pool_invariants("test request delivery");
    /* disabled at Stage A: the test request is not opened */
    reset_all();
    s_module_rc = (int)0x80010001u;
    put_text(F_TEST_REQ, "1");
    boot_and_setup();
    check(s_files[F_TEST_REQ].opens == 0u && s_files[F_TEST_REQ].exists,
          "test request untouched when the feature is off");
#else
    reset_all();
    put_text(F_TEST_REQ, "1");
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    check(s_files[F_TEST_REQ].opens == 0u && s_files[F_TEST_REQ].exists,
          "without ISAAC_VITA_TROPHY_TEST_REQUEST the file is never opened");
    expect_prefix("KAGE VITA TROPHY TEST:", 0u);
    check_u(s_unlock_calls, 0u, "no test unlock without the define");
#endif
    check_u(s_unknown_opens, 0u, "only known paths opened");
}

/* ---- --save ----------------------------------------------------------------- */

static int run_save(const char *path)
{
    FILE *file = fopen(path, "rb");
    size_t size;
    int index;
    uint32_t stored = 0u;
    uint32_t crc = 0u;

    if (file == NULL) {
        printf("save %s: cannot open\n", path);
        return 1;
    }
    reset_all();
    size = fread(s_big_save, 1u, BIG_SAVE_BYTES, file);
    fclose(file);
    s_files[F_SAVE1].size = (uint32_t)size;
    s_files[F_SAVE1].exists = 1;
    put_text(F_IMPORT_REQ, "1");
    boot_and_setup();
    (void)isaac_vita_trophies_worker_step();
    if (size >= 0x14u) {
        stored = (uint32_t)s_big_save[size - 4u] |
                 ((uint32_t)s_big_save[size - 3u] << 8) |
                 ((uint32_t)s_big_save[size - 2u] << 16) |
                 ((uint32_t)s_big_save[size - 1u] << 24);
        crc = isaac_vita_trophies_oracle_checksum(s_big_save + 0x10,
                                                  (uint32_t)size - 0x14u);
    }
    index = log_index_prefix("KAGE VITA TROPHY IMPORT:");
    printf("save %s: size=%u crc=0x%08x stored=0x%08x %s\n", path,
           (unsigned)size, (unsigned)crc, (unsigned)stored,
           index >= 0 ? s_log[index] : "<no import line>");
    return index >= 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--dump-map") == 0)
        return dump_map(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "--save") == 0) {
        int failures = 0;
        int i;

        for (i = 2; i < argc; ++i)
            failures += run_save(argv[i]);
        return failures != 0;
    }
    ref_table_build();
    case_map();
    case_boot_ok();
    case_failure_rows();
    case_observer();
    case_worker();
    case_import();
    case_test_request();
    case_store();
    case_pair();
    case_pending_delivery();
    case_pool();
    case_import_four_sets();
    case_model(1);
    if (s_failures != 0u) {
        dump_log();
        printf("host_vita_trophies_oracle: FAIL (%u of %u checks)\n",
               s_failures, s_checks);
        return 1;
    }
    printf("host_vita_trophies_oracle: checks=%u variant=%s\n", s_checks,
#if defined(ISAAC_VITA_TROPHY_TEST_REQUEST)
           "test-request"
#else
           "no-test-request"
#endif
    );
    printf("host_vita_trophies_oracle: PASS\n");
    return 0;
}
