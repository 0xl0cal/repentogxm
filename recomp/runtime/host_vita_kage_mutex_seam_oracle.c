/* Host differential oracle for the KAGE Mutex::Lock(-1)/Unlock native seam
 * (host_vita_kage_mutex_seam.c, ISAAC_VITA_KAGE_MUTEX_SEAM).
 *
 * The reference is the ACTUAL generated translation of sub_00562e00 and
 * sub_00562ec0: the test writes both bodies from the corpus (or a fresh
 * gen_all render) into kage_mutex_seam_bodies.inc with the `#if
 * defined(__vita__)` sync-IAT sites selected, and this file compiles them
 * with the production unit macros (GUEST_GPR_LOCAL/GUEST_FLAGS_LOCAL,
 * GUEST_GENERATED_STACK_GUARD as chosen by the build, GUEST_STACK_REQUIRED=1)
 * against the real host_vita_sync.c + vita_sync_services.c endpoint and the
 * real inline fast-path header.  guest_try_direct_sync_import_call is the
 * verbatim guest.c route (census, sampler word, inline path, endpoint).
 *
 * Every step runs twice from an identical snapshot (CPU, whole guest arena,
 * IAT page, counters):
 *   old: the translated body alone;
 *   new: `if (!isaac_vita_kage_mutex_*_try(c)) body(c);` -- exactly the
 *        statement gen_all emits.
 * CPU struct, arena and every counter delta must agree byte for byte.  When
 * the seam rejects, the CPU, arena and counters must be untouched before the
 * body runs.  An independent acceptance predicate is evaluated for every
 * state and must equal the seam's decision (no over-rejection either).
 * States whose translated body cannot terminate here (timed wait through
 * the generic import route, foreign-owner wait, Sleep loop) are driven as
 * rejection-only cases. */
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
    !defined(ISAAC_VITA_GUEST_SAMPLER)
#error Compile with every census owner definition so all counters are live
#endif

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* ---- Vita service mocks (same shape as vita_sync_inline_fastpath_oracle) -- */

#define MAIN_THREAD_ID      ((int32_t)UINT32_C(0x40010003))
#define CONTENDER_THREAD_ID ((int32_t)UINT32_C(0x40010005))

static int32_t s_thread_id = MAIN_THREAD_ID;
static unsigned s_checks;
static unsigned s_delay_calls;
static unsigned s_log_calls;
static unsigned s_fault_calls;
static unsigned s_logger_calls;
static unsigned s_guest_call_calls;
static jmp_buf s_fault_env;
static int s_fault_scope;
static uint32_t s_step_number;

unsigned g_host_import_calls;
GuestPhaseProfileCounters g_guest_phase_profile_counters;
uint32_t g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
volatile uint32_t g_kage_guest_last_indirect_target;
int g_isaac_vita_import_ids_ready;
static unsigned char s_coverage_functions[16384];
static unsigned char s_coverage_imports[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
static unsigned char s_coverage_cases[8192];
unsigned char *g_guest_coverage_functions = s_coverage_functions;
unsigned char *g_guest_coverage_imports = s_coverage_imports;
unsigned char *g_guest_coverage_cases = s_coverage_cases;
uint32_t g_guest_fs_base_cached;

void isaac_vita_log(const char *format, ...)
{
    (void)format;
    ++s_log_calls;
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

int sceKernelDelayThread(unsigned int usec)
{
    ++s_delay_calls;
    return usec == 1U ? 0 : -1;
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

/* ---- guest.c pieces the endpoint and the bodies link against ------------ */

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    ++s_fault_calls;
    c->fault = what;
    c->fault_addr = addr;
    c->exit_api = NULL;
    c->stop_kind = GUEST_RUN_FAULT;
    if (s_fault_scope)
        longjmp(s_fault_env, 1);
    abort();
}

uint32_t guest_fs_base(CPU *__restrict c)
{
    (void)c;
    return 0U;
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
/* The checked-corpus entry points (guest.c's GUEST_STACK_HOT_NOINLINE
 * wrappers) over the validators above; the pc argument is diagnostic only. */
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

GUEST_STACK_HOT_NOINLINE void gpush_generated(
    CPU *__restrict c, uint32_t value)
{
    gpush_at(c, value, 0U);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_generated(CPU *__restrict c)
{
    return gpop_at(c, 0U);
}
#endif

/* KAGE's logger sub_0055e330 ('Trying to lock mutex that has not been
 * initialized'): the only translated callee of the two bodies.  As in
 * test_gpr_locals_semantics.py it pops its return word and leaves the
 * register file alone; the oracle only counts it. */
void sub_0055e330(CPU *__restrict c)
{
    ++s_logger_calls;
    c->esp += 4U;
}

/* The generic import route (timed wait: GetTickCount / TryEnterCS / Sleep,
 * or a sync site whose slot word or table is not authenticated).  The seam
 * never reaches these states with a handled result; the driver never runs a
 * body that would come here. */
void guest_call(CPU *__restrict c, uint32_t addr)
{
    ++s_guest_call_calls;
    fprintf(stderr, "unexpected guest_call(%08x) in a replayed body at step "
                    "%u (esp %08x ecx %08x enter-slot %08x leave-slot %08x)\n",
            addr, s_step_number, c->esp, c->ecx,
            ld32(GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS),
            ld32(GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS));
    exit(3);
}

/* Verbatim guest.c route under ISAAC_VITA_SYNC_IMPORT_FASTPATH +
 * ISAAC_VITA_SYNC_INLINE_FASTPATH + ISAAC_VITA_GUEST_SAMPLER, with
 * g_isaac_vita_import_ids_ready standing for guest.c's
 * g_vita_sync_import_fastpath_ready (both are the return value of the same
 * registration, which also proved rows 60/61 == the two slots). */
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
#include "kage_mutex_seam_bodies.inc"

/* ---- arena -------------------------------------------------------------- */

/* Guest memory is host memory below 4 GiB (ld32 casts the guest address to a
 * host pointer).  `hint` == 0 asks for any 32-bit-addressable range; a
 * non-zero hint is the exact required address (the IAT page). */
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
#define MUTEX_ROW_OFF   UINT32_C(0x6000)
#define MUTEX_STRIDE    16U
#define CS_ROW_OFF      UINT32_C(0x7000)
#define CS_STRIDE       32U
#define OBJECT_COUNT    8U
/* The bodies read the IAT slots at their absolute guest addresses, so this
 * page is mapped fixed.  On x86-64 it lies inside AddressSanitizer's
 * shadow region: run the oracle plain or under UBSan, not ASan. */
#define IAT_PAGE        UINT32_C(0x98606000)
#define IAT_BYTES       UINT32_C(0x1000)
#define ENTER_SLOT      (GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS)
#define LEAVE_SLOT      (GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS)
#define LOCK_RETURN     UINT32_C(0x00007b8a)   /* a refcount caller's word */
#define UNLOCK_RETURN   UINT32_C(0x00007b94)
#define COVERAGE_LOCK   7968U
#define COVERAGE_UNLOCK 7969U

static uint8_t *s_arena;
static uint32_t s_base;
static uint8_t *s_iat;
static uint8_t s_snapshot[ARENA_BYTES];
static uint8_t s_old_arena[ARENA_BYTES];
static uint8_t s_iat_snapshot[IAT_BYTES];

static uint32_t mutex_address(uint32_t index)
{
    return s_base + MUTEX_ROW_OFF + index * MUTEX_STRIDE;
}

static uint32_t cs_address(uint32_t index)
{
    return s_base + CS_ROW_OFF + index * CS_STRIDE;
}

static void make_free(uint32_t index)
{
    uint32_t address = cs_address(index);

    st32(address, ISAAC_VITA_SYNC_CS_MAGIC);
    st32(address + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
    st32(address + 8U, 0U);
    st32(address + 12U, ISAAC_VITA_SYNC_CS_VERSION);
    st32(address + 16U, 0U);
    st32(address + 20U, 0U);
    st8(address + 0x18U, 0U);
    st32(mutex_address(index), 0x9875d000U);          /* vtable, unused */
    st8(mutex_address(index) + 4U, 1U);                /* initialized */
    st32(mutex_address(index) + 8U, address);
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
    GuestPhaseProfileCounters profile;
    uint32_t import_calls[2];
    unsigned char coverage_imports[2];
    unsigned char coverage_functions[2];
    uint32_t sampler_word;
    isaac_vita_sync_mutex_pool_snapshot pool;
} observation;

static void observe(observation *out, const CPU *c)
{
    /* Padding bytes take part in the byte-wise comparisons below. */
    memset(out, 0, sizeof *out);
    out->cpu = *c;
    out->host_import_calls = g_host_import_calls;
    out->faults = s_fault_calls;
    out->logs = s_log_calls;
    out->delays = s_delay_calls;
    out->logger_calls = s_logger_calls;
    out->guest_calls = s_guest_call_calls;
    out->profile = g_guest_phase_profile_counters;
    out->import_calls[0] =
        g_guest_phase_profile_import_calls[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS];
    out->import_calls[1] =
        g_guest_phase_profile_import_calls[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS];
    out->coverage_imports[0] =
        s_coverage_imports[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS];
    out->coverage_imports[1] =
        s_coverage_imports[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS];
    out->coverage_functions[0] = s_coverage_functions[COVERAGE_LOCK];
    out->coverage_functions[1] = s_coverage_functions[COVERAGE_UNLOCK];
    out->sampler_word = g_kage_guest_last_indirect_target;
    isaac_vita_sync_get_mutex_pool_snapshot(&out->pool);
}

/* Every host-side counter the two routes may touch.  A differential step
 * restores them between the old and the new route, so both start from the
 * same census and the new-route observation is the census of exactly one
 * logical operation (the directed phases check absolute +1 effects). */
typedef struct counters_snapshot {
    unsigned host_import_calls;
    unsigned faults;
    unsigned logs;
    unsigned delays;
    unsigned logger_calls;
    unsigned guest_calls;
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
    g_guest_phase_profile_counters = in->profile;
    memcpy(g_guest_phase_profile_import_calls, in->import_calls,
           sizeof in->import_calls);
    memcpy(s_coverage_imports, in->coverage_imports,
           sizeof in->coverage_imports);
    memcpy(s_coverage_functions, in->coverage_functions,
           sizeof in->coverage_functions);
    g_kage_guest_last_indirect_target = in->sampler_word;
}

/* The mutex pool census lives inside vita_sync_services.c and cannot be
 * restored between the routes: compare its deltas. */
#define DELTA_EQUAL(field) \
    ((old->field) - (before->field) == (fresh->field) - (old->field))
/* Everything else was restored to `before` between the routes. */
#define SAME(field) ((old->field) == (fresh->field))

static int observations_equal(const observation *before,
                              const observation *old,
                              const observation *fresh)
{
    if (memcmp(&old->cpu, &fresh->cpu, sizeof old->cpu) != 0)
        return 0;
    if (!SAME(host_import_calls) || !SAME(faults) ||
        !SAME(logs) || !SAME(delays) ||
        !SAME(logger_calls) || !SAME(guest_calls))
        return 0;
    if (memcmp(&old->profile, &fresh->profile, sizeof old->profile) != 0)
        return 0;
    if (!SAME(import_calls[0]) || !SAME(import_calls[1]))
        return 0;
    if (old->coverage_imports[0] != fresh->coverage_imports[0] ||
        old->coverage_imports[1] != fresh->coverage_imports[1] ||
        old->coverage_functions[0] != fresh->coverage_functions[0] ||
        old->coverage_functions[1] != fresh->coverage_functions[1] ||
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
            fprintf(stderr, "  CPU byte %zu: old=%02x new=%02x\n",
                    i, pa[i], pb[i]);
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

static void run_body(CPU *c, body_fn body)
{
    s_fault_scope = 1;
    if (setjmp(s_fault_env) == 0)
        body(c);
    s_fault_scope = 0;
}

static uint32_t s_lock_hits;
static uint32_t s_unlock_hits;
static uint32_t s_rejections;
static uint32_t s_differentials;
static uint32_t s_rejection_only;

/* The seam as the generated unit calls it, then the exact translated body on
 * rejection.  Returns the seam's decision. */
static int route_new(CPU *c, int unlock, body_fn body, int run_fallback)
{
    int handled = unlock ? isaac_vita_kage_mutex_unlock_try(c)
                         : isaac_vita_kage_mutex_lock_try(c);
    if (handled) {
        if (unlock)
            ++s_unlock_hits;
        else
            ++s_lock_hits;
        return 1;
    }
    ++s_rejections;
    if (run_fallback)
        run_body(c, body);
    return 0;
}

static void clear_fault(CPU *c)
{
    c->fault = NULL;
    c->fault_addr = 0U;
    c->stop_kind = GUEST_RUN_RETURNED;
}

/* Both routes from one snapshot; leaves the new-route state in place.
 * expect: 1 = seam must handle, 0 = must reject, -1 = predicate decides. */
static int differential_step(CPU *c, int unlock, int expect, uint32_t step)
{
    observation before, old, fresh, pre_seam, after_seam;
    static counters_snapshot counters;
    CPU saved = *c;
    body_fn body = unlock ? sub_00562ec0 : sub_00562e00;
    int handled;

    ++s_differentials;
    s_step_number = step;
    /* The generated prologue (guest_coverage_function) runs before the seam
     * statement; the body repeats it idempotently on the old route. */
    guest_coverage_function(unlock ? COVERAGE_UNLOCK : COVERAGE_LOCK);
    observe(&before, c);
    counters_save(&counters);
    memcpy(s_snapshot, s_arena, ARENA_BYTES);
    memcpy(s_iat_snapshot, s_iat, IAT_BYTES);

    run_body(c, body);
    observe(&old, c);
    memcpy(s_old_arena, s_arena, ARENA_BYTES);

    *c = saved;
    counters_restore(&counters);
    memcpy(s_arena, s_snapshot, ARENA_BYTES);
    /* Rejection must leave everything untouched before the body runs
     * (pre_seam differs from `before` only in the unrestorable pool census
     * the old route may have advanced through the endpoint). */
    observe(&pre_seam, c);
    handled = route_new(c, unlock, body, 0);
    observe(&after_seam, c);
    if (!handled) {
        if (!observations_identical(&pre_seam, &after_seam) ||
            memcmp(s_snapshot, s_arena, ARENA_BYTES) != 0) {
            fprintf(stderr, "seam rejected with side effects at step %u\n",
                    step);
            dump_cpu_diff(&pre_seam.cpu, &after_seam.cpu);
            return 1;
        }
        run_body(c, body);
    }
    observe(&fresh, c);

    if (!observations_equal(&before, &old, &fresh) ||
        memcmp(s_old_arena, s_arena, ARENA_BYTES) != 0 ||
        memcmp(s_iat_snapshot, s_iat, IAT_BYTES) != 0) {
        fprintf(stderr,
                "differential mismatch at step %u unlock=%d handled=%d\n",
                step, unlock, handled);
        dump_cpu_diff(&old.cpu, &fresh.cpu);
        fprintf(stderr, "  imports %u/%u faults %u/%u logs %u/%u logger %u/%u "
                        "calls %u/%u sync %u/%u import60 %u/%u import61 %u/%u "
                        "sampler %08x/%08x arena %s (first diff +%x) iat %s\n",
                old.host_import_calls, fresh.host_import_calls,
                old.faults, fresh.faults, old.logs, fresh.logs,
                old.logger_calls, fresh.logger_calls,
                old.profile.guest_calls, fresh.profile.guest_calls,
                old.profile.sync_fastpath, fresh.profile.sync_fastpath,
                old.import_calls[0], fresh.import_calls[0],
                old.import_calls[1], fresh.import_calls[1],
                old.sampler_word, fresh.sampler_word,
                memcmp(s_old_arena, s_arena, ARENA_BYTES) ? "DIFFERS" : "same",
                arena_first_diff(s_old_arena, s_arena),
                memcmp(s_iat_snapshot, s_iat, IAT_BYTES) ? "DIFFERS" : "same");
        return 1;
    }
    if (expect >= 0 && handled != expect) {
        fprintf(stderr, "seam decision %d != expected %d at step %u\n",
                handled, expect, step);
        return 1;
    }
    clear_fault(c);
    return 0;
}

/* A state whose translated body cannot terminate here: the seam must
 * reject and touch nothing. */
static int rejection_only_step(CPU *c, int unlock, uint32_t step)
{
    observation before, after;

    ++s_rejection_only;
    s_step_number = step;
    guest_coverage_function(unlock ? COVERAGE_UNLOCK : COVERAGE_LOCK);
    observe(&before, c);
    memcpy(s_snapshot, s_arena, ARENA_BYTES);
    memcpy(s_iat_snapshot, s_iat, IAT_BYTES);
    if (route_new(c, unlock, NULL, 0)) {
        fprintf(stderr, "seam handled a non-terminating state at step %u\n",
                step);
        return 1;
    }
    observe(&after, c);
    if (!observations_identical(&before, &after) ||
        memcmp(s_snapshot, s_arena, ARENA_BYTES) != 0 ||
        memcmp(s_iat_snapshot, s_iat, IAT_BYTES) != 0) {
        fprintf(stderr, "seam rejected with side effects at step %u\n", step);
        dump_cpu_diff(&before.cpu, &after.cpu);
        return 1;
    }
    return 0;
}

/* ---- independent acceptance predicate --------------------------------- */

static int frame_inside(const CPU *c, uint32_t frame, uint32_t ret)
{
    uint32_t floor = c->stack_floor, ceiling = c->stack_ceiling;
    if (c->stack_owner != c || floor >= ceiling)
        return 0;
    /* Word-aligned ESP (the ARM body's STRD pushes fault otherwise), then
     * esp - frame >= floor and esp + ret <= ceiling, evaluated wrap-free. */
    return (c->esp % 4U) == 0U &&
           c->esp >= floor && c->esp - floor >= frame &&
           ceiling - c->esp >= ret && c->esp <= ceiling;
}

static int cs_object_ok(uint32_t cs)
{
    if ((cs & 3U) != 0U || cs == 0U || cs > UINT32_MAX - 23U)
        return 0;
    if (cs - s_base >= ARENA_BYTES - 32U)
        return 0;                      /* not in the arena: unreadable here */
    return ld32(cs) == ISAAC_VITA_SYNC_CS_MAGIC &&
           ld32(cs + 4U) == (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE &&
           ld32(cs + 12U) == ISAAC_VITA_SYNC_CS_VERSION;
}

static int slot_ok(uint32_t slot_rva)
{
    uint32_t word = ld32(GUEST_IMAGE_BASE + slot_rva);
    return g_isaac_vita_import_ids_ready &&
           (word == slot_rva || word == GUEST_IMAGE_BASE + slot_rva);
}

/* Spelled out with 64-bit interval endpoints (independent of the seam's
 * modular one-compare form): the object bytes the body reads ([this+4],
 * [this+8..+11]) or the CS bytes it reads/writes (+0..+24) lie inside the
 * pushed frame [inner, inner + frame). */
static int aliases_frame(uint32_t object, uint32_t cs, uint32_t inner,
                         uint32_t frame)
{
    uint64_t f0 = inner, f1 = (uint64_t)inner + frame;
    uint64_t o0 = (uint64_t)object + 4U, o1 = (uint64_t)object + 12U;
    uint64_t c0 = cs, c1 = (uint64_t)cs + 25U;
    return (o0 < f1 && f0 < o1) || (c0 < f1 && f0 < c1);
}

static int expect_lock(const CPU *c)
{
    uint32_t object = c->ecx, cs, owner, depth;
    if (c->vita_sync_thread_id != g_isaac_vita_sync_inline_owner)
        return 0;
    if (!frame_inside(c, 24U, 8U))
        return 0;
    if (ld8(object + 4U) == 0U || ld32(c->esp + 4U) != UINT32_MAX)
        return 0;
    cs = ld32(object + 8U);
    if (!slot_ok(ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS) || !cs_object_ok(cs))
        return 0;
    if (aliases_frame(object, cs, c->esp - 24U, 24U))
        return 0;
    owner = ld32(cs + 16U);
    depth = ld32(cs + 20U);
    if (owner == c->vita_sync_thread_id) {
        if (depth == 0U || depth == UINT32_MAX)
            return 0;
    } else if (owner != 0U || depth != 0U) {
        return 0;
    }
    return ld8(cs + 0x18U) == 0U;
}

static int expect_unlock(const CPU *c)
{
    uint32_t object = c->ecx, cs;
    if (c->vita_sync_thread_id != g_isaac_vita_sync_inline_owner)
        return 0;
    if (!frame_inside(c, 12U, 4U))
        return 0;
    if (ld8(object + 4U) == 0U)
        return 0;
    cs = ld32(object + 8U);
    if (!slot_ok(ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS) || !cs_object_ok(cs))
        return 0;
    if (aliases_frame(object, cs, c->esp - 12U, 12U))
        return 0;
    return ld32(cs + 16U) == c->vita_sync_thread_id && ld32(cs + 20U) != 0U;
}

/* Can the translated body run to completion in this harness?  Lock: not
 * through the generic import route (timed wait, unauthenticated slot or
 * table), not into a foreign-owner wait, not into the Sleep loop.  Unlock:
 * the endpoint faults or completes; only the generic route is excluded. */
static int body_terminates(const CPU *c, int unlock)
{
    uint32_t object = c->ecx, cs, owner;
    if (unlock) {
        /* The body stores byte [cs+0x18] before its import; on the device
         * that store lands in guest memory (then the endpoint faults), here
         * it is replayable only inside the arena. */
        cs = ld32(object + 8U);
        if (cs - s_base >= ARENA_BYTES - 32U)
            return 0;
        return slot_ok(ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS);
    }
    if (!slot_ok(ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS))
        return 0;
    if (ld32(c->esp + 4U) != UINT32_MAX)
        return 0;
    cs = ld32(object + 8U);
    if (cs - s_base >= ARENA_BYTES - 32U || (cs & 3U) != 0U)
        return 1;                      /* boundary fault in the endpoint */
    if (ld32(cs) != ISAAC_VITA_SYNC_CS_MAGIC ||
        ld32(cs + 4U) != (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
        ld32(cs + 12U) != ISAAC_VITA_SYNC_CS_VERSION)
        return 1;                      /* validation fault in the endpoint */
    owner = ld32(cs + 16U);
    if (owner != 0U && owner != c->vita_sync_thread_id)
        return 0;                      /* would wait for a foreign owner */
    if (owner == c->vita_sync_thread_id && ld32(cs + 20U) == 0U)
        return 1;                      /* state fault in the endpoint */
    if (owner == c->vita_sync_thread_id && ld32(cs + 20U) == UINT32_MAX)
        return 1;                      /* depth overflow: endpoint decides */
    return ld8(cs + 0x18U) == 0U;      /* else the Sleep loop */
}

static int step(CPU *c, int unlock, uint32_t number)
{
    int expect = unlock ? expect_unlock(c) : expect_lock(c);
    if (body_terminates(c, unlock))
        return differential_step(c, unlock, expect, number);
    if (expect) {
        fprintf(stderr, "predicate accepts a non-terminating state at %u\n",
                number);
        return 1;
    }
    return rejection_only_step(c, unlock, number);
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

static void write_frame(CPU *c, uint32_t esp, uint32_t return_word,
                        uint32_t argument)
{
    c->esp = esp;
    if (esp - s_base < ARENA_BYTES - 8U) {
        memcpy(s_arena + (esp - s_base), &return_word, 4U);
        memcpy(s_arena + (esp - s_base) + 4U, &argument, 4U);
    }
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

    if (r < 84U)
        return floor + 32U + rnd_below((ceiling - floor - 48U) / 4U) * 4U;
    switch (r - 84U) {
    case 0:  return floor + 24U;        /* first valid Lock frame */
    case 1:  return floor + 20U;        /* Lock frame below the floor */
    case 2:  return floor + 12U;        /* first valid Unlock frame */
    case 3:  return floor + 8U;
    case 4:  return ceiling - 8U;       /* last valid Lock frame */
    case 5:  return ceiling - 4U;       /* last valid Unlock frame */
    case 6:  return ceiling;
    case 7:  return ceiling + 4U;
    case 8:  return floor;
    case 9:  return floor - 8U;
    case 10: return floor + 26U;        /* misaligned, in range */
    case 11: return ceiling - 10U;
    case 12: return s_base + 0x800U;    /* far below */
    default: return floor + 64U;
    }
}

static void corrupt(uint32_t index, uint32_t self)
{
    uint32_t address = cs_address(index);
    switch (rnd_below(14U)) {
    case 0: st32(address, ISAAC_VITA_SYNC_CS_MAGIC ^ 1U); break;
    case 1: st32(address + 4U, 0x40010003U); break;
    case 2: st32(address + 12U, 1U); break;
    case 3: st32(address + 16U, (uint32_t)CONTENDER_THREAD_ID);
            st32(address + 20U, 1U); break;
    case 4: st32(address + 16U, UINT32_MAX); st32(address + 20U, 0U); break;
    case 5: st32(address + 16U, self); st32(address + 20U, 0U); break;
    case 6: st32(address + 16U, self); st32(address + 20U, UINT32_MAX); break;
    case 7: st32(address + 16U, 0U); st32(address + 20U, 3U); break;
    case 8: st32(address + 16U, self); st32(address + 20U, UINT32_MAX - 1U);
            break;
    case 9: st8(address + 0x18U, 1U); break;               /* busy flag */
    case 10: st8(mutex_address(index) + 4U, 0U); break;     /* uninit */
    case 11: st32(mutex_address(index) + 8U, address + 2U); break;
    case 12: st32(mutex_address(index) + 8U, 0U); break;
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
        /* State-aware choice: a Lock(-1) on an object this thread already
         * holds is the non-terminating Sleep loop (rejection-only), so an
         * owned object mostly unlocks and a free one mostly locks; the
         * program then keeps producing handled pairs and the odd double. */
        int unlock = ld32(cs_address(index) + 16U) == self
                         ? rnd_below(100U) < 70U : rnd_below(100U) < 10U;
        uint32_t timeout = UINT32_MAX;

        if (r < 6U)
            corrupt(index, self);
        if (rnd_below(100U) < 4U) {
            switch (rnd_below(4U)) {
            case 0: timeout = 0U; break;
            case 1: timeout = 1U; break;
            case 2: timeout = UINT32_MAX - 1U; break;
            default: timeout = 0x7fffffffU; break;
            }
        }
        /* Slot words: the RVA token spelling or a non-token (every site
         * then takes the generic route: rejection-only until iat_tokens()
         * below restores the VA spelling). */
        if (rnd_below(2000U) == 0U)
            st32(ENTER_SLOT, rnd_below(2U) ? ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS
                                           : 0x12345678U);
        if (rnd_below(2000U) == 0U)
            st32(LEAVE_SLOT, rnd_below(2U) ? ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS
                                           : 0U);
        randomize_registers(c);
        c->ecx = mutex_address(index);
        write_frame(c, pick_esp(), unlock ? UNLOCK_RETURN : LOCK_RETURN,
                    timeout);
        if (step(c, unlock, number))
            return 1;
        /* Corruption persists until the object is re-initialized: keep the
         * population mostly healthy so handled and rejected states both stay
         * frequent. */
        if (rnd_below(8U) == 0U)
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
    g_kage_guest_last_indirect_target = 0x00562e00U;   /* the vtable target */
}

int main(int argc, char **argv)
{
    CPU cpu;
    uint32_t steps = 200000U;
    uint32_t i;
    uint32_t k;
    void *iat_map;
    observation before, after;

    if (argc > 1)
        steps = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        s_rng = strtoull(argv[2], NULL, 0) | 1U;

    s_arena = map_low(ARENA_BYTES, 0U);
    CHECK(s_arena != NULL);
    CHECK((uintptr_t)s_arena <= UINT32_MAX - ARENA_BYTES);
    s_base = (uint32_t)(uintptr_t)s_arena;
    memset(s_arena, 0, ARENA_BYTES);
    iat_map = map_low(IAT_BYTES, (uintptr_t)IAT_PAGE);
    CHECK(iat_map != NULL && (uintptr_t)iat_map == IAT_PAGE);
    s_iat = (uint8_t *)iat_map;
    memset(s_iat, 0, IAT_BYTES);
    iat_tokens();
    g_isaac_vita_import_ids_ready = 1;

    /* Park the live-object census where no replayed operation can cross a
     * power of two (the create receipt fires there). */
    for (i = 0U; i < 140000U; ++i)
        CHECK(isaac_vita_sync_create_recursive_mutex() ==
              ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
    for (i = 0U; i < OBJECT_COUNT; ++i)
        make_free(i);
    reset_counters();

    /* Phase 0: unarmed latch and unbound CPU never take the seam. */
    cpu_init(&cpu);
    CHECK(g_isaac_vita_sync_inline_owner == ISAAC_VITA_SYNC_INLINE_OWNER_NONE);
    cpu.ecx = mutex_address(0U);
    write_frame(&cpu, cpu.stack_ceiling - 64U, LOCK_RETURN, UINT32_MAX);
    CHECK(!rejection_only_step(&cpu, 0, 1U));
    write_frame(&cpu, cpu.stack_ceiling - 64U, UNLOCK_RETURN, 0U);
    CHECK(!rejection_only_step(&cpu, 1, 2U));
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(cpu.vita_sync_thread_id == (uint32_t)MAIN_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);

    /* Phase 1: directed success states with exact expected values.
     *
     * Nested Lock states first.  The x86 body spins in Sleep(1000) while the
     * busy byte is set (a real nested Lock(-1) on the same thread deadlocks
     * the game), so the recursive-owner state is entered with the byte
     * cleared: exactly the owner == self, depth + 1 transition of the inline
     * Enter fast path, which the seam must replay identically. */
    {
        uint32_t object = mutex_address(0U);
        uint32_t cs = cs_address(0U);
        uint32_t esp = cpu.stack_ceiling - 64U;

        for (k = 0U; k < 5U; ++k) {
            uint32_t low_before;
            cpu.eax = 0xa5a5a500U + k;
            cpu.ebx = 0x11110000U + k;
            cpu.esi = 0x22220000U + k;
            cpu.edi = 0x33330000U + k;
            cpu.ebp = 0x44440000U + k;
            cpu.edx = 0x55550000U + k;
            cpu.ecx = object;
            cpu.stack_low_water = cpu.stack_ceiling;
            low_before = cpu.stack_low_water;
            if (k)
                st8(cs + 0x18U, 0U);
            write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, 0, 1, 1000U + k));
            observe(&after, &cpu);
            CHECK(ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID &&
                  ld32(cs + 20U) == k + 1U && ld8(cs + 0x18U) == 1U);
            /* mov al, 1: the upper 24 bits are the caller's. */
            CHECK(cpu.eax == 0xa5a5a501U);
            CHECK(cpu.ecx == object && cpu.edx == 0x55550000U + k &&
                  cpu.ebx == 0x11110000U + k && cpu.esi == 0x22220000U + k &&
                  cpu.edi == 0x33330000U + k && cpu.ebp == 0x44440000U + k);
            CHECK(cpu.esp == esp + 8U && !cpu.fault);
            /* The pushed frame below the entry ESP, as the body leaves it. */
            CHECK(ld32(esp - 4U) == 0x44440000U + k &&
                  ld32(esp - 8U) == 0x11110000U + k &&
                  ld32(esp - 12U) == 0x22220000U + k &&
                  ld32(esp - 16U) == 0x33330000U + k &&
                  ld32(esp - 20U) == cs && ld32(esp - 24U) == 0x00562e2fU);
#if GUEST_GENERATED_STACK_GUARD
            CHECK(cpu.stack_low_water == esp - 24U);
#else
            CHECK(cpu.stack_low_water == esp - 20U);
#endif
            (void)low_before;
            CHECK(after.host_import_calls == before.host_import_calls + 1U);
            CHECK(after.profile.guest_calls == before.profile.guest_calls + 1U);
            CHECK(after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 1U);
            CHECK(after.import_calls[1] == before.import_calls[1] + 1U &&
                  after.import_calls[0] == before.import_calls[0]);
            CHECK(after.coverage_imports[1] == 1U);
            CHECK(after.sampler_word == 0x00562e00U);
            CHECK(after.faults == before.faults &&
                  after.logger_calls == before.logger_calls);
        }
        CHECK(s_lock_hits == 5U && s_unlock_hits == 0U);
        for (k = 5U; k > 0U; --k) {
            cpu.eax = 0xdeadbeefU;
            cpu.esi = 0x66660000U + k;
            cpu.ecx = object;
            cpu.stack_low_water = cpu.stack_ceiling;
            write_frame(&cpu, esp, UNLOCK_RETURN, 0x99999999U);
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, 1, 1, 2000U + k));
            observe(&after, &cpu);
            CHECK(ld32(cs + 20U) == k - 1U && ld8(cs + 0x18U) == 0U);
            CHECK(ld32(cs + 16U) == (k > 1U ? (uint32_t)MAIN_THREAD_ID : 0U));
            CHECK(cpu.eax == cs && cpu.esi == 0x66660000U + k &&
                  cpu.esp == esp + 4U && !cpu.fault);
            CHECK(ld32(esp - 4U) == 0x66660000U + k &&
                  ld32(esp - 8U) == cs && ld32(esp - 12U) == 0x00562ee6U);
#if GUEST_GENERATED_STACK_GUARD
            CHECK(cpu.stack_low_water == esp - 12U);
#else
            CHECK(cpu.stack_low_water == esp - 8U);
#endif
            CHECK(after.host_import_calls == before.host_import_calls + 1U);
            CHECK(after.profile.guest_calls == before.profile.guest_calls + 1U);
            CHECK(after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 1U);
            CHECK(after.import_calls[0] == before.import_calls[0] + 1U &&
                  after.import_calls[1] == before.import_calls[1]);
            CHECK(after.coverage_imports[0] == 1U);
            CHECK(after.sampler_word == 0x00562e00U);
        }
        CHECK(s_unlock_hits == 5U && s_rejections == 2U);

        /* The production pattern: balanced Lock(-1)/Unlock pairs on a free
         * object (TryAddRef/AddRef/Release around a reference count). */
        for (k = 0U; k < 5U; ++k) {
            cpu.eax = 0x12345600U + k;
            cpu.ecx = object;
            write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, 0, 1, 1100U + k));
            observe(&after, &cpu);
            CHECK(ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID &&
                  ld32(cs + 20U) == 1U && ld8(cs + 0x18U) == 1U);
            CHECK(cpu.eax == (((0x12345600U + k) & 0xFFFFFF00U) | 1U) &&
                  cpu.esp == esp + 8U && !cpu.fault);
            CHECK(after.host_import_calls == before.host_import_calls + 1U &&
                  after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 1U &&
                  after.import_calls[1] == before.import_calls[1] + 1U);
            cpu.eax = 0xdeadbeefU;
            cpu.ecx = object;
            write_frame(&cpu, esp, UNLOCK_RETURN, 0U);
            observe(&before, &cpu);
            CHECK(!differential_step(&cpu, 1, 1, 1200U + k));
            observe(&after, &cpu);
            CHECK(ld32(cs + 16U) == 0U && ld32(cs + 20U) == 0U &&
                  ld8(cs + 0x18U) == 0U);
            CHECK(cpu.eax == cs && cpu.esp == esp + 4U && !cpu.fault);
            CHECK(after.host_import_calls == before.host_import_calls + 1U &&
                  after.profile.sync_fastpath ==
                  before.profile.sync_fastpath + 1U &&
                  after.import_calls[0] == before.import_calls[0] + 1U);
        }
        CHECK(s_lock_hits == 10U && s_unlock_hits == 10U &&
              s_rejections == 2U);
    }

    /* Phase 2: every fallback state, directed. */
    {
        uint32_t object = mutex_address(1U);
        uint32_t cs = cs_address(1U);
        uint32_t esp = cpu.stack_ceiling - 96U;
        uint32_t rejections;

#define LOCK_FRAME(argument) do { \
        cpu.ecx = object; cpu.eax = 0x0badf00dU; \
        write_frame(&cpu, esp, LOCK_RETURN, (argument)); } while (0)
#define UNLOCK_FRAME() do { \
        cpu.ecx = object; cpu.eax = 0x0badf00dU; \
        write_frame(&cpu, esp, UNLOCK_RETURN, 0U); } while (0)

        /* Uninitialized mutex: the logging path, then the same lock. */
        make_free(1U);
        st8(object + 4U, 0U);
        LOCK_FRAME(UINT32_MAX);
        rejections = s_rejections;
        CHECK(!differential_step(&cpu, 0, 0, 3000U));
        CHECK(s_rejections == rejections + 1U && s_logger_calls == 1U);
        CHECK(ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID &&
              cpu.eax == 0x0badf001U);
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3001U));
        CHECK(s_logger_calls == 2U && ld32(cs + 16U) == 0U);
        make_free(1U);

        /* Timed waits go through the generic import route. */
        LOCK_FRAME(0U);
        CHECK(!rejection_only_step(&cpu, 0, 3002U));
        LOCK_FRAME(1U);
        CHECK(!rejection_only_step(&cpu, 0, 3003U));
        LOCK_FRAME(UINT32_MAX - 1U);
        CHECK(!rejection_only_step(&cpu, 0, 3004U));
        LOCK_FRAME(0x7fffffffU);
        CHECK(!rejection_only_step(&cpu, 0, 3005U));

        /* Busy flag set: the Sleep(1000) loop must run translated. */
        st8(cs + 0x18U, 1U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!rejection_only_step(&cpu, 0, 3006U));
        st8(cs + 0x18U, 0U);

        /* Foreign owner: the endpoint would wait. */
        st32(cs + 16U, (uint32_t)CONTENDER_THREAD_ID);
        st32(cs + 20U, 1U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!rejection_only_step(&cpu, 0, 3007U));
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3008U));   /* endpoint fault */
        make_free(1U);

        /* CLAIMING owner. */
        st32(cs + 16U, UINT32_MAX);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!rejection_only_step(&cpu, 0, 3009U));
        make_free(1U);

        /* Depth wrap edges. */
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(cs + 20U, UINT32_MAX);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3010U));
        st32(cs + 20U, UINT32_MAX - 1U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 1, 3011U));
        CHECK(ld32(cs + 20U) == UINT32_MAX);
        make_free(1U);

        /* Owner self with depth 0, free object with depth 3. */
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(cs + 20U, 0U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3012U));
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3013U));
        make_free(1U);
        st32(cs + 20U, 3U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3014U));
        make_free(1U);

        /* Unlock while free. */
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3015U));
        make_free(1U);

        /* Corrupt magic / handle / version. */
        st32(cs, ISAAC_VITA_SYNC_CS_MAGIC ^ 1U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3016U));
        make_free(1U);
        st32(cs + 4U, 0x40010003U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3017U));
        make_free(1U);
        st32(cs + 12U, 1U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3018U));
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3019U));
        make_free(1U);

        /* Misaligned, null and top-of-memory critical-section pointers. */
        st32(object + 8U, cs + 2U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3020U));
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3021U));
        st32(object + 8U, 0U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3022U));
        st32(object + 8U, UINT32_C(0xfffffff0));
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3023U));
        /* Unlock would store byte [cs+0x18] = guest address 8 before the
         * endpoint faults; not host-mapped here, so rejection-only. */
        UNLOCK_FRAME();
        CHECK(!rejection_only_step(&cpu, 1, 3024U));
        make_free(1U);

        /* IAT slot not the loader token / table not registered: the
         * translated site takes the generic route. */
        st32(ENTER_SLOT, 0x12345678U);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!rejection_only_step(&cpu, 0, 3025U));
        iat_tokens();
        st32(LEAVE_SLOT, 0U);
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(cs + 20U, 1U);
        UNLOCK_FRAME();
        CHECK(!rejection_only_step(&cpu, 1, 3026U));
        iat_tokens();
        make_free(1U);
        g_isaac_vita_import_ids_ready = 0;
        LOCK_FRAME(UINT32_MAX);
        CHECK(!rejection_only_step(&cpu, 0, 3027U));
        g_isaac_vita_import_ids_ready = 1;

        /* RVA-form slot words are the other accepted token spelling. */
        st32(ENTER_SLOT, ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS);
        st32(LEAVE_SLOT, ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS);
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 1, 3028U));
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 1, 3029U));
        iat_tokens();

        /* Disabled latch: the endpoint completes without waiting. */
        isaac_vita_sync_inline_disable();
        LOCK_FRAME(UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 3030U));
        UNLOCK_FRAME();
        CHECK(!differential_step(&cpu, 1, 0, 3031U));
        g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
        cpu_init(&cpu);
        isaac_vita_sync_bind_current_thread(&cpu);
        CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
        make_free(1U);

        /* Unbound CPU (thread id 0): never inline; a rejection-only case
         * because its endpoint visit would disarm the latch. */
        {
            CPU second;
            cpu_init(&second);
            second.ecx = object;
            write_frame(&second, second.stack_ceiling - 96U, LOCK_RETURN,
                        UINT32_MAX);
            CHECK(!rejection_only_step(&second, 0, 3032U));
            write_frame(&second, second.stack_ceiling - 96U, UNLOCK_RETURN, 0U);
            CHECK(!rejection_only_step(&second, 1, 3033U));
            /* Another bound identity. */
            second.vita_sync_thread_id = (uint32_t)CONTENDER_THREAD_ID;
            write_frame(&second, second.stack_ceiling - 96U, LOCK_RETURN,
                        UINT32_MAX);
            CHECK(!rejection_only_step(&second, 0, 3034U));
            /* Copied CPU: stack ownership fails. */
            second = cpu;
            second.ecx = object;
            write_frame(&second, second.stack_ceiling - 96U, LOCK_RETURN,
                        UINT32_MAX);
            CHECK(!rejection_only_step(&second, 0, 3035U));
        }
#undef LOCK_FRAME
#undef UNLOCK_FRAME
    }

    /* Phase 2b: hostile aliasing of the pushed frame.  The translated body
     * reads the object after its first pushes and the critical section after
     * all of them, so an object or CS lying inside [esp - frame, esp) is
     * corrupted by the call itself (the x86 body does the same; such memory
     * is dead stack for any well-formed guest).  The seam evaluates its
     * predicates before it stores anything, so it must refuse these frames
     * outright; the mirror cases just outside the frame must stay handled. */
    {
        uint32_t esp = cpu.stack_ceiling - 128U;
        uint32_t object = mutex_address(3U);
        uint32_t cs = cs_address(3U);
        uint32_t cs4 = cs_address(4U);
        uint32_t alias;

        /* A: Lock, object inside the frame: push esi lands on its init byte,
         * push ebx on its CS pointer (the body then locks "CS 0" -> fault). */
        make_free(3U);
        alias = esp - 16U;
        st32(alias, 0x9875d000U);
        st8(alias + 4U, 1U);
        st32(alias + 8U, cs);
        cpu.ecx = alias; cpu.eax = 0x0badf00dU; cpu.ebx = 0U; cpu.esi = 1U;
        cpu.edi = 0x33333333U; cpu.ebp = 0x44444444U;
        write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 4000U));
        CHECK(ld32(cs + 16U) == 0U);            /* the real CS stays free */

        /* B: Lock, CS inside the frame (words under the pushes, busy byte
         * under push ebp): the body's inline path sees a corrupt version. */
        make_free(3U);
        alias = esp - 28U;
        st32(alias, ISAAC_VITA_SYNC_CS_MAGIC);
        st32(alias + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
        st32(alias + 8U, 0U);
        st32(alias + 12U, ISAAC_VITA_SYNC_CS_VERSION);
        st32(alias + 16U, 0U);
        st32(alias + 20U, 0U);
        st8(alias + 24U, 0U);
        st32(object + 8U, alias);
        cpu.ecx = object; cpu.eax = 0x0badf00dU; cpu.ebx = 0U; cpu.esi = 0U;
        cpu.edi = 0U; cpu.ebp = 0U;
        write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 0, 4001U));

        /* C: Lock, CS words just below the frame but its busy byte under the
         * return-word push: the body would spin in Sleep(1000). */
        make_free(3U);
        alias = esp - 48U;
        st32(alias, ISAAC_VITA_SYNC_CS_MAGIC);
        st32(alias + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
        st32(alias + 8U, 0U);
        st32(alias + 12U, ISAAC_VITA_SYNC_CS_VERSION);
        st32(alias + 16U, 0U);
        st32(alias + 20U, 0U);
        st8(alias + 24U, 0U);
        st32(object + 8U, alias);
        cpu.ecx = object; cpu.eax = 0x0badf00dU;
        write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
        CHECK(!rejection_only_step(&cpu, 0, 4002U));

        /* D: Unlock, object inside the frame: push esi lands on its CS
         * pointer, so the body unlocks esi's CS, not the object's. */
        make_free(3U);
        make_free(4U);
        st32(cs + 16U, (uint32_t)MAIN_THREAD_ID);  st32(cs + 20U, 1U);
        st8(cs + 0x18U, 1U);
        st32(cs4 + 16U, (uint32_t)MAIN_THREAD_ID); st32(cs4 + 20U, 1U);
        st8(cs4 + 0x18U, 1U);
        alias = esp - 12U;
        st32(alias, 0x9875d000U);
        st8(alias + 4U, 1U);
        st32(alias + 8U, cs);
        cpu.ecx = alias; cpu.eax = 0x0badf00dU; cpu.esi = cs4;
        write_frame(&cpu, esp, UNLOCK_RETURN, 0U);
        CHECK(!differential_step(&cpu, 1, 0, 4003U));
        CHECK(ld32(cs4 + 16U) == 0U && ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID);

        /* E: Unlock, CS straddling the frame: its depth word sits where the
         * return-word push lands (owner/depth/busy otherwise valid). */
        make_free(3U);
        make_free(4U);
        alias = esp - 32U;
        st32(alias, ISAAC_VITA_SYNC_CS_MAGIC);
        st32(alias + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
        st32(alias + 8U, 0U);
        st32(alias + 12U, ISAAC_VITA_SYNC_CS_VERSION);
        st32(alias + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(alias + 20U, 1U);
        st8(alias + 24U, 1U);
        st32(object + 8U, alias);
        cpu.ecx = object; cpu.eax = 0x0badf00dU; cpu.esi = 0x66666666U;
        write_frame(&cpu, esp, UNLOCK_RETURN, 0U);
        CHECK(!differential_step(&cpu, 1, 0, 4004U));

        /* F: no over-rejection.  A CS immediately below the Lock frame (span
         * [esp-52, esp-27) against [esp-24, esp)) and, separately, an object
         * immediately below the Lock frame (span [esp-32, esp-24)) and below
         * the Unlock frame (span [esp-20, esp-12) against [esp-12, esp))
         * stay on the handled path. */
        make_free(3U);
        alias = esp - 52U;
        st32(alias, ISAAC_VITA_SYNC_CS_MAGIC);
        st32(alias + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
        st32(alias + 8U, 0U);
        st32(alias + 12U, ISAAC_VITA_SYNC_CS_VERSION);
        st32(alias + 16U, 0U);
        st32(alias + 20U, 0U);
        st8(alias + 24U, 0U);
        st32(object + 8U, alias);
        cpu.ecx = object; cpu.eax = 0x0badf00dU;
        write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 1, 4005U));
        CHECK(ld32(alias + 16U) == (uint32_t)MAIN_THREAD_ID &&
              ld8(alias + 24U) == 1U && cpu.eax == 0x0badf001U);
        cpu.ecx = object; cpu.eax = 0x0badf00dU;
        write_frame(&cpu, esp, UNLOCK_RETURN, 0U);
        CHECK(!differential_step(&cpu, 1, 1, 4006U));
        CHECK(ld32(alias + 16U) == 0U && cpu.eax == alias);
        make_free(3U);
        /* Object right below the Lock frame, its CS elsewhere (row 3). */
        st32(esp - 36U, 0x9875d000U);
        st8(esp - 32U, 1U);
        st32(esp - 28U, cs);
        cpu.ecx = esp - 36U; cpu.eax = 0x0badf00dU;
        write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
        CHECK(!differential_step(&cpu, 0, 1, 4007U));
        CHECK(ld32(cs + 16U) == (uint32_t)MAIN_THREAD_ID &&
              ld8(cs + 0x18U) == 1U && cpu.eax == 0x0badf001U);
        /* Object right below the Unlock frame. */
        st32(esp - 24U, 0x9875d000U);
        st8(esp - 20U, 1U);
        st32(esp - 16U, cs);
        cpu.ecx = esp - 24U; cpu.eax = 0x0badf00dU;
        write_frame(&cpu, esp, UNLOCK_RETURN, 0U);
        CHECK(!differential_step(&cpu, 1, 1, 4008U));
        CHECK(ld32(cs + 16U) == 0U && ld8(cs + 0x18U) == 0U && cpu.eax == cs);
        make_free(3U);
        make_free(4U);
        printf("hostile aliasing: 5 refused, 4 handled\n");
    }

    /* Phase 3: the stack predicate, exhaustively over the owned interval plus
     * both margins.  Lock accepts exactly word-aligned floor+24 <= esp <=
     * ceiling-8, Unlock exactly word-aligned floor+12 <= esp <= ceiling-4
     * (the odd ESPs of the sweep exercise the alignment refusal); every
     * state runs both
     * routes (below the floor and above the ceiling the endpoint faults or,
     * on the raw stack, the body completes -- either way both routes agree). */
    {
        uint32_t object = mutex_address(2U);
        uint32_t floor = cpu.stack_floor, ceiling = cpu.stack_ceiling;
        uint32_t sweep = 0U;

        for (i = STACK_FLOOR_OFF - 64U; i <= STACK_CEIL_OFF + 64U; i += 2U) {
            uint32_t esp = s_base + i;
            int fast;

            make_free(2U);
            cpu.ecx = object;
            cpu.eax = 0xa51ca11eU;
            cpu.ebx = 0x0b0b0b0bU;
            cpu.esi = 0x51515151U;
            cpu.edi = 0xd1d1d1d1U;
            cpu.ebp = 0xb9b9b9b9U;
            write_frame(&cpu, esp, LOCK_RETURN, UINT32_MAX);
            fast = expect_lock(&cpu);
            CHECK(fast == (esp >= floor + 24U && esp <= ceiling - 8U &&
                           (esp & 3U) == 0U));
            CHECK(!differential_step(&cpu, 0, fast, 5000U + i));
            if (fast)
                CHECK(ld32(cs_address(2U) + 16U) == (uint32_t)MAIN_THREAD_ID &&
                      cpu.esp == esp + 8U);
            /* Unlock from an owned object at the same ESP. */
            make_free(2U);
            st32(cs_address(2U) + 16U, (uint32_t)MAIN_THREAD_ID);
            st32(cs_address(2U) + 20U, 1U);
            st8(cs_address(2U) + 0x18U, 1U);
            cpu.ecx = object;
            write_frame(&cpu, esp, UNLOCK_RETURN, 0U);
            fast = expect_unlock(&cpu);
            CHECK(fast == (esp >= floor + 12U && esp <= ceiling - 4U &&
                           (esp & 3U) == 0U));
            CHECK(!differential_step(&cpu, 1, fast, 6000U + i));
            if (fast)
                CHECK(ld32(cs_address(2U) + 16U) == 0U && cpu.esp == esp + 4U &&
                      cpu.eax == cs_address(2U));
            ++sweep;
        }
        make_free(2U);
        cpu_init(&cpu);
        isaac_vita_sync_bind_current_thread(&cpu);
        CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
        printf("stack sweep: %u ESP values x2\n", sweep);
    }

    /* Phase 4: seeded random program over eight objects. */
    for (i = 0U; i < OBJECT_COUNT; ++i)
        make_free(i);
    iat_tokens();
    {
        uint32_t lock_hits = s_lock_hits, unlock_hits = s_unlock_hits;
        uint32_t rejected = s_rejections;
        CHECK(!random_phase(&cpu, steps));
        printf("random program: lock_hits=%u unlock_hits=%u rejected=%u "
               "differentials=%u rejection_only=%u\n",
               s_lock_hits - lock_hits, s_unlock_hits - unlock_hits,
               s_rejections - rejected, s_differentials, s_rejection_only);
        CHECK(s_lock_hits - lock_hits > steps / 8U);
        CHECK(s_unlock_hits - unlock_hits > steps / 10U);
        CHECK(s_rejections - rejected > steps / 8U);
        CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
    }

    printf("Vita KAGE mutex seam oracle: PASS "
           "(stack_guard=%d steps=%u lock_hits=%u unlock_hits=%u rejected=%u "
           "differentials=%u rejection_only=%u checks=%u imports=%u faults=%u "
           "logger=%u)\n",
           GUEST_GENERATED_STACK_GUARD, steps, s_lock_hits, s_unlock_hits,
           s_rejections, s_differentials, s_rejection_only, s_checks,
           g_host_import_calls, s_fault_calls, s_logger_calls);
    return 0;
}
