/* Native lifecycle oracle for the single-lease ImagePng texel scratch block.
 * The ARM build selects the small link-only branch at the end of this file;
 * that ELF is inspected and is never executed on the build host. */
#ifdef ISAAC_VITA_TEXEL_SCRATCH_LINK_PROBE

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"

_Alignas(KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES)
static unsigned char
    s_probe_block[KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES];
static int s_probe_live;

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    (void)name;
    (void)option;
    if (s_probe_live || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES)
        return -1;
    s_probe_live = 1;
    return 0x2345;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid != 0x2345 || !base || !s_probe_live)
        return -1;
    *base = s_probe_block;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid != 0x2345 || !s_probe_live)
        return -1;
    s_probe_live = 0;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    va_end(arguments);
}

int main(void)
{
    isaac_vita_texel_scratch_decision decision;
    void *pointer;

    if (isaac_vita_texel_scratch_malloc(
            KAGE_VITA_TEXEL_PNG_LOADER_RETURN,
            KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
            0x8a000000U, 0x8a400000U, &decision) !=
                ISAAC_VITA_TEXEL_SCRATCH_HANDLED ||
        !decision.pointer)
        return 1;
    pointer = decision.pointer;
    if (isaac_vita_texel_scratch_realloc(
            pointer, 4096U, &decision) !=
                ISAAC_VITA_TEXEL_SCRATCH_HANDLED ||
        decision.pointer != pointer)
        return 2;
    if (isaac_vita_texel_scratch_free(pointer, &decision) !=
            ISAAC_VITA_TEXEL_SCRATCH_HANDLED)
        return 3;
    return 0;
}

#elif defined(ISAAC_VITA_TEXEL_SCRATCH_ROUTING_ORACLE)

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
#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"

#define ROUTING_STACK_BYTES  0x4000U
#define ROUTING_SCRATCH_BASE 0x70000000U
#define ROUTING_SCRATCH_UID  ((SceUID)0x4567)
#define ROUTING_FAILURE_LOG_CAPACITY 16U
#define ROUTING_FAILURE_LOG_BYTES    384U
#define ROUTING_FAILURE_LOG_PREFIX   "guest heap allocation failed:"
#define ROUTING_STREAM_QUEUE_RETURN_RVA 0x005a23fbU

static unsigned s_routing_native_malloc_calls;
static size_t s_routing_native_malloc_size;
static unsigned s_routing_diagnostic_calls;
static size_t s_routing_diagnostic_size;
static uint32_t s_routing_diagnostic_owner;
static unsigned s_routing_diagnostic_depth;
static const char *s_routing_diagnostic_reason;
static unsigned s_routing_memblock_alloc_calls;
static unsigned s_routing_memblock_get_calls;
static unsigned s_routing_memblock_free_calls;
static int s_routing_memblock_live;
static int s_routing_reserve_failure;
static const char *s_routing_import_name;
static int s_routing_import_handled;
static unsigned s_routing_failure_log_calls;
static unsigned s_routing_failure_log_truncated;
static char s_routing_failure_logs[ROUTING_FAILURE_LOG_CAPACITY]
                                  [ROUTING_FAILURE_LOG_BYTES];

#define ROUTING_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "texel scratch routing oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* host_vita_heap.c alone is built with -Dmalloc=oracle_guest_malloc. */
void *oracle_guest_malloc(size_t size)
{
    ++s_routing_native_malloc_calls;
    s_routing_native_malloc_size = size;
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
    ++s_routing_diagnostic_calls;
    s_routing_diagnostic_size = failed_request;
    s_routing_diagnostic_owner = owner_return_rva;
    s_routing_diagnostic_depth = owner_chain_depth;
    s_routing_diagnostic_reason = failure_reason;
    (void)ledger_live_requested;
    (void)ledger_largest_live_request;
    (void)ledger_live_count;
    (void)ledger_requested_accounting_exact;
    (void)guest_stack_floor;
    (void)guest_stack_ceiling;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_routing_memblock_alloc_calls;
    if (s_routing_reserve_failure)
        return -0x511;
    if (s_routing_memblock_live || !name ||
        strcmp(name, "isaac_texel_scratch") != 0 ||
        type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES || option)
        return -0x512;
    s_routing_memblock_live = 1;
    return ROUTING_SCRATCH_UID;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_routing_memblock_get_calls;
    if (uid != ROUTING_SCRATCH_UID || !base ||
        !s_routing_memblock_live)
        return -0x513;
    *base = (void *)(uintptr_t)ROUTING_SCRATCH_BASE;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_routing_memblock_free_calls;
    if (uid != ROUTING_SCRATCH_UID || !s_routing_memblock_live)
        return -0x514;
    s_routing_memblock_live = 0;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char line[ROUTING_FAILURE_LOG_BYTES];
    size_t length;
    unsigned index;
    int written;

    va_start(arguments, format);
    written = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (written < 0 ||
        strncmp(line, ROUTING_FAILURE_LOG_PREFIX,
                sizeof ROUTING_FAILURE_LOG_PREFIX - 1U) != 0)
        return;

    index = s_routing_failure_log_calls++;
    if ((size_t)written >= sizeof line) {
        ++s_routing_failure_log_truncated;
        return;
    }
    if (index >= ROUTING_FAILURE_LOG_CAPACITY) {
        ++s_routing_failure_log_truncated;
        return;
    }
    length = (size_t)written;
    memcpy(s_routing_failure_logs[index], line, length + 1U);
}

static int routing_failure_log_matches(unsigned index, uint32_t request,
                                       uint32_t owner)
{
    char expected[ROUTING_FAILURE_LOG_BYTES];
    int written;

    if (index >= s_routing_failure_log_calls ||
        index >= ROUTING_FAILURE_LOG_CAPACITY)
        return 0;
    written = snprintf(
        expected, sizeof expected,
        "guest heap allocation failed: request=%u owner=0x%08x "
        "reason=native-malloc-failed ledger_capacity=64 ledger_count=0 "
        "ledger_tombstones=0 rehash_target_bytes=0 room_slab_live=0",
        (unsigned)request, (unsigned)owner);
    return written > 0 && (size_t)written < sizeof expected &&
           strcmp(s_routing_failure_logs[index], expected) == 0;
}

static uint32_t *routing_prepare_call(CPU *cpu, void *stack,
                                      uint32_t argument0,
                                      uint32_t argument1)
{
    uint32_t *frame = (uint32_t *)((unsigned char *)stack + 0x1000U);
    uint32_t floor = (uint32_t)(uintptr_t)stack;

    memset(cpu, 0, sizeof *cpu);
    memset(frame, 0xcc, 24U * sizeof *frame);
    frame[0] = 0xdec0de00U;
    frame[1] = argument0;
    frame[2] = argument1;
    cpu->esp = (uint32_t)(uintptr_t)frame;
    cpu->stack_owner = cpu;
    cpu->stack_floor = floor;
    cpu->stack_ceiling = floor + ROUTING_STACK_BYTES;
    cpu->stack_low_water = cpu->esp;
    return frame;
}

static void routing_heap_import(CPU *__restrict cpu)
{
    s_routing_import_handled =
        isaac_vita_heap_import(cpu, s_routing_import_name);
}

static int routing_run_import(CPU *cpu, const char *name,
                              uint32_t initial_esp)
{
    int stopped;

    s_routing_import_name = name;
    s_routing_import_handled = 0;
    stopped = guest_run_until_stop(cpu, routing_heap_import);
    if (stopped == GUEST_RUN_FAULT)
        return cpu->fault && cpu->esp == initial_esp;
    return stopped == GUEST_RUN_RETURNED && !cpu->fault &&
           s_routing_import_handled == 1 &&
           cpu->esp == initial_esp + 4U;
}

static int routing_texel_malloc(CPU *cpu, void *stack, uint32_t size,
                                uint32_t owner_return, int has_adapter,
                                uint32_t *result)
{
    uint32_t *import_frame = routing_prepare_call(cpu, stack, size, 0U);
    uint32_t *concrete_frame = import_frame + 8U;
    uint32_t *wrapper_frame = import_frame + 12U;
    uint32_t *adapter_frame = import_frame + 16U;
    uint32_t initial_esp = cpu->esp;

    import_frame[0] = KAGE_VITA_TEXEL_MALLOC_IAT_RETURN_RVA;
    concrete_frame[0] = (uint32_t)(uintptr_t)wrapper_frame;
    concrete_frame[1] = KAGE_VITA_TEXEL_WRAPPER_RETURN_RVA;
    wrapper_frame[0] = has_adapter
        ? (uint32_t)(uintptr_t)adapter_frame : 0xdec0de11U;
    wrapper_frame[1] = has_adapter
        ? KAGE_VITA_TEXEL_ADAPTER_RETURN_RVA : owner_return;
    adapter_frame[0] = 0xdec0de12U;
    adapter_frame[1] = owner_return;
    cpu->ebp = (uint32_t)(uintptr_t)concrete_frame;
    if (!routing_run_import(
            cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int routing_generic_malloc(CPU *cpu, void *stack, uint32_t size,
                                  uint32_t *result)
{
    uint32_t initial_esp;

    (void)routing_prepare_call(cpu, stack, size, 0U);
    initial_esp = cpu->esp;
    if (!routing_run_import(
            cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int routing_raw_arg2_malloc(CPU *cpu, void *stack, uint32_t size,
                                   uint32_t raw_arg2, uint32_t *result)
{
    uint32_t *frame = routing_prepare_call(cpu, stack, size, 0U);
    uint32_t initial_esp = cpu->esp;

    frame[3] = raw_arg2;
    if (!routing_run_import(
            cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int routing_operator_new_malloc(CPU *cpu, void *stack, uint32_t size,
                                       uint32_t owner_return,
                                       uint32_t *result)
{
    uint32_t *frame = routing_prepare_call(cpu, stack, size, 0U);
    uint32_t initial_esp = cpu->esp;

    frame[0] = ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA;
    frame[3] = owner_return;
    if (!routing_run_import(
            cpu, ISAAC_VITA_HEAP_MALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

static int routing_free(CPU *cpu, void *stack, uint32_t pointer)
{
    uint32_t initial_esp;

    (void)routing_prepare_call(cpu, stack, pointer, 0U);
    initial_esp = cpu->esp;
    return routing_run_import(
        cpu, ISAAC_VITA_HEAP_FREE_NAME, initial_esp);
}

static int routing_realloc(CPU *cpu, void *stack, uint32_t pointer,
                           uint32_t size, uint32_t *result)
{
    uint32_t initial_esp;

    (void)routing_prepare_call(cpu, stack, pointer, size);
    initial_esp = cpu->esp;
    if (!routing_run_import(
            cpu, ISAAC_VITA_HEAP_REALLOC_NAME, initial_esp))
        return 0;
    *result = cpu->eax;
    return 1;
}

int main(void)
{
    CPU cpu;
    void *stack;
    uint32_t result;

    stack = mmap(NULL, ROUTING_STACK_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    ROUTING_CHECK(stack != MAP_FAILED &&
                  (uintptr_t)stack <= UINT32_MAX &&
                  (uintptr_t)stack + ROUTING_STACK_BYTES <= UINT32_MAX);
    ROUTING_CHECK(!((uintptr_t)stack <
                        ROUTING_SCRATCH_BASE +
                            KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES &&
                    ROUTING_SCRATCH_BASE <
                        (uintptr_t)stack + ROUTING_STACK_BYTES));

#ifdef ISAAC_VITA_TEXEL_SCRATCH_8M
    /* The diagnostic limit is a routing decision, not truncation.  Both the
     * first byte over 8 MiB and the frozen maximum reach ordinary malloc with
     * their original sizes, without attempting a USER_RW reservation. */
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 1U &&
                  s_routing_native_malloc_size ==
                      KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U &&
                  s_routing_memblock_alloc_calls == 0U);
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 2U &&
                  s_routing_native_malloc_size ==
                      KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES &&
                  s_routing_memblock_alloc_calls == 0U &&
                  s_routing_diagnostic_calls == 2U);
    s_routing_native_malloc_calls = 0U;
    s_routing_native_malloc_size = 0U;
    s_routing_diagnostic_calls = 0U;
    s_routing_diagnostic_size = 0U;
    s_routing_diagnostic_owner = 0U;
    s_routing_diagnostic_depth = 0U;
    s_routing_diagnostic_reason = NULL;
    s_routing_failure_log_calls = 0U;
    s_routing_failure_log_truncated = 0U;
    memset(s_routing_failure_logs, 0, sizeof s_routing_failure_logs);
#endif

    /* A clean dedicated-block failure must not swallow the request.  The
     * actual heap owner calls ordinary malloc and then the exact-stack OOM
     * diagnostic, preserving both fallback and attribution. */
    s_routing_reserve_failure = 1;
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U);
    ROUTING_CHECK(s_routing_memblock_alloc_calls == 1U &&
                  s_routing_memblock_get_calls == 0U &&
                  s_routing_memblock_free_calls == 0U);
    ROUTING_CHECK(s_routing_native_malloc_calls == 1U &&
                  s_routing_native_malloc_size ==
                      KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST);
    ROUTING_CHECK(s_routing_diagnostic_calls == 1U &&
                  s_routing_diagnostic_size ==
                      KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST &&
                  s_routing_diagnostic_owner ==
                      KAGE_VITA_TEXEL_PNG_LOADER_RETURN &&
                  s_routing_diagnostic_depth == 3U &&
                  s_routing_diagnostic_reason &&
                  strcmp(s_routing_diagnostic_reason,
                         "native-malloc-failed") == 0);
    ROUTING_CHECK(s_routing_failure_log_calls == 1U &&
                  s_routing_failure_log_truncated == 0U &&
                  routing_failure_log_matches(
                      0U, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST, 0U));

    /* The next request reserves successfully and bypasses newlib. */
    s_routing_reserve_failure = 0;
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == ROUTING_SCRATCH_BASE);
    ROUTING_CHECK(s_routing_memblock_alloc_calls == 2U &&
                  s_routing_memblock_get_calls == 1U &&
                  s_routing_memblock_live);
    ROUTING_CHECK(s_routing_native_malloc_calls == 1U &&
                  s_routing_diagnostic_calls == 1U);

    /* The heap dispatcher must preserve scratch realloc ownership: an
     * in-capacity resize is in-place, oversize is a handled NULL retaining
     * the old lease, and neither is allowed to reach newlib. */
    ROUTING_CHECK(routing_realloc(
        &cpu, stack, ROUTING_SCRATCH_BASE, 1048576U, &result));
    ROUTING_CHECK(!cpu.fault && result == ROUTING_SCRATCH_BASE &&
                  s_routing_native_malloc_calls == 1U &&
                  s_routing_diagnostic_calls == 1U);
    ROUTING_CHECK(routing_realloc(
        &cpu, stack, ROUTING_SCRATCH_BASE,
        KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 1U &&
                  s_routing_diagnostic_calls == 1U);

    /* A second request while the singleton lease is live cannot alias it;
     * it follows the same ordinary-malloc/diagnostic fallback. */
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, 2097152U,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U);
    ROUTING_CHECK(s_routing_memblock_alloc_calls == 2U &&
                  s_routing_native_malloc_calls == 2U &&
                  s_routing_native_malloc_size == 2097152U &&
                  s_routing_diagnostic_calls == 2U);

    /* Other known texture owners remain outside this narrow allocator, while
     * a generic stack must not consume the texture diagnostic at all. */
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
        KAGE_VITA_TEXEL_LOADER_RETURN_0, 0, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 3U &&
                  s_routing_diagnostic_calls == 3U &&
                  s_routing_diagnostic_owner ==
                      KAGE_VITA_TEXEL_LOADER_RETURN_0);
    ROUTING_CHECK(routing_generic_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 4U &&
                  s_routing_diagnostic_calls == 3U);
    ROUTING_CHECK(s_routing_failure_log_calls == 4U &&
                  routing_failure_log_matches(
                      3U, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST, 0U));

    /* Raw malloc arg2 belongs to the separate ANM2 seam.  Even the PNG RVA
     * there cannot classify a texel allocation without the frozen EBP chain. */
    ROUTING_CHECK(routing_raw_arg2_malloc(
        &cpu, stack, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 5U &&
                  s_routing_diagnostic_calls == 3U);
    ROUTING_CHECK(s_routing_failure_log_calls == 5U &&
                  routing_failure_log_matches(
                      4U, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST, 0U));

    ROUTING_CHECK(routing_raw_arg2_malloc(
        &cpu, stack, 307200U, ROUTING_STREAM_QUEUE_RETURN_RVA, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 6U &&
                  s_routing_diagnostic_calls == 3U);
    ROUTING_CHECK(s_routing_failure_log_calls == 6U &&
                  routing_failure_log_matches(5U, 307200U, 0U));

    /* [ESP+12] is an owner only at the frozen common operator-new wrapper.
     * The same word on either generic stack above stays unattributed. */
    ROUTING_CHECK(routing_operator_new_malloc(
        &cpu, stack, 307200U, ROUTING_STREAM_QUEUE_RETURN_RVA, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 7U &&
                  s_routing_diagnostic_calls == 3U);
    ROUTING_CHECK(s_routing_failure_log_calls == 7U &&
                  s_routing_failure_log_truncated == 0U &&
                  routing_failure_log_matches(
                      6U, 307200U, ROUTING_STREAM_QUEUE_RETURN_RVA));

    ROUTING_CHECK(routing_realloc(
        &cpu, stack, ROUTING_SCRATCH_BASE, 0U, &result));
    ROUTING_CHECK(!cpu.fault && result == 0U &&
                  s_routing_native_malloc_calls == 7U &&
                  s_routing_diagnostic_calls == 3U);

    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, 1480192U,
        KAGE_VITA_TEXEL_LOADER_RETURN_4, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == ROUTING_SCRATCH_BASE &&
                  s_routing_native_malloc_calls == 7U);
    ROUTING_CHECK(routing_free(&cpu, stack, ROUTING_SCRATCH_BASE));
    ROUTING_CHECK(!cpu.fault);
    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, 2088960U,
        KAGE_VITA_TEXEL_LOADER_RETURN_5, 0, &result));
    ROUTING_CHECK(!cpu.fault && result == ROUTING_SCRATCH_BASE &&
                  s_routing_native_malloc_calls == 7U);
    ROUTING_CHECK(routing_free(&cpu, stack, ROUTING_SCRATCH_BASE));
    ROUTING_CHECK(!cpu.fault);

    ROUTING_CHECK(routing_texel_malloc(
        &cpu, stack, 4096U,
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 1, &result));
    ROUTING_CHECK(!cpu.fault && result == ROUTING_SCRATCH_BASE &&
                  s_routing_memblock_alloc_calls == 2U &&
                  s_routing_native_malloc_calls == 7U);

    ROUTING_CHECK(routing_free(
        &cpu, stack, ROUTING_SCRATCH_BASE));
    ROUTING_CHECK(!cpu.fault && s_routing_memblock_free_calls == 0U &&
                  s_routing_memblock_live);
    ROUTING_CHECK(routing_free(
        &cpu, stack, ROUTING_SCRATCH_BASE));
    ROUTING_CHECK(cpu.fault && strstr(cpu.fault, "double/stale free"));

    ROUTING_CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    ROUTING_CHECK(!s_routing_memblock_live &&
                  s_routing_memblock_free_calls == 1U);
    ROUTING_CHECK(munmap(stack, ROUTING_STACK_BYTES) == 0);
    puts("Vita texel scratch heap fallback/routing oracle: PASS");
    return 0;
}

#else

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"

#define ORACLE_STACK_FLOOR       0x90000000U
#define ORACLE_STACK_CEILING     0x90400000U
#define ORACLE_TEXEL_UID         ((SceUID)0x2345)
#define ORACLE_ANM2_UID          ((SceUID)0x3456)
#define ORACLE_ANM2_BYTES        0x002a2000U
#define ORACLE_LOG_CAPACITY      64U
#define ORACLE_LOG_BYTES         320U
#define ORACLE_CANARY_BYTE       0xa7U
#define ORACLE_BLOCK_BYTE        0x6dU

typedef struct oracle_texel_storage {
    unsigned char leading_canary[KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES];
    unsigned char block[KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES];
    unsigned char trailing_canary[KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES];
} oracle_texel_storage;

_Alignas(KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES)
static oracle_texel_storage s_texel_storage;
#define s_texel_block (s_texel_storage.block)
_Alignas(KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES)
static unsigned char s_anm2_block[ORACLE_ANM2_BYTES];

static int s_alloc_result;
static int s_get_result;
static int s_free_result;
static size_t s_base_bias;
static uintptr_t s_literal_base;
static int s_texel_live;
static int s_anm2_live;
static unsigned s_texel_alloc_calls;
static unsigned s_texel_get_calls;
static unsigned s_texel_free_calls;
static unsigned s_anm2_alloc_calls;
static unsigned s_anm2_get_calls;
static unsigned s_anm2_free_calls;
static SceKernelMemBlockType s_last_type;
static SceSize s_last_size;
static SceKernelAllocMemBlockOpt *s_last_option;
static const char *s_last_name;
static unsigned s_log_count;
static char s_logs[ORACLE_LOG_CAPACITY][ORACLE_LOG_BYTES];

static int s_inject_concurrency;
static int s_thread_result;
static int s_concurrent_result;
static isaac_vita_texel_scratch_decision s_concurrent_decision;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "texel scratch oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int bytes_are(const unsigned char *bytes, size_t size,
                     unsigned char expected)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != expected)
            return 0;
    }
    return 1;
}

static void reset_texel_canaries(void)
{
    memset(s_texel_storage.leading_canary, ORACLE_CANARY_BYTE,
           sizeof s_texel_storage.leading_canary);
    memset(s_texel_block, ORACLE_BLOCK_BYTE, sizeof s_texel_block);
    memset(s_texel_storage.trailing_canary, ORACLE_CANARY_BYTE,
           sizeof s_texel_storage.trailing_canary);
}

static int texel_canaries_are_intact(void)
{
    return bytes_are(s_texel_storage.leading_canary,
                     sizeof s_texel_storage.leading_canary,
                     ORACLE_CANARY_BYTE) &&
        bytes_are(s_texel_storage.trailing_canary,
                  sizeof s_texel_storage.trailing_canary,
                  ORACLE_CANARY_BYTE);
}

static void *concurrent_malloc(void *unused)
{
    (void)unused;
    s_concurrent_result = isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 8192U,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING,
        &s_concurrent_decision);
    return NULL;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    if (name && strcmp(name, "isaac_anm2_scratch") == 0) {
        ++s_anm2_alloc_calls;
        if (s_anm2_live || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
            size != ORACLE_ANM2_BYTES || option)
            return -0x701;
        s_anm2_live = 1;
        return ORACLE_ANM2_UID;
    }

    ++s_texel_alloc_calls;
    s_last_name = name;
    s_last_type = type;
    s_last_size = size;
    s_last_option = option;
    if (s_alloc_result < 0)
        return (SceUID)s_alloc_result;
    if (s_texel_live)
        return -0x702;
    s_texel_live = 1;

    /* The production lifecycle lock is held across this syscall.  Starting
     * and joining here makes the second request truly concurrent without a
     * timing-sensitive barrier in the oracle. */
    if (s_inject_concurrency) {
        pthread_t thread;

        s_inject_concurrency = 0;
        s_thread_result = pthread_create(
            &thread, NULL, concurrent_malloc, NULL);
        if (s_thread_result == 0)
            s_thread_result = pthread_join(thread, NULL);
    }
    return (SceUID)s_alloc_result;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == ORACLE_ANM2_UID) {
        ++s_anm2_get_calls;
        if (!base || !s_anm2_live)
            return -0x703;
        *base = s_anm2_block;
        return 0;
    }

    ++s_texel_get_calls;
    if (uid != (SceUID)s_alloc_result || !s_texel_live)
        return -0x704;
    if (s_get_result < 0) {
        if (base)
            *base = NULL;
        return s_get_result;
    }
    if (!base)
        return -0x705;
    *base = s_literal_base
        ? (void *)s_literal_base : s_texel_block + s_base_bias;
    return s_get_result;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == ORACLE_ANM2_UID) {
        ++s_anm2_free_calls;
        if (!s_anm2_live)
            return -0x706;
        s_anm2_live = 0;
        return 0;
    }

    ++s_texel_free_calls;
    if (uid != (SceUID)s_alloc_result || !s_texel_live)
        return -0x707;
    if (s_free_result >= 0)
        s_texel_live = 0;
    return s_free_result;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    if (s_log_count >= ORACLE_LOG_CAPACITY)
        return;
    va_start(arguments, format);
    (void)vsnprintf(s_logs[s_log_count], ORACLE_LOG_BYTES,
                    format, arguments);
    va_end(arguments);
    ++s_log_count;
}

static void reset_fake(void)
{
    s_alloc_result = (int)ORACLE_TEXEL_UID;
    s_get_result = 0;
    s_free_result = 0;
    s_base_bias = 0U;
    s_literal_base = 0U;
    s_texel_live = 0;
    s_anm2_live = 0;
    s_texel_alloc_calls = 0U;
    s_texel_get_calls = 0U;
    s_texel_free_calls = 0U;
    s_anm2_alloc_calls = 0U;
    s_anm2_get_calls = 0U;
    s_anm2_free_calls = 0U;
    s_last_type = (SceKernelMemBlockType)0;
    s_last_size = 0U;
    s_last_option = (SceKernelAllocMemBlockOpt *)(uintptr_t)1U;
    s_last_name = NULL;
    s_log_count = 0U;
    memset(s_logs, 0, sizeof s_logs);
    reset_texel_canaries();
    s_inject_concurrency = 0;
    s_thread_result = -1;
    s_concurrent_result = 99;
    memset(&s_concurrent_decision, 0xa5,
           sizeof s_concurrent_decision);
}

static int exact_malloc(size_t size,
                        isaac_vita_texel_scratch_decision *decision)
{
    return isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, size,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, decision);
}

static uintptr_t page_up(uintptr_t value)
{
    return (value + KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U) &
        ~((uintptr_t)KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U);
}

int main(void)
{
    isaac_vita_texel_scratch_decision decision;
    isaac_vita_texel_scratch_snapshot snapshot;
    SceUID anm2_uid;
    void *anm2_base = NULL;
    void *first;
    void *maximum;
    void *resized;
    unsigned i;
    int result;

    CHECK(KAGE_VITA_TEXEL_PNG_LOADER_RETURN ==
          KAGE_VITA_TEXEL_LOADER_RETURN_1);
    CHECK(KAGE_VITA_TEXEL_LOADER_RETURN_4 !=
          KAGE_VITA_TEXEL_LOADER_RETURN_5);
    CHECK(KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES == 0x00bd6000U);
    CHECK(KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES == 0x00801000U);
    CHECK(KAGE_VITA_TEXEL_OOM_REQUEST == 8388608U);
    CHECK((KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES &
           (KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U)) == 0U);
    CHECK(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES >=
          KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST);
#ifdef ISAAC_VITA_TEXEL_SCRATCH_8M
    CHECK(KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES == 0x00801000U);
    CHECK(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES == 8388608U);
#else
    CHECK(KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES == 0x00bd6000U);
    CHECK(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES == 0x00bd6000U);
#endif
    CHECK(((uintptr_t)s_texel_block &
           (KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U)) == 0U);
    CHECK(((uintptr_t)s_anm2_block &
           (KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U)) == 0U);

    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    reset_fake();

    /* Foreign owners and pointers stay on the ordinary guest-heap path.
     * Request-range fallback is also explicit and never reserves a block. */
    memset(&decision, 0xa5, sizeof decision);
    result = isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN - 1U, 4096U,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    CHECK(!decision.pointer && !decision.fault &&
          decision.fault_value == 0U && s_texel_alloc_calls == 0U);
    CHECK(exact_malloc(0U, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    CHECK(exact_malloc(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U,
                       &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
#ifdef ISAAC_VITA_TEXEL_SCRATCH_8M
    CHECK(exact_malloc(KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
#endif
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
#ifdef ISAAC_VITA_TEXEL_SCRATCH_8M
    CHECK(snapshot.oversize_fallback_count == 3U &&
#else
    CHECK(snapshot.oversize_fallback_count == 2U &&
#endif
          snapshot.base == 0U && !snapshot.live);
    CHECK(isaac_vita_texel_scratch_free(NULL, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    CHECK(isaac_vita_texel_scratch_free(s_anm2_block, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    CHECK(isaac_vita_texel_scratch_realloc(s_anm2_block, 16U,
                                           &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);

    /* Both frozen ProceduralImageBase sites fill, upload, and free their local
     * backing synchronously.  They may reuse the same one-live lease, but may
     * never alias an already-live request. */
    result = isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_LOADER_RETURN_4, 1480192U,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED && decision.pointer);
    first = decision.pointer;
    CHECK(isaac_vita_texel_scratch_malloc(
              KAGE_VITA_TEXEL_LOADER_RETURN_5, 2088960U,
              ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    CHECK(isaac_vita_texel_scratch_free(first, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    for (i = 0U; i < 3U; ++i) {
        result = isaac_vita_texel_scratch_malloc(
            KAGE_VITA_TEXEL_LOADER_RETURN_4, 1480192U,
            ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision);
        CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED &&
              decision.pointer == first);
        CHECK(isaac_vita_texel_scratch_free(first, &decision) ==
              ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    }
    result = isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_LOADER_RETURN_5, 2088960U,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED &&
          decision.pointer == first);
    CHECK(isaac_vita_texel_scratch_free(first, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.acquired_count == 5U && snapshot.freed_count == 5U &&
          snapshot.busy_fallback_count == 1U &&
          snapshot.high_water == 2088960U && !snapshot.live);
    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    reset_fake();

    result = isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 4096U,
        ORACLE_STACK_FLOOR, ORACLE_STACK_FLOOR, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "stack range") &&
          s_texel_alloc_calls == 0U);
    result = exact_malloc(4096U, NULL);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          s_texel_alloc_calls == 0U);
    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    reset_fake();

    /* Native allocation failure falls through to the ordinary guest heap.
     * Get/base/range failures roll their complete transactions back and do
     * the same, so each exact loader request remains retryable. */
    s_alloc_result = -0x111;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer && !decision.fault);
    CHECK(s_texel_alloc_calls == 1U && s_texel_get_calls == 0U &&
          s_texel_free_calls == 0U && !s_texel_live);
    CHECK(s_last_name && strcmp(s_last_name, "isaac_texel_scratch") == 0);
    CHECK(s_last_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW &&
          s_last_size == KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES &&
          s_last_option == NULL);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == 0U && snapshot.uid == -1 &&
          !snapshot.poisoned && !snapshot.live);

    s_alloc_result = (int)ORACLE_TEXEL_UID;
    s_get_result = -0x222;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer);
    CHECK(!s_texel_live && s_texel_free_calls == 1U);
    CHECK(strstr(s_logs[s_log_count - 1U], "status=get-base-failed"));

    s_get_result = 0;
    s_base_bias = 1U;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer);
    CHECK(!s_texel_live && s_texel_free_calls == 2U);
    CHECK(strstr(s_logs[s_log_count - 1U], "status=invalid-base"));

    s_base_bias = 0U;
    s_literal_base = page_up(KAGE_VITA_TEXEL_LINK_END);
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer);
    CHECK(!s_texel_live && s_texel_free_calls == 3U);
    CHECK(strstr(s_logs[s_log_count - 1U], "status=range-overlap"));

    s_literal_base = KAGE_VITA_TEXEL_GUEST_IMAGE_BASE;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer);
    CHECK(!s_texel_live && s_texel_free_calls == 4U);
    CHECK(strstr(s_logs[s_log_count - 1U], "status=range-overlap"));

    s_literal_base = ORACLE_STACK_FLOOR;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer);
    CHECK(!s_texel_live && s_texel_free_calls == 5U);
    CHECK(strstr(s_logs[s_log_count - 1U], "status=range-overlap"));

    /* Failure to free an invalid reservation is the one poison edge: the
     * leaked UID remains owned and no replacement allocation is attempted. */
    s_literal_base = 0U;
    s_get_result = -0x333;
    s_free_result = -0x444;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "rollback failed"));
    CHECK(s_texel_live);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.poisoned == 1U &&
          snapshot.uid == ORACLE_TEXEL_UID && snapshot.base == 0U);
    {
        unsigned allocations_before = s_texel_alloc_calls;
        result = exact_malloc(4096U, &decision);
        CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
              decision.fault && strstr(decision.fault, "poisoned") &&
              s_texel_alloc_calls == allocations_before);
    }
    s_free_result = 0;
    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    CHECK(!s_texel_live);
    reset_fake();

    /* Model an already-live ANM2 USER_RW block.  The fake kernel assigns the
     * texel request a distinct UID and non-overlapping range, just as the real
     * memblock allocator must; releasing one cannot release the other. */
    anm2_uid = sceKernelAllocMemBlock(
        "isaac_anm2_scratch", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        ORACLE_ANM2_BYTES, NULL);
    CHECK(anm2_uid == ORACLE_ANM2_UID && s_anm2_live);
    CHECK(sceKernelGetMemBlockBase(anm2_uid, &anm2_base) == 0 && anm2_base);

    /* A literal low oracle base lets the later-stack range check exercise
     * both the safe and overlapping cases despite the 64-bit host address
     * space used by this executable. */
    s_literal_base = 0x91000000U;
    s_inject_concurrency = 1;
    result = exact_malloc(4096U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED && decision.pointer);
    first = decision.pointer;
    CHECK(s_thread_result == 0);
    CHECK(s_concurrent_result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    CHECK(!s_concurrent_decision.pointer &&
          !s_concurrent_decision.fault &&
          s_concurrent_decision.fault_value == 0U);
    CHECK(first != anm2_base);
    CHECK((uintptr_t)first + KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES <=
              (uintptr_t)anm2_base ||
          (uintptr_t)anm2_base + ORACLE_ANM2_BYTES <= (uintptr_t)first);
    CHECK(s_texel_alloc_calls == 1U && s_texel_get_calls == 1U &&
          s_texel_free_calls == 0U && s_texel_live && s_anm2_live);
    CHECK(s_last_name && strcmp(s_last_name, "isaac_texel_scratch") == 0);

    /* A live lease never aliases: both a real concurrent request and a
     * sequential reentrant request return NOT_HANDLED with a cleared pointer. */
    memset(&decision, 0xa5, sizeof decision);
    result = exact_malloc(8192U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer && !decision.fault &&
          decision.fault_value == 0U);
    result = exact_malloc(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U,
                          &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer && !decision.fault);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.live == 1U && snapshot.live_size == 4096U &&
          snapshot.busy_fallback_count == 1U &&
          snapshot.oversize_fallback_count == 1U);

    CHECK(isaac_vita_texel_scratch_free(anm2_base, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED);
    result = isaac_vita_texel_scratch_free(
        (unsigned char *)first + 16U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "interior"));
    result = isaac_vita_texel_scratch_realloc(
        (unsigned char *)first + 16U, 32U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "interior"));
    CHECK(isaac_vita_texel_scratch_free(first, NULL) ==
          ISAAC_VITA_TEXEL_SCRATCH_REJECTED);
    CHECK(isaac_vita_texel_scratch_realloc(first, 2048U, NULL) ==
          ISAAC_VITA_TEXEL_SCRATCH_REJECTED);
    CHECK(isaac_vita_texel_scratch_free(first, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(s_texel_free_calls == 0U && s_texel_live);
    result = isaac_vita_texel_scratch_free(first, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "double/stale"));

    /* The maximum measured PNG request reuses the retained base exactly and
     * establishes the lifetime high-water mark without another syscall. */
    CHECK(exact_malloc(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES,
                       &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    maximum = decision.pointer;
    CHECK(maximum == first && s_texel_alloc_calls == 1U);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.live_size == KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES &&
          snapshot.high_water ==
              KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES);
    CHECK(isaac_vita_texel_scratch_free(maximum, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);

    /* realloc is exact-base only: in-range sizes are in-place, oversize is a
     * standard handled failure retaining the old lease, and zero frees it. */
    CHECK(exact_malloc(8192U, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    resized = decision.pointer;
    CHECK(resized == first);
    CHECK(isaac_vita_texel_scratch_realloc(
              resized, 4096U, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(decision.pointer == resized);
    CHECK(isaac_vita_texel_scratch_realloc(
              resized, KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
              &decision) == ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(decision.pointer == resized);
    CHECK(isaac_vita_texel_scratch_realloc(
              resized, KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U,
              &decision) == ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(!decision.pointer && !decision.fault);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.live == 1U &&
          snapshot.live_size == KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST);
    CHECK(isaac_vita_texel_scratch_realloc(resized, 0U, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(!decision.pointer && !decision.fault);
    result = isaac_vita_texel_scratch_realloc(resized, 16U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "stale"));

    result = isaac_vita_texel_scratch_malloc(
        KAGE_VITA_TEXEL_PNG_LOADER_RETURN, 4096U,
        0x91800000U, 0x91c00000U, &decision);
    CHECK(result == ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED &&
          !decision.pointer && !decision.fault);

    CHECK(exact_malloc(4096U, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(decision.pointer == first);
    CHECK(isaac_vita_texel_scratch_free(first, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == 0x91000000U &&
          snapshot.uid == ORACLE_TEXEL_UID && !snapshot.poisoned &&
          !snapshot.live && snapshot.live_size == 0U &&
          snapshot.high_water ==
              KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES &&
          snapshot.acquired_count == 4U && snapshot.freed_count == 4U &&
          snapshot.busy_fallback_count == 1U &&
          snapshot.oversize_fallback_count == 1U);

    /* Production retains the texel block.  Test-only reset releases that one
     * UID and proves the independently live ANM2-like block is untouched. */
    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);
    CHECK(!s_texel_live && s_texel_free_calls == 1U && s_anm2_live &&
          s_anm2_free_calls == 0U);
    CHECK(sceKernelFreeMemBlock(anm2_uid) == 0);
    CHECK(!s_anm2_live && s_anm2_free_calls == 1U);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == 0U && snapshot.uid == -1 &&
          !snapshot.poisoned && !snapshot.live &&
          snapshot.acquired_count == 0U && snapshot.freed_count == 0U);

    /* Exercise the actual oracle storage after the low-address overlap
     * model.  Touching both ends of the admitted request must leave the
     * surrounding canaries and the 8 MiB mode's padding page unchanged.
     * Oversize realloc remains a handled NULL and retains the old lease. */
    reset_fake();
    CHECK(exact_malloc(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES,
                       &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    maximum = decision.pointer;
    CHECK(maximum == s_texel_block);
    ((unsigned char *)maximum)[0] = 0x11U;
    ((unsigned char *)maximum)
        [KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES - 1U] = 0x22U;
    CHECK(texel_canaries_are_intact());
    CHECK(bytes_are(
        s_texel_block + KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES,
        KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES -
            KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES,
        ORACLE_BLOCK_BYTE));
    CHECK(isaac_vita_texel_scratch_realloc(
              maximum, KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES + 1U,
              &decision) == ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(!decision.pointer && !decision.fault);
    CHECK(isaac_vita_texel_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.live && snapshot.live_size ==
              KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES);
    CHECK(isaac_vita_texel_scratch_realloc(
              maximum, 4096U, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(decision.pointer == maximum);
    CHECK(texel_canaries_are_intact());
    CHECK(isaac_vita_texel_scratch_free(maximum, &decision) ==
          ISAAC_VITA_TEXEL_SCRATCH_HANDLED);
    CHECK(texel_canaries_are_intact());
    CHECK(isaac_vita_texel_scratch_oracle_reset() == 0);

    puts("Vita texel scratch lifecycle/concurrency/canary oracle: PASS");
    return 0;
}

#endif
