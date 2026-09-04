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
#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"

#define ORACLE_STACK_BYTES 0x4000U
#define ORACLE_ANM2_SCRATCH_BASE 0x70000000U
#define ORACLE_TEXEL_SCRATCH_BASE 0x72000000U
#define ORACLE_ANM2_SCRATCH_UID 0x2345
#define ORACLE_TEXEL_SCRATCH_UID 0x3456

_Static_assert(ORACLE_ANM2_SCRATCH_BASE +
                   ISAAC_VITA_ANM2_MEMBLOCK_BYTES <=
                   ORACLE_TEXEL_SCRATCH_BASE,
               "combined scratch oracle ranges overlap");

static unsigned s_native_malloc_calls;
static size_t s_native_malloc_size;
static unsigned s_texel_diagnostic_calls;
static size_t s_texel_diagnostic_size;
static uint32_t s_texel_diagnostic_owner;
static unsigned s_texel_diagnostic_chain_depth;
static const char *s_texel_diagnostic_failure_reason;
static size_t s_texel_diagnostic_live_requested;
static size_t s_texel_diagnostic_largest_live_request;
static size_t s_texel_diagnostic_live_count;
static int s_texel_diagnostic_requested_exact;
static unsigned s_memblock_alloc_calls;
static unsigned s_memblock_get_calls;
static unsigned s_memblock_free_calls;
static unsigned s_fail_anm2_physical_pages;
static int s_anm2_memblock_live;
static int s_texel_memblock_live;
static const char *s_routing_import_name;
static int s_routing_import_handled;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ANM2 routing oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* host_vita_heap.c alone is compiled with -Dmalloc=oracle_guest_malloc so
 * ordinary guest allocations can be made deterministic without replacing
 * calloc for the heap ownership ledger itself. */
void *oracle_guest_malloc(size_t size)
{
    ++s_native_malloc_calls;
    s_native_malloc_size = size;
    return NULL;
}

void kage_vita_texel_oom_diagnostic(
    size_t failed_request, uint32_t owner_return_rva,
    unsigned owner_chain_depth, const char *failure_reason,
    size_t ledger_live_requested,
    size_t ledger_largest_live_request, size_t ledger_live_count,
    int ledger_requested_accounting_exact, uint32_t guest_stack_floor,
    uint32_t guest_stack_ceiling)
{
    ++s_texel_diagnostic_calls;
    s_texel_diagnostic_size = failed_request;
    s_texel_diagnostic_owner = owner_return_rva;
    s_texel_diagnostic_chain_depth = owner_chain_depth;
    s_texel_diagnostic_failure_reason = failure_reason;
    s_texel_diagnostic_live_requested = ledger_live_requested;
    s_texel_diagnostic_largest_live_request =
        ledger_largest_live_request;
    s_texel_diagnostic_live_count = ledger_live_count;
    s_texel_diagnostic_requested_exact =
        ledger_requested_accounting_exact;
    (void)guest_stack_floor;
    (void)guest_stack_ceiling;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_memblock_alloc_calls;
    if (!name || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || option)
        return -1;
    if (strcmp(name, "isaac_anm2_scratch") == 0 &&
        size == ISAAC_VITA_ANM2_MEMBLOCK_BYTES &&
        !s_anm2_memblock_live) {
        if (s_fail_anm2_physical_pages) {
            --s_fail_anm2_physical_pages;
            return (SceUID)ISAAC_VITA_ANM2_NO_FREE_PHYSICAL_PAGE;
        }
        s_anm2_memblock_live = 1;
        return ORACLE_ANM2_SCRATCH_UID;
    }
    if (strcmp(name, "isaac_texel_scratch") == 0 &&
        size == KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES &&
        !s_texel_memblock_live) {
        s_texel_memblock_live = 1;
        return ORACLE_TEXEL_SCRATCH_UID;
    }
    return -1;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_memblock_get_calls;
    if (!base)
        return -1;
    if (uid == ORACLE_ANM2_SCRATCH_UID && s_anm2_memblock_live) {
        *base = (void *)(uintptr_t)ORACLE_ANM2_SCRATCH_BASE;
        return 0;
    }
    if (uid == ORACLE_TEXEL_SCRATCH_UID && s_texel_memblock_live) {
        *base = (void *)(uintptr_t)ORACLE_TEXEL_SCRATCH_BASE;
        return 0;
    }
    return -1;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_memblock_free_calls;
    if (uid == ORACLE_ANM2_SCRATCH_UID && s_anm2_memblock_live) {
        s_anm2_memblock_live = 0;
        return 0;
    }
    if (uid == ORACLE_TEXEL_SCRATCH_UID && s_texel_memblock_live) {
        s_texel_memblock_live = 0;
        return 0;
    }
    return -1;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    va_end(arguments);
}

static uint32_t *prepare_call(CPU *cpu, void *stack, uint32_t argument0,
                              uint32_t argument1, uint32_t owner_return)
{
    uint32_t *frame = (uint32_t *)((unsigned char *)stack + 0x2000U);
    uint32_t floor = (uint32_t)(uintptr_t)stack;

    memset(cpu, 0, sizeof *cpu);
    memset(frame, 0xcc, 5U * sizeof *frame);
    frame[0] = ISAAC_VITA_ANM2_MALLOC_IMPORT_RETURN_RVA;
    frame[1] = argument0;
    frame[2] = argument1;
    frame[3] = owner_return;
    cpu->esp = (uint32_t)(uintptr_t)frame;
    cpu->stack_owner = cpu;
    cpu->stack_floor = floor;
    cpu->stack_ceiling = floor + ORACLE_STACK_BYTES;
    cpu->stack_low_water = cpu->esp;
    return frame;
}

static void route_heap_import(CPU *__restrict cpu)
{
    s_routing_import_handled =
        isaac_vita_heap_import(cpu, s_routing_import_name);
}

static int run_heap_import(CPU *cpu, const char *name, uint32_t initial_esp)
{
    int stopped;

    s_routing_import_name = name;
    s_routing_import_handled = 0;
    stopped = guest_run_until_stop(cpu, route_heap_import);
    if (stopped == GUEST_RUN_FAULT)
        return cpu->fault && cpu->esp == initial_esp;
    return stopped == GUEST_RUN_RETURNED && !cpu->fault &&
           s_routing_import_handled == 1 && cpu->esp == initial_esp + 4U;
}

static int call_malloc(CPU *cpu, void *stack, uint32_t size,
                       uint32_t owner_return, uint32_t *result)
{
    uint32_t initial_esp;

    (void)prepare_call(cpu, stack, size, 0xdec0de01U, owner_return);
    initial_esp = cpu->esp;
    if (!run_heap_import(cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int call_malloc_with_texel_chain(
    CPU *cpu, void *stack, uint32_t size, uint32_t raw_owner_return,
    uint32_t texel_owner_return, int has_adapter, uint32_t *result)
{
    uint32_t *import_frame = prepare_call(
        cpu, stack, size, 0xdec0de11U, raw_owner_return);
    uint32_t *concrete_frame = import_frame + 8U;
    uint32_t *wrapper_frame = import_frame + 12U;
    uint32_t *adapter_frame = import_frame + 16U;
    uint32_t initial_esp = cpu->esp;

    import_frame[0] = KAGE_VITA_TEXEL_MALLOC_IAT_RETURN_RVA;
    concrete_frame[0] = (uint32_t)(uintptr_t)wrapper_frame;
    concrete_frame[1] = KAGE_VITA_TEXEL_WRAPPER_RETURN_RVA;
    wrapper_frame[0] = has_adapter
        ? (uint32_t)(uintptr_t)adapter_frame : 0xdec0de13U;
    wrapper_frame[1] = has_adapter
        ? KAGE_VITA_TEXEL_ADAPTER_RETURN_RVA : texel_owner_return;
    adapter_frame[0] = 0xdec0de14U;
    adapter_frame[1] = texel_owner_return;
    cpu->ebp = (uint32_t)(uintptr_t)concrete_frame;
    if (!run_heap_import(cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int call_free(CPU *cpu, void *stack, uint32_t pointer)
{
    uint32_t initial_esp;

    (void)prepare_call(cpu, stack, pointer, 0xdec0de02U, 0xdec0de03U);
    initial_esp = cpu->esp;
    return run_heap_import(cpu, ISAAC_VITA_HEAP_FREE_NAME, initial_esp);
}

static int call_realloc(CPU *cpu, void *stack, uint32_t pointer,
                        uint32_t size)
{
    uint32_t initial_esp;

    (void)prepare_call(cpu, stack, pointer, size, 0xdec0de04U);
    initial_esp = cpu->esp;
    return run_heap_import(cpu, ISAAC_VITA_HEAP_REALLOC_NAME, initial_esp);
}

int main(void)
{
    static const uint32_t owners[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
        ISAAC_VITA_ANM2_OWNER_RETURN_0,
        ISAAC_VITA_ANM2_OWNER_RETURN_1,
        ISAAC_VITA_ANM2_OWNER_RETURN_2,
        ISAAC_VITA_ANM2_OWNER_RETURN_3,
        ISAAC_VITA_ANM2_OWNER_RETURN_4,
        ISAAC_VITA_ANM2_OWNER_RETURN_5
    };
    static const uint32_t offsets[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
        0U, 420000U, 840000U, 1260000U, 1680000U, 2220000U
    };
    CPU cpu;
    isaac_vita_anm2_scratch_snapshot anm2_snapshot;
    isaac_vita_texel_scratch_snapshot texel_snapshot;
    void *stack;
    uint32_t pointers[ISAAC_VITA_ANM2_SEGMENT_COUNT];
    uint32_t result;
    unsigned index;

    stack = mmap(NULL, ORACLE_STACK_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(stack != MAP_FAILED && (uintptr_t)stack <= UINT32_MAX &&
          (uintptr_t)stack + ORACLE_STACK_BYTES <= UINT32_MAX);
    CHECK(!((uintptr_t)stack < ORACLE_ANM2_SCRATCH_BASE +
                               ISAAC_VITA_ANM2_MEMBLOCK_BYTES &&
            ORACLE_ANM2_SCRATCH_BASE <
                (uintptr_t)stack + ORACLE_STACK_BYTES));
    CHECK(!((uintptr_t)stack < ORACLE_TEXEL_SCRATCH_BASE +
                               KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES &&
            ORACLE_TEXEL_SCRATCH_BASE <
                (uintptr_t)stack + ORACLE_STACK_BYTES));

    /* The first request deliberately also has the exact PNG EBP chain.  The
     * raw ANM2 owner at ESP+12 must win before texel-chain resolution; all six
     * exact raw owners retain their fixed offsets and session semantics. */
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        uint32_t size = index < 4U
            ? ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES
            : ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES;
        if (index == 0U) {
            CHECK(call_malloc_with_texel_chain(
                &cpu, stack, size, owners[index],
                KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
        }
        else {
            CHECK(call_malloc(
                &cpu, stack, size, owners[index], &result));
        }
        CHECK(!cpu.fault &&
              result == ORACLE_ANM2_SCRATCH_BASE + offsets[index]);
        pointers[index] = result;
    }
    CHECK(s_memblock_alloc_calls == 1U && s_memblock_get_calls == 1U &&
          s_memblock_free_calls == 0U && s_anm2_memblock_live &&
          !s_texel_memblock_live);
    CHECK(s_native_malloc_calls == 0U && s_texel_diagnostic_calls == 0U);

    /* Exact-base free is consumed before the ordinary ownership ledger. */
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        CHECK(call_free(&cpu, stack, pointers[index]));
        CHECK(!cpu.fault);
    }
    CHECK(s_memblock_free_calls == 0U && s_anm2_memblock_live &&
          !s_texel_memblock_live);
    CHECK(call_free(&cpu, stack, pointers[0]));
    CHECK(cpu.fault && strstr(cpu.fault, "double free"));

    /* Exact owner plus wrong size fails closed.  Generic NULL malloc results
     * must not consume the texture-only one-shot diagnostic. */
    CHECK(call_malloc(&cpu, stack,
                      ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES + 1U,
                      ISAAC_VITA_ANM2_OWNER_RETURN_0, &result));
    CHECK(cpu.fault && strstr(cpu.fault, "wrong size"));
    CHECK(s_native_malloc_calls == 0U && s_texel_diagnostic_calls == 0U);

    CHECK(call_malloc(&cpu, stack, 8388608U, 0x00123456U, &result));
    CHECK(!cpu.fault && result == 0U && s_native_malloc_calls == 1U &&
          s_native_malloc_size == 8388608U &&
          s_texel_diagnostic_calls == 0U);
    CHECK(call_malloc(&cpu, stack, 4660224U, 0x00123456U, &result));
    CHECK(!cpu.fault && result == 0U && s_native_malloc_calls == 2U &&
          s_native_malloc_size == 4660224U &&
          s_texel_diagnostic_calls == 0U);
    CHECK(call_malloc(&cpu, stack,
                      ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
                      0x00123456U, &result));
    CHECK(!cpu.fault && result == 0U && s_native_malloc_calls == 3U &&
          s_native_malloc_size == ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES &&
          s_texel_diagnostic_calls == 0U);

    /* A known non-PNG loader has an exact direct EBP chain but still belongs
     * to the ordinary heap and its diagnostic after native malloc fails. */
    CHECK(call_malloc_with_texel_chain(
        &cpu, stack, KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST,
        0xdec0de12U, KAGE_VITA_TEXEL_LOADER_RETURN_0, 0, &result));
    CHECK(!cpu.fault && result == 0U && s_native_malloc_calls == 4U &&
          s_texel_diagnostic_calls == 1U &&
          s_texel_diagnostic_size == KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST &&
          s_texel_diagnostic_owner == KAGE_VITA_TEXEL_LOADER_RETURN_0 &&
          s_texel_diagnostic_chain_depth == 2U &&
          strcmp(s_texel_diagnostic_failure_reason,
                 "native-malloc-failed") == 0 &&
          s_texel_diagnostic_live_requested == 0U &&
          s_texel_diagnostic_largest_live_request == 0U &&
          s_texel_diagnostic_live_count == 0U &&
          s_texel_diagnostic_requested_exact == 1);

    /* The exact adapted PNG chain is consumed before the ordinary heap. */
    CHECK(call_malloc_with_texel_chain(
        &cpu, stack, KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST,
        0xdec0de12U, KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    CHECK(!cpu.fault && result == ORACLE_TEXEL_SCRATCH_BASE &&
          s_native_malloc_calls == 4U && s_texel_diagnostic_calls == 1U);
    CHECK(s_memblock_alloc_calls == 2U && s_memblock_get_calls == 2U &&
          s_memblock_free_calls == 0U && s_anm2_memblock_live &&
          s_texel_memblock_live);
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&anm2_snapshot));
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&texel_snapshot));
    CHECK(anm2_snapshot.base == ORACLE_ANM2_SCRATCH_BASE &&
          anm2_snapshot.uid == ORACLE_ANM2_SCRATCH_UID &&
          texel_snapshot.base == ORACLE_TEXEL_SCRATCH_BASE &&
          texel_snapshot.uid == ORACLE_TEXEL_SCRATCH_UID &&
          texel_snapshot.live == 1U &&
          texel_snapshot.live_size == KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST);
    CHECK(anm2_snapshot.base + ISAAC_VITA_ANM2_MEMBLOCK_BYTES <=
              texel_snapshot.base ||
          texel_snapshot.base + KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES <=
              anm2_snapshot.base);

    /* Exact bases are dispatched to their owners before the ordinary ledger;
     * both native memblocks remain persistent after their logical frees. */
    CHECK(call_free(&cpu, stack, result));
    CHECK(!cpu.fault && s_memblock_free_calls == 0U &&
          s_anm2_memblock_live && s_texel_memblock_live);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&texel_snapshot));
    CHECK(texel_snapshot.live == 0U && texel_snapshot.freed_count == 1U);
    CHECK(call_free(&cpu, stack, result));
    CHECK(cpu.fault && strstr(cpu.fault, "double/stale free"));

    /* ANM2 realloc was intentionally not taught scratch ownership. */
    CHECK(call_realloc(&cpu, stack, pointers[0], 64U));
    CHECK(cpu.fault && strstr(cpu.fault, "realloc received a foreign"));

    CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    CHECK(!s_anm2_memblock_live && s_texel_memblock_live &&
          s_memblock_free_calls == 1U);
    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    CHECK(!s_anm2_memblock_live && !s_texel_memblock_live &&
          s_memblock_free_calls == 2U);

    /* The exact hardware failure must leave the ANM2 seam and reach the
     * ordinary heap import for all six owners.  This oracle's ordinary malloc
     * deliberately returns NULL; the overflow-router oracle independently
     * proves that the production configuration then tries its live mspace. */
    {
        unsigned alloc_before = s_memblock_alloc_calls;
        unsigned get_before = s_memblock_get_calls;
        unsigned native_before = s_native_malloc_calls;
        unsigned diagnostic_before = s_texel_diagnostic_calls;

        s_fail_anm2_physical_pages = 1U;
        for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
            uint32_t size = index < 4U
                ? ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES
                : ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES;

            CHECK(call_malloc(&cpu, stack, size, owners[index], &result));
            CHECK(!cpu.fault && result == 0U);
        }
        CHECK(s_fail_anm2_physical_pages == 0U &&
              s_memblock_alloc_calls == alloc_before + 1U &&
              s_memblock_get_calls == get_before &&
              s_native_malloc_calls ==
                  native_before + ISAAC_VITA_ANM2_SEGMENT_COUNT &&
              s_texel_diagnostic_calls == diagnostic_before);
        CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&anm2_snapshot));
        CHECK(!anm2_snapshot.base && anm2_snapshot.uid == -1 &&
              !anm2_snapshot.poisoned &&
              anm2_snapshot.guest_heap_fallback == 1U &&
              anm2_snapshot.session_id == 1U &&
              anm2_snapshot.live_count == 0U &&
              anm2_snapshot.acquired_count ==
                  ISAAC_VITA_ANM2_SEGMENT_COUNT);

        CHECK(call_malloc(&cpu, stack, ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
                          ISAAC_VITA_ANM2_OWNER_RETURN_1, &result));
        CHECK(cpu.fault && strstr(cpu.fault, "restart at AA5D") &&
              s_native_malloc_calls ==
                  native_before + ISAAC_VITA_ANM2_SEGMENT_COUNT);
        CHECK(call_malloc(&cpu, stack, ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
                          ISAAC_VITA_ANM2_OWNER_RETURN_0, &result));
        CHECK(!cpu.fault && result == 0U &&
              s_native_malloc_calls ==
                  native_before + ISAAC_VITA_ANM2_SEGMENT_COUNT + 1U);
        CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&anm2_snapshot));
        CHECK(anm2_snapshot.guest_heap_fallback == 1U &&
              anm2_snapshot.session_id == 2U &&
              anm2_snapshot.acquired_count == 1U);
        CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    }
    CHECK(munmap(stack, ORACLE_STACK_BYTES) == 0);
    puts("Vita combined ANM2/texel scratch heap routing oracle: PASS");
    return 0;
}
