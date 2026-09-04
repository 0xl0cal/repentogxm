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
#include "host_vita_heap.h"
#include "host_vita_ogg_emergency.h"
#include "kage_vita_texture_memory.h"

#define ORACLE_STACK_BYTES 0x4000U
#define ORACLE_SLOT_BASE   0x70000000U
#define ORACLE_HEAP_OVERLAP_BASE  0x83000000U
#define ORACLE_IMAGE_OVERLAP_BASE 0x98000000U
#define ORACLE_STACK_OVERLAP_BASE 0x74000000U
#define ORACLE_SLOT_UID    0x4567

_Static_assert(ORACLE_HEAP_OVERLAP_BASE >= KAGE_VITA_TEXEL_LINK_END &&
                   ORACLE_HEAP_OVERLAP_BASE +
                       ISAAC_VITA_OGG_OPEN_BACKING_BYTES <=
                   KAGE_VITA_TEXEL_HEAP_RETAINED_END,
               "heap-overlap fixture left the frozen retained heap");
_Static_assert(ORACLE_IMAGE_OVERLAP_BASE >=
                   KAGE_VITA_TEXEL_GUEST_IMAGE_BASE &&
               ORACLE_IMAGE_OVERLAP_BASE +
                       ISAAC_VITA_OGG_OPEN_BACKING_BYTES <=
                   KAGE_VITA_TEXEL_GUEST_IMAGE_END,
               "image-overlap fixture left the frozen guest image");

enum oracle_memblock_mode {
    ORACLE_MEMBLOCK_READY,
    ORACLE_MEMBLOCK_ALLOC_FAIL,
    ORACLE_MEMBLOCK_GET_FAIL,
    ORACLE_MEMBLOCK_MISALIGNED,
    ORACLE_MEMBLOCK_HEAP_OVERLAP,
    ORACLE_MEMBLOCK_IMAGE_OVERLAP,
    ORACLE_MEMBLOCK_STACK_OVERLAP,
    ORACLE_MEMBLOCK_ROLLBACK_FAIL
};

static enum oracle_memblock_mode s_memblock_mode;
static int s_memblock_live;
static unsigned s_memblock_alloc_calls;
static unsigned s_memblock_get_calls;
static unsigned s_memblock_free_calls;
static unsigned s_log_calls;
static unsigned s_native_malloc_calls;
static size_t s_native_malloc_size;
static uintptr_t s_native_malloc_result;
static unsigned s_native_calloc_calls;
static int s_fail_native_calloc;
static const char *s_routing_import_name;
static int s_routing_import_handled;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "OGG emergency oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void *oracle_guest_malloc(size_t size)
{
    ++s_native_malloc_calls;
    s_native_malloc_size = size;
    return (void *)s_native_malloc_result;
}

void *oracle_guest_calloc(size_t count, size_t size)
{
    ++s_native_calloc_calls;
    if (s_fail_native_calloc)
        return NULL;
    return calloc(count, size);
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_memblock_alloc_calls;
    if (!name || strcmp(name, "isaac_ogg_emergency") != 0 ||
        type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != ISAAC_VITA_OGG_OPEN_BACKING_BYTES || option ||
        s_memblock_live || s_memblock_mode == ORACLE_MEMBLOCK_ALLOC_FAIL)
        return -1;
    s_memblock_live = 1;
    return ORACLE_SLOT_UID;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_memblock_get_calls;
    if (uid != ORACLE_SLOT_UID || !base || !s_memblock_live)
        return -1;
    if (s_memblock_mode == ORACLE_MEMBLOCK_GET_FAIL)
        return -2;
    if (s_memblock_mode == ORACLE_MEMBLOCK_MISALIGNED)
        *base = (void *)(uintptr_t)(ORACLE_SLOT_BASE + 1U);
    else if (s_memblock_mode == ORACLE_MEMBLOCK_HEAP_OVERLAP)
        *base = (void *)(uintptr_t)ORACLE_HEAP_OVERLAP_BASE;
    else if (s_memblock_mode == ORACLE_MEMBLOCK_IMAGE_OVERLAP)
        *base = (void *)(uintptr_t)ORACLE_IMAGE_OVERLAP_BASE;
    else if (s_memblock_mode == ORACLE_MEMBLOCK_STACK_OVERLAP)
        *base = (void *)(uintptr_t)ORACLE_STACK_OVERLAP_BASE;
    else
        *base = (void *)(uintptr_t)ORACLE_SLOT_BASE;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_memblock_free_calls;
    if (uid != ORACLE_SLOT_UID || !s_memblock_live)
        return -1;
    if (s_memblock_mode == ORACLE_MEMBLOCK_ROLLBACK_FAIL)
        return -3;
    s_memblock_live = 0;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    ++s_log_calls;
    va_start(arguments, format);
    va_end(arguments);
}

static void clear_memblock_counters(void)
{
    s_memblock_alloc_calls = 0U;
    s_memblock_get_calls = 0U;
    s_memblock_free_calls = 0U;
    s_log_calls = 0U;
}

static int direct_module_oracle(void)
{
    isaac_vita_ogg_emergency_decision decision;
    isaac_vita_ogg_emergency_snapshot snapshot;
    void *pointer;

    s_memblock_mode = ORACLE_MEMBLOCK_READY;
    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0);
    clear_memblock_counters();

    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_QUEUE_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_QUEUE_DECODER_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_THIRD_SIZE_OWNER_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES - 1U,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(s_memblock_alloc_calls == 0U);

    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74400000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_REJECTED && decision.fault);
    CHECK(isaac_vita_ogg_emergency_oracle_force_lock(1));
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(isaac_vita_ogg_emergency_oracle_force_lock(0));
    CHECK(s_memblock_alloc_calls == 0U);

    s_memblock_mode = ORACLE_MEMBLOCK_ALLOC_FAIL;
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(s_memblock_alloc_calls == 1U && !s_memblock_live);

    s_memblock_mode = ORACLE_MEMBLOCK_GET_FAIL;
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(s_memblock_alloc_calls == 2U && s_memblock_get_calls == 1U &&
          s_memblock_free_calls == 1U && !s_memblock_live);

    s_memblock_mode = ORACLE_MEMBLOCK_MISALIGNED;
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(s_memblock_alloc_calls == 3U && s_memblock_free_calls == 2U &&
          !s_memblock_live);

    static const enum oracle_memblock_mode overlap_modes[] = {
        ORACLE_MEMBLOCK_HEAP_OVERLAP,
        ORACLE_MEMBLOCK_IMAGE_OVERLAP,
        ORACLE_MEMBLOCK_STACK_OVERLAP
    };
    for (size_t index = 0U;
         index < sizeof overlap_modes / sizeof overlap_modes[0]; ++index) {
        s_memblock_mode = overlap_modes[index];
        CHECK(isaac_vita_ogg_emergency_malloc(
                  ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
                  ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
                  ORACLE_STACK_OVERLAP_BASE,
                  ORACLE_STACK_OVERLAP_BASE + ORACLE_STACK_BYTES,
                  &decision) == ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
        CHECK(!s_memblock_live);
    }
    CHECK(s_memblock_alloc_calls == 6U && s_memblock_free_calls == 5U);

    s_memblock_mode = ORACLE_MEMBLOCK_ROLLBACK_FAIL;
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x70000000U,
              0x70000000U + ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              &decision) == ISAAC_VITA_OGG_EMERGENCY_REJECTED);
    CHECK(decision.fault && strstr(decision.fault, "rollback failed") &&
          s_memblock_live);
    CHECK(isaac_vita_ogg_emergency_oracle_snapshot(&snapshot));
    CHECK(snapshot.poisoned == 1U);
    s_memblock_mode = ORACLE_MEMBLOCK_READY;
    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0 &&
          !s_memblock_live);

    clear_memblock_counters();
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_HANDLED);
    pointer = decision.pointer;
    CHECK(pointer == (void *)(uintptr_t)ORACLE_SLOT_BASE);
    CHECK(s_memblock_alloc_calls == 1U && s_memblock_get_calls == 1U &&
          s_memblock_free_calls == 0U && s_memblock_live);
    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED);
    CHECK(isaac_vita_ogg_emergency_oracle_snapshot(&snapshot));
    CHECK(snapshot.live == 1U &&
          snapshot.live_size == ISAAC_VITA_OGG_OPEN_BACKING_BYTES &&
          snapshot.acquired_count == 1U &&
          snapshot.live_fallback_count == 1U);

    CHECK(isaac_vita_ogg_emergency_free(
              (void *)((uintptr_t)pointer + 4U), &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_REJECTED);
    CHECK(isaac_vita_ogg_emergency_realloc(
              (void *)((uintptr_t)pointer + 4U), 16U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_REJECTED);
    CHECK(isaac_vita_ogg_emergency_realloc(
              pointer, ISAAC_VITA_OGG_OPEN_BACKING_BYTES + 1U,
              &decision) == ISAAC_VITA_OGG_EMERGENCY_HANDLED &&
          decision.pointer == NULL);
    CHECK(isaac_vita_ogg_emergency_oracle_snapshot(&snapshot));
    CHECK(snapshot.live == 1U &&
          snapshot.live_size == ISAAC_VITA_OGG_OPEN_BACKING_BYTES);
    CHECK(isaac_vita_ogg_emergency_realloc(
              pointer, 128U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_HANDLED &&
          decision.pointer == pointer);

    CHECK(isaac_vita_ogg_emergency_oracle_force_lock(1));
    CHECK(isaac_vita_ogg_emergency_free(pointer, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_REJECTED);
    CHECK(isaac_vita_ogg_emergency_realloc(pointer, 64U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_REJECTED);
    CHECK(isaac_vita_ogg_emergency_oracle_force_lock(0));

    CHECK(isaac_vita_ogg_emergency_free(pointer, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_HANDLED);
    CHECK(isaac_vita_ogg_emergency_free(pointer, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_REJECTED);
    CHECK(s_memblock_free_calls == 0U && s_memblock_live);

    CHECK(isaac_vita_ogg_emergency_malloc(
              ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
              ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
              0x74000000U, 0x74400000U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_HANDLED);
    CHECK(decision.pointer == pointer && s_memblock_alloc_calls == 1U);
    CHECK(isaac_vita_ogg_emergency_realloc(pointer, 0U, &decision) ==
          ISAAC_VITA_OGG_EMERGENCY_HANDLED &&
          decision.pointer == NULL);
    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0 &&
          !s_memblock_live && s_memblock_free_calls == 1U);
    return 0;
}

static uint32_t *prepare_call(CPU *cpu, void *stack, uint32_t argument0,
                              uint32_t argument1, uint32_t owner_return,
                              uint32_t wrapper_return)
{
    uint32_t *frame = (uint32_t *)((unsigned char *)stack + 0x2000U);
    uint32_t floor = (uint32_t)(uintptr_t)stack;

    memset(cpu, 0, sizeof *cpu);
    memset(frame, 0xcc, 4U * sizeof *frame);
    frame[0] = wrapper_return;
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
                       uint32_t owner_return, uint32_t wrapper_return,
                       uint32_t *result)
{
    uint32_t initial_esp;

    (void)prepare_call(
        cpu, stack, size, 0xdec0de01U, owner_return, wrapper_return);
    initial_esp = cpu->esp;
    if (!run_heap_import(cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int call_free(CPU *cpu, void *stack, uint32_t pointer)
{
    uint32_t initial_esp;

    (void)prepare_call(
        cpu, stack, pointer, 0U, 0U, 0xdec0de02U);
    initial_esp = cpu->esp;
    return run_heap_import(cpu, ISAAC_VITA_HEAP_FREE_NAME, initial_esp);
}

static int call_realloc(CPU *cpu, void *stack, uint32_t pointer,
                        uint32_t size, uint32_t *result)
{
    uint32_t initial_esp;

    (void)prepare_call(
        cpu, stack, pointer, size, 0U, 0xdec0de03U);
    initial_esp = cpu->esp;
    if (!run_heap_import(cpu, ISAAC_VITA_HEAP_REALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int heap_routing_oracle(void)
{
    CPU cpu;
    void *stack;
    uint32_t result;
    uint32_t pointer;
    unsigned native_before;
    unsigned alloc_before;

    s_memblock_mode = ORACLE_MEMBLOCK_READY;
    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0);
    clear_memblock_counters();
    s_native_malloc_calls = 0U;
    s_native_malloc_size = 0U;
    s_native_malloc_result = 0U;
    s_native_calloc_calls = 0U;

    stack = mmap(NULL, ORACLE_STACK_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(stack != MAP_FAILED && (uintptr_t)stack <= UINT32_MAX &&
          (uintptr_t)stack + ORACLE_STACK_BYTES <= UINT32_MAX);
    CHECK(!((uintptr_t)stack <
                ORACLE_SLOT_BASE + ISAAC_VITA_OGG_OPEN_BACKING_BYTES &&
            ORACLE_SLOT_BASE < (uintptr_t)stack + ORACLE_STACK_BYTES));

    /* A ledger-reserve failure is not a native-malloc failure and must not
     * consume or reserve the emergency slot. */
    s_fail_native_calloc = 1;
    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
    CHECK(!cpu.fault && result == 0U && s_native_calloc_calls == 1U &&
          s_native_malloc_calls == 0U && s_memblock_alloc_calls == 0U);
    s_fail_native_calloc = 0;

    /* Even the exact owner/size must keep an ordinary native success.  The
     * fixed low32 fixture remains ledger-owned until process exit and is never
     * handed to host free. */
    s_native_malloc_result = 0x71000000U;
    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
    CHECK(!cpu.fault && result == 0x71000000U &&
          s_native_malloc_calls == 1U && s_memblock_alloc_calls == 0U);
    s_native_malloc_result = 0U;

    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        0xdec0de04U, &result));
    CHECK(!cpu.fault && result == 0U && s_native_malloc_calls == 2U &&
          s_memblock_alloc_calls == 0U);

    static const uint32_t excluded_owners[] = {
        ISAAC_VITA_OGG_QUEUE_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_OGG_QUEUE_DECODER_RETURN_RVA,
        ISAAC_VITA_OGG_THIRD_SIZE_OWNER_RVA
    };
    for (size_t index = 0U;
         index < sizeof excluded_owners / sizeof excluded_owners[0];
         ++index) {
        CHECK(call_malloc(
            &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
            excluded_owners[index],
            ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
        CHECK(!cpu.fault && result == 0U &&
              s_memblock_alloc_calls == 0U);
    }
    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES - 1U,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
    CHECK(!cpu.fault && result == 0U && s_memblock_alloc_calls == 0U);

    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
    CHECK(!cpu.fault && result == ORACLE_SLOT_BASE &&
          s_native_malloc_size == ISAAC_VITA_OGG_OPEN_BACKING_BYTES &&
          s_memblock_alloc_calls == 1U && s_memblock_get_calls == 1U);
    pointer = result;

    native_before = s_native_malloc_calls;
    alloc_before = s_memblock_alloc_calls;
    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
    CHECK(!cpu.fault && result == 0U &&
          s_native_malloc_calls == native_before + 1U &&
          s_memblock_alloc_calls == alloc_before);

    CHECK(call_realloc(&cpu, stack, pointer, 128U, &result));
    CHECK(!cpu.fault && result == pointer);
    CHECK(call_free(&cpu, stack, pointer + 4U));
    CHECK(cpu.fault && strstr(cpu.fault, "interior OGG emergency"));
    CHECK(call_free(&cpu, stack, pointer));
    CHECK(!cpu.fault && s_memblock_free_calls == 0U && s_memblock_live);
    CHECK(call_free(&cpu, stack, pointer));
    CHECK(cpu.fault && strstr(cpu.fault, "double/stale free"));

    CHECK(call_malloc(
        &cpu, stack, ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
        ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
        ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA, &result));
    CHECK(!cpu.fault && result == pointer && s_memblock_alloc_calls == 1U);
    CHECK(call_realloc(&cpu, stack, pointer, 0U, &result));
    CHECK(!cpu.fault && result == 0U);

    CHECK(isaac_vita_ogg_emergency_oracle_reset() == 0 &&
          !s_memblock_live && s_memblock_free_calls == 1U);
    CHECK(munmap(stack, ORACLE_STACK_BYTES) == 0);
    return 0;
}

int main(void)
{
    if (direct_module_oracle() != 0 || heap_routing_oracle() != 0)
        return 1;
    puts("Vita OGG emergency lifecycle/heap-routing oracle: PASS");
    return 0;
}
