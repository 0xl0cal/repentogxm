/* ISAAC_VITA_HEAP_FREE_PREFILTER differential trace oracle (Linux host gcc,
 * test only).  The real heap import (host_vita_heap.c) routes a deterministic
 * trace through the real OGG emergency, ANM2 scratch and texel scratch owners
 * (their *_ORACLE builds) at the measured device layout.  The same trace runs
 * in a build without the prefilter and in one with it; vita/
 * test_heap_free_prefilter.sh requires the [phase] lines (FNV-1a digests of
 * every call's result, fault text, fault address, stack and handled flag,
 * plus the native/observer/memblock counters and the three owners' state
 * snapshots at each phase end) to be byte-identical.
 *
 * The three owners' free entry points are linked through --wrap so the oracle
 * counts every call: without the prefilter they run for every free, with it
 * only for pointers inside a published block (checked here per free against
 * the owners' snapshot base and their own extent constants), and the calls
 * made for pointers inside a block must be the same in both builds.  With the
 * prefilter the exported range records must agree with the owners' snapshots
 * at every free.
 *
 * Call shapes are copied from host_vita_anm2_scratch_routing_oracle.c
 * (prepare_call, call_malloc_with_texel_chain) and
 * host_vita_ogg_emergency_oracle.c (the Open owner + wrapper frame).  Not a
 * Vita timing measurement. */
#define _GNU_SOURCE

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <psp2/kernel/sysmem.h>

#include "guest.h"
#include "host_vita_anm2_scratch.h"
#include "host_vita_heap.h"
#include "host_vita_ogg_emergency.h"
#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"
#if defined(ISAAC_VITA_HEAP_FREE_PREFILTER)
#include "host_vita_heap_free_prefilter.h"
#endif

/* The device layout measured in the A2S boot receipts (RENDER60 W-heap):
 * texel scratch at 0x8e600000, ANM2 scratch at 0x91100000..0x913a2000.  The
 * OGG emergency slot was not reserved on the device; it goes after ANM2. */
#define ORACLE_TEXEL_BASE 0x8e600000U
#define ORACLE_ANM2_BASE  0x91100000U
#define ORACLE_OGG_BASE   0x91400000U
#define ORACLE_TEXEL_UID  0x3456
#define ORACLE_ANM2_UID   0x2345
#define ORACLE_OGG_UID    0x4567
#define ORACLE_STACK_BASE 0x4f000000U
#define ORACLE_STACK_BYTES 0x4000U
#define ORACLE_BANK_BASE  0x50000000U
#define ORACLE_BANK_BYTES 0x01000000U
#define ORACLE_CELLS 65536U

_Static_assert(ORACLE_TEXEL_BASE + KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES <=
                   ORACLE_ANM2_BASE &&
               ORACLE_ANM2_BASE + ISAAC_VITA_ANM2_MEMBLOCK_BYTES <=
                   ORACLE_OGG_BASE &&
               ORACLE_OGG_BASE + ISAAC_VITA_OGG_OPEN_BACKING_BYTES <=
                   KAGE_VITA_TEXEL_GUEST_IMAGE_BASE &&
               ORACLE_TEXEL_BASE >= KAGE_VITA_TEXEL_HEAP_RETAINED_END,
               "oracle block layout overlaps");

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "heap free prefilter oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* ---- deterministic native backend (host_vita_heap.c alone is compiled
 * with -Dmalloc/-Dfree/-Drealloc renamed to these; its ledger keeps the
 * host calloc, whose blocks reach oracle_guest_free and go back to free) */
typedef struct oracle_cell { uint32_t at, cap, used, live; } oracle_cell;
static unsigned char *s_bank;
static size_t s_bank_bump;
static oracle_cell s_cells[ORACLE_CELLS];
static unsigned s_cell_count;
static size_t s_fail_size;
static unsigned s_native_malloc_calls, s_native_malloc_null;
static unsigned s_native_free_calls, s_native_realloc_calls;
static unsigned s_observer_calls;
static uint32_t s_observer_last;
static unsigned s_diagnostic_calls;
static unsigned s_memblock_alloc, s_memblock_get, s_memblock_free;
static int s_ogg_live, s_anm2_live, s_texel_live;

static int bank_owns(const void *pointer)
{
    uintptr_t value = (uintptr_t)pointer;

    return s_bank && value >= (uintptr_t)s_bank + 16U &&
           value < (uintptr_t)s_bank + ORACLE_BANK_BYTES;
}

static oracle_cell *bank_cell(void *pointer)
{
    uint32_t index;

    memcpy(&index, (unsigned char *)pointer - 16U, sizeof index);
    if (index >= s_cell_count ||
        (uintptr_t)s_bank + s_cells[index].at != (uintptr_t)pointer)
        abort();
    return &s_cells[index];
}

void *oracle_guest_malloc(size_t size)
{
    unsigned index;
    size_t span;

    ++s_native_malloc_calls;
    if (s_fail_size && size == s_fail_size) {
        ++s_native_malloc_null;
        return NULL;
    }
    size = size ? size : 1U;
    for (index = 0U; index < s_cell_count; ++index)
        if (!s_cells[index].live && s_cells[index].cap >= size)
            break;
    if (index == s_cell_count) {
        span = (size + 31U) & ~(size_t)15U;
        if (s_cell_count == ORACLE_CELLS ||
            span > ORACLE_BANK_BYTES - s_bank_bump) {
            ++s_native_malloc_null;
            return NULL;
        }
        s_cells[index].at = (uint32_t)(s_bank_bump + 16U);
        s_cells[index].cap = (uint32_t)(span - 16U);
        memcpy(s_bank + s_bank_bump, &index, sizeof index);
        s_bank_bump += span;
        ++s_cell_count;
    }
    s_cells[index].used = (uint32_t)size;
    s_cells[index].live = 1U;
    return s_bank + s_cells[index].at;
}

void oracle_guest_free(void *pointer)
{
    oracle_cell *cell;

    if (!pointer)
        return;
    if (!bank_owns(pointer)) {
        free(pointer); /* the ledger's own calloc blocks */
        return;
    }
    cell = bank_cell(pointer);
    if (!cell->live)
        abort(); /* the heap ledger must never double-free natively */
    cell->live = 0U;
    ++s_native_free_calls;
}

void *oracle_guest_realloc(void *pointer, size_t size)
{
    oracle_cell *cell;
    void *replacement;

    if (!pointer)
        return oracle_guest_malloc(size);
    if (!bank_owns(pointer))
        return realloc(pointer, size);
    ++s_native_realloc_calls;
    cell = bank_cell(pointer);
    if (!cell->live)
        abort();
    if (!size) {
        oracle_guest_free(pointer);
        return NULL;
    }
    if (cell->cap >= size) {
        cell->used = (uint32_t)size;
        return pointer;
    }
    replacement = oracle_guest_malloc(size);
    if (!replacement)
        return NULL;
    memcpy(replacement, pointer, cell->used < size ? cell->used : size);
    oracle_guest_free(pointer);
    return replacement;
}

void isaac_nv_guest_buffer_freed(void *pointer)
{
    ++s_observer_calls;
    s_observer_last = (uint32_t)(uintptr_t)pointer;
}

void kage_vita_texel_oom_diagnostic(
    size_t failed_request, uint32_t owner_return_rva,
    unsigned owner_chain_depth, const char *failure_reason,
    size_t ledger_live_requested,
    size_t ledger_largest_live_request, size_t ledger_live_count,
    int ledger_requested_accounting_exact, uint32_t guest_stack_floor,
    uint32_t guest_stack_ceiling)
{
    (void)failed_request; (void)owner_return_rva; (void)owner_chain_depth;
    (void)failure_reason; (void)ledger_live_requested;
    (void)ledger_largest_live_request; (void)ledger_live_count;
    (void)ledger_requested_accounting_exact; (void)guest_stack_floor;
    (void)guest_stack_ceiling;
    ++s_diagnostic_calls;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    va_end(arguments);
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_memblock_alloc;
    if (!name || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || option)
        return -1;
    if (!strcmp(name, "isaac_ogg_emergency") &&
        size == ISAAC_VITA_OGG_OPEN_BACKING_BYTES && !s_ogg_live) {
        s_ogg_live = 1;
        return ORACLE_OGG_UID;
    }
    if (!strcmp(name, "isaac_anm2_scratch") &&
        size == ISAAC_VITA_ANM2_MEMBLOCK_BYTES && !s_anm2_live) {
        s_anm2_live = 1;
        return ORACLE_ANM2_UID;
    }
    if (!strcmp(name, "isaac_texel_scratch") &&
        size == KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES && !s_texel_live) {
        s_texel_live = 1;
        return ORACLE_TEXEL_UID;
    }
    return -1;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_memblock_get;
    if (!base)
        return -1;
    if (uid == ORACLE_OGG_UID && s_ogg_live)
        *base = (void *)(uintptr_t)ORACLE_OGG_BASE;
    else if (uid == ORACLE_ANM2_UID && s_anm2_live)
        *base = (void *)(uintptr_t)ORACLE_ANM2_BASE;
    else if (uid == ORACLE_TEXEL_UID && s_texel_live)
        *base = (void *)(uintptr_t)ORACLE_TEXEL_BASE;
    else
        return -1;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_memblock_free;
    if (uid == ORACLE_OGG_UID && s_ogg_live)
        s_ogg_live = 0;
    else if (uid == ORACLE_ANM2_UID && s_anm2_live)
        s_anm2_live = 0;
    else if (uid == ORACLE_TEXEL_UID && s_texel_live)
        s_texel_live = 0;
    else
        return -1;
    return 0;
}

/* ---- counted owner free entry points (-Wl,--wrap=...) */
enum { OWNER_OGG, OWNER_ANM2, OWNER_TEXEL, OWNERS };
static unsigned s_owner_calls[OWNERS];
int __real_isaac_vita_ogg_emergency_free(
    void *pointer, isaac_vita_ogg_emergency_decision *decision);
int __real_isaac_vita_anm2_scratch_free(
    void *pointer, isaac_vita_anm2_scratch_decision *decision);
int __real_isaac_vita_texel_scratch_free(
    void *pointer, isaac_vita_texel_scratch_decision *decision);
int __wrap_isaac_vita_ogg_emergency_free(
    void *pointer, isaac_vita_ogg_emergency_decision *decision);
int __wrap_isaac_vita_anm2_scratch_free(
    void *pointer, isaac_vita_anm2_scratch_decision *decision);
int __wrap_isaac_vita_texel_scratch_free(
    void *pointer, isaac_vita_texel_scratch_decision *decision);

int __wrap_isaac_vita_ogg_emergency_free(
    void *pointer, isaac_vita_ogg_emergency_decision *decision)
{
    ++s_owner_calls[OWNER_OGG];
    return __real_isaac_vita_ogg_emergency_free(pointer, decision);
}

int __wrap_isaac_vita_anm2_scratch_free(
    void *pointer, isaac_vita_anm2_scratch_decision *decision)
{
    ++s_owner_calls[OWNER_ANM2];
    return __real_isaac_vita_anm2_scratch_free(pointer, decision);
}

int __wrap_isaac_vita_texel_scratch_free(
    void *pointer, isaac_vita_texel_scratch_decision *decision)
{
    ++s_owner_calls[OWNER_TEXEL];
    return __real_isaac_vita_texel_scratch_free(pointer, decision);
}

/* ---- digest */
static uint64_t s_digest;
static unsigned s_phase_ops, s_phase_frees, s_phase_faults;

static void fold(uint64_t value)
{
    unsigned byte;

    for (byte = 0U; byte < 8U; ++byte) {
        s_digest ^= (value >> (8U * byte)) & 0xffU;
        s_digest *= UINT64_C(1099511628211);
    }
}

static void fold_text(const char *text)
{
    if (!text) {
        fold(0U);
        return;
    }
    for (; *text; ++text) {
        s_digest ^= (unsigned char)*text;
        s_digest *= UINT64_C(1099511628211);
    }
    fold(1U);
}

/* ---- calls through the real import */
static void *s_stack;
static CPU s_cpu;
static int s_trace_ops; /* ISAAC_PREFILTER_ORACLE_TRACE=1: one line per call */
static const char *s_import_name;
static int s_import_handled;

static uint32_t *prepare_call(uint32_t frame0, uint32_t argument0,
                              uint32_t argument1, uint32_t owner_return)
{
    uint32_t *frame = (uint32_t *)((unsigned char *)s_stack + 0x2000U);
    uint32_t floor = (uint32_t)(uintptr_t)s_stack;

    memset(&s_cpu, 0, sizeof s_cpu);
    memset(frame, 0xcc, 20U * sizeof *frame);
    frame[0] = frame0;
    frame[1] = argument0;
    frame[2] = argument1;
    frame[3] = owner_return;
    s_cpu.esp = (uint32_t)(uintptr_t)frame;
    s_cpu.stack_owner = &s_cpu;
    s_cpu.stack_floor = floor;
    s_cpu.stack_ceiling = floor + ORACLE_STACK_BYTES;
    s_cpu.stack_low_water = s_cpu.esp;
    return frame;
}

static void route_heap_import(CPU *__restrict cpu)
{
    s_import_handled = isaac_vita_heap_import(cpu, s_import_name);
}

/* Runs one import and folds everything observable about its result. */
static uint32_t run_import(unsigned kind, const char *name, uint32_t argument)
{
    uint32_t initial_esp = s_cpu.esp;
    int stopped;

    s_import_name = name;
    s_import_handled = 0;
    stopped = guest_run_until_stop(&s_cpu, route_heap_import);
    CHECK(stopped == GUEST_RUN_RETURNED || stopped == GUEST_RUN_FAULT);
    ++s_phase_ops;
    if (s_cpu.fault)
        ++s_phase_faults;
    fold(kind);
    fold(argument);
    fold((uint64_t)stopped);
    fold((uint64_t)s_import_handled);
    fold(s_cpu.eax);
    fold_text(s_cpu.fault);
    fold(s_cpu.fault ? s_cpu.fault_addr : 0U);
    fold(s_cpu.esp - initial_esp);
    if (s_trace_ops)
        printf("[op] kind=%u arg=%08x stop=%d handled=%d eax=%08x esp=%d "
               "fault=%s at=%08x\n", kind, (unsigned)argument, stopped,
               s_import_handled, (unsigned)s_cpu.eax,
               (int)(s_cpu.esp - initial_esp),
               s_cpu.fault ? s_cpu.fault : "-",
               (unsigned)(s_cpu.fault ? s_cpu.fault_addr : 0U));
    return s_cpu.fault ? 0U : s_cpu.eax;
}

static uint32_t guest_malloc_owned(uint32_t size, uint32_t owner_return)
{
    (void)prepare_call(ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, size,
                       0xdec0de01U, owner_return);
    return run_import(1U, ISAAC_VITA_HEAP_MALLOC_NAME, size);
}

static uint32_t guest_malloc(uint32_t size)
{
    return guest_malloc_owned(size, 0x00123456U);
}

/* host_vita_anm2_scratch_routing_oracle.c call_malloc_with_texel_chain with
 * the adapted PNG chain (has_adapter = 1). */
static uint32_t guest_malloc_texel(void)
{
    uint32_t *import_frame = prepare_call(
        KAGE_VITA_TEXEL_MALLOC_IAT_RETURN_RVA,
        KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST, 0xdec0de11U, 0xdec0de12U);
    uint32_t *concrete_frame = import_frame + 8U;
    uint32_t *wrapper_frame = import_frame + 12U;
    uint32_t *adapter_frame = import_frame + 16U;

    concrete_frame[0] = (uint32_t)(uintptr_t)wrapper_frame;
    concrete_frame[1] = KAGE_VITA_TEXEL_WRAPPER_RETURN_RVA;
    wrapper_frame[0] = (uint32_t)(uintptr_t)adapter_frame;
    wrapper_frame[1] = KAGE_VITA_TEXEL_ADAPTER_RETURN_RVA;
    adapter_frame[0] = 0xdec0de14U;
    adapter_frame[1] = KAGE_VITA_TEXEL_PNG_LOADER_RETURN;
    s_cpu.ebp = (uint32_t)(uintptr_t)concrete_frame;
    return run_import(2U, ISAAC_VITA_HEAP_MALLOC_NAME,
                      KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST);
}

/* The Open owner after an ordinary native failure of exactly its size
 * (host_vita_ogg_emergency_oracle.c heap_routing_oracle). */
static uint32_t guest_malloc_ogg(void)
{
    uint32_t result;

    s_fail_size = ISAAC_VITA_OGG_OPEN_BACKING_BYTES;
    result = guest_malloc_owned(ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
                                ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA);
    s_fail_size = 0U;
    return result;
}

static const uint32_t s_anm2_owners[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
    ISAAC_VITA_ANM2_OWNER_RETURN_0, ISAAC_VITA_ANM2_OWNER_RETURN_1,
    ISAAC_VITA_ANM2_OWNER_RETURN_2, ISAAC_VITA_ANM2_OWNER_RETURN_3,
    ISAAC_VITA_ANM2_OWNER_RETURN_4, ISAAC_VITA_ANM2_OWNER_RETURN_5
};

static void guest_malloc_anm2_session(uint32_t out[ISAAC_VITA_ANM2_SEGMENT_COUNT])
{
    unsigned index;

    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        uint32_t size = index < 4U ? ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES
                                   : ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES;

        (void)prepare_call(ISAAC_VITA_ANM2_MALLOC_IMPORT_RETURN_RVA, size,
                           0xdec0de01U, s_anm2_owners[index]);
        out[index] = run_import(3U, ISAAC_VITA_HEAP_MALLOC_NAME, size);
    }
}

/* ---- per-free bookkeeping */
static unsigned s_calls_inside[OWNERS], s_calls_outside[OWNERS];
static unsigned s_frees_inside, s_frees_outside;

static int in_block(uintptr_t base, uint32_t bytes, uint32_t pointer)
{
    return base && pointer >= base && pointer - base < bytes;
}

/* Inside a published block, judged from the owners' own snapshots and extent
 * constants (not from the range records). */
static int inside_published_block(uint32_t pointer)
{
    isaac_vita_ogg_emergency_snapshot ogg;
    isaac_vita_anm2_scratch_snapshot anm2;
    isaac_vita_texel_scratch_snapshot texel;

    CHECK(isaac_vita_ogg_emergency_oracle_snapshot(&ogg));
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&anm2));
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&texel));
    return in_block(ogg.base, ISAAC_VITA_OGG_OPEN_BACKING_BYTES, pointer) ||
           in_block(anm2.base, ISAAC_VITA_ANM2_MEMBLOCK_BYTES, pointer) ||
           in_block(texel.base, KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES,
                    pointer);
}

static void guest_free(uint32_t pointer)
{
    unsigned before[OWNERS], owner;
    int inside = inside_published_block(pointer);

#if defined(ISAAC_VITA_HEAP_FREE_PREFILTER)
    {
        /* The exported records say exactly what the owners' state says. */
        int may_own =
            isaac_vita_heap_free_range_may_own(
                &isaac_vita_ogg_emergency_free_range, pointer) |
            isaac_vita_heap_free_range_may_own(
                &isaac_vita_anm2_scratch_free_range, pointer) |
            isaac_vita_heap_free_range_may_own(
                &isaac_vita_texel_scratch_free_range, pointer);

        CHECK(may_own == inside);
    }
#endif
    memcpy(before, s_owner_calls, sizeof before);
    (void)prepare_call(0xdec0de02U, pointer, 0U, 0xdec0de03U);
    (void)run_import(4U, ISAAC_VITA_HEAP_FREE_NAME, pointer);
    ++s_phase_frees;
    if (inside)
        ++s_frees_inside;
    else
        ++s_frees_outside;
    for (owner = 0U; owner < OWNERS; ++owner) {
        unsigned delta = s_owner_calls[owner] - before[owner];

        if (inside)
            s_calls_inside[owner] += delta;
        else
            s_calls_outside[owner] += delta;
    }
}

static void phase_end(const char *name)
{
    isaac_vita_ogg_emergency_snapshot ogg;
    isaac_vita_anm2_scratch_snapshot anm2;
    isaac_vita_texel_scratch_snapshot texel;

    CHECK(isaac_vita_ogg_emergency_oracle_snapshot(&ogg));
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&anm2));
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&texel));
    fold(s_native_malloc_calls); fold(s_native_malloc_null);
    fold(s_native_free_calls); fold(s_native_realloc_calls);
    fold(s_observer_calls); fold(s_observer_last); fold(s_diagnostic_calls);
    fold(s_memblock_alloc); fold(s_memblock_get); fold(s_memblock_free);
    fold(ogg.base); fold((uint32_t)ogg.uid); fold(ogg.poisoned);
    fold(ogg.live); fold(ogg.live_size); fold(ogg.acquired_count);
    fold(ogg.freed_count); fold(ogg.live_fallback_count);
    fold(ogg.reserve_failure_count);
    fold(anm2.base); fold((uint32_t)anm2.uid); fold(anm2.poisoned);
    fold(anm2.guest_heap_fallback); fold(anm2.session_id);
    fold(anm2.live_count); fold(anm2.acquired_count); fold(anm2.freed_count);
    fold(anm2.peak_live);
    fold(texel.base); fold((uint32_t)texel.uid); fold(texel.poisoned);
    fold(texel.live); fold(texel.live_size); fold(texel.high_water);
    fold(texel.acquired_count); fold(texel.freed_count);
    fold(texel.busy_fallback_count); fold(texel.oversize_fallback_count);
    printf("[phase] %s ops=%u frees=%u faults=%u observer=%u native=%u/%u "
           "anm2=%u/%u texel=%u/%u ogg=%u/%u digest=%016llx\n",
           name, s_phase_ops, s_phase_frees, s_phase_faults,
           s_observer_calls, s_native_malloc_calls, s_native_free_calls,
           anm2.acquired_count, anm2.freed_count,
           texel.acquired_count, texel.freed_count,
           ogg.acquired_count, ogg.freed_count,
           (unsigned long long)s_digest);
    s_phase_ops = s_phase_frees = s_phase_faults = 0U;
}

/* Boundary and foreign pointers around every block (published or not). */
static void sweep(void)
{
    static const struct { uint32_t base, bytes; } blocks[] = {
        { ORACLE_OGG_BASE, ISAAC_VITA_OGG_OPEN_BACKING_BYTES },
        { ORACLE_ANM2_BASE, ISAAC_VITA_ANM2_MEMBLOCK_BYTES },
        { ORACLE_TEXEL_BASE, KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES },
    };
    unsigned block;

    guest_free(0U);
    guest_free(0xffffffffU);
    guest_free((uint32_t)(uintptr_t)s_bank + 0x10U);
    for (block = 0U; block < sizeof blocks / sizeof blocks[0]; ++block) {
        uint32_t base = blocks[block].base, end = base + blocks[block].bytes;

        guest_free(base - 16U);
        guest_free(base - 1U);
        guest_free(base + 1U);
        guest_free(base + 16U);
        guest_free(base + 420000U);  /* ANM2 site 1 offset */
        guest_free(base + 0x00801000U - 16U); /* 8M texel class edge */
        guest_free(end - 16U);
        guest_free(end - 1U);
        guest_free(end);
        guest_free(end + 1U);
        guest_free(end + 0x1000U);
    }
}

static uint32_t s_rng = 0x2545f491U;

static uint32_t next_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* Device-shaped mix (A2S P3: 162 mallocs and 153 frees per update; block
 * frees ~0.06 % of all frees): ANM2 sessions, texel loads, OGG emergency
 * opens, foreign and boundary frees, on top of ordinary ledger traffic. */
static uint32_t s_live[8192];
static unsigned s_live_count;

static void mix(unsigned updates)
{
    unsigned update, index;

    for (update = 0U; update < updates; ++update) {
        for (index = 0U; index < 162U; ++index) {
            uint32_t roll = next_random();
            uint32_t size = 16U * (1U + roll % 32U);
            uint32_t pointer;

            if ((roll >> 8) % 64U == 0U)
                size = 4096U;
            if ((roll >> 14) % 1024U == 0U)
                size = 65536U;
            pointer = guest_malloc(size);
            if (pointer && s_live_count < sizeof s_live / sizeof s_live[0])
                s_live[s_live_count++] = pointer;
            else if (pointer)
                guest_free(pointer);
        }
        for (index = 0U; index < 153U || s_live_count > 4096U; ++index) {
            unsigned pick;

            if (!s_live_count) {
                guest_free(0U);
                continue;
            }
            pick = next_random() % s_live_count;
            guest_free(s_live[pick]);
            s_live[pick] = s_live[--s_live_count];
        }
        if (update % 10U == 3U) {
            uint32_t roll = next_random();

            /* a foreign pointer near the blocks, or a stale ledger one */
            guest_free(roll % 2U
                ? ORACLE_TEXEL_BASE + roll % (ORACLE_OGG_BASE + 0x80000U -
                                              ORACLE_TEXEL_BASE)
                : (uint32_t)(uintptr_t)s_bank + 16U + (roll % 4096U) * 16U);
        }
        if (update % 100U == 50U) {
            uint32_t sites[ISAAC_VITA_ANM2_SEGMENT_COUNT];

            guest_malloc_anm2_session(sites);
            for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index)
                guest_free(sites[index]);
        }
        if (update % 50U == 25U) {
            uint32_t texel = guest_malloc_texel();

            guest_free(texel + (update % 150U == 25U ? 16U : 0U));
            guest_free(texel);
        }
        if (update % 200U == 100U)
            guest_free(guest_malloc_ogg());
    }
}

static void drain(void)
{
    while (s_live_count)
        guest_free(s_live[--s_live_count]);
}

int main(void)
{
    uint32_t sites[ISAAC_VITA_ANM2_SEGMENT_COUNT], texel, ogg;
    unsigned index;

    s_digest = UINT64_C(1469598103934665603);
    s_trace_ops = getenv("ISAAC_PREFILTER_ORACLE_TRACE") != NULL;
    /* Fixed addresses: the digests fold guest pointers, so both builds
     * (and every run) must see the same bank and stack. */
    s_stack = mmap((void *)(uintptr_t)ORACLE_STACK_BASE, ORACLE_STACK_BYTES,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    s_bank = mmap((void *)(uintptr_t)ORACLE_BANK_BASE, ORACLE_BANK_BYTES,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(s_stack == (void *)(uintptr_t)ORACLE_STACK_BASE &&
          s_bank == (void *)(uintptr_t)ORACLE_BANK_BASE);
    CHECK((uintptr_t)s_stack + ORACLE_STACK_BYTES <= ORACLE_TEXEL_BASE &&
          (uintptr_t)s_bank + ORACLE_BANK_BYTES <= ORACLE_TEXEL_BASE);
    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0 &&
          isaac_vita_anm2_scratch_oracle_reset() == 0 &&
          isaac_vita_texel_scratch_oracle_reset() == 0);

    /* 1. nothing published: every free is foreign to the blocks */
    sweep();
    phase_end("unpublished");

    /* 2. publish all three (device order: texel, ANM2, then OGG) */
    texel = guest_malloc_texel();
    guest_malloc_anm2_session(sites);
    ogg = guest_malloc_ogg();
    CHECK(texel == ORACLE_TEXEL_BASE && sites[0] == ORACLE_ANM2_BASE &&
          ogg == ORACLE_OGG_BASE);
    phase_end("publish");

    /* 3. boundaries with every block live, then the exact frees, the
     * double frees and the stale frees */
    sweep();
    guest_free(texel);
    guest_free(ogg);
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index)
        guest_free(sites[index]);
    guest_free(texel);
    guest_free(ogg);
    guest_free(sites[0]);
    guest_free(sites[5]);
    sweep();
    phase_end("published");

    /* 4. the device-shaped mix */
    mix(2000U);
    drain();
    phase_end("mix");

    /* 5. owners reset (base unpublished, extent cleared): the old block
     * addresses are foreign again */
    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0 &&
          isaac_vita_anm2_scratch_oracle_reset() == 0 &&
          isaac_vita_texel_scratch_oracle_reset() == 0);
    sweep();
    mix(200U);
    drain();
    phase_end("reset");

    /* 6. published again after the reset */
    texel = guest_malloc_texel();
    guest_malloc_anm2_session(sites);
    ogg = guest_malloc_ogg();
    sweep();
    mix(300U);
    drain();
    phase_end("republished");

    printf("[calls] frees inside=%u outside=%u; owner calls inside "
           "ogg=%u anm2=%u texel=%u\n", s_frees_inside, s_frees_outside,
           s_calls_inside[OWNER_OGG], s_calls_inside[OWNER_ANM2],
           s_calls_inside[OWNER_TEXEL]);
    printf("[outside] owner calls for pointers outside every block: "
           "ogg=%u anm2=%u texel=%u\n", s_calls_outside[OWNER_OGG],
           s_calls_outside[OWNER_ANM2], s_calls_outside[OWNER_TEXEL]);
    CHECK(s_frees_inside > 0U && s_frees_outside > s_frees_inside);
#if defined(ISAAC_VITA_HEAP_FREE_PREFILTER)
    /* With the prefilter no owner runs for a pointer outside every block. */
    CHECK(!s_calls_outside[OWNER_OGG] && !s_calls_outside[OWNER_ANM2] &&
          !s_calls_outside[OWNER_TEXEL]);
    puts("Heap free prefilter differential oracle (prefilter ON): PASS");
#else
    /* Without it every free reaches the first owner. */
    CHECK(s_calls_outside[OWNER_OGG] == s_frees_outside);
    puts("Heap free prefilter differential oracle (prefilter OFF): PASS");
#endif
    return 0;
}
