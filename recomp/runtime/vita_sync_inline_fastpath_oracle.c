/* Host differential oracle for the inline single-owner Enter/Leave path.
 *
 * One guest arena (mmap MAP_32BIT so guest address == host address) holds a
 * bound synthetic stack and a row of CRITICAL_SECTION objects.  Every step of
 * a seeded random program is executed twice from an identical snapshot:
 *   old: the registered endpoint isaac_vita_sync_import_indexed only;
 *   new: isaac_vita_sync_inline_enter/leave first, endpoint on fallback
 * and the complete CPU struct, the complete arena, g_host_import_calls, the
 * fault/log/delay counters and the mutex pool telemetry deltas must agree
 * byte for byte.  A pthread contender owns one object for the whole random
 * phase (TryEnter/Leave/Delete against a foreign owner), then releases it
 * while the main thread blocks in Enter through both routes.  Finally the
 * thread latch is driven through every disarming transition. */
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr/eventflag.h>

#include "guest.h"
#include "host_vita_import_id.h"
#include "host_vita_sync.h"
#include "host_vita_sync_fastpath.h"
#include "vita_sync_services.h"

#if !defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
#error This oracle requires ISAAC_VITA_SYNC_INLINE_FASTPATH
#endif
#if !GUEST_STACK_REQUIRED
#error This oracle mirrors production: compile with GUEST_STACK_REQUIRED=1
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* ---- Vita service mocks (same shape as vita_sync_embedded_host_oracle) ---- */

#define MAIN_THREAD_ID      ((int32_t)UINT32_C(0x40010003))
#define CONTENDER_THREAD_ID ((int32_t)UINT32_C(0x40010005))

static _Thread_local int32_t s_thread_id = MAIN_THREAD_ID;
static unsigned s_delay_calls;
static unsigned s_log_calls;
static unsigned s_fault_calls;
static jmp_buf s_fault_env;
static int s_fault_scope;

unsigned g_host_import_calls;

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
    __atomic_add_fetch(&s_delay_calls, 1U, __ATOMIC_RELAXED);
    if (usec != 1U)
        return -1;
    sched_yield();
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

/* ---- guest.c pieces the endpoint links against ------------------------- */

/* Production guest_fault never returns: guest_run_until_stop's scope
 * longjmps out of the endpoint.  The replay reproduces that exactly. */
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

/* ---- arena ---------------------------------------------------------------- */

#define ARENA_BYTES     UINT32_C(0x8000)
#define STACK_FLOOR_OFF UINT32_C(0x1000)
#define STACK_CEIL_OFF  UINT32_C(0x5000)
#define CS_ROW_OFF      UINT32_C(0x6000)
#define CS_STRIDE       32U
#define CS_COUNT        8U
#define CONTENDED_CS    (CS_COUNT - 1U)
#define RETURN_RVA      UINT32_C(0x00562e2f)

static uint8_t *s_arena;
static uint32_t s_base;
static uint8_t s_snapshot[ARENA_BYTES];
static uint8_t s_old_arena[ARENA_BYTES];

static uint32_t cs_address(uint32_t index)
{
    return s_base + CS_ROW_OFF + index * CS_STRIDE;
}

/* ---- endpoint routes ------------------------------------------------------ */

static uint32_t s_local_init_spin;
static uint32_t s_local_init_plain;
static uint32_t s_local_enter;
static uint32_t s_local_leave;
static uint32_t s_local_delete;
static uint32_t s_local_try_enter;

static int find_local(const char *name, uint32_t *out)
{
    uint32_t i;
    for (i = 0U; i < ISAAC_VITA_SYNC_IMPORT_COUNT; ++i) {
        const char *candidate = isaac_vita_sync_import_name(i);
        if (candidate && strcmp(candidate, name) == 0) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

static uint32_t s_fast_hits;
static uint32_t s_fast_misses;

/* The registered endpoint under a fault scope, as guest_run_until_stop
 * runs it in production. */
static void endpoint(CPU *c, uint32_t local)
{
    s_fault_scope = 1;
    if (setjmp(s_fault_env) == 0)
        (void)isaac_vita_sync_import_indexed(c, local, &g_host_import_calls);
    s_fault_scope = 0;
}

/* The old route is the registered endpoint alone. */
static void route_old(CPU *c, uint32_t local)
{
    endpoint(c, local);
}

/* The new route is exactly what guest.c does under the option. */
static void route_new(CPU *c, uint32_t local)
{
    int handled = 0;

    if (local == s_local_enter)
        handled = isaac_vita_sync_inline_enter(c, &g_host_import_calls);
    else if (local == s_local_leave)
        handled = isaac_vita_sync_inline_leave(c, &g_host_import_calls);
    if (handled) {
        ++s_fast_hits;
        return;
    }
    if (local == s_local_enter || local == s_local_leave)
        ++s_fast_misses;
    endpoint(c, local);
}

/* ---- observation ---------------------------------------------------------- */

typedef struct observation {
    CPU cpu;
    unsigned host_import_calls;
    unsigned faults;
    unsigned logs;
    unsigned delays;
    isaac_vita_sync_mutex_pool_snapshot pool;
} observation;

static void observe(observation *out, const CPU *c)
{
    out->cpu = *c;
    out->host_import_calls = g_host_import_calls;
    out->faults = s_fault_calls;
    out->logs = s_log_calls;
    out->delays = __atomic_load_n(&s_delay_calls, __ATOMIC_RELAXED);
    isaac_vita_sync_get_mutex_pool_snapshot(&out->pool);
}

/* Counters are process globals that the replay cannot restore, so the old
 * route's delta (old - before) is compared with the new route's delta
 * (fresh - old). */
#define DELTA_EQUAL(field)     ((old->field) - (before->field) == (fresh->field) - (old->field))

static int observations_equal(const observation *before,
                              const observation *old,
                              const observation *fresh)
{
    if (memcmp(&old->cpu, &fresh->cpu, sizeof old->cpu) != 0)
        return 0;
    if (!DELTA_EQUAL(host_import_calls) || !DELTA_EQUAL(faults) ||
        !DELTA_EQUAL(logs) || !DELTA_EQUAL(delays))
        return 0;
    if (!DELTA_EQUAL(pool.live) || !DELTA_EQUAL(pool.creates) ||
        !DELTA_EQUAL(pool.deletes) || !DELTA_EQUAL(pool.waits) ||
        !DELTA_EQUAL(pool.wait_failures) || !DELTA_EQUAL(pool.owner_failures) ||
        !DELTA_EQUAL(pool.state_failures))
        return 0;
    return 1;
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
    fprintf(stderr, "  esp old=%08x new=%08x eax old=%08x new=%08x "
                    "fault old=%s new=%s low_water old=%08x new=%08x\n",
            a->esp, b->esp, a->eax, b->eax,
            a->fault ? a->fault : "-", b->fault ? b->fault : "-",
            a->stack_low_water, b->stack_low_water);
}

/* Run one operation through both routes from the same snapshot and leave the
 * (identical) new-route state in place. */
static int differential_step(CPU *c, uint32_t local, uint32_t step)
{
    observation before, old, fresh;
    CPU saved = *c;
    unsigned fast_hits_before = s_fast_hits;

    observe(&before, c);
    memcpy(s_snapshot, s_arena, ARENA_BYTES);

    route_old(c, local);
    observe(&old, c);
    memcpy(s_old_arena, s_arena, ARENA_BYTES);

    *c = saved;
    memcpy(s_arena, s_snapshot, ARENA_BYTES);
    route_new(c, local);
    observe(&fresh, c);

    if (!observations_equal(&before, &old, &fresh) ||
        memcmp(s_old_arena, s_arena, ARENA_BYTES) != 0) {
        fprintf(stderr,
                "differential mismatch at step %u local=%u fast=%u\n",
                step, local, s_fast_hits != fast_hits_before);
        dump_cpu_diff(&old.cpu, &fresh.cpu);
        fprintf(stderr, "  imports %u/%u faults %u/%u logs %u/%u "
                        "waits %u/%u owner_failures %u/%u "
                        "state_failures %u/%u arena %s\n",
                old.host_import_calls, fresh.host_import_calls,
                old.faults, fresh.faults, old.logs, fresh.logs,
                old.pool.waits, fresh.pool.waits,
                old.pool.owner_failures, fresh.pool.owner_failures,
                old.pool.state_failures, fresh.pool.state_failures,
                memcmp(s_old_arena, s_arena, ARENA_BYTES) ? "DIFFERS"
                                                          : "same");
        return 1;
    }
    /* The production scope stops at a fault; the replay clears it and
     * continues, which both routes have already agreed on. */
    c->fault = NULL;
    c->fault_addr = 0U;
    c->stop_kind = GUEST_RUN_RETURNED;
    return 0;
}

/* ---- random program -------------------------------------------------------- */

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

/* Guest memory is byte-addressed x86 state; a misaligned ESP is a legal
 * replay input, so the harness writes the frame with memcpy. */
static void write_call_frame(CPU *c, uint32_t esp, uint32_t argument)
{
    uint32_t return_rva = RETURN_RVA;

    c->esp = esp;
    if (esp - s_base < ARENA_BYTES - 8U) {
        memcpy(s_arena + (esp - s_base), &return_rva, 4U);
        memcpy(s_arena + (esp - s_base) + 4U, &argument, 4U);
    }
}

static uint32_t pick_esp(void)
{
    uint32_t floor = s_base + STACK_FLOOR_OFF;
    uint32_t ceiling = s_base + STACK_CEIL_OFF;
    uint32_t r = rnd_below(100U);

    if (r < 84U)
        return floor + 8U + rnd_below((ceiling - floor - 16U) / 4U) * 4U;
    switch (r - 84U) {
    case 0:  return floor;              /* valid: esp+8 <= ceiling */
    case 1:  return floor + 4U;
    case 2:  return ceiling - 8U;       /* last valid */
    case 3:  return ceiling - 4U;       /* pop ok, adjust faults */
    case 4:  return ceiling;            /* address fault */
    case 5:  return ceiling + 4U;
    case 6:  return floor - 4U;         /* below floor */
    case 7:  return floor - 8U;
    case 8:  return s_base;             /* far below */
    case 9:  return s_base + ARENA_BYTES - 8U;
    case 10: return UINT32_C(0xfffffff8);
    case 11: return floor + 2U;         /* misaligned, still in range */
    case 12: return ceiling - 10U;
    case 13: return floor - 2U;
    case 14: return ceiling - 6U;
    default: return floor + 16U;
    }
}

static uint32_t pick_cs_argument(uint32_t index)
{
    uint32_t r = rnd_below(100U);
    if (r < 90U)
        return cs_address(index);
    switch (r - 90U) {
    case 0: return 0U;
    case 1: return cs_address(index) + 1U;
    case 2: return cs_address(index) + 2U;
    case 3: return UINT32_C(0xfffffff0);
    case 4: return UINT32_C(0xffffffec);
    case 5: return s_base + CS_ROW_OFF - 64U;     /* not an object */
    case 6: return s_base + STACK_FLOOR_OFF + 64U; /* stack bytes */
    default: break;
    }
    /* Object-interior arguments: an Init there writes a 24-byte object that
     * overlaps the next row, so keep them two rows away from the contender. */
    if (index + 2U > CONTENDED_CS)
        return cs_address(index);
    switch (r - 97U) {
    case 0: return cs_address(index) + 4U;
    case 1: return cs_address(index) + 8U;
    default: return cs_address(index) + 16U;
    }
}

/* The endpoint waits (forever, in a single-thread replay) exactly when the
 * argument names a boundary-valid object whose owner word is a foreign
 * non-zero identity.  Both routes see the same state, so the program
 * substitutes the bounded TryEnter for those steps. */
static int enter_would_wait(uint32_t argument, uint32_t self)
{
    uint32_t owner;

    if (argument - s_base >= ARENA_BYTES - 24U || (argument & 3U) != 0U)
        return 0;
    owner = ld32(argument + 16U);
    return owner != 0U && owner != self;
}

/* Returns non-zero when the object is left owned by a foreign or CLAIMING
 * identity: a replayed Enter on it would wait for a release that never
 * comes, so the caller drives the bounded operations and restores it. */
static int corrupt(uint32_t index, uint32_t self)
{
    uint32_t address = cs_address(index);
    switch (rnd_below(12U)) {
    case 0: st32(address, ISAAC_VITA_SYNC_CS_MAGIC ^ 1U); break;
    case 1: st32(address + 4U, 0x40010003U); break;   /* kernel-like uid */
    case 2: st32(address + 12U, 1U); break;
    case 3: st32(address + 16U, (uint32_t)CONTENDER_THREAD_ID);
            st32(address + 20U, 1U); return 1;
    case 4: st32(address + 16U, UINT32_MAX); st32(address + 20U, 0U);
            return 1;
    case 5: st32(address + 16U, self); st32(address + 20U, 0U); break;
    case 6: st32(address + 16U, self); st32(address + 20U, UINT32_MAX); break;
    case 7: st32(address + 16U, 0U); st32(address + 20U, 3U); break;
    case 8: st32(address + 16U, self); st32(address + 20U, UINT32_MAX - 1U);
            break;
    case 9: /* restore a sane initialized, free object */
            st32(address, ISAAC_VITA_SYNC_CS_MAGIC);
            st32(address + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
            st32(address + 8U, 0U);
            st32(address + 12U, ISAAC_VITA_SYNC_CS_VERSION);
            st32(address + 16U, 0U);
            st32(address + 20U, 0U);
            break;
    case 10: memset(s_arena + CS_ROW_OFF + index * CS_STRIDE, 0, 24U); break;
    default: st32(address + 8U, rnd()); break;      /* spin count is inert */
    }
    return 0;
}

static int random_phase(CPU *c, uint32_t steps)
{
    uint32_t step;
    uint32_t self = c->vita_sync_thread_id;

    for (step = 0U; step < steps; ++step) {
        uint32_t index = rnd_below(CS_COUNT);
        uint32_t r = rnd_below(100U);
        uint32_t local;

        c->eax = rnd();
        c->ecx = rnd();
        if (r < 6U) {
            if (index != CONTENDED_CS && corrupt(index, self)) {
                uint32_t address = cs_address(index);
                uint32_t bounded[3];

                bounded[0] = s_local_try_enter;
                bounded[1] = s_local_leave;
                bounded[2] = s_local_delete;
                for (r = 0U; r < 3U; ++r) {
                    write_call_frame(c, pick_esp(), address);
                    if (differential_step(c, bounded[r], step))
                        return 1;
                }
                st32(address + 16U, 0U);
                st32(address + 20U, 0U);
            }
            continue;
        }
        if (r < 44U) {
            /* Enter on the contender's object would wait forever. */
            if (index == CONTENDED_CS)
                index = rnd_below(CONTENDED_CS);
            local = s_local_enter;
        } else if (r < 80U) {
            local = s_local_leave;
        } else if (r < 90U) {
            local = s_local_try_enter;
        } else if (r < 94U) {
            local = s_local_delete;
        } else {
            local = rnd_below(2U) ? s_local_init_plain : s_local_init_spin;
        }
        {
            uint32_t argument = pick_cs_argument(index);

            if (local == s_local_enter && enter_would_wait(argument, self))
                local = s_local_try_enter;
            write_call_frame(c, pick_esp(), argument);
        }
        if (local == s_local_init_spin && c->esp - s_base < ARENA_BYTES - 12U) {
            uint32_t spin = rnd_below(5000U);
            memcpy(s_arena + (c->esp - s_base) + 8U, &spin, 4U);
        }
        if (differential_step(c, local, step))
            return 1;
    }
    return 0;
}

/* ---- contender ------------------------------------------------------------- */

static uint32_t s_contender_acquired;
static uint32_t s_contender_release;
static uint32_t s_contender_released;
static uint32_t s_contender_bind_done;
static CPU *s_contender_bind_cpu;

static void *contender_main(void *unused)
{
    uint32_t *owner_word = (uint32_t *)(uintptr_t)(cs_address(CONTENDED_CS) + 16U);
    uint32_t *depth_word = (uint32_t *)(uintptr_t)(cs_address(CONTENDED_CS) + 20U);
    struct timespec pause = { 0, 20L * 1000L * 1000L };
    int result;

    (void)unused;
    s_thread_id = CONTENDER_THREAD_ID;
    result = isaac_vita_sync_lock_userspace_mutex_for_thread(
        owner_word, depth_word, CONTENDER_THREAD_ID);
    if (result != 0)
        return (void *)(intptr_t)result;
    __atomic_store_n(&s_contender_acquired, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s_contender_release, __ATOMIC_ACQUIRE))
        sched_yield();
    nanosleep(&pause, NULL);
    result = isaac_vita_sync_unlock_userspace_mutex_for_thread(
        owner_word, depth_word, CONTENDER_THREAD_ID);
    __atomic_store_n(&s_contender_released, 1U, __ATOMIC_RELEASE);
    if (result != 0)
        return (void *)(intptr_t)result;

    /* Second bound identity: the production binder on another thread. */
    while (!__atomic_load_n(&s_contender_bind_cpu, __ATOMIC_ACQUIRE))
        sched_yield();
    isaac_vita_sync_bind_current_thread(s_contender_bind_cpu);
    __atomic_store_n(&s_contender_bind_done, 1U, __ATOMIC_RELEASE);
    return NULL;
}

/* ---- main ------------------------------------------------------------------ */

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

/* Known initialized, free object regardless of what the random program left. */
static void make_free(uint32_t index)
{
    uint32_t address = cs_address(index);

    st32(address, ISAAC_VITA_SYNC_CS_MAGIC);
    st32(address + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
    st32(address + 8U, 0U);
    st32(address + 12U, ISAAC_VITA_SYNC_CS_VERSION);
    st32(address + 16U, 0U);
    st32(address + 20U, 0U);
}

static int init_object(CPU *c, uint32_t index)
{
    write_call_frame(c, c->stack_ceiling - 16U, cs_address(index));
    route_old(c, s_local_init_plain);
    return c->fault == NULL;
}

int main(int argc, char **argv)
{
    CPU cpu;
    CPU second;
    pthread_t contender;
    void *contender_result;
    uint32_t steps = 200000U;
    uint32_t i;
    unsigned waits_before;
    observation before, old, fresh;
    CPU saved;
    isaac_vita_sync_mutex_pool_snapshot pool;

    if (argc > 1)
        steps = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        s_rng = strtoull(argv[2], NULL, 0) | 1U;

    s_arena = mmap(NULL, ARENA_BYTES, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(s_arena != MAP_FAILED);
    CHECK((uintptr_t)s_arena <= UINT32_MAX - ARENA_BYTES);
    s_base = (uint32_t)(uintptr_t)s_arena;
    memset(s_arena, 0, ARENA_BYTES);

    CHECK(find_local(ISAAC_VITA_SYNC_INIT_CS_NAME, &s_local_init_spin));
    CHECK(find_local(ISAAC_VITA_SYNC_PLAIN_CS_NAME, &s_local_init_plain));
    CHECK(find_local(ISAAC_VITA_SYNC_ENTER_CS_NAME, &s_local_enter));
    CHECK(find_local(ISAAC_VITA_SYNC_LEAVE_CS_NAME, &s_local_leave));
    CHECK(find_local(ISAAC_VITA_SYNC_DELETE_CS_NAME, &s_local_delete));
    CHECK(find_local(ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME, &s_local_try_enter));
    CHECK(s_local_enter == ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS);
    CHECK(s_local_leave == ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS);

    /* The live-object census is process state the replay cannot restore,
     * and its create receipt fires exactly when a new peak is a power of
     * two.  Park the count between 2^17 and 2^18 so no replayed init or
     * delete can ever cross a power of two or reach zero. */
    for (i = 0U; i < 140000U; ++i)
        CHECK(isaac_vita_sync_create_recursive_mutex() ==
              ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);

    cpu_init(&cpu);
    for (i = 0U; i < CS_COUNT; ++i)
        CHECK(init_object(&cpu, i));

    /* Phase 0: unarmed latch and unbound CPU never take the inline path. */
    CHECK(g_isaac_vita_sync_inline_owner == ISAAC_VITA_SYNC_INLINE_OWNER_NONE);
    write_call_frame(&cpu, cpu.stack_ceiling - 16U, cs_address(0U));
    CHECK(!isaac_vita_sync_inline_enter(&cpu, &g_host_import_calls));
    CHECK(g_host_import_calls == CS_COUNT && cpu.esp == cpu.stack_ceiling - 16U);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(cpu.vita_sync_thread_id == (uint32_t)MAIN_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);

    /* Phase 1: directed states through both routes. */
    {
        uint32_t address = cs_address(0U);
        uint32_t k;

        for (k = 0U; k < 5U; ++k) {          /* recursion depth 1..5 */
            write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
            CHECK(!differential_step(&cpu, s_local_enter, 1000U + k));
            CHECK(ld32(address + 16U) == (uint32_t)MAIN_THREAD_ID &&
                  ld32(address + 20U) == k + 1U &&
                  cpu.esp == cpu.stack_ceiling - 24U && !cpu.fault);
        }
        CHECK(s_fast_hits == 5U && s_fast_misses == 0U);
        for (k = 5U; k > 0U; --k) {
            write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
            CHECK(!differential_step(&cpu, s_local_leave, 2000U + k));
            CHECK(ld32(address + 20U) == k - 1U && !cpu.fault);
        }
        CHECK(ld32(address + 16U) == 0U && s_fast_hits == 10U);
        /* Leave while free: endpoint fault, inline falls back. */
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        CHECK(!differential_step(&cpu, s_local_leave, 3000U));
        CHECK(s_fast_misses == 1U && s_fast_hits == 10U);
        /* Depth overflow edge: owner self, depth UINT32_MAX -> endpoint. */
        st32(address + 16U, (uint32_t)MAIN_THREAD_ID);
        st32(address + 20U, UINT32_MAX);
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        CHECK(!differential_step(&cpu, s_local_enter, 3001U));
        CHECK(s_fast_misses == 2U && ld32(address + 20U) == UINT32_MAX);
        st32(address + 20U, UINT32_MAX - 1U);
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        CHECK(!differential_step(&cpu, s_local_enter, 3002U));
        CHECK(s_fast_hits == 11U && ld32(address + 20U) == UINT32_MAX);
        st32(address + 16U, 0U);
        st32(address + 20U, 0U);
    }

    /* Phase 2: contender owns the last object during the random program. */
    CHECK(pthread_create(&contender, NULL, contender_main, NULL) == 0);
    while (!__atomic_load_n(&s_contender_acquired, __ATOMIC_ACQUIRE))
        sched_yield();
    CHECK(ld32(cs_address(CONTENDED_CS) + 16U) == (uint32_t)CONTENDER_THREAD_ID);
    CHECK(!random_phase(&cpu, steps));
    CHECK(s_fast_hits > steps / 8U);
    CHECK(s_fast_misses > steps / 50U);
    CHECK(ld32(cs_address(CONTENDED_CS) + 16U) == (uint32_t)CONTENDER_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);

    /* Phase 3: blocking Enter on the contended object through the new route
     * must fall back to the waiting endpoint and complete on release. */
    {
        uint32_t address = cs_address(CONTENDED_CS);
        isaac_vita_sync_mutex_pool_snapshot snap;

        cpu_init(&cpu);
        isaac_vita_sync_bind_current_thread(&cpu);
        st32(address, ISAAC_VITA_SYNC_CS_MAGIC);
        st32(address + 4U, (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE);
        st32(address + 12U, ISAAC_VITA_SYNC_CS_VERSION);
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        /* Foreign owner at rest (the contender is parked on its flag, so this
         * read is ordered after its acquisition): the inline path refuses
         * and leaves the frame untouched. */
        CHECK(!isaac_vita_sync_inline_enter(&cpu, &g_host_import_calls));
        CHECK(cpu.esp == cpu.stack_ceiling - 32U &&
              ld32(address + 16U) == (uint32_t)CONTENDER_THREAD_ID);
        isaac_vita_sync_get_mutex_pool_snapshot(&snap);
        waits_before = snap.waits;
        __atomic_store_n(&s_contender_release, 1U, __ATOMIC_RELEASE);
        /* What route_new does after the refusal: the registered endpoint,
         * whose atomic protocol is the only code that may race the release. */
        endpoint(&cpu, s_local_enter);
        /* The contender publishes its receipt after the unlock that let the
         * endpoint proceed, so wait for the receipt before reading it. */
        while (!__atomic_load_n(&s_contender_released, __ATOMIC_ACQUIRE))
            sched_yield();
        CHECK(!cpu.fault && cpu.esp == cpu.stack_ceiling - 24U);
        CHECK(ld32(address + 16U) == (uint32_t)MAIN_THREAD_ID &&
              ld32(address + 20U) == 1U);
        isaac_vita_sync_get_mutex_pool_snapshot(&snap);
        CHECK(snap.waits > waits_before);
        /* Now free again through both routes: recursion then release. */
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        CHECK(!differential_step(&cpu, s_local_enter, 4000U));
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        CHECK(!differential_step(&cpu, s_local_leave, 4001U));
        write_call_frame(&cpu, cpu.stack_ceiling - 32U, address);
        CHECK(!differential_step(&cpu, s_local_leave, 4002U));
        CHECK(ld32(address + 16U) == 0U && ld32(address + 20U) == 0U);
    }

    /* Phase 4a: a second CPU identity at the endpoint disarms the latch. */
    for (i = 0U; i < 4U; ++i)
        make_free(i);
    cpu_init(&second);
    second.vita_sync_thread_id = (uint32_t)CONTENDER_THREAD_ID;
    write_call_frame(&second, second.stack_ceiling - 32U, cs_address(1U));
    route_old(&second, s_local_try_enter);
    CHECK(!second.fault && second.eax == 1U);
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);
    write_call_frame(&second, second.stack_ceiling - 32U, cs_address(1U));
    route_old(&second, s_local_leave);
    CHECK(!second.fault);
    /* The main CPU now stays on the endpoint; both routes still agree. */
    write_call_frame(&cpu, cpu.stack_ceiling - 32U, cs_address(0U));
    CHECK(!isaac_vita_sync_inline_enter(&cpu, &g_host_import_calls));
    saved = cpu;
    observe(&before, &cpu);
    memcpy(s_snapshot, s_arena, ARENA_BYTES);
    route_old(&cpu, s_local_enter);
    observe(&old, &cpu);
    memcpy(s_old_arena, s_arena, ARENA_BYTES);
    cpu = saved;
    memcpy(s_arena, s_snapshot, ARENA_BYTES);
    route_new(&cpu, s_local_enter);
    observe(&fresh, &cpu);
    CHECK(observations_equal(&before, &old, &fresh) &&
          memcmp(s_old_arena, s_arena, ARENA_BYTES) == 0);
    write_call_frame(&cpu, cpu.stack_ceiling - 32U, cs_address(0U));
    route_old(&cpu, s_local_leave);
    CHECK(!cpu.fault);
    /* Disabled is sticky across a re-bind of the owner itself. */
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);

    /* Phase 4b: a second bound thread disarms (production binder path). */
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
    cpu_init(&second);
    __atomic_store_n(&s_contender_bind_cpu, &second, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s_contender_bind_done, __ATOMIC_ACQUIRE))
        sched_yield();
    CHECK(pthread_join(contender, &contender_result) == 0);
    CHECK(contender_result == NULL);
    CHECK(second.vita_sync_thread_id == (uint32_t)CONTENDER_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);
    write_call_frame(&cpu, cpu.stack_ceiling - 32U, cs_address(0U));
    CHECK(!isaac_vita_sync_inline_enter(&cpu, &g_host_import_calls));

    /* Phase 4c: an unbound CPU (thread id 0) never runs inline and, while
     * the latch is armed, disarms it at the endpoint. */
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
    cpu_init(&second);                /* vita_sync_thread_id == 0 */
    write_call_frame(&second, second.stack_ceiling - 32U, cs_address(2U));
    CHECK(!isaac_vita_sync_inline_enter(&second, &g_host_import_calls));
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
    route_old(&second, s_local_enter);
    CHECK(!second.fault &&
          ld32(cs_address(2U) + 16U) == (uint32_t)MAIN_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);
    write_call_frame(&second, second.stack_ceiling - 32U, cs_address(2U));
    route_old(&second, s_local_leave);
    CHECK(!second.fault);

    /* Phase 4e: the explicit spawn-time disarm.  From the armed state the
     * owner's very next inline attempt refuses with no side effect, both
     * routes keep agreeing on the endpoint, and a re-bind of the owner does
     * not re-arm; from NONE the first bind can no longer arm. */
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(g_isaac_vita_sync_inline_owner == (uint32_t)MAIN_THREAD_ID);
    make_free(0U);
    write_call_frame(&cpu, cpu.stack_ceiling - 32U, cs_address(0U));
    CHECK(isaac_vita_sync_inline_cs_address(&cpu) == cs_address(0U));
    isaac_vita_sync_inline_disable();
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);
    CHECK(isaac_vita_sync_inline_cs_address(&cpu) == 0U);
    CHECK(!isaac_vita_sync_inline_enter(&cpu, &g_host_import_calls));
    CHECK(cpu.esp == cpu.stack_ceiling - 32U &&
          ld32(cs_address(0U) + 16U) == 0U &&
          ld32(cs_address(0U) + 20U) == 0U);
    CHECK(!differential_step(&cpu, s_local_enter, 7000U));
    CHECK(ld32(cs_address(0U) + 16U) == (uint32_t)MAIN_THREAD_ID &&
          ld32(cs_address(0U) + 20U) == 1U && !cpu.fault);
    write_call_frame(&cpu, cpu.stack_ceiling - 32U, cs_address(0U));
    CHECK(!differential_step(&cpu, s_local_leave, 7001U));
    CHECK(ld32(cs_address(0U) + 16U) == 0U && !cpu.fault);
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(cpu.vita_sync_thread_id == (uint32_t)MAIN_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
    isaac_vita_sync_inline_disable();
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    CHECK(cpu.vita_sync_thread_id == (uint32_t)MAIN_THREAD_ID);
    CHECK(g_isaac_vita_sync_inline_owner ==
          ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED);
    write_call_frame(&cpu, cpu.stack_ceiling - 32U, cs_address(0U));
    CHECK(!isaac_vita_sync_inline_enter(&cpu, &g_host_import_calls));
    CHECK(cpu.esp == cpu.stack_ceiling - 32U);

    /* Phase 4d: the stack predicate alone, exhaustively over the owned
     * interval plus both margins: the inline path accepts an ESP exactly when
     * the endpoint completes without a fault.  Below the floor the endpoint
     * fetches [esp+4] from the floor, takes the lock and then faults on the
     * return-address pop: a partial-effect fatal fault that the inline path
     * must (and does) leave to the endpoint. */
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
    cpu_init(&cpu);
    isaac_vita_sync_bind_current_thread(&cpu);
    for (i = STACK_FLOOR_OFF - 64U; i <= STACK_CEIL_OFF + 64U; i += 2U) {
        uint32_t esp = s_base + i;
        unsigned faults_before = s_fault_calls;
        int fast;

        write_call_frame(&cpu, esp, cs_address(3U));
        cpu.eax = 0xa51ca11eU;
        fast = isaac_vita_sync_inline_cs_address(&cpu) != 0U;
        CHECK(fast == (esp >= cpu.stack_floor &&
                       esp <= cpu.stack_ceiling - 8U));
        CHECK(!differential_step(&cpu, s_local_enter, 5000U + i));
        if (fast) {
            CHECK(s_fault_calls == faults_before);
            CHECK(ld32(cs_address(3U) + 16U) == (uint32_t)MAIN_THREAD_ID);
            write_call_frame(&cpu, esp, cs_address(3U));
            CHECK(!differential_step(&cpu, s_local_leave, 6000U + i));
            CHECK(s_fault_calls == faults_before);
            CHECK(ld32(cs_address(3U) + 16U) == 0U);
        } else {
            /* Both routes ran the endpoint and both faulted. */
            CHECK(s_fault_calls == faults_before + 2U);
            if (ld32(cs_address(3U) + 16U) != 0U)
                CHECK(esp < cpu.stack_floor && esp + 4U >= cpu.stack_floor);
            make_free(3U);
        }
    }

    isaac_vita_sync_get_mutex_pool_snapshot(&pool);
    printf("Vita sync inline fast path oracle: PASS "
           "(steps=%u fast_hits=%u fast_misses=%u imports=%u faults=%u "
           "waits=%u owner_failures=%u state_failures=%u live=%u)\n",
           steps, s_fast_hits, s_fast_misses, g_host_import_calls,
           s_fault_calls, pool.waits, pool.owner_failures,
           pool.state_failures, pool.live);
    return 0;
}
