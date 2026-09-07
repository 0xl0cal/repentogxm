/* Host differential oracle for the KAGE ReferenceCount AddRef / weak-lock /
 * Release native seam (host_vita_kage_refcount_seam.c,
 * ISAAC_VITA_KAGE_REFCOUNT_SEAM).
 *
 * The reference is the ACTUAL generated translation of sub_00007af0,
 * sub_00007b50 and sub_00007b70 together with the two mutex wrappers
 * sub_00562e00 / sub_00562ec0 they call.  recomp/test_vita_kage_refcount_seam.py
 * writes the bodies from a fresh gen_all render (the corpus spelling) into
 * kage_refcount_seam_bodies.inc in three spellings per refcount root:
 *
 *   sub_X        the exact unit text (entry hook, fenced seam statement,
 *                translated rest); the VERIFY rerun and the nested
 *                sub_00007b50(c) call inside sub_00007b70 use it;
 *   sub_X_plain  entry hook + translated rest, no seam, nested call to
 *                sub_00007b50_plain: the production system without the knob
 *                (the OLD route);
 *   sub_X_rest   translated rest only: what follows the seam statement in
 *                the unit (the NEW route runs the entry hook, the seam and,
 *                on rejection, this).
 *
 * The mutex wrappers keep their own seam (`#if defined(__vita__) &&
 * defined(ISAAC_VITA_KAGE_MUTEX_SEAM)` selected), exactly the production
 * configuration the refcount seam's census identity argument assumes.  The
 * bodies are a translation unit of their own (kage_refcount_seam_bodies.c,
 * written by the test around the .inc) compiled with the OWNER UNIT's
 * define set -- GUEST_FLAGS_LOCAL=1, GUEST_GPR_LOCAL 0/1,
 * GUEST_GENERATED_STACK_GUARD 0/1, GUEST_STACK_REQUIRED=1,
 * GUEST_COVERAGE_HOOKS=0 and, like guest_0000.c / guest_0166.c in every
 * production build, WITHOUT ISAAC_VITA_PHASE_PROFILE and
 * ISAAC_VITA_GUEST_DISPATCH_TABLE: their direct-edge census macros are the
 * no-ops they are on the Vita and the fenced lookup-cache hook is gone.
 * This oracle TU, the seam TU, the mutex seam TU and the endpoint carry
 * the census owner definitions exactly as guest.c and the seam TUs do in
 * a counting build.  Everything links against the real host_vita_sync.c +
 * vita_sync_services.c endpoint, the real inline fast-path header, the real
 * host_vita_kage_mutex_seam.c and the verbatim guest.c
 * guest_try_direct_sync_import_call route (copied from the mutex oracle).
 *
 * Every step runs twice from one snapshot (CPU, whole arena, IAT page,
 * every counter):
 *   old: sub_X_plain(c)                                  (no seam anywhere)
 *   new: guest_coverage_function(id); if (!try(c)) sub_X_rest(c)
 * (a directed phase proves new == the exact unit text sub_X(c), handled
 * and rejected alike, and == old up to VERIFY's documented double count).
 * CPU struct (memcmp, so stack_low_water and the flag fields take part),
 * arena (memcmp, dead words included), IAT page and every counter delta
 * must agree.  On rejection the seam must have touched nothing before the
 * body runs.  An independent acceptance predicate written from the design
 * (64-bit interval arithmetic, body read set) must equal the seam's decision
 * at every step: no over-rejection, no over-acceptance.
 *
 * Termination is decided empirically: guest_call (an unpredicted target,
 * i.e. every state the translated body cannot complete here) records the
 * target and longjmps to the run scope; a foreign-owner wait in the sync
 * endpoint is broken after a few DelayThread polls into the endpoint's own
 * "lock failed" fault.  A state whose old route escaped is a rejection-only
 * state: the seam must have rejected it.  Both routes must still agree byte
 * for byte on the partial state they left behind.
 *
 * With ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY the seam reruns the translated
 * body after each handled call; the oracle then also requires
 * verify_mismatches == 0, models the documented double count (one extra
 * function entry per handled call -- every other counter, the phase
 * profile struct included, must be equal) and, in the synthetic-stack-guard
 * leg, injects a fault into one rerun through the guarded push (the only
 * oracle-owned code a rerun of an accepted state executes) to prove the
 * bypass flag is cleared and the fault is re-raised to the real scope. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr/eventflag.h>

#include "guest.h"
#include "host_vita_import_id.h"
#include "host_vita_kage_mutex_seam.h"
#include "host_vita_kage_refcount_seam.h"
#include "host_vita_sync.h"
#include "host_vita_sync_fastpath.h"
#include "kage_vita_guest_sampler.h"
#include "vita_sync_services.h"

#if !defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
#error This oracle requires ISAAC_VITA_SYNC_INLINE_FASTPATH
#endif
#if !GUEST_STACK_REQUIRED
#error This oracle mirrors production: compile with GUEST_STACK_REQUIRED=1
#endif
#if !defined(ISAAC_VITA_PHASE_PROFILE) || \
    !defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    !defined(ISAAC_VITA_PROFILE_IMPORT_KINDS) || \
    !defined(ISAAC_VITA_GUEST_SAMPLER) || \
    !defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
#error Compile with every census owner definition so all counters are live
#endif
#if !defined(GUEST_FLAGS_LOCAL) || !GUEST_FLAGS_LOCAL
#error The refcount seam is only exact for the GUEST_FLAGS_LOCAL=1 corpus
#endif
#if !defined(ISAAC_VITA_KAGE_MUTEX_SEAM) || !defined(ISAAC_VITA_KAGE_REFCOUNT_SEAM)
#error Compile with both seams enabled (the production configuration)
#endif
#if defined(ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY)
#define ORACLE_VERIFY 1
#define ORACLE_VERIFY_TEXT "1"
#else
#define ORACLE_VERIFY 0
#define ORACLE_VERIFY_TEXT "0"
#endif

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* ---- Vita service mocks (same shape as the mutex oracle) --------------- */

#define MAIN_THREAD_ID      ((int32_t)UINT32_C(0x40010003))
#define CONTENDER_THREAD_ID ((int32_t)UINT32_C(0x40010005))

static int32_t s_thread_id = MAIN_THREAD_ID;
static unsigned s_checks;
static unsigned s_delay_calls;
static unsigned s_delay_streak;
static unsigned s_hang_breaks;
static unsigned s_log_calls;            /* non-seam receipts (pool etc.) */
static unsigned s_fault_calls;
static unsigned s_logger_calls;
static unsigned s_guest_call_calls;
static unsigned s_lookup_hits;
static unsigned s_escape_kind;          /* 0 none, 1 guest_call */
static uint32_t s_escape_target;
static uint32_t s_step_number;
static uint32_t s_step_reason;        /* the seam's decision of the last run_try */
static int s_step_left;               /* last step's body left the run scope */
static int s_trace;                   /* ORACLE_TRACE: print every step */
static int s_step_escaped;            /* ... through guest_call */
static CPU *s_active_cpu;
#if GUEST_GENERATED_STACK_GUARD
static int s_inject_rerun_fault;      /* VERIFY: fault the next rerun push */
#endif

unsigned g_host_import_calls;
GuestPhaseProfileCounters g_guest_phase_profile_counters;
uint32_t g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
uint32_t g_guest_function_entries;
volatile uint32_t g_kage_guest_last_indirect_target;
int g_isaac_vita_import_ids_ready;
static unsigned char s_coverage_functions[16384];
static unsigned char s_coverage_imports[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
static unsigned char s_coverage_cases[8192];
/* The Vita runtime keeps the function-coverage pointer NULL for the whole
 * process (guest.c), the generated units compile with GUEST_COVERAGE_HOOKS=0
 * (so does the bodies TU here) and the seam TU keeps the hooks like guest.c:
 * the differential therefore runs with the NULL pointer -- neither route can
 * write a function-coverage byte -- and a directed case points it at the
 * array for one call to pin the replayed ids.  The import bytes stay live:
 * both routes reach them through GUEST_COVERAGE_HOOKS=1 code (the mutex
 * seam TU and the refcount seam's verbatim kage_mutex_note_import copy). */
unsigned char *g_guest_coverage_functions = NULL;
unsigned char *g_guest_coverage_imports = s_coverage_imports;
unsigned char *g_guest_coverage_cases = s_coverage_cases;
uint32_t g_guest_fs_base_cached;

/* guest.c's private run scope: guest_fault / guest_exit longjmp to
 * ((guest_run_scope *)c->run_scope)->env.  The seam's VERIFY rerun mirrors
 * this layout (kage_refcount_run_scope); the python test pins both texts. */
typedef struct guest_run_scope {
    jmp_buf env;
} guest_run_scope;

static guest_run_scope s_run_scope;

/* ---- seam receipt capture ---------------------------------------------- */

#define SEAM_PREFIX "[isaac-kage] refcount seam"
#define SEAM_STATS_FORMAT \
    "[isaac-kage] refcount seam: addref=%u/%u weaklock=%u/%u" \
    "(alive=%u,dead=%u) release=%u/%u rejected(latch,esp,frame,slot,init," \
    "vt,cs,alias,owner,busy,zero,last)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u" \
    " imports=%u verify=%u/%u skipped=%u"

static unsigned s_seam_lines;
static unsigned s_banner_lines;
static unsigned s_banner_first;
static unsigned s_fallback_lines;
static unsigned s_fallback_by_reason[ISAAC_KRS_REASON_COUNT];
static unsigned s_fallback_unknown;
static unsigned s_stats_lines;
static unsigned s_stats_lines_bad;
static unsigned s_verify_match_lines;
static unsigned s_verify_mismatch_lines;
static unsigned s_verify_skipped_lines;
static unsigned s_unknown_seam_lines;
static char s_banner_text[512];
static char s_last_seam_line[512];
static char s_last_stats_line[512];

static const char *const s_reason_names[ISAAC_KRS_REASON_COUNT] = {
    "handled", "latch", "esp", "frame", "slot", "init", "vt", "cs", "alias",
    "owner", "busy", "zero", "last"
};

static void format_expected_stats(char *out, size_t size)
{
    const isaac_vita_kage_refcount_seam_stats *s =
        isaac_vita_kage_refcount_seam_stats_get();
    snprintf(out, size, SEAM_STATS_FORMAT,
             (unsigned)s->handled[ISAAC_KRS_FN_ADDREF],
             (unsigned)s->calls[ISAAC_KRS_FN_ADDREF],
             (unsigned)s->handled[ISAAC_KRS_FN_WEAKLOCK],
             (unsigned)s->calls[ISAAC_KRS_FN_WEAKLOCK],
             (unsigned)s->weak_alive, (unsigned)s->weak_dead,
             (unsigned)s->handled[ISAAC_KRS_FN_RELEASE],
             (unsigned)s->calls[ISAAC_KRS_FN_RELEASE],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_LATCH],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_ESP],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_FRAME],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_SLOT],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_INIT],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_VT],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_CS],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_ALIAS],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_OWNER],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_BUSY],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_ZERO],
             (unsigned)s->reasons[ISAAC_KRS_REJECT_LAST],
             (unsigned)s->imports_replayed,
             (unsigned)s->verify_mismatches, (unsigned)s->verify_runs,
             (unsigned)s->verify_skipped_fault);
}

void isaac_vita_log(const char *format, ...)
{
    char text[512];
    va_list ap;

    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    if (strncmp(text, SEAM_PREFIX, sizeof SEAM_PREFIX - 1U) != 0) {
        ++s_log_calls;
        return;
    }
    ++s_seam_lines;
    strcpy(s_last_seam_line, text);
    if (strncmp(text, SEAM_PREFIX ": roots=", sizeof SEAM_PREFIX + 7U) == 0) {
        if (s_seam_lines == 1U)
            s_banner_first = 1U;
        ++s_banner_lines;
        strcpy(s_banner_text, text);
    } else if (strncmp(text, SEAM_PREFIX " fallback: fn=",
                       sizeof SEAM_PREFIX + 13U) == 0) {
        const char *reason = strstr(text, " reason=");
        unsigned i;
        ++s_fallback_lines;
        if (reason) {
            reason += 8;
            for (i = 1U; i < ISAAC_KRS_REASON_COUNT; ++i) {
                size_t n = strlen(s_reason_names[i]);
                if (strncmp(reason, s_reason_names[i], n) == 0 &&
                    reason[n] == ' ') {
                    ++s_fallback_by_reason[i];
                    return;
                }
            }
        }
        ++s_fallback_unknown;
    } else if (strncmp(text, SEAM_PREFIX ": addref=",
                       sizeof SEAM_PREFIX + 8U) == 0) {
        char expected[512];
        ++s_stats_lines;
        strcpy(s_last_stats_line, text);
        format_expected_stats(expected, sizeof expected);
        if (strcmp(expected, text) != 0) {
            ++s_stats_lines_bad;
            fprintf(stderr, "stats line differs from the counters:\n  %s\n  %s\n",
                    text, expected);
        }
    } else if (strncmp(text, SEAM_PREFIX " VERIFY: ",
                       sizeof SEAM_PREFIX + 8U) == 0) {
        if (strstr(text, "result=MATCH"))
            ++s_verify_match_lines;
        else if (strstr(text, "result=MISMATCH"))
            ++s_verify_mismatch_lines;
        else if (strstr(text, "verify skipped (fault)"))
            ++s_verify_skipped_lines;
        else
            ++s_unknown_seam_lines;
    } else {
        ++s_unknown_seam_lines;
    }
}

int sceClibPrintf(const char *format, ...)
{
    (void)format;
    return 0;
}

int sceKernelGetThreadId(void)
{
    return s_thread_id;
}

/* A foreign-owner wait in the endpoint polls DelayThread(1) forever (no
 * second thread exists here).  After a few polls the mock fails the wait,
 * which the endpoint turns into its own "lock failed" guest_fault: the body
 * terminates and both routes see the identical fault. */
int sceKernelDelayThread(unsigned int usec)
{
    ++s_delay_calls;
    if (usec != 1U)
        return -1;
    if (++s_delay_streak > 8U) {
        s_delay_streak = 0U;
        ++s_hang_breaks;
        return -1;
    }
    return 0;
}

SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *option)
{
    (void)name; (void)attr; (void)bits; (void)option;
    return 1;
}
int sceKernelSetEventFlag(SceUID uid, unsigned int bits)
{ (void)uid; (void)bits; return 0; }
int sceKernelClearEventFlag(SceUID uid, unsigned int bits)
{ (void)uid; (void)bits; return 0; }
int sceKernelWaitEventFlag(SceUID uid, unsigned int bits,
                           unsigned int wait, unsigned int *out,
                           unsigned int *timeout)
{ (void)uid; (void)bits; (void)wait; (void)out; (void)timeout; return 0; }
int sceKernelDeleteEventFlag(SceUID uid)
{ (void)uid; return 0; }

/* ---- guest.c pieces the endpoint, the seam and the bodies link against -- */

/* guest.c's route: record, then longjmp to the CPU's run scope. */
void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    guest_run_scope *scope;
    ++s_fault_calls;
    c->fault = what;
    c->fault_addr = addr;
    c->exit_api = NULL;
    c->stop_kind = GUEST_RUN_FAULT;
    scope = (guest_run_scope *)c->run_scope;
    if (scope)
        longjmp(scope->env, 1);
    abort();
}

uint32_t guest_fs_base(CPU *__restrict c)
{
    (void)c;
    return 0U;
}

/* guest.c's predicted-edge census hook (++g_guest_lookup_cache_hits).  No
 * route reaches it here: a generated unit is compiled without
 * ISAAC_VITA_PHASE_PROFILE, so the bodies' fenced calls are compiled out
 * (the bodies TU has no declaration for it), and the seam replays no
 * direct-edge census.  The count must stay zero (checked at the end). */
void guest_phase_profile_note_lookup_cache_hit(void)
{
    ++s_lookup_hits;
}

static const char *stack_fault_text(uint32_t kind)
{
    switch (kind) {
    case GUEST_STACK_FAULT_UNBOUND: return "guest stack is not bound";
    case GUEST_STACK_FAULT_OWNER:   return "guest stack owner mismatch";
    case GUEST_STACK_FAULT_ACCESS:  return "guest stack access out of range";
    case GUEST_STACK_FAULT_POP:     return "guest stack pop out of range";
    case GUEST_STACK_FAULT_ADJUST:  return "guest stack adjust out of range";
    default:                        return "guest stack violation";
    }
}

GUEST_STACK_COLD_NOINLINE int guest_stack_violation(
    CPU *__restrict c, uint32_t pc, uint32_t kind, uint32_t address,
    uint32_t size)
{
    if (!c)
        return 0;
    c->stack_fault_kind = kind;
    c->stack_fault_address = address;
    c->stack_fault_size = size;
    c->stack_fault_pc = pc;
    c->stack_fault_native_site = (uintptr_t)0U;
    guest_fault(c, pc, stack_fault_text(kind));
    return 0;
}

GUEST_STACK_COLD_NOINLINE int guest_stack_owner_violation(
    CPU *__restrict c, uint32_t pc)
{
    uint32_t kind;
    if (!c)
        return 0;
    kind = c->stack_owner ? GUEST_STACK_FAULT_OWNER
                          : GUEST_STACK_FAULT_UNBOUND;
    return guest_stack_violation(c, pc, kind, c->esp, 0U);
}

/* Verbatim GUEST_STACK_REQUIRED=1 validators from guest.c. */
GUEST_STACK_HOT_NOINLINE int guest_stack_set(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(value - c->stack_floor > capacity))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_SET, value, 0U);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

GUEST_STACK_HOT_NOINLINE int guest_stack_adjust(
    CPU *__restrict c, uint32_t amount, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            offset > capacity || amount > capacity - offset))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ADJUST, c->esp, amount);
    c->esp += amount;
    return 1;
}

GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address(
    CPU *__restrict c, uint32_t address, uint32_t size, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            size == 0U || size > capacity || offset > capacity - size)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ACCESS, address, size);
        return 0U;
    }
    guest_stack_note_low(c, address);
    return address;
}

GUEST_STACK_HOT_NOINLINE void gpush_at(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity, offset, next;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset < 4U || offset > capacity)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_PUSH, c->esp - 4U, 4U);
        return;
    }
    next = c->esp - 4U;
    c->esp = next;
    guest_stack_note_low(c, next);
    st32(next, value);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_at(
    CPU *__restrict c, uint32_t pc)
{
    uint32_t capacity, offset, value;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(capacity < 4U || offset > capacity - 4U)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_POP, c->esp, 4U);
        return 0U;
    }
    value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

#if GUEST_GENERATED_STACK_GUARD
GUEST_STACK_HOT_NOINLINE int guest_stack_set_generated(
    CPU *__restrict c, uint32_t value)
{
    return guest_stack_set(c, value, 0U);
}

GUEST_STACK_HOT_NOINLINE int guest_stack_adjust_generated(
    CPU *__restrict c, uint32_t amount)
{
    return guest_stack_adjust(c, amount, 0U);
}

GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address_generated(
    CPU *__restrict c, uint32_t address, uint32_t size)
{
    return guest_stack_address(c, address, size, 0U);
}

/* Under VERIFY the oracle can make one rerun fault here: the guarded push
 * is the only oracle-owned code a rerun of an accepted state executes, and
 * it runs inside the seam's private rerun scope exactly when the CPU's run
 * scope is not the oracle's.  (The stack_guard=0 legs have no such hook:
 * their pushes are the inline raw stores, as in production.) */
GUEST_STACK_HOT_NOINLINE void gpush_generated(
    CPU *__restrict c, uint32_t value)
{
    if (s_inject_rerun_fault && c->run_scope != NULL &&
        c->run_scope != (void *)&s_run_scope) {
        s_inject_rerun_fault = 0;
        guest_fault(c, 0x00007b50U, "oracle: injected rerun fault");
    }
    gpush_at(c, value, 0U);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_generated(CPU *__restrict c)
{
    return gpop_at(c, 0U);
}
#endif

/* KAGE's logger sub_0055e330 ('Trying to lock mutex that has not been
 * initialized'): the only translated callee of the mutex wrappers besides
 * the imports.  Pops its return word, leaves the register file alone. */
void sub_0055e330(CPU *__restrict c)
{
    ++s_logger_calls;
    c->esp += 4U;
}

/* The generic route (an unpredicted vtable slot, a non-token IAT word, the
 * Sleep loop, the destroy path's virtual calls): the translated body leaves
 * the harness here.  Recorded, then raised to the run scope like a fault so
 * both routes can still be compared on the state they left behind. */
void guest_call(CPU *__restrict c, uint32_t addr)
{
    guest_run_scope *scope;
    ++s_guest_call_calls;
    s_escape_kind = 1U;
    s_escape_target = addr;
    c->fault = "oracle: guest_call escape";
    c->fault_addr = addr;
    c->exit_api = NULL;
    c->stop_kind = GUEST_RUN_FAULT;
    scope = (guest_run_scope *)c->run_scope;
    if (scope)
        longjmp(scope->env, 1);
    fprintf(stderr, "guest_call(%08x) outside a run scope at step %u\n",
            addr, s_step_number);
    abort();
}

/* Verbatim guest.c route under ISAAC_VITA_SYNC_IMPORT_FASTPATH +
 * ISAAC_VITA_SYNC_INLINE_FASTPATH + ISAAC_VITA_GUEST_SAMPLER, with
 * g_isaac_vita_import_ids_ready standing for guest.c's
 * g_vita_sync_import_fastpath_ready (both are the return value of the same
 * registration, which also proved rows 60/61 == the two slots).  Copied
 * from host_vita_kage_mutex_seam_oracle.c. */
int guest_try_direct_sync_import_call(
    CPU *__restrict c, uint32_t target, uint32_t exact_slot_rva)
{
    uint32_t import_id;
    uint32_t sync_index;
    uint32_t sampler_enclosing_target;

    if (exact_slot_rva == ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS) {
        import_id = ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS;
        sync_index = ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS;
    } else if (exact_slot_rva == ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS) {
        import_id = ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS;
        sync_index = ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS;
    } else {
        return 0;
    }
    if (!g_isaac_vita_import_ids_ready ||
        !guest_direct_translated_target(target, exact_slot_rva))
        return 0;
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
    g_kage_guest_last_indirect_target = target;
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH();
    GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id);
    guest_coverage_import(import_id);
    if (sync_index == ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS
            ? isaac_vita_sync_inline_enter(c, &g_host_import_calls)
            : isaac_vita_sync_inline_leave(c, &g_host_import_calls)) {
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
        return 1;
    }
    if (isaac_vita_sync_import_indexed(
            c, sync_index, &g_host_import_calls)) {
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
        return 1;
    }
    guest_fault(c, target,
                "validated Vita sync import rejected its local ID");
    return 1;
}

/* ---- the translated bodies -------------------------------------------- */

void sub_00562e00(CPU *__restrict c);
void sub_00562ec0(CPU *__restrict c);
void sub_00007af0(CPU *__restrict c);
void sub_00007b50(CPU *__restrict c);
void sub_00007b70(CPU *__restrict c);
void sub_00007af0_plain(CPU *__restrict c);
void sub_00007b50_plain(CPU *__restrict c);
void sub_00007b70_plain(CPU *__restrict c);
void sub_00007af0_rest(CPU *__restrict c);
void sub_00007b50_rest(CPU *__restrict c);
void sub_00007b70_rest(CPU *__restrict c);
/* The definitions are kage_refcount_seam_bodies.c (the test writes it around
 * kage_refcount_seam_bodies.inc), a translation unit of its own compiled
 * with the owner unit's define set -- see the header comment -- and linked
 * with this oracle. */

/* ---- arena -------------------------------------------------------------- */

static void *map_low(uint32_t bytes, uintptr_t hint)
{
#if defined(_WIN32)
    static const uintptr_t candidates[] = {
        0x10000000U, 0x20000000U, 0x30000000U, 0x40000000U, 0x50000000U,
        0x60000000U, 0x70000000U, 0x08000000U
    };
    size_t i;
    if (hint)
        return VirtualAlloc((LPVOID)hint, bytes, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE);
    for (i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        void *p = VirtualAlloc((LPVOID)candidates[i], bytes,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p)
            return p;
    }
    return NULL;
#else
    void *p;
    if (hint) {
        p = mmap((void *)hint, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        return p == MAP_FAILED ? NULL : p;
    }
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

#define ARENA_BYTES     UINT32_C(0x10000)
#define STACK_FLOOR_OFF UINT32_C(0x1000)
#define STACK_CEIL_OFF  UINT32_C(0x5000)
#define OBJECT_ROW_OFF  UINT32_C(0x6000)   /* 8 control blocks */
#define OBJECT_STRIDE   32U
#define OBJECT_COUNT    8U
#define MVT_ROW_OFF     UINT32_C(0x6200)   /* mutex vtables */
#define CVT_ROW_OFF     UINT32_C(0x6400)   /* counter vtables */
#define TOBJ_OFF        UINT32_C(0x6600)   /* a T object ([0] = its vtable) */
#define TVT_OFF         UINT32_C(0x6700)   /* T vtable, slot 0x60 foreign */
#define VT_STRIDE       32U
#define CS_ROW_OFF      UINT32_C(0x7000)   /* 8 critical sections + spares */
#define CS_STRIDE       32U
#define CS_SPARE_INDEX  8U
#define SPARE_OFF       UINT32_C(0x8000)   /* scratch for relocated objects */
/* The bodies and the seam read the IAT words at their absolute guest
 * addresses.  Mapped fixed on a 64 KiB boundary (Windows granularity); on
 * x86-64 the range lies inside ASan's shadow: run plain or under UBSan. */
#define IAT_PAGE        UINT32_C(0x98606000)
#define IAT_MAP_BASE    UINT32_C(0x98600000)
#define IAT_MAP_BYTES   UINT32_C(0x10000)
#define IAT_BYTES       UINT32_C(0x1000)
#define ENTER_SLOT      (GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS)
#define LEAVE_SLOT      (GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS)
#define SLEEP_SLOT      (GUEST_IMAGE_BASE + UINT32_C(0x00606138))
#define IAT_SYNC_WORDS  LEAVE_SLOT          /* [Leave, Enter) 8 bytes */

#define LOCK_RVA        UINT32_C(0x00562e00)
#define UNLOCK_RVA      UINT32_C(0x00562ec0)
#define ADDREF_RVA      UINT32_C(0x00007b50)
#define LOCK_VA         (GUEST_IMAGE_BASE + LOCK_RVA)
#define UNLOCK_VA       (GUEST_IMAGE_BASE + UNLOCK_RVA)
#define ADDREF_VA       (GUEST_IMAGE_BASE + ADDREF_RVA)
#define FOREIGN_LOCK    UINT32_C(0x98123450)
#define FOREIGN_UNLOCK  UINT32_C(0x98123460)
#define FOREIGN_ADDREF  UINT32_C(0x98123470)

#define RET_ENTER_CS    UINT32_C(0x00562e2f)
#define RET_LEAVE_CS    UINT32_C(0x00562ee6)
#define RET_RELEASE_LOCK   UINT32_C(0x00007aff)
#define RET_RELEASE_UNLOCK UINT32_C(0x00007b44)
#define RET_ADDREF_LOCK    UINT32_C(0x00007b5f)
#define RET_WEAK_LOCK      UINT32_C(0x00007b7f)
#define RET_WEAK_DEAD_UNLOCK  UINT32_C(0x00007b91)
#define RET_WEAK_ALIVE_UNLOCK UINT32_C(0x00007b98)
#define RET_WEAK_ADDREF    UINT32_C(0x00007b9f)

#define COVERAGE_RELEASE  259U
#define COVERAGE_ADDREF   260U
#define COVERAGE_WEAKLOCK 261U
#define COVERAGE_LOCK     7968U
#define COVERAGE_UNLOCK   7969U

#define FN_ADDREF   ISAAC_KRS_FN_ADDREF
#define FN_WEAKLOCK ISAAC_KRS_FN_WEAKLOCK
#define FN_RELEASE  ISAAC_KRS_FN_RELEASE

/* Mutex vtable rows. */
#define MVT_FROZEN         0U   /* slots 3/4 = Lock/Unlock image VAs */
#define MVT_FROZEN_RVA     1U   /* the RVA spelling of both */
#define MVT_LOCK_FOREIGN   2U   /* slot 3 foreign, slot 4 frozen */
#define MVT_UNLOCK_FOREIGN 3U   /* slot 3 frozen, slot 4 foreign */
#define MVT_SPARE          4U   /* frozen; register filler for alias cases */
/* Counter vtable rows. */
#define CVT_FROZEN         0U   /* slot 2 = AddRef image VA */
#define CVT_FROZEN_RVA     1U
#define CVT_FOREIGN        2U

static uint8_t *s_arena;
static uint32_t s_base;
static uint8_t *s_iat;
static uint8_t s_snapshot[ARENA_BYTES];
static uint8_t s_old_arena[ARENA_BYTES];
static uint8_t s_iat_snapshot[IAT_BYTES];
static uint8_t s_old_iat[IAT_BYTES];

static uint32_t object_address(uint32_t index)
{
    return s_base + OBJECT_ROW_OFF + index * OBJECT_STRIDE;
}

static uint32_t cs_address(uint32_t index)
{
    return s_base + CS_ROW_OFF + index * CS_STRIDE;
}

static uint32_t mvt_row(uint32_t row)
{
    return s_base + MVT_ROW_OFF + row * VT_STRIDE;
}

static uint32_t cvt_row(uint32_t row)
{
    return s_base + CVT_ROW_OFF + row * VT_STRIDE;
}

static void make_cs(uint32_t address)
{
    st32(address, ISAAC_VITA_SYNC_CS_MAGIC);
    st32(address + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
    st32(address + 8U, 0U);
    st32(address + 12U, ISAAC_VITA_SYNC_CS_VERSION);
    st32(address + 16U, 0U);
    st32(address + 20U, 0U);
    st32(address + 24U, 0U);                /* busy byte + padding */
    st32(address + 28U, 0U);
}

/* A control block KAGE::ReferenceCount<T>: +0 counter vtable, +4 strong,
 * +6 weak, +8 Mutex {vtable, initialized, CRITICAL_SECTION*}, +0x14 T*. */
static void make_object_at(uint32_t object, uint32_t cs, uint32_t strong)
{
    st32(object, cvt_row(CVT_FROZEN));
    st16(object + 4U, (uint16_t)strong);
    st16(object + 6U, 1U);
    st32(object + 8U, mvt_row(MVT_FROZEN));
    st32(object + 0xCU, 1U);                /* initialized byte, zero pad */
    st32(object + 0x10U, cs);
    st32(object + 0x14U, 0U);
    st32(object + 0x18U, 0U);
    st32(object + 0x1CU, 0U);
}

static void make_free(uint32_t index)
{
    make_cs(cs_address(index));
    make_object_at(object_address(index), cs_address(index), 2U);
}

static void make_vtables(void)
{
    uint32_t row, i;

    for (row = 0U; row < 5U; ++row) {
        uint32_t base = mvt_row(row);
        for (i = 0U; i < 8U; ++i)
            st32(base + i * 4U, 0x98700000U + row * 0x100U + i * 0x10U);
        st32(base + 0xCU, row == MVT_FROZEN_RVA ? LOCK_RVA :
                          row == MVT_LOCK_FOREIGN ? FOREIGN_LOCK : LOCK_VA);
        st32(base + 0x10U, row == MVT_FROZEN_RVA ? UNLOCK_RVA :
                           row == MVT_UNLOCK_FOREIGN ? FOREIGN_UNLOCK :
                           UNLOCK_VA);
    }
    for (row = 0U; row < 3U; ++row) {
        uint32_t base = cvt_row(row);
        st32(base, 0x98008200U);                    /* Free */
        st32(base + 4U, 0x98007b70U);               /* weak-lock */
        st32(base + 8U, row == CVT_FROZEN_RVA ? ADDREF_RVA :
                        row == CVT_FOREIGN ? FOREIGN_ADDREF : ADDREF_VA);
        st32(base + 0xCU, 0x98007af0U);             /* Release */
        st32(base + 0x10U, 0x98007ad0U);
        st32(base + 0x14U, 0x98007a80U);            /* ReleaseWeak (generic) */
        st32(base + 0x18U, 0U);
        st32(base + 0x1CU, 0U);
    }
    /* The T object and its vtable: slot 0x60 is a generic (escaping) call. */
    st32(s_base + TOBJ_OFF, s_base + TVT_OFF);
    for (i = 0U; i < 0x80U; i += 4U)
        st32(s_base + TVT_OFF + i, 0x98300000U + i);
}

static void iat_tokens(void)
{
    st32(ENTER_SLOT, ENTER_SLOT);
    st32(LEAVE_SLOT, LEAVE_SLOT);
}

/* ---- observation ------------------------------------------------------- */

typedef struct observation {
    CPU cpu;
    unsigned host_import_calls;
    unsigned faults;
    unsigned logs;
    unsigned delays;
    unsigned logger_calls;
    unsigned guest_calls;
    unsigned lookup_hits;
    unsigned escape_kind;
    uint32_t escape_target;
    uint32_t function_entries;
    GuestPhaseProfileCounters profile;
    uint32_t import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
    unsigned char coverage_imports[2];
    unsigned char coverage_functions[5];
    uint32_t sampler_word;
    isaac_vita_sync_mutex_pool_snapshot pool;
} observation;

static void observe(observation *out, const CPU *c)
{
    memset(out, 0, sizeof *out);
    out->cpu = *c;
    out->host_import_calls = g_host_import_calls;
    out->faults = s_fault_calls;
    out->logs = s_log_calls;
    out->delays = s_delay_calls;
    out->logger_calls = s_logger_calls;
    out->guest_calls = s_guest_call_calls;
    out->lookup_hits = s_lookup_hits;
    out->escape_kind = s_escape_kind;
    out->escape_target = s_escape_target;
    out->function_entries = g_guest_function_entries;
    out->profile = g_guest_phase_profile_counters;
    memcpy(out->import_calls, g_guest_phase_profile_import_calls,
           sizeof out->import_calls);
    out->coverage_imports[0] =
        s_coverage_imports[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS];
    out->coverage_imports[1] =
        s_coverage_imports[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS];
    out->coverage_functions[0] = s_coverage_functions[COVERAGE_LOCK];
    out->coverage_functions[1] = s_coverage_functions[COVERAGE_UNLOCK];
    out->coverage_functions[2] = s_coverage_functions[COVERAGE_RELEASE];
    out->coverage_functions[3] = s_coverage_functions[COVERAGE_ADDREF];
    out->coverage_functions[4] = s_coverage_functions[COVERAGE_WEAKLOCK];
    out->sampler_word = g_kage_guest_last_indirect_target;
    isaac_vita_sync_get_mutex_pool_snapshot(&out->pool);
}

typedef struct counters_snapshot {
    unsigned host_import_calls;
    unsigned faults;
    unsigned logs;
    unsigned delays;
    unsigned logger_calls;
    unsigned guest_calls;
    unsigned lookup_hits;
    unsigned escape_kind;
    uint32_t escape_target;
    uint32_t function_entries;
    GuestPhaseProfileCounters profile;
    uint32_t import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
    unsigned char coverage_imports[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
    unsigned char coverage_functions[16384];
    uint32_t sampler_word;
} counters_snapshot;

static void counters_save(counters_snapshot *out)
{
    out->host_import_calls = g_host_import_calls;
    out->faults = s_fault_calls;
    out->logs = s_log_calls;
    out->delays = s_delay_calls;
    out->logger_calls = s_logger_calls;
    out->guest_calls = s_guest_call_calls;
    out->lookup_hits = s_lookup_hits;
    out->escape_kind = s_escape_kind;
    out->escape_target = s_escape_target;
    out->function_entries = g_guest_function_entries;
    out->profile = g_guest_phase_profile_counters;
    memcpy(out->import_calls, g_guest_phase_profile_import_calls,
           sizeof out->import_calls);
    memcpy(out->coverage_imports, s_coverage_imports,
           sizeof out->coverage_imports);
    memcpy(out->coverage_functions, s_coverage_functions,
           sizeof out->coverage_functions);
    out->sampler_word = g_kage_guest_last_indirect_target;
}

static void counters_restore(const counters_snapshot *in)
{
    g_host_import_calls = in->host_import_calls;
    s_fault_calls = in->faults;
    s_log_calls = in->logs;
    s_delay_calls = in->delays;
    s_logger_calls = in->logger_calls;
    s_guest_call_calls = in->guest_calls;
    s_lookup_hits = in->lookup_hits;
    s_escape_kind = in->escape_kind;
    s_escape_target = in->escape_target;
    g_guest_function_entries = in->function_entries;
    g_guest_phase_profile_counters = in->profile;
    memcpy(g_guest_phase_profile_import_calls, in->import_calls,
           sizeof in->import_calls);
    memcpy(s_coverage_imports, in->coverage_imports,
           sizeof in->coverage_imports);
    memcpy(s_coverage_functions, in->coverage_functions,
           sizeof in->coverage_functions);
    g_kage_guest_last_indirect_target = in->sampler_word;
}

#define DELTA_EQUAL(field) \
    ((old->field) - (before->field) == (fresh->field) - (old->field))
#define SAME(field) ((old->field) == (fresh->field))

/* extra_entries: the VERIFY rerun's documented double count (one body entry
 * per handled call); zero without VERIFY.  Every other counter must agree
 * exactly -- the phase-profile struct included, which is what the device's
 * VERIFY compare and ph120's g(c)/g(l) identity rest on. */
static int observations_equal(const observation *before,
                              const observation *old,
                              const observation *fresh,
                              uint32_t extra_entries)
{
    if (memcmp(&old->cpu, &fresh->cpu, sizeof old->cpu) != 0)
        return 0;
    if (!SAME(host_import_calls) || !SAME(faults) ||
        !SAME(logs) || !SAME(delays) ||
        !SAME(logger_calls) || !SAME(guest_calls) || !SAME(lookup_hits) ||
        !SAME(escape_kind) || !SAME(escape_target))
        return 0;
    if (old->function_entries + extra_entries != fresh->function_entries)
        return 0;
    if (memcmp(&old->profile, &fresh->profile, sizeof old->profile) != 0)
        return 0;
    if (memcmp(old->import_calls, fresh->import_calls,
               sizeof old->import_calls) != 0)
        return 0;
    if (memcmp(old->coverage_imports, fresh->coverage_imports,
               sizeof old->coverage_imports) != 0 ||
        memcmp(old->coverage_functions, fresh->coverage_functions,
               sizeof old->coverage_functions) != 0 ||
        old->sampler_word != fresh->sampler_word)
        return 0;
    if (!DELTA_EQUAL(pool.live) || !DELTA_EQUAL(pool.creates) ||
        !DELTA_EQUAL(pool.deletes) || !DELTA_EQUAL(pool.waits) ||
        !DELTA_EQUAL(pool.wait_failures) || !DELTA_EQUAL(pool.owner_failures) ||
        !DELTA_EQUAL(pool.state_failures))
        return 0;
    return 1;
}

static int observations_identical(const observation *a, const observation *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void dump_cpu_diff(const CPU *a, const CPU *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    size_t i;
    for (i = 0; i < sizeof *a; ++i)
        if (pa[i] != pb[i])
            fprintf(stderr, "  CPU byte %u: old=%02x new=%02x\n",
                    (unsigned)i, pa[i], pb[i]);
    fprintf(stderr, "  eax %08x/%08x ecx %08x/%08x edx %08x/%08x ebx %08x/%08x\n"
                    "  esp %08x/%08x ebp %08x/%08x esi %08x/%08x edi %08x/%08x\n"
                    "  low_water %08x/%08x fault %s/%s\n",
            a->eax, b->eax, a->ecx, b->ecx, a->edx, b->edx, a->ebx, b->ebx,
            a->esp, b->esp, a->ebp, b->ebp, a->esi, b->esi, a->edi, b->edi,
            a->stack_low_water, b->stack_low_water,
            a->fault ? a->fault : "-", b->fault ? b->fault : "-");
}

static uint32_t arena_first_diff(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0U; i < ARENA_BYTES; ++i)
        if (a[i] != b[i])
            return i;
    return ARENA_BYTES;
}

/* ---- routes ------------------------------------------------------------- */

typedef void (*body_fn)(CPU *__restrict);

static body_fn plain_body(int fn)
{
    return fn == FN_ADDREF ? sub_00007b50_plain :
           fn == FN_WEAKLOCK ? sub_00007b70_plain : sub_00007af0_plain;
}

static body_fn rest_body(int fn)
{
    return fn == FN_ADDREF ? sub_00007b50_rest :
           fn == FN_WEAKLOCK ? sub_00007b70_rest : sub_00007af0_rest;
}

static body_fn unit_body(int fn)
{
    return fn == FN_ADDREF ? sub_00007b50 :
           fn == FN_WEAKLOCK ? sub_00007b70 : sub_00007af0;
}

static uint32_t coverage_id(int fn)
{
    return fn == FN_ADDREF ? COVERAGE_ADDREF :
           fn == FN_WEAKLOCK ? COVERAGE_WEAKLOCK : COVERAGE_RELEASE;
}

static uint32_t frame_bytes(int fn)
{
    return fn == FN_WEAKLOCK ? 52U : 40U;
}

static uint32_t caller_return(int fn)
{
    /* The predicted callers' return words: 7b70:7b9c -> 7b9f, 78a0:78c8 ->
     * 78ca, 7790:77c1 -> 77c3. */
    return fn == FN_ADDREF ? 0x00007b9fU :
           fn == FN_WEAKLOCK ? 0x000078caU : 0x000077c3U;
}

/* Returns 0 when the body returned, 1 when it left through the run scope
 * (fault or escape). */
static int run_body(CPU *c, body_fn body)
{
    int left;
    s_active_cpu = c;
    s_delay_streak = 0U;
    c->run_scope = &s_run_scope;
    if (setjmp(s_run_scope.env) == 0) {
        body(c);
        left = 0;
    } else {
        left = 1;
    }
    c->run_scope = NULL;
    return left;
}

static int seam_try(CPU *c, int fn)
{
    return fn == FN_ADDREF ? isaac_vita_kage_refcount_addref_try(c) :
           fn == FN_WEAKLOCK ? isaac_vita_kage_refcount_weaklock_try(c) :
           isaac_vita_kage_refcount_release_try(c);
}

/* The seam under the run scope (its VERIFY re-raise of a faulting rerun
 * arrives here).  Returns the decision, or -1 when it left the scope. */
static int run_try(CPU *c, int fn)
{
    int result;
    s_active_cpu = c;
    s_delay_streak = 0U;
    c->run_scope = &s_run_scope;
    if (setjmp(s_run_scope.env) == 0) {
        result = seam_try(c, fn);
        /* Read before any translated fallback runs: a rejected weak-lock's
         * body calls the seamed AddRef unit, which sets last_reason again. */
        s_step_reason = isaac_vita_kage_refcount_seam_stats_get()->last_reason;
    } else {
        result = -1;
    }
    c->run_scope = NULL;
    return result;
}

static void clear_fault(CPU *c)
{
    c->fault = NULL;
    c->fault_addr = 0U;
    c->stop_kind = GUEST_RUN_RETURNED;
    s_escape_kind = 0U;
    s_escape_target = 0U;
}

static uint32_t s_handled[3];
static uint32_t s_rejections;
static uint32_t s_differentials;
static uint32_t s_escapes;
static uint32_t s_body_faults;

static uint32_t stats_handled_total(void)
{
    const isaac_vita_kage_refcount_seam_stats *s =
        isaac_vita_kage_refcount_seam_stats_get();
    return s->handled[0] + s->handled[1] + s->handled[2];
}

/* Both routes from one snapshot; leaves the new-route state in place.
 * expect: 1 = seam must handle, 0 = must reject, -1 = predicate decides. */
static int differential_step(CPU *c, int fn, int expect, uint32_t step)
{
    observation before, old, fresh, pre_seam, after_seam;
    static counters_snapshot counters;
    const isaac_vita_kage_refcount_seam_stats *stats =
        isaac_vita_kage_refcount_seam_stats_get();
    CPU saved;
    uint32_t calls0, handled0, total0;
    uint32_t extra_entries = 0U;
    int handled, old_left, old_escaped;

    ++s_differentials;
    s_step_number = step;
    if (s_trace)
        fprintf(stderr, "step %u fn=%d ecx=%08x esp=%08x\n", step, fn,
                c->ecx, c->esp);
    saved = *c;
    observe(&before, c);
    counters_save(&counters);
    memcpy(s_snapshot, s_arena, ARENA_BYTES);
    memcpy(s_iat_snapshot, s_iat, IAT_BYTES);

    /* OLD: the production body without the knob (its own entry hook, no
     * seam anywhere, the nested AddRef plain too). */
    old_left = run_body(c, plain_body(fn));
    old_escaped = s_escape_kind != 0U;
    observe(&old, c);
    memcpy(s_old_arena, s_arena, ARENA_BYTES);
    memcpy(s_old_iat, s_iat, IAT_BYTES);
    if (old_left && !old_escaped)
        ++s_body_faults;

    *c = saved;
    counters_restore(&counters);
    memcpy(s_arena, s_snapshot, ARENA_BYTES);
    memcpy(s_iat, s_iat_snapshot, IAT_BYTES);
    /* NEW: the unit as generated -- entry hook, seam, and on rejection the
     * translated rest (whose nested AddRef is the seamed unit). */
    guest_coverage_function(coverage_id(fn));
    observe(&pre_seam, c);
    calls0 = stats->calls[fn];
    handled0 = stats->handled[fn];
    total0 = stats_handled_total();
    handled = run_try(c, fn);
    if (handled < 0) {
        fprintf(stderr, "seam left through the run scope at step %u (%s)\n",
                step, c->fault ? c->fault : "-");
        return 1;
    }
    if (stats->calls[fn] != calls0 + 1U ||
        stats->handled[fn] != handled0 + (handled ? 1U : 0U)) {
        fprintf(stderr, "seam call census drifted at step %u\n", step);
        return 1;
    }
    observe(&after_seam, c);
    if (!handled) {
        if (!observations_identical(&pre_seam, &after_seam) ||
            memcmp(s_snapshot, s_arena, ARENA_BYTES) != 0 ||
            memcmp(s_iat_snapshot, s_iat, IAT_BYTES) != 0) {
            fprintf(stderr, "seam rejected with side effects at step %u\n",
                    step);
            dump_cpu_diff(&pre_seam.cpu, &after_seam.cpu);
            return 1;
        }
        (void)run_body(c, rest_body(fn));
    }
    observe(&fresh, c);
#if ORACLE_VERIFY
    /* Every handled seam call on the new route (the outer one or a nested
     * AddRef inside a rejected weak-lock) reran its body once: one more
     * body entry; nothing else is counted twice. */
    extra_entries = stats_handled_total() - total0;
#else
    (void)total0;
#endif

    if (!observations_equal(&before, &old, &fresh, extra_entries) ||
        memcmp(s_old_arena, s_arena, ARENA_BYTES) != 0 ||
        memcmp(s_old_iat, s_iat, IAT_BYTES) != 0) {
        fprintf(stderr,
                "differential mismatch at step %u fn=%d handled=%d "
                "old_left=%d escaped=%d\n",
                step, fn, handled, old_left, old_escaped);
        dump_cpu_diff(&old.cpu, &fresh.cpu);
        fprintf(stderr, "  imports %u/%u faults %u/%u logs %u/%u logger %u/%u "
                        "calls %u/%u lookups %u/%u sync %u/%u import60 %u/%u "
                        "import61 %u/%u entries %u/%u hits %u/%u "
                        "sampler %08x/%08x escape %u:%08x/%u:%08x "
                        "arena %s (first diff +%x) iat %s\n",
                old.host_import_calls, fresh.host_import_calls,
                old.faults, fresh.faults, old.logs, fresh.logs,
                old.logger_calls, fresh.logger_calls,
                old.profile.guest_calls, fresh.profile.guest_calls,
                old.profile.guest_lookups, fresh.profile.guest_lookups,
                old.profile.sync_fastpath, fresh.profile.sync_fastpath,
                old.import_calls[60], fresh.import_calls[60],
                old.import_calls[61], fresh.import_calls[61],
                old.function_entries, fresh.function_entries,
                old.lookup_hits, fresh.lookup_hits,
                old.sampler_word, fresh.sampler_word,
                old.escape_kind, old.escape_target,
                fresh.escape_kind, fresh.escape_target,
                memcmp(s_old_arena, s_arena, ARENA_BYTES) ? "DIFFERS" : "same",
                arena_first_diff(s_old_arena, s_arena),
                memcmp(s_old_iat, s_iat, IAT_BYTES) ? "DIFFERS" : "same");
        return 1;
    }
    if (old_escaped && handled) {
        fprintf(stderr, "seam handled a non-terminating state at step %u "
                        "(guest_call %08x)\n", step, old.escape_target);
        return 1;
    }
    if (expect >= 0 && handled != expect) {
        fprintf(stderr, "seam decision %d != expected %d at step %u fn=%d "
                        "(old_left=%d escaped=%d reason=%u)\n",
                handled, expect, step, fn, old_left, old_escaped,
                (unsigned)stats->last_reason);
        dump_cpu_diff(&before.cpu, &fresh.cpu);
        return 1;
    }
    if (handled)
        ++s_handled[fn];
    else
        ++s_rejections;
    if (old_escaped)
        ++s_escapes;
    s_step_left = old_left;
    s_step_escaped = old_escaped;
    clear_fault(c);
    return 0;
}

/* ---- independent acceptance predicate --------------------------------- */

static int readable(uint32_t address, uint32_t bytes)
{
    uint64_t a = address, e = (uint64_t)address + bytes;
    if (a >= s_base && e <= (uint64_t)s_base + ARENA_BYTES)
        return 1;
    if (a >= IAT_PAGE && e <= (uint64_t)IAT_PAGE + IAT_BYTES)
        return 1;
    return 0;
}

static int overlap64(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len)
{
    uint64_t a0 = a, a1 = (uint64_t)a + a_len;
    uint64_t b0 = b, b1 = (uint64_t)b + b_len;
    return a0 < b1 && b0 < a1;
}

static int frame_inside(const CPU *c, uint32_t frame)
{
    uint64_t floor = c->stack_floor, ceiling = c->stack_ceiling;
    uint64_t esp = c->esp;
    if (c->stack_owner != c || floor >= ceiling)
        return 0;
    return esp >= floor + frame && esp + 4U <= ceiling;
}

static int target_ok(uint32_t word, uint32_t rva)
{
    return word == rva || word == GUEST_IMAGE_BASE + rva;
}

static int slot_ok(uint32_t slot_rva)
{
    uint32_t word = ld32(GUEST_IMAGE_BASE + slot_rva);
    return g_isaac_vita_import_ids_ready && target_ok(word, slot_rva);
}

static int cs_object_ok(uint32_t cs)
{
    if ((cs & 3U) != 0U || cs < 4U || cs > UINT32_C(0xffffffe8))
        return 0;
    if (!readable(cs, 0x1CU))
        return 0;                      /* never generated; defensive */
    return ld32(cs) == ISAAC_VITA_SYNC_CS_MAGIC &&
           ld32(cs + 4U) == (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE &&
           ld32(cs + 12U) == ISAAC_VITA_SYNC_CS_VERSION;
}

/* Written from the design (section 4.2) with 64-bit interval endpoints;
 * the seam's modular one-compare form must agree on every generated state. */
static int expect_helper(const CPU *c, int fn)
{
    uint32_t frame = frame_bytes(fn);
    uint32_t object = c->ecx, mvt, cs, cvt = 0U, strong, owner, depth, inner;
    int alive;

    if (c->vita_sync_thread_id != g_isaac_vita_sync_inline_owner)
        return 0;
    if ((c->esp & 3U) != 0U)
        return 0;
    if (!frame_inside(c, frame))
        return 0;
    if (!slot_ok(ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS) ||
        !slot_ok(ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS))
        return 0;
    if (!readable(object, 0x18U))
        return 0;
    mvt = ld32(object + 8U);
    if (!readable(mvt + 0xCU, 8U))
        return 0;
    if (!target_ok(ld32(mvt + 0xCU), LOCK_RVA) ||
        !target_ok(ld32(mvt + 0x10U), UNLOCK_RVA))
        return 0;
    if (ld8(object + 0xCU) == 0U)
        return 0;
    cs = ld32(object + 0x10U);
    if (!cs_object_ok(cs))
        return 0;
    owner = ld32(cs + 16U);
    depth = ld32(cs + 20U);
    if (owner == c->vita_sync_thread_id) {
        if (depth == 0U || depth == UINT32_MAX)
            return 0;
    } else if (owner != 0U || depth != 0U) {
        return 0;
    }
    if (ld8(cs + 0x18U) != 0U)
        return 0;
    strong = ld16(object + 4U);
    alive = fn == FN_WEAKLOCK && strong != 0U;
    if (alive) {
        cvt = ld32(object);
        if (!readable(cvt + 8U, 4U))
            return 0;
        if (!target_ok(ld32(cvt + 8U), ADDREF_RVA))
            return 0;
    }
    inner = c->esp - frame;
    if (overlap64(inner, frame, object, 0x18U) ||
        overlap64(inner, frame, cs, 0x1CU) ||
        overlap64(inner, frame, mvt + 0xCU, 8U) ||
        overlap64(inner, frame, IAT_SYNC_WORDS, 8U) ||
        overlap64(cs, 0x1CU, object, 0x18U) ||
        overlap64(cs, 0x1CU, mvt + 0xCU, 8U) ||
        overlap64(cs, 0x1CU, IAT_SYNC_WORDS, 8U) ||
        overlap64(object + 4U, 2U, mvt + 0xCU, 8U) ||
        overlap64(object + 4U, 2U, IAT_SYNC_WORDS, 8U))
        return 0;
    if (alive &&
        (overlap64(inner, frame, cvt + 8U, 4U) ||
         overlap64(cs, 0x1CU, cvt + 8U, 4U) ||
         overlap64(object + 4U, 2U, cvt + 8U, 4U)))
        return 0;
    if (fn == FN_RELEASE && strong < 2U)
        return 0;
    return 1;
}

static int step(CPU *c, int fn, uint32_t number)
{
    return differential_step(c, fn, expect_helper(c, fn), number);
}

/* ---- random program ---------------------------------------------------- */

static uint64_t s_rng = UINT64_C(0x9e3779b97f4a7c15);

static uint32_t rnd(void)
{
    uint64_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s_rng = x;
    return (uint32_t)(x >> 11);
}

static uint32_t rnd_below(uint32_t n)
{
    return rnd() % n;
}

static void write_frame(CPU *c, uint32_t esp, uint32_t return_word)
{
    c->esp = esp;
    if (esp - s_base < ARENA_BYTES - 4U)
        memcpy(s_arena + (esp - s_base), &return_word, 4U);
}

static void randomize_registers(CPU *c)
{
    c->eax = rnd();
    c->ebx = rnd();
    c->edx = rnd();
    c->esi = rnd();
    c->edi = rnd();
    c->ebp = rnd();
}

static uint32_t pick_esp(void)
{
    uint32_t floor = s_base + STACK_FLOOR_OFF;
    uint32_t ceiling = s_base + STACK_CEIL_OFF;
    uint32_t r = rnd_below(100U);

    if (r < 80U)
        return floor + 64U + rnd_below((ceiling - floor - 96U) / 4U) * 4U;
    switch (r - 80U) {
    case 0:  return floor + 40U;        /* first valid AddRef/Release frame */
    case 1:  return floor + 36U;
    case 2:  return floor + 44U;        /* weak dead: over-rejection window */
    case 3:  return floor + 48U;
    case 4:  return floor + 52U;        /* first valid weak-lock frame */
    case 5:  return ceiling - 4U;       /* last valid frame */
    case 6:  return ceiling;
    case 7:  return ceiling + 4U;
    case 8:  return floor;
    case 9:  return floor - 8U;
    case 10: return floor + 66U;        /* misaligned, in range */
    case 11: return floor + 65U;
    case 12: return floor + 67U;
    case 13: return ceiling - 6U;
    case 14: return s_base + 0x800U;    /* far below the floor */
    case 15: return floor + 56U;
    default: return floor + 128U;
    }
}

static uint32_t pick_strong(void)
{
    uint32_t r = rnd_below(100U);
    if (r < 8U) return 0U;
    if (r < 16U) return 1U;
    if (r < 20U) return 0xFFFFU;
    if (r < 23U) return 0xFFFEU;
    if (r < 26U) return 0x7FFFU;
    if (r < 30U) return 2U;
    return rnd_below(0x10000U);
}

static void corrupt(uint32_t index, uint32_t self)
{
    uint32_t object = object_address(index);
    uint32_t address = cs_address(index);
    switch (rnd_below(25U)) {
    case 0: st32(address, ISAAC_VITA_SYNC_CS_MAGIC ^ 1U); break;
    case 1: st32(address + 4U, 0x40010003U); break;
    case 2: st32(address + 12U, 1U); break;
    case 3: st32(address + 16U, (uint32_t)CONTENDER_THREAD_ID);
            st32(address + 20U, 1U); break;                /* foreign owner */
    case 4: st32(address + 16U, UINT32_MAX); st32(address + 20U, 0U); break;
    case 5: st32(address + 16U, self); st32(address + 20U, 0U); break;
    case 6: st32(address + 16U, self); st32(address + 20U, UINT32_MAX); break;
    case 7: st32(address + 16U, 0U); st32(address + 20U, 3U); break;
    case 8: st32(address + 16U, self); st32(address + 20U, UINT32_MAX - 1U);
            break;
    case 9: st8(address + 0x18U, 1U); break;               /* busy flag */
    case 10: st8(object + 0xCU, 0U); break;                /* uninitialized */
    case 11: st32(object + 0x10U, address + 2U); break;    /* misaligned CS */
    case 12: st32(object + 0x10U, 0U); break;
    case 13: st32(object + 0x10U, UINT32_C(0xfffffff0)); break;
    case 14: st32(object + 8U, mvt_row(MVT_FROZEN_RVA)); break;
    case 15: st32(object + 8U, mvt_row(MVT_LOCK_FOREIGN)); break;
    case 16: st32(object + 8U, mvt_row(MVT_UNLOCK_FOREIGN)); break;
    case 17: st32(object, cvt_row(CVT_FROZEN_RVA)); break;
    case 18: st32(object, cvt_row(CVT_FOREIGN)); break;
    case 19: st32(object + 0x14U, s_base + TOBJ_OFF); break; /* live T */
    case 20: st32(address + 16U, self); st32(address + 20U, 1U); break;
    case 21: st32(address + 16U, self); st32(address + 20U, 2U); break;
    case 22: st32(object + 0x10U, cs_address((index + 1U) % OBJECT_COUNT));
             break;
    default: make_free(index); break;
    }
}

static int random_phase(CPU *c, uint32_t steps)
{
    uint32_t number;
    uint32_t self = c->vita_sync_thread_id;

    for (number = 0U; number < steps; ++number) {
        uint32_t index = rnd_below(OBJECT_COUNT);
        uint32_t r = rnd_below(100U);
        uint32_t object = object_address(index);
        uint32_t esp;
        int fn = r < 40U ? FN_WEAKLOCK : r < 70U ? FN_ADDREF : FN_RELEASE;
        int left;

        st16(object + 4U, (uint16_t)pick_strong());
        st16(object + 6U, (uint16_t)rnd_below(0x10000U));
        if (rnd_below(100U) < 8U)
            corrupt(index, self);
        if (rnd_below(2000U) == 0U)
            st32(ENTER_SLOT, rnd_below(2U) ? ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS
                                           : 0x12345678U);
        if (rnd_below(2000U) == 0U)
            st32(LEAVE_SLOT, rnd_below(2U) ? ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS
                                           : 0U);
        randomize_registers(c);
        esp = pick_esp();
        write_frame(c, esp, caller_return(fn));
        c->ecx = object;
        /* Objects at the frame boundary: at the return word (the body never
         * writes at or above E) or immediately below the pushed frame; both
         * must stay handled.  The return word is then the object's own first
         * word, exactly as a caller whose control block sits there. */
        if (rnd_below(100U) < 3U && esp - s_base >= 128U &&
            esp - s_base < ARENA_BYTES - 64U) {
            uint32_t where = rnd_below(3U);
            uint32_t at = where == 0U ? esp : esp + 8U;
            if (where == 2U) {
                /* Immediately below the pushed frame only a well-formed
                 * object is placed: a corrupted one can take a body path
                 * that pushes deeper than the seam's frame (KAGE::Log on an
                 * uninitialized Mutex) and overwrite itself -- a hostile
                 * state the x86 body faults on as well, and one that leaves
                 * the harness through unreadable pointers. */
                uint32_t strong = ld16(object + 4U);
                make_free(index);
                st16(object + 4U, (uint16_t)strong);
                at = esp - frame_bytes(fn) - 0x18U;
            }
            memcpy(s_arena + (at - s_base), s_arena + (object - s_base), 0x18U);
            c->ecx = at;
        }
        if (step(c, fn, number))
            return 1;
        left = c->fault != NULL;
        clear_fault(c);
        if (left || rnd_below(8U) == 0U)
            make_free(index);
        if (rnd_below(128U) == 0U)
            iat_tokens();
    }
    return 0;
}

/* ---- main --------------------------------------------------------------- */

static void cpu_init(CPU *c)
{
    memset(c, 0, sizeof *c);
    c->stack_owner = c;
    c->stack_floor = s_base + STACK_FLOOR_OFF;
    c->stack_ceiling = s_base + STACK_CEIL_OFF;
    c->stack_low_water = c->stack_ceiling;
    c->esp = c->stack_ceiling;
    c->ebp = c->stack_ceiling;
    c->stop_kind = GUEST_RUN_RETURNED;
}

static void reset_counters(void)
{
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    memset(g_guest_phase_profile_import_calls, 0,
           sizeof g_guest_phase_profile_import_calls);
    memset(s_coverage_imports, 0, sizeof s_coverage_imports);
    memset(s_coverage_functions, 0, sizeof s_coverage_functions);
    g_host_import_calls = 0U;
    g_guest_function_entries = 0U;
    g_kage_guest_last_indirect_target = 0x00007b70U;   /* a vtable target */
}

#define SET_REGS(c, k) do { \
    (c)->eax = 0xa5a5a500U + (k); (c)->ebx = 0x11110000U + (k); \
    (c)->esi = 0x22220000U + (k); (c)->edi = 0x33330000U + (k); \
    (c)->ebp = 0x44440000U + (k); (c)->edx = 0x55550000U + (k); } while (0)

#define LOW_WATER(esp, inner) \
    (GUEST_GENERATED_STACK_GUARD ? (esp) - (inner) : (esp) - (inner) + 4U)

static int check_regs_kept(const CPU *c, uint32_t k, uint32_t object)
{
    return c->ecx == object + 8U && c->edx == 0x55550000U + k &&
           c->ebx == 0x11110000U + k && c->esi == 0x22220000U + k &&
           c->edi == 0x33330000U + k && c->ebp == 0x44440000U + k &&
           !c->fault;
}

int main(int argc, char **argv)
{
    CPU cpu;
    uint32_t steps = 200000U;
    uint32_t i;
    uint32_t k;
    void *iat_map;
    observation before, after;
    const isaac_vita_kage_refcount_seam_stats *stats =
        isaac_vita_kage_refcount_seam_stats_get();

    setvbuf(stdout, NULL, _IONBF, 0);
    s_trace = getenv("ORACLE_TRACE") != NULL;
    if (argc > 1)
        steps = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        s_rng = strtoull(argv[2], NULL, 0) | 1U;

    s_arena = map_low(ARENA_BYTES, 0U);
    CHECK(s_arena != NULL);
    CHECK((uintptr_t)s_arena <= UINT32_MAX - ARENA_BYTES);
    s_base = (uint32_t)(uintptr_t)s_arena;
    memset(s_arena, 0, ARENA_BYTES);
    iat_map = map_low(IAT_MAP_BYTES, (uintptr_t)IAT_MAP_BASE);
    CHECK(iat_map != NULL && (uintptr_t)iat_map == IAT_MAP_BASE);
    s_iat = (uint8_t *)(uintptr_t)IAT_PAGE;
    memset((uint8_t *)iat_map, 0, IAT_MAP_BYTES);
    iat_tokens();
    g_isaac_vita_import_ids_ready = 1;

    /* Park the live-object census where no replayed operation can cross a
     * power of two (the create receipt fires there). */
    for (i = 0U; i < 140000U; ++i)
        CHECK(isaac_vita_sync_create_recursive_mutex() ==
              ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
    make_vtables();
    for (i = 0U; i < OBJECT_COUNT; ++i)
        make_free(i);
    for (i = CS_SPARE_INDEX; i < CS_SPARE_INDEX + 4U; ++i)
        make_cs(cs_address(i));
    reset_counters();

    /* Phase 0: unarmed latch and unbound CPU never take the seam (all three
     * helpers; the body would disarm the latch through the endpoint, so the
     * seam is exercised alone and must touch nothing). */
    cpu_init(&cpu);
    CHECK(g_isaac_vita_sync_inline_owner == ISAAC_VITA_SYNC_INLINE_OWNER_NONE);
    for (k = 0U; k < 3U; ++k) {
        int fn = (int)k;
        observation pre, post;
        cpu.ecx = object_address(0U);
        write_frame(&cpu, cpu.stack_ceiling - 64U, caller_return(fn));
        guest_coverage_function(coverage_id(fn));
        observe(&pre, &cpu);
        memcpy(s_snapshot, s_arena, ARENA_BYTES);
        CHECK(run_try(&cpu, fn) == 0);
        observe(&post, &cpu);
        CHECK(observations_identical(&pre, &post) &&
              memcmp(s_snapshot, s_arena, ARENA_BYTES) == 0);
        CHECK(stats->last_reason == ISAAC_KRS_REJECT_LATCH);
    }
    CHECK(s_banner_lines == 1U && s_banner_first == 1U);
    CHECK(strcmp(s_banner_text,
                 "[isaac-kage] refcount seam: roots=7af0,7b50,7b70 "
                 "lock=00562e00 unlock=00562ec0 addref=00007b50 "
                 "slots=006060fc,006060f8 frame=40/52 verify="
                 ORACLE_VERIFY_TEXT " build=refcount-seam:unstamped") == 0);
    CHECK(s_fallback_by_reason[ISAAC_KRS_REJECT_LATCH] == 1U);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(cpu.vita_sync_thread_id == (uint32_t)MAIN_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);

    /* Phase 1: directed success states with the exact expected values of
     * the design's dead-word tables (4.3) and census rows (4.4). */
    {
        uint32_t object = object_address(0U);
        uint32_t cs = cs_address(0U);
        uint32_t esp = cpu.stack_ceiling - 64U;
        static const uint32_t strongs[] = {
            0U, 1U, 2U, 3U, 0x7ffeU, 0x7fffU, 0x8000U, 0xfffeU, 0xffffU
        };

        /* AddRef: every count value, u16 wrap. */
        for (k = 0U; k < sizeof strongs / sizeof strongs[0]; ++k) {
            make_free(0U);
            st16(object + 4U, (uint16_t)strongs[k]);
            st16(object + 6U, 0x1234U);
            SET_REGS(&cpu, k);
            cpu.ecx = object;
            cpu.stack_low_water = cpu.stack_ceiling;
            write_frame(&cpu, esp, caller_return(FN_ADDREF));
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, FN_ADDREF, 1, 1000U + k));
            observe(&after, &cpu);
            CHECK(ld16(object + 4U) == (uint16_t)(strongs[k] + 1U));
            CHECK(ld16(object + 6U) == 0x1234U);
            CHECK(cpu.eax == cs);
            CHECK(check_regs_kept(&cpu, k, object) && cpu.esp == esp + 4U);
            CHECK(memcmp(&before.cpu.fl, &after.cpu.fl, sizeof after.cpu.fl) == 0);
            CHECK(ld32(esp - 4U) == 0x22220000U + k &&
                  ld32(esp - 8U) == cs &&
                  ld32(esp - 12U) == RET_LEAVE_CS &&
                  ld32(esp - 16U) == RET_ADDREF_LOCK &&
                  ld32(esp - 20U) == 0x44440000U + k &&
                  ld32(esp - 24U) == 0x11110000U + k &&
                  ld32(esp - 28U) == object &&
                  ld32(esp - 32U) == 0x33330000U + k &&
                  ld32(esp - 36U) == cs &&
                  ld32(esp - 40U) == RET_ENTER_CS);
            CHECK(cpu.stack_low_water == LOW_WATER(esp, 40U));
            CHECK(ld32(cs + 16U) == 0U && ld32(cs + 20U) == 0U &&
                  ld8(cs + 0x18U) == 0U);
            /* The census the translated route records for one handled
             * AddRef: the two seam-handled imports (guest.c's NOTE_CALL +
             * SYNC_FASTPATH + IMPORT + coverage + host import count) and
             * three body entries; the two direct edges record nothing
             * (no ISAAC_VITA_PHASE_PROFILE in a generated unit), so
             * g(l) and the lookup-cache hits stay put. */
            CHECK(after.host_import_calls == before.host_import_calls + 2U);
            CHECK(after.profile.guest_calls == before.profile.guest_calls + 2U);
            CHECK(after.profile.guest_lookups == before.profile.guest_lookups);
            CHECK(after.lookup_hits == before.lookup_hits);
            CHECK(after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 2U);
            CHECK(after.import_calls[61] == before.import_calls[61] + 1U &&
                  after.import_calls[60] == before.import_calls[60] + 1U);
            CHECK(after.coverage_imports[0] == 1U &&
                  after.coverage_imports[1] == 1U);
            CHECK(after.coverage_functions[0] == 0U &&
                  after.coverage_functions[1] == 0U);
            CHECK(after.function_entries ==
                  before.function_entries + 3U + (uint32_t)ORACLE_VERIFY);
            CHECK(after.sampler_word == 0x00007b70U);
            CHECK(after.faults == before.faults &&
                  after.logger_calls == before.logger_calls);
        }
        CHECK(s_handled[FN_ADDREF] == 9U);

        /* Release with count >= 2. */
        for (k = 2U; k < sizeof strongs / sizeof strongs[0]; ++k) {
            make_free(0U);
            st16(object + 4U, (uint16_t)strongs[k]);
            SET_REGS(&cpu, k);
            cpu.ecx = object;
            cpu.stack_low_water = cpu.stack_ceiling;
            write_frame(&cpu, esp, caller_return(FN_RELEASE));
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, FN_RELEASE, 1, 1100U + k));
            observe(&after, &cpu);
            CHECK(ld16(object + 4U) == (uint16_t)(strongs[k] - 1U));
            CHECK(cpu.eax == ((cs & 0xFFFFFF00U) | 1U));
            CHECK(check_regs_kept(&cpu, k, object) && cpu.esp == esp + 4U);
            CHECK(ld32(esp - 4U) == 0x22220000U + k &&
                  ld32(esp - 8U) == 0x33330000U + k &&
                  ld32(esp - 12U) == RET_RELEASE_UNLOCK &&
                  ld32(esp - 16U) == object &&
                  ld32(esp - 20U) == cs &&
                  ld32(esp - 24U) == RET_LEAVE_CS &&
                  ld32(esp - 28U) == object &&
                  ld32(esp - 32U) == 0x33330000U + k &&
                  ld32(esp - 36U) == cs &&
                  ld32(esp - 40U) == RET_ENTER_CS);
            CHECK(cpu.stack_low_water == LOW_WATER(esp, 40U));
            CHECK(after.host_import_calls == before.host_import_calls + 2U);
            CHECK(after.profile.guest_calls == before.profile.guest_calls + 2U);
            CHECK(after.profile.guest_lookups == before.profile.guest_lookups);
            CHECK(after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 2U);
            CHECK(after.function_entries ==
                  before.function_entries + 3U + (uint32_t)ORACLE_VERIFY);
            CHECK(after.coverage_functions[2] == 0U);
        }
        CHECK(s_handled[FN_RELEASE] == 7U);

        /* Release with count 0: Lock, Unlock, al = 1 -- runs translated. */
        make_free(0U);
        st16(object + 4U, 0U);
        SET_REGS(&cpu, 0U);
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_RELEASE));
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 1200U));
        CHECK(stats->last_reason == ISAAC_KRS_REJECT_ZERO);
        CHECK(ld16(object + 4U) == 0U && cpu.eax == ((cs & 0xFFFFFF00U) | 1U) &&
              cpu.esp == esp + 4U && !cpu.fault);
        /* Release with count 1: the destroy path leaves the harness through
         * the T destructor (live T) or the ReleaseWeak tail jump (no T). */
        make_free(0U);
        st16(object + 4U, 1U);
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_RELEASE));
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 1201U));
        CHECK(stats->last_reason == ISAAC_KRS_REJECT_LAST);
        CHECK(s_escapes == 1U && ld16(object + 4U) == 0U);
        make_free(0U);
        st16(object + 4U, 1U);
        st32(object + 0x14U, s_base + TOBJ_OFF);
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_RELEASE));
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 1202U));
        CHECK(stats->last_reason == ISAAC_KRS_REJECT_LAST && s_escapes == 2U);
        CHECK(ld32(cs + 16U) == 0U && ld8(cs + 0x18U) == 0U);
        make_free(0U);

        /* Weak-lock, dead (count 0): no count change, al = 0. */
        for (k = 0U; k < 3U; ++k) {
            make_free(0U);
            st16(object + 4U, 0U);
            st16(object + 6U, (uint16_t)(7U + k));
            SET_REGS(&cpu, k);
            cpu.ecx = object;
            cpu.stack_low_water = cpu.stack_ceiling;
            write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 1300U + k));
            observe(&after, &cpu);
            CHECK(ld16(object + 4U) == 0U && ld16(object + 6U) == 7U + k);
            CHECK(cpu.eax == (cs & 0xFFFFFF00U));
            CHECK(check_regs_kept(&cpu, k, object) && cpu.esp == esp + 4U);
            CHECK(ld32(esp - 4U) == 0x22220000U + k &&
                  ld32(esp - 8U) == 0x33330000U + k &&
                  ld32(esp - 12U) == RET_WEAK_DEAD_UNLOCK &&
                  ld32(esp - 16U) == 0x22220000U + k &&
                  ld32(esp - 20U) == cs &&
                  ld32(esp - 24U) == RET_LEAVE_CS &&
                  ld32(esp - 28U) == 0x22220000U + k &&
                  ld32(esp - 32U) == object &&
                  ld32(esp - 36U) == cs &&
                  ld32(esp - 40U) == RET_ENTER_CS);
            CHECK(cpu.stack_low_water == LOW_WATER(esp, 40U));
            CHECK(after.host_import_calls == before.host_import_calls + 2U);
            CHECK(after.profile.guest_calls == before.profile.guest_calls + 2U);
            CHECK(after.profile.guest_lookups == before.profile.guest_lookups);
            CHECK(after.function_entries ==
                  before.function_entries + 3U + (uint32_t)ORACLE_VERIFY);
            CHECK(stats->weak_dead == k + 1U);
        }

        /* Weak-lock, alive: Unlock then AddRef (two pairs), u16 wrap. */
        for (k = 1U; k < sizeof strongs / sizeof strongs[0]; ++k) {
            make_free(0U);
            st16(object + 4U, (uint16_t)strongs[k]);
            SET_REGS(&cpu, k);
            cpu.ecx = object;
            cpu.stack_low_water = cpu.stack_ceiling;
            write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 1400U + k));
            observe(&after, &cpu);
            CHECK(ld16(object + 4U) == (uint16_t)(strongs[k] + 1U));
            CHECK(cpu.eax == ((cs & 0xFFFFFF00U) | 1U));
            CHECK(check_regs_kept(&cpu, k, object) && cpu.esp == esp + 4U);
            CHECK(ld32(esp - 4U) == 0x22220000U + k &&
                  ld32(esp - 8U) == 0x33330000U + k &&
                  ld32(esp - 12U) == RET_WEAK_ADDREF &&
                  ld32(esp - 16U) == 0x22220000U + k &&
                  ld32(esp - 20U) == cs &&
                  ld32(esp - 24U) == RET_LEAVE_CS &&
                  ld32(esp - 28U) == RET_ADDREF_LOCK &&
                  ld32(esp - 32U) == 0x44440000U + k &&
                  ld32(esp - 36U) == 0x11110000U + k &&
                  ld32(esp - 40U) == object &&
                  ld32(esp - 44U) == object &&
                  ld32(esp - 48U) == cs &&
                  ld32(esp - 52U) == RET_ENTER_CS);
            CHECK(cpu.stack_low_water == LOW_WATER(esp, 52U));
            /* Two pairs and the AddRef body: four imports, six entries. */
            CHECK(after.host_import_calls == before.host_import_calls + 4U);
            CHECK(after.profile.guest_calls == before.profile.guest_calls + 4U);
            CHECK(after.profile.guest_lookups == before.profile.guest_lookups);
            CHECK(after.lookup_hits == before.lookup_hits);
            CHECK(after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 4U);
            CHECK(after.import_calls[61] == before.import_calls[61] + 2U &&
                  after.import_calls[60] == before.import_calls[60] + 2U);
            CHECK(after.coverage_functions[3] == 0U &&
                  after.coverage_functions[4] == 0U);
            CHECK(after.function_entries ==
                  before.function_entries + 6U + (uint32_t)ORACLE_VERIFY);
            CHECK(after.sampler_word == 0x00007b70U);
        }
        CHECK(s_handled[FN_WEAKLOCK] == 11U && stats->weak_alive == 8U);

        /* Function-coverage ids.  With the pointer temporarily live the
         * seam's replayed entry hooks name exactly the elided bodies
         * (Lock, Unlock, and AddRef on the alive path) and never its own
         * root; the plain bodies, compiled GUEST_COVERAGE_HOOKS=0 like a
         * generated unit, write no byte at all.  Host-only: on the Vita
         * the pointer is NULL for both routes (see the definition). */
        {
            uint32_t nonzero = 0U;
            g_guest_coverage_functions = s_coverage_functions;
            memset(s_coverage_functions, 0, sizeof s_coverage_functions);
            make_free(0U);
            st16(object + 4U, 5U);
            SET_REGS(&cpu, 0U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
            CHECK(run_try(&cpu, FN_WEAKLOCK) == 1);
            CHECK(s_coverage_functions[COVERAGE_LOCK] == 1U &&
                  s_coverage_functions[COVERAGE_UNLOCK] == 1U &&
                  s_coverage_functions[COVERAGE_ADDREF] == 1U &&
                  s_coverage_functions[COVERAGE_WEAKLOCK] == 0U &&
                  s_coverage_functions[COVERAGE_RELEASE] == 0U);
            clear_fault(&cpu);
            memset(s_coverage_functions, 0, sizeof s_coverage_functions);
            make_free(0U);
            st16(object + 4U, 5U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_RELEASE));
            CHECK(run_try(&cpu, FN_RELEASE) == 1);
            CHECK(s_coverage_functions[COVERAGE_LOCK] == 1U &&
                  s_coverage_functions[COVERAGE_UNLOCK] == 1U &&
                  s_coverage_functions[COVERAGE_ADDREF] == 0U &&
                  s_coverage_functions[COVERAGE_RELEASE] == 0U);
            clear_fault(&cpu);
            memset(s_coverage_functions, 0, sizeof s_coverage_functions);
            make_free(0U);
            st16(object + 4U, 5U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
            CHECK(run_body(&cpu, plain_body(FN_WEAKLOCK)) == 0);
            for (k = 0U; k < sizeof s_coverage_functions; ++k)
                nonzero += s_coverage_functions[k];
            CHECK(nonzero == 0U && ld16(object + 4U) == 6U);
            clear_fault(&cpu);
            g_guest_coverage_functions = NULL;
            make_free(0U);
        }

        /* Recursive owner (owner self, depth k): the net no-op holds. */
        for (k = 1U; k <= 3U; ++k) {
            make_free(0U);
            st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
            st32(cs + 20U, k);
            st16(object + 4U, 5U);
            SET_REGS(&cpu, k);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_ADDREF));
            CHECK(!differential_step(&cpu, FN_ADDREF, 1, 1500U + k));
            CHECK(ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID &&
                  ld32(cs + 20U) == k && ld8(cs + 0x18U) == 0U);
            CHECK(ld16(object + 4U) == 6U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 1510U + k));
            CHECK(ld32(cs + 20U) == k && ld16(object + 4U) == 7U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_RELEASE));
            CHECK(!differential_step(&cpu, FN_RELEASE, 1, 1520U + k));
            CHECK(ld32(cs + 20U) == k && ld16(object + 4U) == 6U);
        }
        /* Depth UINT32_MAX - 1: Enter reaches the maximum, Leave returns. */
        make_free(0U);
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(cs + 20U, UINT32_MAX - 1U);
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_ADDREF));
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 1530U));
        CHECK(ld32(cs + 20U) == UINT32_MAX - 1U);
        make_free(0U);
        CHECK(s_rejections == 3U);
        printf("directed success: addref=%u release=%u weaklock=%u "
               "(alive=%u dead=%u)\n",
               s_handled[FN_ADDREF], s_handled[FN_RELEASE],
               s_handled[FN_WEAKLOCK], stats->weak_alive, stats->weak_dead);
    }

    /* Phase 1b: the unit text.  The new route above is spelled as
     * `entry hook; if (!try) rest` -- the exact unit body (sub_X with the
     * fenced seam kept) must produce the identical state and census from the
     * same snapshot, handled and rejected alike. */
    {
        uint32_t object = object_address(1U);
        uint32_t esp = cpu.stack_ceiling - 96U;
        static const struct { int fn; uint32_t strong; uint32_t esp_delta; }
            cases[] = {
            { FN_ADDREF, 4U, 0U }, { FN_RELEASE, 4U, 0U },
            { FN_WEAKLOCK, 0U, 0U }, { FN_WEAKLOCK, 4U, 0U },
            { FN_RELEASE, 0U, 0U }, { FN_ADDREF, 4U, 2U },
            { FN_WEAKLOCK, 3U, 0x2000U + 8U },
        };
        static counters_snapshot counters;
        observation entry, route_a, route_b, route_c;
        static uint8_t arena_a[ARENA_BYTES];
        CPU saved;
        uint32_t handled_a, handled_b, calls_a, calls_b;
        uint32_t extra_entries;

        for (k = 0U; k < sizeof cases / sizeof cases[0]; ++k) {
            int fn = cases[k].fn;
            make_free(1U);
            st16(object + 4U, (uint16_t)cases[k].strong);
            SET_REGS(&cpu, k);
            cpu.ecx = object;
            write_frame(&cpu, esp + cases[k].esp_delta, caller_return(fn));
            saved = cpu;
            counters_save(&counters);
            memcpy(s_snapshot, s_arena, ARENA_BYTES);
            observe(&entry, &cpu);
            handled_a = stats_handled_total();
            calls_a = stats->calls[fn];
            guest_coverage_function(coverage_id(fn));
            if (run_try(&cpu, fn) == 0)
                (void)run_body(&cpu, rest_body(fn));
            handled_a = stats_handled_total() - handled_a;
            calls_a = stats->calls[fn] - calls_a;
            observe(&route_a, &cpu);
            memcpy(arena_a, s_arena, ARENA_BYTES);

            /* B: the exact unit text (entry hook, fenced seam, rest). */
            cpu = saved;
            counters_restore(&counters);
            memcpy(s_arena, s_snapshot, ARENA_BYTES);
            handled_b = stats_handled_total();
            calls_b = stats->calls[fn];
            (void)run_body(&cpu, unit_body(fn));
            handled_b = stats_handled_total() - handled_b;
            calls_b = stats->calls[fn] - calls_b;
            observe(&route_b, &cpu);
            if (!observations_identical(&route_a, &route_b) ||
                memcmp(arena_a, s_arena, ARENA_BYTES) != 0 ||
                handled_a != handled_b || calls_a != calls_b) {
                fprintf(stderr, "unit text differs from the decomposed route "
                                "(case %u fn=%d handled %u/%u calls %u/%u)\n",
                        k, fn, handled_a, handled_b, calls_a, calls_b);
                dump_cpu_diff(&route_a.cpu, &route_b.cpu);
                return 1;
            }
            CHECK(handled_a == (k < 4U ? 1U : 0U) && calls_a == 1U);
            clear_fault(&cpu);

            /* C: the production body without the knob (entry hook inside,
             * no seam, nested AddRef plain): the same state and census as
             * the seamed unit, up to VERIFY's documented double count. */
            cpu = saved;
            counters_restore(&counters);
            memcpy(s_arena, s_snapshot, ARENA_BYTES);
            (void)run_body(&cpu, plain_body(fn));
            observe(&route_c, &cpu);
            extra_entries = ORACLE_VERIFY ? handled_a : 0U;
            if (!observations_equal(&entry, &route_c, &route_a, extra_entries) ||
                memcmp(arena_a, s_arena, ARENA_BYTES) != 0) {
                fprintf(stderr, "seamed unit differs from the plain body "
                                "(case %u fn=%d handled %u)\n",
                        k, fn, handled_a);
                dump_cpu_diff(&route_c.cpu, &route_a.cpu);
                return 1;
            }
            clear_fault(&cpu);
        }
        make_free(1U);
        printf("unit text identity: %u cases (decomposed == unit == plain)\n",
               (unsigned)(sizeof cases / sizeof cases[0]));
    }

    /* Phase 2: every fallback reason, directed. */
    {
        uint32_t object = object_address(1U);
        uint32_t cs = cs_address(1U);
        uint32_t esp = cpu.stack_ceiling - 96U;
        uint32_t floor = cpu.stack_floor, ceiling = cpu.stack_ceiling;

#define FRAME_FOR(fn, at) do { \
        cpu.ecx = object; cpu.eax = 0x0badf00dU; \
        write_frame(&cpu, (at), caller_return(fn)); } while (0)
#define EXPECT_REASON(reason) do { \
        if (s_step_reason != (uint32_t)(reason)) \
            fprintf(stderr, "seam reason %s, expected %s\n", \
                    s_reason_names[s_step_reason], \
                    s_reason_names[(reason)]); \
        CHECK(s_step_reason == (uint32_t)(reason)); } while (0)

        /* esp: every misalignment, all three helpers (the body completes on
         * the host; on ARM the STRD pushes would fault). */
        for (k = 1U; k <= 3U; ++k) {
            make_free(1U);
            FRAME_FOR(FN_ADDREF, esp + k);
            CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2000U + k));
            EXPECT_REASON(ISAAC_KRS_REJECT_ESP);
            make_free(1U);
            FRAME_FOR(FN_WEAKLOCK, esp + k);
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2010U + k));
            EXPECT_REASON(ISAAC_KRS_REJECT_ESP);
            make_free(1U);
            FRAME_FOR(FN_RELEASE, esp + k);
            CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2020U + k));
            EXPECT_REASON(ISAAC_KRS_REJECT_ESP);
        }

        /* frame: the 40/52-byte boundaries at the floor, the return word at
         * the ceiling.  The weak-lock dead path needs only 40 bytes but the
         * seam (and the acceptance predicate) use 52 on both paths: the
         * window (floor+40, floor+52) is the documented over-rejection. */
        make_free(1U);
        FRAME_FOR(FN_ADDREF, floor + 36U);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2100U));
        EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        make_free(1U);
        FRAME_FOR(FN_ADDREF, floor + 40U);
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 2101U));
        make_free(1U);
        FRAME_FOR(FN_RELEASE, floor + 36U);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2102U));
        EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        make_free(1U);
        FRAME_FOR(FN_RELEASE, floor + 40U);
        CHECK(!differential_step(&cpu, FN_RELEASE, 1, 2103U));
        for (k = 40U; k <= 52U; k += 4U) {
            make_free(1U);
            st16(object + 4U, 0U);              /* dead path */
            FRAME_FOR(FN_WEAKLOCK, floor + k);
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, k == 52U, 2110U + k));
            if (k != 52U)
                EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
            make_free(1U);
            FRAME_FOR(FN_WEAKLOCK, floor + k);   /* alive path */
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, k == 52U, 2120U + k));
            if (k != 52U)
                EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        }
        make_free(1U);
        FRAME_FOR(FN_ADDREF, ceiling - 4U);
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 2130U));
        make_free(1U);
        FRAME_FOR(FN_ADDREF, ceiling);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2131U));
        EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        make_free(1U);
        FRAME_FOR(FN_WEAKLOCK, ceiling);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2132U));
        EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        make_free(1U);
        FRAME_FOR(FN_RELEASE, ceiling + 4U);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2133U));
        EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        make_free(1U);
        FRAME_FOR(FN_ADDREF, floor - 8U);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2134U));
        EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
        {
            /* Copied CPU: stack ownership fails (rejection only: a foreign
             * identity's endpoint visit would disarm the latch). */
            CPU second = cpu;
            observation pre, post;
            second.ecx = object;
            write_frame(&second, esp, caller_return(FN_ADDREF));
            guest_coverage_function(COVERAGE_ADDREF);
            observe(&pre, &second);
            CHECK(run_try(&second, FN_ADDREF) == 0);
            observe(&post, &second);
            CHECK(observations_identical(&pre, &post));
            EXPECT_REASON(ISAAC_KRS_REJECT_FRAME);
            /* Unbound and foreign CPU identities: latch. */
            cpu_init(&second);
            second.ecx = object;
            write_frame(&second, second.stack_ceiling - 96U,
                        caller_return(FN_WEAKLOCK));
            CHECK(run_try(&second, FN_WEAKLOCK) == 0);
            EXPECT_REASON(ISAAC_KRS_REJECT_LATCH);
            second.vita_sync_thread_id = (uint32_t)CONTENDER_THREAD_ID;
            write_frame(&second, second.stack_ceiling - 96U,
                        caller_return(FN_RELEASE));
            CHECK(run_try(&second, FN_RELEASE) == 0);
            EXPECT_REASON(ISAAC_KRS_REJECT_LATCH);
        }

        /* slot: a non-token Enter word (the body's Lock takes the generic
         * route), a non-token Leave word only (mixed: the body's Lock
         * completes and its Unlock escapes -- the seam is stricter than the
         * translated route only here, and this state never terminates), the
         * table not registered, and the RVA spelling of both (handled). */
        make_free(1U);
        st32(ENTER_SLOT, 0x12345678U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2200U));
        EXPECT_REASON(ISAAC_KRS_REJECT_SLOT);
        CHECK(s_step_escaped);               /* Lock's generic import */
        iat_tokens();
        make_free(1U);
        st32(LEAVE_SLOT, 0U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2201U));
        EXPECT_REASON(ISAAC_KRS_REJECT_SLOT);
        /* Left owned by the escape: the translated Unlock clears the busy
         * byte, then its Leave import leaves the harness. */
        CHECK(ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID &&
              ld32(cs + 20U) == 1U && ld8(cs + 0x18U) == 0U);
        iat_tokens();
        make_free(1U);
        st32(LEAVE_SLOT, 0U);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2202U));
        EXPECT_REASON(ISAAC_KRS_REJECT_SLOT);
        iat_tokens();
        make_free(1U);
        g_isaac_vita_import_ids_ready = 0;
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2203U));
        EXPECT_REASON(ISAAC_KRS_REJECT_SLOT);
        g_isaac_vita_import_ids_ready = 1;
        make_free(1U);
        st32(ENTER_SLOT, ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS);
        st32(LEAVE_SLOT, ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 2204U));
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 2205U));
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 1, 2206U));
        iat_tokens();

        /* init: the logging path, then the same lock/unlock (terminates). */
        make_free(1U);
        st8(object + 0xCU, 0U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2300U));
        EXPECT_REASON(ISAAC_KRS_REJECT_INIT);
        CHECK(s_logger_calls >= 2U && ld16(object + 4U) == 3U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2301U));
        EXPECT_REASON(ISAAC_KRS_REJECT_INIT);
        CHECK(ld16(object + 4U) == 4U);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2302U));
        EXPECT_REASON(ISAAC_KRS_REJECT_INIT);
        CHECK(ld16(object + 4U) == 3U);

        /* vt: slot 3 foreign / slot 4 frozen and vice versa (guest_call
         * escapes), the RVA spelling (handled), the counter vtable's AddRef
         * slot foreign with a live count (escape) and with a dead count
         * (never dereferenced: handled). */
        make_free(1U);
        st32(object + 8U, mvt_row(MVT_LOCK_FOREIGN));
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2400U));
        EXPECT_REASON(ISAAC_KRS_REJECT_VT);
        CHECK(s_step_escaped);               /* foreign Lock target */
        make_free(1U);
        st32(object + 8U, mvt_row(MVT_UNLOCK_FOREIGN));
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2401U));
        EXPECT_REASON(ISAAC_KRS_REJECT_VT);
        make_free(1U);
        st32(object + 8U, mvt_row(MVT_UNLOCK_FOREIGN));
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2402U));
        EXPECT_REASON(ISAAC_KRS_REJECT_VT);
        make_free(1U);
        st32(object + 8U, mvt_row(MVT_LOCK_FOREIGN));
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2403U));
        EXPECT_REASON(ISAAC_KRS_REJECT_VT);
        make_free(1U);
        st32(object + 8U, mvt_row(MVT_FROZEN_RVA));
        st32(object, cvt_row(CVT_FROZEN_RVA));
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 2404U));
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 2405U));
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 1, 2406U));
        make_free(1U);
        st32(object, cvt_row(CVT_FOREIGN));
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2407U));
        EXPECT_REASON(ISAAC_KRS_REJECT_VT);
        CHECK(ld32(cs + 16U) == 0U);        /* Lock/Unlock done, AddRef gone */
        make_free(1U);
        st32(object, cvt_row(CVT_FOREIGN));
        st16(object + 4U, 0U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 2408U));
        make_free(1U);
        st32(object, cvt_row(CVT_FOREIGN));
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 2409U));
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 1, 2410U));

        /* cs: magic / handle / version (endpoint faults), misaligned, null,
         * top of memory (boundary faults). */
        make_free(1U);
        st32(cs, ISAAC_VITA_SYNC_CS_MAGIC ^ 1U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2500U));
        EXPECT_REASON(ISAAC_KRS_REJECT_CS);
        CHECK(s_step_left && !s_step_escaped);   /* endpoint fault */
        make_free(1U);
        st32(cs + 4U, 0x40010003U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2501U));
        EXPECT_REASON(ISAAC_KRS_REJECT_CS);
        make_free(1U);
        st32(cs + 12U, 1U);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2502U));
        EXPECT_REASON(ISAAC_KRS_REJECT_CS);
        make_free(1U);
        st32(object + 0x10U, cs + 2U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2503U));
        EXPECT_REASON(ISAAC_KRS_REJECT_CS);
        make_free(1U);
        st32(object + 0x10U, 0U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2504U));
        EXPECT_REASON(ISAAC_KRS_REJECT_CS);
        make_free(1U);
        st32(object + 0x10U, UINT32_C(0xfffffff0));
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2505U));
        EXPECT_REASON(ISAAC_KRS_REJECT_CS);

        /* owner: foreign owner (the endpoint's wait, broken into its fault
         * here), CLAIMING, self with depth 0, free with depth 3, self at
         * depth UINT32_MAX. */
        make_free(1U);
        st32(cs + 16U, (uint32_t)CONTENDER_THREAD_ID);
        st32(cs + 20U, 1U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2600U));
        EXPECT_REASON(ISAAC_KRS_REJECT_OWNER);
        CHECK(s_hang_breaks >= 2U && s_step_left && !s_step_escaped);
        make_free(1U);
        st32(cs + 16U, (uint32_t)CONTENDER_THREAD_ID);
        st32(cs + 20U, 1U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2601U));
        EXPECT_REASON(ISAAC_KRS_REJECT_OWNER);
        make_free(1U);
        st32(cs + 16U, UINT32_MAX);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2602U));
        EXPECT_REASON(ISAAC_KRS_REJECT_OWNER);
        make_free(1U);
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(cs + 20U, 0U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2603U));
        EXPECT_REASON(ISAAC_KRS_REJECT_OWNER);
        make_free(1U);
        st32(cs + 20U, 3U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2604U));
        EXPECT_REASON(ISAAC_KRS_REJECT_OWNER);
        make_free(1U);
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(cs + 20U, UINT32_MAX);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2605U));
        EXPECT_REASON(ISAAC_KRS_REJECT_OWNER);

        /* busy: the Sleep(1000) loop (generic Sleep import: escape). */
        make_free(1U);
        st8(cs + 0x18U, 1U);
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2700U));
        EXPECT_REASON(ISAAC_KRS_REJECT_BUSY);
        CHECK(s_step_escaped);               /* Sleep import */
        make_free(1U);
        st8(cs + 0x18U, 1U);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2701U));
        EXPECT_REASON(ISAAC_KRS_REJECT_BUSY);
        make_free(1U);
        st8(cs + 0x18U, 1U);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2702U));
        EXPECT_REASON(ISAAC_KRS_REJECT_BUSY);
        make_free(1U);

        /* Disabled latch: the endpoint completes without waiting. */
        isaac_vita_sync_inline_disable();
        FRAME_FOR(FN_ADDREF, esp);
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 2800U));
        EXPECT_REASON(ISAAC_KRS_REJECT_LATCH);
        CHECK(ld16(object + 4U) == 3U && cpu.eax == cs);
        FRAME_FOR(FN_WEAKLOCK, esp);
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 0, 2801U));
        CHECK(ld16(object + 4U) == 4U);
        FRAME_FOR(FN_RELEASE, esp);
        CHECK(!differential_step(&cpu, FN_RELEASE, 0, 2802U));
        CHECK(ld16(object + 4U) == 3U);
        g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
        cpu_init(&cpu);
        isaac_vita_sync_bind_current_thread(&cpu);
        CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
        make_free(1U);
#undef FRAME_FOR
        for (k = 1U; k < ISAAC_KRS_REASON_COUNT; ++k)
            if (k != ISAAC_KRS_REJECT_ALIAS)
                CHECK(s_fallback_by_reason[k] == 1U);
        printf("directed fallbacks: %u rejections, %u escapes, %u body faults\n",
               s_rejections, s_escapes, s_body_faults);
    }

    /* Phase 2b: hostile aliasing.  The bodies read the count, the vtable
     * slots, the CS pointer and the IAT words AFTER their pushes and after
     * Lock's CS stores, the seam reads everything once and stores nothing
     * into the CS; every overlap between a writer and a later reader is
     * refused (fail closed) and the translated body runs -- completing
     * identically, faulting or escaping, as the x86 would.  Mirror cases
     * just outside every span must stay handled.  Registers that the pushes
     * may drop onto object words hold readable vtable/CS addresses. */
    {
        uint32_t esp = cpu.stack_ceiling - 256U;
        uint32_t object = object_address(3U);
        uint32_t cs = cs_address(3U);
        uint32_t spare_cs = cs_address(CS_SPARE_INDEX);
        uint32_t filler = mvt_row(MVT_SPARE);
        uint32_t alias;
        int fn;

#define ALIAS_REGS() do { \
        cpu.eax = 0x0badf00dU; cpu.edx = 0x0d0d0d0dU; cpu.ebx = spare_cs; \
        cpu.esi = filler; cpu.edi = filler; cpu.ebp = filler; } while (0)
/* An object at the return word owns [E]: its vtable word IS the word the
 * final ret pops (a caller whose control block sits there), so the frame's
 * return word is rewritten as the counter vtable pointer. */
#define ALIAS_STEP(fn, this_, expect_, number) do { \
        ALIAS_REGS(); cpu.ecx = (this_); \
        write_frame(&cpu, esp, caller_return(fn)); \
        if ((this_) == esp) st32(esp, cvt_row(CVT_FROZEN)); \
        CHECK(!differential_step(&cpu, (fn), (expect_), (number))); \
        if (!(expect_)) EXPECT_REASON(ISAAC_KRS_REJECT_ALIAS); } while (0)

        /* A: object at the return word (no byte below E is the object's) and
         * just below the pushed frame: handled for every helper.  Object
         * one word into the frame: refused. */
        for (fn = 0; fn < 3; ++fn) {
            uint32_t strong = fn == FN_WEAKLOCK ? 0U : 5U;
            make_free(3U);
            alias = esp;
            make_object_at(alias, cs, strong);
            ALIAS_STEP(fn, alias, 1, 3000U + (uint32_t)fn);
            CHECK(ld32(cs + 16U) == 0U);
            make_free(3U);
            alias = esp - frame_bytes(fn) - 0x18U;
            make_object_at(alias, cs, strong);
            ALIAS_STEP(fn, alias, 1, 3010U + (uint32_t)fn);
            make_free(3U);
            alias = esp - frame_bytes(fn) - 0x14U;      /* T* under the frame */
            make_object_at(alias, cs, strong);
            ALIAS_STEP(fn, alias, 0, 3020U + (uint32_t)fn);
            make_free(3U);
            alias = esp - 4U;                            /* cvt under push esi */
            make_object_at(alias, cs, strong);
            ALIAS_STEP(fn, alias, 0, 3030U + (uint32_t)fn);
            make_free(3U);
            alias = esp - 0x18U;                         /* whole block inside */
            make_object_at(alias, cs, strong);
            ALIAS_STEP(fn, alias, 0, 3040U + (uint32_t)fn);
            make_free(3U);
            alias = esp - 40U;                           /* block = Lock frame */
            make_object_at(alias, cs, strong);
            ALIAS_STEP(fn, alias, 0, 3050U + (uint32_t)fn);
            make_free(3U);
            make_cs(spare_cs);
        }
        /* Objects straddling the stack floor or the ceiling are not the
         * frame predicate's business (only the pushed frame and the return
         * word must lie inside the owned stack): handled, the pushes never
         * reach them.  An object straddling the frame's top word is hit by
         * the first pushes: refused. */
        for (fn = 0; fn < 3; ++fn) {
            uint32_t floor = cpu.stack_floor, ceiling = cpu.stack_ceiling;
            uint32_t span = fn == FN_WEAKLOCK ? 52U : 40U;
            make_free(3U);
            make_object_at(floor - 8U, cs, 5U);
            ALIAS_REGS();
            cpu.ecx = floor - 8U;
            /* E-52 = floor+20 > object end (floor+16): admitted. */
            write_frame(&cpu, floor + 72U, caller_return(fn));
            CHECK(!differential_step(&cpu, fn, 1, 3070U + (uint32_t)fn));
            CHECK(ld16(floor - 4U) == (fn == FN_RELEASE ? 4U : 6U));
            /* E-52 = floor+12 touches the object's last span word
             * (this+0x14): the weak-lock frame is refused (alias), the
             * 40-byte frames (E-40 = floor+24) stay admitted. */
            make_free(3U);
            make_object_at(floor - 8U, cs, 5U);
            ALIAS_REGS();
            cpu.ecx = floor - 8U;
            write_frame(&cpu, floor + 64U, caller_return(fn));
            CHECK(!differential_step(&cpu, fn, span == 40U, 3075U + (uint32_t)fn));
            if (span == 52U)
                EXPECT_REASON(ISAAC_KRS_REJECT_ALIAS);
            else
                CHECK(ld16(floor - 4U) == (fn == FN_RELEASE ? 4U : 6U));
            make_free(3U);
            make_object_at(ceiling - 8U, cs, 5U);
            ALIAS_REGS();
            cpu.ecx = ceiling - 8U;
            write_frame(&cpu, ceiling - 64U, caller_return(fn));
            CHECK(!differential_step(&cpu, fn, 1, 3080U + (uint32_t)fn));
            CHECK(ld16(ceiling - 4U) == (fn == FN_RELEASE ? 4U : 6U));
            make_free(3U);
            make_object_at(esp - 8U, cs, 5U);       /* [this+8] = mvt is [E] */
            ALIAS_REGS();
            cpu.ecx = esp - 8U;
            write_frame(&cpu, esp, caller_return(fn));
            st32(esp, mvt_row(MVT_FROZEN));
            CHECK(!differential_step(&cpu, fn, 0, 3090U + (uint32_t)fn));
            EXPECT_REASON(ISAAC_KRS_REJECT_ALIAS);
            make_free(3U);
            make_cs(spare_cs);
        }

        /* Weak-lock alive with the object at the return word and just below
         * the 52-byte frame; the 40-byte position (dead path's frame) is
         * refused by the 52-byte span although the dead body never touches
         * it: the documented over-rejection. */
        make_free(3U);
        make_object_at(esp, cs, 6U);
        ALIAS_STEP(FN_WEAKLOCK, esp, 1, 3060U);
        CHECK(ld16(esp + 4U) == 7U);
        make_free(3U);
        make_object_at(esp - 52U - 0x18U, cs, 6U);
        ALIAS_STEP(FN_WEAKLOCK, esp - 52U - 0x18U, 1, 3061U);
        make_free(3U);
        make_object_at(esp - 40U - 0x18U, cs, 0U);
        ALIAS_STEP(FN_WEAKLOCK, esp - 40U - 0x18U, 0, 3062U);
        CHECK(ld16(esp - 40U - 0x18U + 4U) == 0U && cpu.eax == (cs & 0xFFFFFF00U));
        make_free(3U);
        make_object_at(esp - 40U - 0x18U, cs, 6U);
        ALIAS_STEP(FN_WEAKLOCK, esp - 40U - 0x18U, 0, 3063U);

        /* B: the critical section inside / straddling the frame (a valid CS
         * image whose words the pushes overwrite), and just below it. */
        for (fn = 0; fn < 3; ++fn) {
            make_free(3U);
            alias = esp - 32U;
            make_cs(alias);
            st32(object + 0x10U, alias);
            ALIAS_STEP(fn, object, 0, 3100U + (uint32_t)fn);
            make_free(3U);
            alias = esp - frame_bytes(fn) - 0x1CU + 4U;  /* busy byte inside */
            make_cs(alias);
            st32(object + 0x10U, alias);
            ALIAS_STEP(fn, object, 0, 3110U + (uint32_t)fn);
            make_free(3U);
            alias = esp - frame_bytes(fn) - 0x1CU;       /* just below */
            make_cs(alias);
            st32(object + 0x10U, alias);
            ALIAS_STEP(fn, object, 1, 3120U + (uint32_t)fn);
            CHECK(ld32(alias + 16U) == 0U && ld8(alias + 0x18U) == 0U);
        }
        make_free(3U);
        st16(object + 4U, 0U);
        alias = esp - 40U - 0x1CU;                       /* dead path over-rejection */
        make_cs(alias);
        st32(object + 0x10U, alias);
        ALIAS_STEP(FN_WEAKLOCK, object, 0, 3130U);
        CHECK(ld32(alias + 16U) == 0U && cpu.eax == (alias & 0xFFFFFF00U));

        /* C: the critical section overlapping the control block while the
         * CS predicates (magic/handle/version, owner/depth, busy) still pass
         * -- the only overlaps the alias check can be reached with, because
         * an owner/depth/busy word on a non-zero object word is refused
         * earlier (owner/busy).
         *   cs = this - 0x18: only the busy byte is on the block (the
         *   vtable pointer's low byte, 0 for these rows): the body sets
         *   busy=1 -> cvt|1 during the lock and restores it; the seam
         *   refuses (fail closed) and the body completes.
         *   cs = this - 0x14: busy on the count's low byte (0x300/0x100),
         *   depth on the vtable pointer (non-zero, not the wrap value),
         *   owner at this-4 = self.  The body's `inc word` then lands on the
         *   busy byte and its Unlock clears it: count != strong+1 -- the
         *   divergence the predicate exists for.
         *   cs = this + 0x14: the CS begins on the T* word; the words the
         *   bodies write (owner, depth, busy) lie above the block, so the
         *   body completes and only the seam's span check refuses. */
        for (fn = 0; fn < 3; ++fn) {
            uint32_t strong = fn == FN_WEAKLOCK ? 0x0100U : 0x0300U;
            make_free(3U);
            alias = object - 0x18U;
            make_cs(alias);                              /* clobbers [this], [this+4] */
            st32(object, cvt_row(CVT_FROZEN));
            st32(object + 4U, strong);
            st32(object + 0x10U, alias);
            ALIAS_STEP(fn, object, 0, 3200U + (uint32_t)fn);
            CHECK(ld32(object) == cvt_row(CVT_FROZEN));
            CHECK(ld16(object + 4U) == (uint16_t)(fn == FN_RELEASE ? strong - 1U
                                                                     : strong + 1U));
            make_free(3U);
            alias = object - 0x14U;
            make_cs(alias);                              /* clobbers [this], [this+4], [this+8] */
            st32(object, cvt_row(CVT_FROZEN));           /* = depth (non-zero) */
            st32(object + 4U, strong);
            st32(object + 8U, mvt_row(MVT_FROZEN));
            st32(alias + 16U, (uint32_t)MAIN_THREAD_ID);   /* owner: this-4 */
            st32(object + 0x10U, alias);
            ALIAS_STEP(fn, object, 0, 3210U + (uint32_t)fn);
            CHECK(ld32(object) == cvt_row(CVT_FROZEN));
            CHECK(fn == FN_WEAKLOCK
                  ? ld16(object + 4U) == 0x0100U          /* +1 then busy cleared */
                  : ld16(object + 4U) != (uint16_t)(strong + 1U));
            make_free(3U);
            alias = object + 0x14U;
            make_cs(alias);                              /* T* = magic; rest above */
            st16(object + 4U, (uint16_t)strong);
            st32(object + 0x10U, alias);
            ALIAS_STEP(fn, object, 0, 3220U + (uint32_t)fn);
            CHECK(ld32(alias) == ISAAC_VITA_SYNC_CS_MAGIC &&
                  ld32(alias + 16U) == 0U && ld8(alias + 0x18U) == 0U);
            CHECK(ld16(object + 4U) == (uint16_t)(fn == FN_RELEASE ? strong - 1U
                                                                     : strong + 1U));
            make_free(2U);               /* neighbours the images spilled on */
            make_free(3U);
            make_free(4U);
        }

        /* D: a critical section overlapping the mutex vtable slots with its
         * predicates intact: busy byte = the Lock slot's low byte (0x00 for
         * the image VA), version/owner/depth on the row's unused slots 0-2.
         * The body's Lock turns the Lock slot into 0x...01 while held and
         * restores it; the seam refuses (cs vs slots). */
        for (fn = 0; fn < 3; ++fn) {
            uint32_t vt = mvt_row(MVT_SPARE);
            make_free(3U);
            make_cs(vt - 0xCU);                          /* busy on Lock slot */
            st32(vt, ISAAC_VITA_SYNC_CS_VERSION);        /* [cs+12] */
            st32(vt + 4U, 0U);                           /* owner */
            st32(vt + 8U, 0U);                           /* depth */
            st32(vt + 0xCU, LOCK_VA);
            st32(vt + 0x10U, UNLOCK_VA);
            st32(object + 8U, vt);
            st32(object + 0x10U, vt - 0xCU);
            ALIAS_STEP(fn, object, 0, 3300U + (uint32_t)fn);
            CHECK(ld32(vt + 0xCU) == LOCK_VA && ld32(vt + 4U) == 0U);
            make_vtables();
            make_free(3U);
        }
        make_free(3U);
        alias = cs_address(CS_SPARE_INDEX + 1U);
        make_cs(alias);
        st32(alias + 8U, ADDREF_VA);                     /* cvt = cs */
        st32(object, alias);
        st32(object + 0x10U, alias);
        st16(object + 4U, 4U);
        ALIAS_STEP(FN_WEAKLOCK, object, 0, 3330U);
        CHECK(ld16(object + 4U) == 5U);                  /* body completed */
        make_free(3U);
        st32(object, alias);
        st16(object + 4U, 0U);                           /* dead: no cvt read */
        st32(object + 0x10U, alias);
        ALIAS_STEP(FN_WEAKLOCK, object, 1, 3331U);
        make_free(3U);
        st32(object, alias);
        st32(object + 0x10U, alias);
        ALIAS_STEP(FN_ADDREF, object, 1, 3332U);
        ALIAS_STEP(FN_RELEASE, object, 1, 3333U);
        make_cs(alias);
        make_free(3U);

        /* E: the count halfword on a vtable slot word.  mvt = this - 0xc
         * makes [mvt+0xc] = [this] (holds the Lock VA) and [mvt+0x10] =
         * [this+4] (holds the Unlock VA as count|weak<<16): the vtable
         * predicate passes; the body's inc/dec turns the Unlock slot into a
         * foreign target and escapes; the seam refuses (count vs slots). */
        make_free(3U);
        st32(object, LOCK_VA);
        st32(object + 4U, UNLOCK_VA);
        st32(object + 8U, object - 0xCU);
        ALIAS_STEP(FN_ADDREF, object, 0, 3400U);
        CHECK(ld32(object + 4U) == UNLOCK_VA + 1U);
        make_free(3U);
        st32(object, LOCK_VA);
        st32(object + 4U, UNLOCK_VA);
        st32(object + 8U, object - 0xCU);
        ALIAS_STEP(FN_RELEASE, object, 0, 3401U);
        CHECK(ld32(object + 4U) == UNLOCK_VA - 1U);
        make_free(3U);
        make_free(2U);

        /* F: the IAT words.  A CS image in the IAT page whose busy byte is
         * the Leave token's low byte: no CS predicate survives an overlap
         * with the two tokens (their low bytes are 0xf8/0xfc, no other CS
         * word may hold them), so this is refused as busy and the body
         * escapes into its Sleep loop.  An object in the IAT page whose count
         * halfword is the Enter token reaches the alias check: the body's
         * inc corrupts the token, the seam refuses. */
        make_free(3U);
        alias = LEAVE_SLOT - 0x18U;
        make_cs(alias);
        iat_tokens();
        st32(object + 0x10U, alias);
        ALIAS_REGS();
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_ADDREF));
        CHECK(!differential_step(&cpu, FN_ADDREF, 0, 3500U));
        EXPECT_REASON(ISAAC_KRS_REJECT_BUSY);
        CHECK(s_step_escaped);
        memset(s_iat, 0, IAT_BYTES);
        iat_tokens();
        make_free(3U);
        alias = LEAVE_SLOT;                              /* this+4 = Enter word */
        make_object_at(alias, cs, 0U);
        iat_tokens();                                    /* [this]=Leave, [this+4]=Enter */
        st32(alias + 8U, mvt_row(MVT_FROZEN));
        ALIAS_STEP(FN_ADDREF, alias, 0, 3501U);
        CHECK(ld32(ENTER_SLOT) == ENTER_SLOT + 1U);
        memset(s_iat, 0, IAT_BYTES);
        iat_tokens();
        make_free(3U);
#undef ALIAS_STEP
#undef ALIAS_REGS
        CHECK(s_fallback_by_reason[ISAAC_KRS_REJECT_ALIAS] == 1U);
        printf("hostile aliasing: %u rejections so far, %u escapes, %u body faults\n",
               s_rejections, s_escapes, s_body_faults);
    }

    /* Phase 3: the stack predicate, exhaustively over the owned interval
     * plus both margins, all three helpers (weak-lock dead and alive).
     * AddRef/Release accept exactly word-aligned floor+40 <= esp <=
     * ceiling-4, weak-lock floor+52 <= esp <= ceiling-4.  Below the floor
     * the checked stack faults and the raw stack completes; both routes
     * agree either way. */
    {
        uint32_t object = object_address(2U);
        uint32_t floor = cpu.stack_floor, ceiling = cpu.stack_ceiling;
        uint32_t sweep = 0U;

        for (i = STACK_FLOOR_OFF - 64U; i <= STACK_CEIL_OFF + 64U; i += 2U) {
            uint32_t esp = s_base + i;
            int fast, aligned = (esp & 3U) == 0U;
            int fn;

            for (fn = 0; fn < 3; ++fn) {
                uint32_t frame = frame_bytes(fn);
                make_free(2U);
                SET_REGS(&cpu, 7U);
                cpu.ecx = object;
                write_frame(&cpu, esp, caller_return(fn));
                fast = expect_helper(&cpu, fn);
                CHECK(fast == (aligned && esp >= floor + frame &&
                               esp <= ceiling - 4U));
                CHECK(!differential_step(&cpu, fn, fast, 5000U + i * 4U +
                                         (uint32_t)fn));
                if (fast)
                    CHECK(cpu.esp == esp + 4U && ld32(cs_address(2U) + 16U) == 0U);
                clear_fault(&cpu);
            }
            make_free(2U);
            st16(object + 4U, 0U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
            fast = expect_helper(&cpu, FN_WEAKLOCK);
            CHECK(fast == (aligned && esp >= floor + 52U && esp <= ceiling - 4U));
            CHECK(!differential_step(&cpu, FN_WEAKLOCK, fast, 5000U + i * 4U + 3U));
            clear_fault(&cpu);
            ++sweep;
        }
        make_free(2U);
        cpu_init(&cpu);
        isaac_vita_sync_bind_current_thread(&cpu);
        CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
        printf("stack sweep: %u ESP values x4\n", sweep);
    }

    /* Phase 4: seeded random program over eight objects. */
    for (i = 0U; i < OBJECT_COUNT; ++i)
        make_free(i);
    make_vtables();
    iat_tokens();
    {
        uint32_t handled0[3] = { s_handled[0], s_handled[1], s_handled[2] };
        uint32_t rejected0 = s_rejections, escapes0 = s_escapes;
        CHECK(!random_phase(&cpu, steps));
        printf("random program: addref=%u weaklock=%u release=%u rejected=%u "
               "escapes=%u body_faults=%u differentials=%u\n",
               s_handled[0] - handled0[0], s_handled[1] - handled0[1],
               s_handled[2] - handled0[2], s_rejections - rejected0,
               s_escapes - escapes0, s_body_faults, s_differentials);
        CHECK(s_handled[FN_ADDREF] - handled0[FN_ADDREF] > steps / 12U);
        CHECK(s_handled[FN_WEAKLOCK] - handled0[FN_WEAKLOCK] > steps / 12U);
        CHECK(s_handled[FN_RELEASE] - handled0[FN_RELEASE] > steps / 12U);
        CHECK(s_rejections - rejected0 > steps / 20U);
        CHECK(s_escapes - escapes0 > steps / 400U);
        CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
    }

    /* Phase 5: receipts.  One banner (first), one fallback line per reason,
     * the stats line at total calls 1000 and every 2^20 thereafter, each
     * equal to the counters at that moment; the exported logger agrees with
     * the exported counters; handled totals agree with the oracle's. */
    {
        uint32_t total = stats->calls[0] + stats->calls[1] + stats->calls[2];
        uint32_t expected_stats = (total >= 1000U ? 1U : 0U) + (total >> 20);
        unsigned a1, a2, w1, w2, wa, wd, r1, r2, rej[12], imp, vm, vr, vs;

        CHECK(s_banner_lines == 1U && s_banner_first == 1U);
        for (k = 1U; k < ISAAC_KRS_REASON_COUNT; ++k)
            CHECK(s_fallback_by_reason[k] == 1U);
        CHECK(s_fallback_lines == ISAAC_KRS_REASON_COUNT - 1U &&
              s_fallback_unknown == 0U && s_unknown_seam_lines == 0U);
        CHECK(s_stats_lines == expected_stats && s_stats_lines_bad == 0U);
        /* The differential steps' handled counts, plus the unit-text phase
         * (two seamed routes for each of its four handled cases: one AddRef,
         * one Release, two weak-locks), plus the coverage-id case (one
         * weak-lock and one Release through the seam alone) and, AddRef
         * only, the nested handled calls inside rejected alive weak-lock
         * bodies. */
        CHECK(stats->handled[FN_ADDREF] >= s_handled[FN_ADDREF] + 2U &&
              stats->handled[FN_WEAKLOCK] == s_handled[FN_WEAKLOCK] + 5U &&
              stats->handled[FN_RELEASE] == s_handled[FN_RELEASE] + 3U);
        CHECK(stats->weak_alive + stats->weak_dead == stats->handled[FN_WEAKLOCK]);
        CHECK(stats->imports_replayed ==
              2U * (stats->handled[FN_ADDREF] + stats->handled[FN_RELEASE] +
                    stats->weak_dead) + 4U * stats->weak_alive);
        k = s_stats_lines;
        isaac_vita_kage_refcount_seam_stats_log();
        CHECK(s_stats_lines == k + 1U && s_stats_lines_bad == 0U);
        CHECK(sscanf(s_last_stats_line, SEAM_STATS_FORMAT,
                     &a1, &a2, &w1, &w2, &wa, &wd, &r1, &r2,
                     &rej[0], &rej[1], &rej[2], &rej[3], &rej[4], &rej[5],
                     &rej[6], &rej[7], &rej[8], &rej[9], &rej[10], &rej[11],
                     &imp, &vm, &vr, &vs) == 24);
        CHECK(a1 == stats->handled[FN_ADDREF] && a2 == stats->calls[FN_ADDREF] &&
              w1 == stats->handled[FN_WEAKLOCK] && w2 == stats->calls[FN_WEAKLOCK] &&
              wa == stats->weak_alive && wd == stats->weak_dead &&
              r1 == stats->handled[FN_RELEASE] && r2 == stats->calls[FN_RELEASE]);
        for (k = 0U; k < 12U; ++k)
            CHECK(rej[k] == stats->reasons[k + 1U]);
        CHECK(imp == stats->imports_replayed && vm == stats->verify_mismatches &&
              vr == stats->verify_runs && vs == stats->verify_skipped_fault);
        {
            uint32_t rejected_total = 0U;
            for (k = 1U; k < ISAAC_KRS_REASON_COUNT; ++k)
                rejected_total += stats->reasons[k];
            CHECK(rejected_total + stats->handled[0] + stats->handled[1] +
                  stats->handled[2] == total);
        }
        printf("receipts: banner=%u fallbacks=%u stats=%u (calls=%u)\n",
               s_banner_lines, s_fallback_lines, s_stats_lines, total);
    }

#if ORACLE_VERIFY
    /* Phase 6: VERIFY.  Every handled call reran its body and matched.  In
     * the synthetic-stack-guard leg one rerun is made to fault through the
     * guarded push (the only oracle-owned code a rerun of an accepted state
     * executes: the bodies' direct-edge census macros are no-ops, exactly
     * as in a generated unit); it is counted, logged and re-raised to the
     * real run scope, and the bypass flag is clear afterwards (the next
     * handled call is handled and verified again). */
    {
        uint32_t object = object_address(0U);
        uint32_t esp = cpu.stack_ceiling - 64U;
        uint32_t runs;

        CHECK(stats->verify_mismatches == 0U && s_verify_mismatch_lines == 0U);
        CHECK(stats->verify_runs == stats_handled_total());
        CHECK(stats->verify_skipped_fault == 0U && s_verify_skipped_lines == 0U);
        /* MATCH receipts are throttled: the first 8 runs and every 4096th
         * (a per-call line would flood the device log). */
        CHECK(stats->verify_runs > 4096U);
        CHECK(s_verify_match_lines == 8U + stats->verify_runs / 4096U);
        runs = stats->verify_runs;
#if GUEST_GENERATED_STACK_GUARD
        {
            uint32_t skipped, handled;
            int result;

            make_free(0U);
            st16(object + 4U, 9U);
            SET_REGS(&cpu, 1U);
            cpu.ecx = object;
            write_frame(&cpu, esp, caller_return(FN_ADDREF));
            guest_coverage_function(COVERAGE_ADDREF);
            skipped = stats->verify_skipped_fault;
            handled = stats->handled[FN_ADDREF];
            s_inject_rerun_fault = 1;
            result = run_try(&cpu, FN_ADDREF);
            CHECK(result == -1);                 /* re-raised to our scope */
            CHECK(s_inject_rerun_fault == 0);
            CHECK(cpu.fault != NULL &&
                  strcmp(cpu.fault, "oracle: injected rerun fault") == 0 &&
                  cpu.fault_addr == 0x00007b50U);
            CHECK(stats->verify_skipped_fault == skipped + 1U &&
                  stats->verify_runs == runs &&
                  stats->handled[FN_ADDREF] == handled + 1U);
            CHECK(s_verify_skipped_lines == 1U &&
                  strstr(s_last_seam_line, "verify skipped (fault)") != NULL &&
                  strstr(s_last_seam_line, "fn=AddRef") != NULL &&
                  strstr(s_last_seam_line,
                         "what=oracle: injected rerun fault") != NULL);
            clear_fault(&cpu);
        }
#else
        printf("verify: fault injection needs the guarded push "
               "(stack_guard=0 leg: not exercised)\n");
#endif
        /* The bypass is clear: a handled call is handled and verified. */
        make_free(0U);
        st16(object + 4U, 9U);
        SET_REGS(&cpu, 1U);
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_WEAKLOCK));
        CHECK(!differential_step(&cpu, FN_WEAKLOCK, 1, 6000U));
        CHECK(stats->verify_runs == runs + 1U && stats->verify_mismatches == 0U);
        CHECK(ld16(object + 4U) == 10U);
        make_free(0U);
        cpu.ecx = object;
        write_frame(&cpu, esp, caller_return(FN_ADDREF));
        CHECK(!differential_step(&cpu, FN_ADDREF, 1, 6001U));
        CHECK(stats->verify_runs == runs + 2U && stats->verify_mismatches == 0U);
        printf("verify: runs=%u mismatches=%u skipped(fault)=%u match_lines=%u\n",
               stats->verify_runs, stats->verify_mismatches,
               stats->verify_skipped_fault, s_verify_match_lines);
    }
#endif

    /* Neither route records the direct edges: no ISAAC_VITA_PHASE_PROFILE in
     * a generated unit, no edge replay in the seam. */
    CHECK(s_lookup_hits == 0U);
    printf("census: lookup-cache hits=%u (the direct edges record nothing on "
           "either route)\n", s_lookup_hits);
    printf("Vita KAGE refcount seam oracle: PASS "
           "(stack_guard=%d gpr_local=%d verify=%d steps=%u "
           "addref=%u weaklock=%u release=%u rejected=%u escapes=%u "
           "body_faults=%u differentials=%u checks=%u imports=%u "
           "faults=%u logger=%u hang_breaks=%u)\n",
           GUEST_GENERATED_STACK_GUARD, GUEST_GPR_LOCAL, ORACLE_VERIFY, steps,
           s_handled[FN_ADDREF], s_handled[FN_WEAKLOCK], s_handled[FN_RELEASE],
           s_rejections, s_escapes, s_body_faults, s_differentials, s_checks,
           g_host_import_calls, s_fault_calls, s_logger_calls, s_hang_breaks);
    return 0;
}
