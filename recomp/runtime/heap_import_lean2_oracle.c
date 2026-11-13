/* ISAAC_VITA_HEAP_IMPORT_LEAN2 host oracle (no Vita, no timing claims).
 *
 * One deterministic mixed workload through the real import table: malloc,
 * calloc, realloc and free, including NULL, interior, foreign, double and
 * retained-boundary frees, forced overflow-mspace allocations and room-slab
 * entries.  Every returned pointer, fault flag/value/message and the final
 * telemetry fold into "[lean2-trace]", which must be identical between the
 * LEAN_IMPORT-only comparator and LEAN_IMPORT+IMPORT_LEAN2(+VERIFY).
 * "[lean2-retag]" is the slab-move floor receipt (only its final token may
 * differ: the comparator stores the inherited token, lean2 skips that store),
 * "[lean2-rollback]" the commit-failure rollback retag, "[lean2]" the lean2
 * receipt (zero sidecars; VERIFY checks and disagreements per kind), and the
 * threaded leg runs import pairs on six threads while the ledger may grow.
 * Every isaac_vita_log call is checked to happen outside the heap lock. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <windows.h>
#include "host_vita_heap.h"
#include "host_vita_import_id.h"

int fixture_setup(void);
int fixture_init(uint32_t, uint32_t);
unsigned fixture_observers(void);
unsigned fixture_native_calls(void);
unsigned fixture_native_frees(void);
void fixture_fail_next(void);

#define IMP_FREE 1U
#define IMP_CALLOC 2U
#define IMP_MALLOC 3U
#define IMP_REALLOC 5U
#define LIVE_MAX 640U
#define STEPS 24000U

static uint64_t digest = UINT64_C(1469598103934665603);
static char banner[256];
static atomic_uint log_lines, log_under_lock, mismatch_lines;
static HANDLE start_event;
static atomic_uint worker_errors;

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "lean2 oracle line %d: %s\n", __LINE__, #x); \
    return 1; } } while (0)

void isaac_vita_log(const char *format, ...)
{
    char line[512];
    va_list ap;

    if (isaac_vita_guest_heap_test_lock_held())
        atomic_fetch_add(&log_under_lock, 1U);
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    atomic_fetch_add(&log_lines, 1U);
    if (strstr(line, "guest heap lean2 import:"))
        snprintf(banner, sizeof banner, "%s", line);
    if (strstr(line, "lean2 verify: FIRST MISMATCH")) {
        atomic_fetch_add(&mismatch_lines, 1U);
        fprintf(stderr, "%s\n", line);
    }
}
void guest_fault(CPU *__restrict c, uint32_t at, const char *why)
{ c->fault = why; c->fault_addr = at; }
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t a, uint32_t n)
{ (void)kind; (void)a; (void)n; guest_fault(c, pc, "stack"); return 0; }
int guest_stack_owner_violation(CPU *__restrict c, uint32_t p)
{ guest_fault(c, p, "owner"); return 0; }

static void fold(uint64_t value)
{ digest = (digest ^ value) * UINT64_C(1099511628211); }

static void fold_text(const char *text)
{
    fold(text ? 0x5a5a5a5aU : 0U);
    while (text && *text)
        fold((unsigned char)*text++);
}

typedef struct import_result {
    uint32_t eax;
    int fault;
    uint32_t fault_addr;
    const char *why;
} import_result;

/* One static guest frame per caller (main = 0, workers 1..6) so every
 * guest stack address fits in 32 bits.  frame[0] is the import return,
 * frame[1..2] the cdecl arguments; frame[3] is both the ANM2 owner word and,
 * behind the operator-new wrapper return, the original caller (the
 * room-entry owner). */
static _Alignas(16) uint32_t s_frames[7][8];

static import_result imp_on(unsigned caller, unsigned kind, uint32_t a0,
                            uint32_t a1, uint32_t owner, uint32_t ret)
{
    uint32_t *frame = s_frames[caller];
    import_result r;
    CPU c;

    memset(&c, 0, sizeof c);
    memset(frame, 0, sizeof s_frames[0]);
    frame[0] = ret;
    frame[1] = a0;
    frame[2] = a1;
    frame[3] = owner;
    c.esp = (uint32_t)(uintptr_t)frame;
    c.stack_floor = c.esp;
    c.stack_ceiling = c.esp + (uint32_t)sizeof s_frames[0];
    c.stack_low_water = c.esp;
    c.stack_owner = &c;
    if (!isaac_vita_heap_import_indexed(&c, kind, NULL))
        abort();
    r.eax = c.eax;
    r.fault = c.fault != NULL;
    r.fault_addr = c.fault_addr;
    r.why = c.fault;
    return r;
}

static import_result imp(unsigned kind, uint32_t a0, uint32_t a1,
                         uint32_t owner, uint32_t ret)
{
    return imp_on(0U, kind, a0, a1, owner, ret);
}

static void fold_result(unsigned op, import_result r)
{
    fold(op);
    fold(r.eax);
    fold((uint64_t)r.fault);
    if (r.fault) {
        fold(r.fault_addr);
        fold_text(r.why);
    }
}

static uint32_t rng = 0x2545f491U;
static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct live_entry { uint32_t p; uint32_t n; int slab; } live_entry;
static live_entry live[LIVE_MAX];
static unsigned live_count;

static void live_add(uint32_t p, uint32_t n, int slab)
{
    if (p && live_count < LIVE_MAX) {
        live[live_count].p = p;
        live[live_count].n = n;
        live[live_count].slab = slab;
        ++live_count;
    }
}

static live_entry live_take(unsigned index)
{
    live_entry e = live[index];

    live[index] = live[--live_count];
    return e;
}

static int heap_terminal(void)
{
    isaac_vita_guest_heap_telemetry_snapshot ov;

    return !isaac_vita_guest_heap_telemetry_snapshot_get(&ov) || ov.terminal;
}

static const uint32_t s_owner_noise[4] = {
    0U, 0x00123456U, 0x0000aa5cU, 0x003e5adfU
};

static int run_workload(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;
    uintptr_t boundary[4];
    unsigned step, i, faults = 0U, slab_live = 0U;
    import_result r;

    CHECK(isaac_vita_guest_heap_test_forbidden_ranges(
        0x82ad1560U, 0x05100000U, (uint32_t)(uintptr_t)s_frames,
        (uint32_t)(uintptr_t)s_frames + sizeof s_frames, &ranges));
    boundary[0] = ranges.retained_newlib.begin;
    boundary[1] = ranges.retained_newlib.end - 1U;
    boundary[2] = ranges.retained_newlib.end;
    boundary[3] = ranges.retained_newlib.begin - 1U;
    for (i = 0U; i < 4U; ++i) {
        r = imp(IMP_FREE, (uint32_t)boundary[i], 0U, 0U,
                ISAAC_VITA_HEAP_FREE_RETURN_RVA);
        fold_result(0x10U + i, r);
        CHECK(r.fault && !heap_terminal());
    }
    for (step = 0U; step < STEPS; ++step) {
        uint32_t pick = next_rand();
        uint32_t op = pick % 100U;
        uint32_t n = 1U + (next_rand() % 48U) * 16U;

        if (live_count >= LIVE_MAX - 4U && op < 50U)
            op = 50U + op % 30U; /* drain instead of growing */
        if (live_count == 0U && op >= 50U && op < 90U)
            op = 0U;
        if (op < 34U) {
            uint32_t owner = s_owner_noise[pick >> 28 & 3U];

            r = imp(IMP_MALLOC, n, 0U, owner,
                    (pick & 0x100U) ? ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA :
                        ISAAC_VITA_HEAP_MALLOC_RETURN_RVA);
            fold_result(1U, r);
            if (r.fault)
                ++faults;
            else
                live_add(r.eax, n, 0);
        }
        else if (op < 42U) {
            uint32_t count = 1U + next_rand() % 8U;

            r = imp(IMP_CALLOC, count, n / 8U + 1U, 0U,
                    ISAAC_VITA_HEAP_CALLOC_RETURN_RVA);
            fold_result(2U, r);
            if (r.fault)
                ++faults;
            else
                live_add(r.eax, count * (n / 8U + 1U), 0);
        }
        else if (op < 46U) {
            /* Forced newlib failure: the overflow mspace serves it. */
            fixture_fail_next();
            r = imp(IMP_MALLOC, n, 0U, 0U, ISAAC_VITA_HEAP_MALLOC_RETURN_RVA);
            fold_result(3U, r);
            if (r.fault)
                ++faults;
            else
                live_add(r.eax, n, 0);
        }
        else if (op < 50U) {
            if (slab_live < 6U) {
                r = imp(IMP_MALLOC, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, 0U,
                        ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
                        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA);
                fold_result(4U, r);
                if (r.fault)
                    ++faults;
                else if (r.eax) {
                    ++slab_live;
                    live_add(r.eax, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, 1);
                }
            }
        }
        else if (op < 74U) {
            live_entry e = live_take(next_rand() % live_count);

            r = imp(IMP_FREE, e.p, 0U, 0U, ISAAC_VITA_HEAP_FREE_RETURN_RVA);
            fold_result(5U, r);
            if (r.fault)
                ++faults;
            if (e.slab)
                --slab_live;
        }
        else if (op < 84U) {
            unsigned index = next_rand() % live_count;
            uint32_t grow = (next_rand() & 1U) ? live[index].n + n : n;

            r = imp(IMP_REALLOC, live[index].p, grow, 0U,
                    ISAAC_VITA_HEAP_REALLOC_RETURN_RVA);
            fold_result(6U, r);
            if (r.fault)
                ++faults;
            else if (r.eax) {
                if (live[index].slab && r.eax != live[index].p) {
                    live[index].slab = 0;
                    --slab_live;
                }
                live[index].p = r.eax;
                live[index].n = grow;
            }
        }
        else if (op < 87U) {
            /* Interior pointer of a ledger block: FOREIGN, the block stays
             * live (slab slots are left to the slab's own oracles). */
            unsigned index = next_rand() % live_count;

            if (!live[index].slab) {
                r = imp(IMP_FREE, live[index].p + 4U, 0U, 0U,
                        ISAAC_VITA_HEAP_FREE_RETURN_RVA);
                fold_result(7U, r);
                CHECK(r.fault);
                ++faults;
            }
        }
        else if (op < 90U) {
            /* Immediate double free: nothing allocates in between. */
            live_entry e = live_take(next_rand() % live_count);

            r = imp(IMP_FREE, e.p, 0U, 0U, ISAAC_VITA_HEAP_FREE_RETURN_RVA);
            fold_result(8U, r);
            CHECK(!r.fault);
            if (e.slab)
                --slab_live;
            else {
                r = imp(IMP_FREE, e.p, 0U, 0U,
                        ISAAC_VITA_HEAP_FREE_RETURN_RVA);
                fold_result(9U, r);
                CHECK(r.fault);
                ++faults;
            }
        }
        else if (op < 94U) {
            r = imp(IMP_FREE, 0U, 0U, 0U, ISAAC_VITA_HEAP_FREE_RETURN_RVA);
            fold_result(10U, r);
            CHECK(!r.fault);
        }
        else if (op < 96U) {
            r = imp(IMP_REALLOC, 0U, n, 0U, ISAAC_VITA_HEAP_REALLOC_RETURN_RVA);
            fold_result(11U, r);
            if (r.fault)
                ++faults;
            else
                live_add(r.eax, n, 0);
        }
        else if (op < 98U) {
            /* Foreign pointer outside every guest heap domain. */
            r = imp(IMP_FREE, 0x0badf00cU, 0U, 0U,
                    ISAAC_VITA_HEAP_FREE_RETURN_RVA);
            fold_result(12U, r);
            CHECK(r.fault);
            ++faults;
        }
        else {
            r = imp(IMP_MALLOC, 0U, 0U, 0U, ISAAC_VITA_HEAP_MALLOC_RETURN_RVA);
            fold_result(13U, r);
            if (r.fault)
                ++faults;
            else
                live_add(r.eax, 0U, 0);
        }
        CHECK(!heap_terminal());
    }
    while (live_count) {
        live_entry e = live_take(live_count - 1U);

        r = imp(IMP_FREE, e.p, 0U, 0U, ISAAC_VITA_HEAP_FREE_RETURN_RVA);
        fold_result(14U, r);
        CHECK(!r.fault);
    }
    {
        isaac_vita_guest_heap_telemetry_snapshot ov;
        isaac_vita_room_entry_slab_snapshot slab;

        CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&ov));
        CHECK(isaac_vita_guest_heap_test_room_snapshot(&slab));
        fold(ov.owned_live_count); fold(ov.peak_live_count);
        fold(ov.peak_requested_bytes); fold(ov.pool_allocations);
        fold(ov.pool_frees); fold(ov.pool_reallocations);
        fold(ov.native_failures); fold(ov.pool_failures);
        fold(ov.native_to_pool); fold(ov.pool_to_native);
        fold(ov.raw_live_count); fold(ov.terminal);
        fold(ov.accounting_valid);
        fold(slab.allocations); fold(slab.frees); fold(slab.live_slots);
        fold(fixture_native_calls()); fold(fixture_native_frees());
        printf("[lean2-trace] digest=%016llx steps=%u faults=%u ledger=%zu "
               "observers=%u slab=%u/%u pool=%u/%u nf=%u native=%u/%u\n",
               (unsigned long long)digest, STEPS, faults,
               isaac_vita_guest_heap_test_live_count(), fixture_observers(),
               slab.allocations, slab.frees, ov.pool_allocations,
               ov.pool_frees, ov.native_failures, fixture_native_calls(),
               fixture_native_frees());
        CHECK(ov.owned_live_count == 0U && slab.live_slots == 0U &&
              isaac_vita_guest_heap_test_live_count() == 0U);
    }
    return 0;
}

static int run_retag(void)
{
    isaac_vita_guest_heap_test_slab_move_floor_receipt receipt;
    import_result r;
    unsigned char *replacement;
    int valid_owner = 0;
    uint64_t keep = digest;

    digest = UINT64_C(1469598103934665603);
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_bootstrap(
        ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
    r = imp(IMP_MALLOC, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, 0U,
            ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
            ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA);
    CHECK(!r.fault && r.eax);
    fold_result(20U, r);
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_phase_set(
        ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD));
    isaac_vita_guest_heap_test_slab_move_floor_receipt_reset();
    replacement = isaac_vita_guest_realloc(
        (void *)(uintptr_t)r.eax, 8U, &valid_owner);
    CHECK(valid_owner && replacement &&
          (uint32_t)(uintptr_t)replacement != r.eax);
    fold((uintptr_t)replacement);
    CHECK(isaac_vita_guest_heap_test_slab_move_floor_receipt_snapshot(
        &receipt));
    fold(receipt.attempts); fold(receipt.inherited_token);
    fold(receipt.retag_succeeded); fold(receipt.commit_succeeded);
    fold(receipt.rollback_attempted); fold(receipt.rollback_succeeded);
    printf("[lean2-retag] digest=%016llx attempts=%u inherited=0x%x "
           "original=0x%x final=0x%x retag=%u commit=%u rollback=%u/%u\n",
           (unsigned long long)digest, receipt.attempts,
           receipt.inherited_token, receipt.replacement_original_token,
           receipt.replacement_final_token, receipt.retag_succeeded,
           receipt.commit_succeeded, receipt.rollback_attempted,
           receipt.rollback_succeeded);
    CHECK(receipt.attempts == 1U && receipt.retag_succeeded == 1U &&
          receipt.commit_succeeded == 1U && receipt.inherited_token != 0U);
#if defined(ISAAC_VITA_HEAP_IMPORT_LEAN2) && ISAAC_VITA_HEAP_IMPORT_LEAN2
    {
        isaac_vita_guest_heap_lean2_stats stats;

        CHECK(isaac_vita_guest_heap_lean2_stats_get(&stats));
        printf("[lean2-retag] live sidecar_nonzero=%u\n",
               stats.sidecar_nonzero);
    }
#endif
    r = imp(IMP_FREE, (uint32_t)(uintptr_t)replacement, 0U, 0U,
            ISAAC_VITA_HEAP_FREE_RETURN_RVA);
    CHECK(!r.fault);
    digest = keep;
    return 0;
}

static DWORD WINAPI worker(void *arg)
{
    unsigned i, id = (unsigned)(uintptr_t)arg;
    uint32_t held[24];

    memset(held, 0, sizeof held);
    WaitForSingleObject(start_event, INFINITE);
    for (i = 0U; i < 6000U; ++i) {
        unsigned slot = (i * 7U + id) % 24U;
        import_result r;

        if (held[slot]) {
            r = imp_on(id, IMP_FREE, held[slot], 0U, 0U,
                       ISAAC_VITA_HEAP_FREE_RETURN_RVA);
            held[slot] = 0U;
            if (r.fault) {
                atomic_fetch_add(&worker_errors, 1U);
                break;
            }
        }
        else {
            r = imp_on(id, IMP_MALLOC, 16U + ((i + id) % 32U) * 8U, 0U, 0U,
                       ISAAC_VITA_HEAP_MALLOC_RETURN_RVA);
            if (r.fault || !r.eax) {
                atomic_fetch_add(&worker_errors, 1U);
                break;
            }
            held[slot] = r.eax;
        }
    }
    for (i = 0U; i < 24U; ++i) {
        if (held[i] && imp_on(id, IMP_FREE, held[i], 0U, 0U,
                              ISAAC_VITA_HEAP_FREE_RETURN_RVA).fault)
            atomic_fetch_add(&worker_errors, 1U);
    }
    return 0;
}

static int run_threads(void)
{
    HANDLE threads[6];
    unsigned i;

    start_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(start_event);
    for (i = 0U; i < 6U; ++i) {
        threads[i] = CreateThread(NULL, 0, worker,
                                  (void *)(uintptr_t)(i + 1U), 0, NULL);
        CHECK(threads[i]);
    }
    SetEvent(start_event);
    WaitForMultipleObjects(6U, threads, TRUE, INFINITE);
    for (i = 0U; i < 6U; ++i)
        CloseHandle(threads[i]);
    CloseHandle(start_event);
    CHECK(!atomic_load(&worker_errors) &&
          isaac_vita_guest_heap_test_live_count() == 0U);
    puts("[lean2-threads] six import callers x6000 malloc/free PASS");
    return 0;
}

/* Last: a forced slab commit failure takes the retag rollback branch and
 * leaves the heap terminal by design. */
static int run_rollback(void)
{
    isaac_vita_guest_heap_test_slab_move_floor_receipt receipt;
    import_result r, native, post_free, post_slab, post_malloc;
    int valid_owner = 0;
    void *replacement;

    /* A newlib block taken before the heap goes terminal: its free after
     * the rollback is the native-range case the inline slab test serves. */
    native = imp(IMP_MALLOC, 64U, 0U, 0U, ISAAC_VITA_HEAP_MALLOC_RETURN_RVA);
    CHECK(!native.fault && native.eax);
    r = imp(IMP_MALLOC, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, 0U,
            ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
            ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA);
    CHECK(!r.fault && r.eax);
    isaac_vita_guest_heap_test_slab_move_floor_receipt_reset();
    isaac_vita_guest_heap_test_room_fail_next_commit();
    replacement = isaac_vita_guest_realloc(
        (void *)(uintptr_t)r.eax, 9U, &valid_owner);
    CHECK(isaac_vita_guest_heap_test_slab_move_floor_receipt_snapshot(
        &receipt));
    /* Terminal routes: which check faults is part of the result (the
     * fault text names it), so lean and lean2 must agree on it. */
    post_free = imp(IMP_FREE, native.eax, 0U, 0U,
                    ISAAC_VITA_HEAP_FREE_RETURN_RVA);
    /* The rolled-back slab block is still live: without the import-entry
     * terminal check its free would reach the slab route instead. */
    post_slab = imp(IMP_FREE, r.eax, 0U, 0U, ISAAC_VITA_HEAP_FREE_RETURN_RVA);
    post_malloc = imp(IMP_MALLOC, 64U, 0U, 0U,
                      ISAAC_VITA_HEAP_MALLOC_RETURN_RVA);
    printf("[lean2-rollback] result=%s valid=%d attempts=%u inherited=0x%x "
           "retag=%u commit=%u rollback=%u/%u terminal=%d "
           "post_free=%d/%s post_slab=%d/%s post_malloc=%d/0x%x/%s\n",
           replacement ? "pointer" : "null", valid_owner, receipt.attempts,
           receipt.inherited_token, receipt.retag_succeeded,
           receipt.commit_succeeded, receipt.rollback_attempted,
           receipt.rollback_succeeded, heap_terminal(),
           post_free.fault, post_free.why ? post_free.why : "-",
           post_slab.fault, post_slab.why ? post_slab.why : "-",
           post_malloc.fault, post_malloc.eax,
           post_malloc.why ? post_malloc.why : "-");
    CHECK(receipt.rollback_attempted == 1U &&
          receipt.rollback_succeeded == 1U);
    return 0;
}

int main(void)
{
    CHECK((uintptr_t)s_frames <= UINT32_MAX - sizeof s_frames &&
          fixture_setup());
    CHECK(fixture_init((uint32_t)(uintptr_t)s_frames,
                       (uint32_t)(uintptr_t)s_frames + sizeof s_frames));
    if (run_workload() || run_retag() || run_threads())
        return 1;
#if defined(ISAAC_VITA_HEAP_IMPORT_LEAN2) && ISAAC_VITA_HEAP_IMPORT_LEAN2
    {
        isaac_vita_guest_heap_lean2_stats stats;

        CHECK(banner[0]);
        CHECK(isaac_vita_guest_heap_lean2_stats_get(&stats));
        printf("[lean2] %s\n", banner);
        printf("[lean2] verify=%d sidecar_nonzero=%u checks=%u/%u/%u/%u "
               "bad=%u/%u/%u/%u mismatch_lines=%u\n",
               stats.verify, stats.sidecar_nonzero, stats.checks[0],
               stats.checks[1], stats.checks[2], stats.checks[3],
               stats.bad[0], stats.bad[1], stats.bad[2], stats.bad[3],
               atomic_load(&mismatch_lines));
    }
#else
    CHECK(!banner[0]);
    puts("[lean2] comparator (IMPORT_LEAN2 not compiled)");
#endif
    if (run_rollback())
        return 1;
    printf("[lean2-log] lines=%u under_lock=%u\n", atomic_load(&log_lines),
           atomic_load(&log_under_lock));
    CHECK(!atomic_load(&log_under_lock));
    puts("Heap import lean2 oracle: PASS");
    return 0;
}
