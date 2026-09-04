/* Integrated host oracle for the production heap router.  The router, USER_RW
 * ledger backend and SceClibMspace backend are real; only kernel/libc entry
 * points are deterministic fakes. */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include "host_vita_heap.h"
#include "host_vita_heap_ledger_memblock.h"
#include "host_vita_room_entry_slab.h"
#include "host_vita_room_entry_external.h"

#define ORACLE_MEMBLOCK_SLOTS 16U
#define ORACLE_MSPACE_ALLOCS 128U
#define ORACLE_LOG_CAPACITY 128U
#define ORACLE_LOG_BYTES 384U
#define ORACLE_ROOM_RESET_SLOTS \
    (ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE + 1U)

#ifdef _WIN32
char _end;
unsigned int _newlib_heap_size_user;
#endif

/* This executable oracle is deliberately a 64-bit sanitizer build, so its
 * uintptr_t/size_t ledger entry is 16 bytes.  Assert its real allocation
 * event here.  The ARM section of test_heap_overflow_router.sh separately
 * requires an 8-byte entry plus exact 0x80000 phase and 0x10000 CURRENT
 * sidecars.  The production __vita__ initializer repeats that combined-layout
 * check before rehash. */
#if UINTPTR_MAX != UINT64_MAX
#error Integrated host overflow router oracle requires a 64-bit host
#endif
#define ORACLE_HOST_LEDGER_USABLE_BYTES  0x00890000U
#define ORACLE_HOST_LEDGER_REQUEST_BYTES 0x00891000U

typedef struct oracle_memblock {
    SceUID uid;
    unsigned char *base;
    size_t size;
    unsigned kind;
    int live;
} oracle_memblock;

typedef struct oracle_mspace_allocation {
    unsigned char *pointer;
    size_t size;
    int live;
} oracle_mspace_allocation;

typedef struct oracle_mspace {
    unsigned char *base;
    size_t size;
    size_t bump;
    oracle_mspace_allocation allocations[ORACLE_MSPACE_ALLOCS];
    int live;
} oracle_mspace;

static oracle_memblock s_memblocks[ORACLE_MEMBLOCK_SLOTS];
static oracle_mspace s_mspace;
static SceUID s_next_uid = 0x400;
static unsigned s_alloc_kinds[32];
static size_t s_alloc_sizes[32];
static size_t s_alloc_event_count;
static unsigned s_fail_overflow_memblock;
static unsigned s_fail_overflow_free;
static unsigned s_fail_ledger_free;
static unsigned s_fail_native_malloc;
static unsigned s_fail_native_calloc;
static unsigned s_fail_native_realloc;
static unsigned s_force_native_realloc_same;
static unsigned s_fail_mspace_malloc;
static unsigned s_fail_mspace_calloc;
static unsigned s_fail_mspace_realloc;
static unsigned s_misalign_mspace_malloc;
static unsigned s_misalign_mspace_realloc;
static unsigned s_out_of_range_mspace_realloc;
static unsigned s_use_static_canary_malloc;
static size_t s_native_malloc_calls;
static size_t s_native_calloc_calls;
static size_t s_native_realloc_calls;
static size_t s_native_free_calls;
static size_t s_mspace_malloc_calls;
static size_t s_mspace_calloc_calls;
static size_t s_mspace_realloc_calls;
static size_t s_mspace_free_calls;
static size_t s_bad_api_calls;
static uint32_t s_import_frame[4];
static char s_logs[ORACLE_LOG_CAPACITY][ORACLE_LOG_BYTES];
static size_t s_log_count;
static size_t s_log_truncations;
static size_t s_log_under_lock;
static size_t s_log_dropped;
static size_t s_raw_snapshot_calls;
static size_t s_external_alloc_calls;
static size_t s_external_get_base_calls;
static size_t s_external_free_calls;
static size_t s_overflow_free_calls;
static size_t s_ledger_free_calls;
static unsigned s_corrupt_room_raw_snapshot_match;
static _Alignas(16) unsigned char s_out_of_range_realloc_storage[512];
static _Alignas(ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES)
unsigned char s_oracle_ledger_block[ORACLE_HOST_LEDGER_REQUEST_BYTES];
static _Alignas(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES)
unsigned char
s_oracle_overflow_block[ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES];
static _Alignas(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES)
unsigned char s_oracle_room_external_blocks[
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS][0x00110000U];
static _Alignas(16) unsigned char s_static_canary_allocation[64];
static int s_static_canary_live;
static void *s_last_native_malloc_result;
static void *s_room_reset_pointers[ORACLE_ROOM_RESET_SLOTS];

enum {
    ORACLE_ALLOC_LEDGER = 1,
    ORACLE_ALLOC_OVERFLOW = 2,
    ORACLE_ALLOC_ROOM_EXTERNAL = 3
};

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void isaac_vita_log(const char *format, ...)
{
    char scratch[ORACLE_LOG_BYTES];
    char *destination = scratch;
    va_list arguments;
    int length;

    if (isaac_vita_guest_heap_test_lock_held())
        ++s_log_under_lock;
    if (s_log_count < ORACLE_LOG_CAPACITY)
        destination = s_logs[s_log_count];
    else
        ++s_log_dropped;
    va_start(arguments, format);
    length = vsnprintf(destination, ORACLE_LOG_BYTES, format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= ORACLE_LOG_BYTES)
        ++s_log_truncations;
    ++s_log_count;
}

static size_t oracle_log_edge_count(size_t begin, const char *edge)
{
    char marker[48];
    size_t count = 0U;
    size_t index;

    if (snprintf(marker, sizeof marker, "heapovf: e=%s ", edge) < 0)
        return SIZE_MAX;
    for (index = begin;
         index < s_log_count && index < ORACLE_LOG_CAPACITY;
         ++index)
        if (strstr(s_logs[index], marker))
            ++count;
    return count;
}

static const char *oracle_log_edge_line(size_t begin, const char *edge)
{
    char marker[48];
    size_t index;

    if (snprintf(marker, sizeof marker, "heapovf: e=%s ", edge) < 0)
        return NULL;
    for (index = begin;
         index < s_log_count && index < ORACLE_LOG_CAPACITY;
         ++index)
        if (strstr(s_logs[index], marker))
            return s_logs[index];
    return NULL;
}

int __wrap_isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot_out)
{
    int result;

    ++s_raw_snapshot_calls;
#ifdef ISAAC_VITA_HEAP_ORACLE_NO_LINKER_WRAP
    result = isaac_vita_heap_overflow_mspace_snapshot_get(snapshot_out);
#else
    extern int __real_isaac_vita_heap_overflow_mspace_snapshot_get(
        isaac_vita_heap_overflow_mspace_snapshot *);
    result = __real_isaac_vita_heap_overflow_mspace_snapshot_get(snapshot_out);
#endif
    /* Corrupt only a snapshot that already describes the first retained raw
     * page.  The countdown lets page publication validate normally, then
     * injects a cold-only internal accounting mismatch in the event claim. */
    if (result && snapshot_out && s_corrupt_room_raw_snapshot_match &&
        snapshot_out->internal_live_count == 1U &&
        snapshot_out->internal_requested_bytes ==
            ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES &&
        --s_corrupt_room_raw_snapshot_match == 0U)
        ++snapshot_out->internal_live_count;
    return result;
}

static void oracle_prepare_heap_import(CPU *cpu,
                                       uint32_t argument0,
                                       uint32_t argument1)
{
    memset(cpu, 0, sizeof *cpu);
    memset(s_import_frame, 0, sizeof s_import_frame);
    s_import_frame[0] = UINT32_C(0x0badc0de);
    s_import_frame[1] = argument0;
    s_import_frame[2] = argument1;
    cpu->esp = (uint32_t)(uintptr_t)s_import_frame;
}

static void oracle_bind_import_stack(CPU *cpu)
{
    uint32_t floor = (uint32_t)(uintptr_t)s_import_frame;

    cpu->esp = floor;
    cpu->stack_floor = floor;
    cpu->stack_ceiling = floor + (uint32_t)sizeof s_import_frame;
    cpu->stack_low_water = floor;
    cpu->stack_owner = cpu;
}

static void oracle_prepare_room_malloc_import(CPU *cpu)
{
    oracle_prepare_heap_import(
        cpu, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, 0U);
    s_import_frame[0] = ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA;
    s_import_frame[3] = ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA;
    oracle_bind_import_stack(cpu);
}

static void oracle_prepare_bound_heap_import(
    CPU *cpu, uint32_t return_rva, uint32_t argument0, uint32_t argument1)
{
    oracle_prepare_heap_import(cpu, argument0, argument1);
    s_import_frame[0] = return_rva;
    oracle_bind_import_stack(cpu);
}

static oracle_memblock *oracle_memblock_find(SceUID uid)
{
    size_t index;

    for (index = 0U; index < ORACLE_MEMBLOCK_SLOTS; ++index)
        if (s_memblocks[index].live && s_memblocks[index].uid == uid)
            return &s_memblocks[index];
    return NULL;
}

static size_t oracle_live_memblocks(void)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < ORACLE_MEMBLOCK_SLOTS; ++index)
        if (s_memblocks[index].live)
            ++count;
    return count;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    oracle_memblock *slot = NULL;
    unsigned kind;
    size_t index;
    void *base;

    if (!name || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || option || !size) {
        ++s_bad_api_calls;
        return -0x101;
    }
    if (strcmp(name, "isaac_heap_ledger") == 0)
        kind = ORACLE_ALLOC_LEDGER;
    else if (strcmp(name, "isaac_heap_overflow") == 0)
        kind = ORACLE_ALLOC_OVERFLOW;
    else if (strcmp(name, "isaac_room_entries") == 0)
        kind = ORACLE_ALLOC_ROOM_EXTERNAL;
    else {
        ++s_bad_api_calls;
        return -0x102;
    }
    if (s_alloc_event_count < sizeof s_alloc_kinds / sizeof s_alloc_kinds[0]) {
        s_alloc_kinds[s_alloc_event_count] = kind;
        s_alloc_sizes[s_alloc_event_count] = size;
    }
    ++s_alloc_event_count;
    if (kind == ORACLE_ALLOC_OVERFLOW && s_fail_overflow_memblock) {
        --s_fail_overflow_memblock;
        return -0x103;
    }
    for (index = 0U; index < ORACLE_MEMBLOCK_SLOTS; ++index) {
        if (!s_memblocks[index].live) {
            slot = &s_memblocks[index];
            break;
        }
    }
    if (!slot)
        return -0x104;
    if (kind == ORACLE_ALLOC_LEDGER &&
        size == sizeof s_oracle_ledger_block)
        base = s_oracle_ledger_block;
    else if (kind == ORACLE_ALLOC_OVERFLOW &&
             size == sizeof s_oracle_overflow_block)
        base = s_oracle_overflow_block;
    else if (kind == ORACLE_ALLOC_ROOM_EXTERNAL &&
             size == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES) {
        base = NULL;
        for (index = 0U;
             index < ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS;
             ++index) {
            size_t live_index;
            int owned = 0;

            for (live_index = 0U;
                 live_index < ORACLE_MEMBLOCK_SLOTS; ++live_index) {
                if (s_memblocks[live_index].live &&
                    s_memblocks[live_index].base ==
                        s_oracle_room_external_blocks[index]) {
                    owned = 1;
                    break;
                }
            }
            if (!owned) {
                base = s_oracle_room_external_blocks[index];
                break;
            }
        }
        if (!base)
            return -0x107;
        ++s_external_alloc_calls;
    }
    else {
        ++s_bad_api_calls;
        return -0x105;
    }
    for (index = 0U; index < ORACLE_MEMBLOCK_SLOTS; ++index) {
        if (s_memblocks[index].live && s_memblocks[index].base == base) {
            ++s_bad_api_calls;
            return -0x106;
        }
    }
    memset(base, 0xa5, size);
    slot->uid = s_next_uid++;
    slot->base = (unsigned char *)base;
    slot->size = size;
    slot->kind = kind;
    slot->live = 1;
    return slot->uid;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    oracle_memblock *slot = oracle_memblock_find(uid);

    if (!slot || !base) {
        ++s_bad_api_calls;
        return -0x201;
    }
    if (slot->kind == ORACLE_ALLOC_ROOM_EXTERNAL)
        ++s_external_get_base_calls;
    *base = slot->base;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    oracle_memblock *slot = oracle_memblock_find(uid);

    if (!slot) {
        ++s_bad_api_calls;
        return -0x301;
    }
    if (slot->kind == ORACLE_ALLOC_OVERFLOW) {
        ++s_overflow_free_calls;
        if (s_fail_overflow_free) {
            --s_fail_overflow_free;
            return -0x302;
        }
    }
    if (slot->kind == ORACLE_ALLOC_LEDGER) {
        ++s_ledger_free_calls;
        if (s_fail_ledger_free) {
            --s_fail_ledger_free;
            return -0x303;
        }
    }
    if (slot->kind == ORACLE_ALLOC_ROOM_EXTERNAL)
        ++s_external_free_calls;
    memset(slot, 0, sizeof *slot);
    return 0;
}

static oracle_mspace_allocation *oracle_mspace_find(void *pointer)
{
    size_t index;

    for (index = 0U; index < ORACLE_MSPACE_ALLOCS; ++index)
        if (s_mspace.allocations[index].live &&
            s_mspace.allocations[index].pointer == pointer)
            return &s_mspace.allocations[index];
    return NULL;
}

static size_t oracle_mspace_live_count(void)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < ORACLE_MSPACE_ALLOCS; ++index)
        if (s_mspace.allocations[index].live)
            ++count;
    return count;
}

static void *oracle_mspace_allocate(size_t size, int zero, int misalign)
{
    oracle_mspace_allocation *slot = NULL;
    size_t offset;
    size_t index;
    unsigned char *pointer;

    if (!s_mspace.live || !size)
        return NULL;
    for (index = 0U; index < ORACLE_MSPACE_ALLOCS; ++index) {
        if (!s_mspace.allocations[index].live) {
            slot = &s_mspace.allocations[index];
            break;
        }
    }
    if (!slot)
        return NULL;
    offset = (s_mspace.bump + 15U) & ~(size_t)15U;
    if (offset > s_mspace.size ||
        size > s_mspace.size - offset - (misalign ? 1U : 0U))
        return NULL;
    pointer = s_mspace.base + offset + (misalign ? 1U : 0U);
    s_mspace.bump = offset + size + (misalign ? 1U : 0U);
    slot->pointer = pointer;
    slot->size = size;
    slot->live = 1;
    memset(pointer, zero ? 0 : 0x6d, size);
    return pointer;
}

SceClibMspace sceClibMspaceCreate(void *memblock, SceSize size)
{
    if (!memblock || size != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES ||
        s_mspace.live) {
        ++s_bad_api_calls;
        return NULL;
    }
    memset(&s_mspace, 0, sizeof s_mspace);
    s_mspace.base = (unsigned char *)memblock;
    s_mspace.size = size;
    s_mspace.bump = 0x1000U;
    s_mspace.live = 1;
    return &s_mspace;
}

void sceClibMspaceDestroy(SceClibMspace mspace)
{
    if (mspace != &s_mspace || !s_mspace.live ||
        oracle_mspace_live_count()) {
        ++s_bad_api_calls;
        return;
    }
    memset(&s_mspace, 0, sizeof s_mspace);
}

void *sceClibMspaceMalloc(SceClibMspace mspace, SceSize size)
{
    int misalign;

    ++s_mspace_malloc_calls;
    if (mspace != &s_mspace || !s_mspace.live) {
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_mspace_malloc) {
        --s_fail_mspace_malloc;
        return NULL;
    }
    misalign = s_misalign_mspace_malloc != 0U;
    if (misalign)
        --s_misalign_mspace_malloc;
    return oracle_mspace_allocate(size, 0, misalign);
}

void *sceClibMspaceCalloc(SceClibMspace mspace, SceSize count, SceSize size)
{
    size_t total;

    ++s_mspace_calloc_calls;
    if (mspace != &s_mspace || !s_mspace.live ||
        __builtin_mul_overflow((size_t)count, (size_t)size, &total)) {
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_mspace_calloc) {
        --s_fail_mspace_calloc;
        return NULL;
    }
    return oracle_mspace_allocate(total, 1, 0);
}

void *sceClibMspaceRealloc(SceClibMspace mspace, void *pointer, SceSize size)
{
    oracle_mspace_allocation *old;
    void *replacement;
    size_t old_size;
    int misalign;

    ++s_mspace_realloc_calls;
    old = oracle_mspace_find(pointer);
    if (mspace != &s_mspace || !s_mspace.live || !old || !size) {
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_mspace_realloc) {
        --s_fail_mspace_realloc;
        return NULL;
    }
    old_size = old->size;
    if (s_out_of_range_mspace_realloc) {
        --s_out_of_range_mspace_realloc;
        if (size > sizeof s_out_of_range_realloc_storage)
            return NULL;
        memcpy(s_out_of_range_realloc_storage, pointer,
               old_size < size ? old_size : size);
        old->live = 0;
        return s_out_of_range_realloc_storage;
    }
    misalign = s_misalign_mspace_realloc != 0U;
    if (misalign)
        --s_misalign_mspace_realloc;
    replacement = oracle_mspace_allocate(size, 0, misalign);
    if (!replacement)
        return NULL;
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    old->live = 0;
    return replacement;
}

void sceClibMspaceFree(SceClibMspace mspace, void *pointer)
{
    oracle_mspace_allocation *allocation = oracle_mspace_find(pointer);

    ++s_mspace_free_calls;
    if (mspace != &s_mspace || !s_mspace.live || !allocation) {
        ++s_bad_api_calls;
        return;
    }
    allocation->live = 0;
}

void *oracle_native_malloc(size_t size)
{
    void *result;

    ++s_native_malloc_calls;
    if (s_fail_native_malloc) {
        --s_fail_native_malloc;
        return NULL;
    }
    if (s_use_static_canary_malloc) {
        --s_use_static_canary_malloc;
        if (s_static_canary_live || size > 16U) {
            ++s_bad_api_calls;
            return NULL;
        }
        memset(s_static_canary_allocation, 0xcc,
               sizeof s_static_canary_allocation);
        s_static_canary_live = 1;
        result = s_static_canary_allocation;
        s_last_native_malloc_result = result;
        return result;
    }
    result = malloc(size);
    s_last_native_malloc_result = result;
    return result;
}

void *oracle_native_calloc(size_t count, size_t size)
{
    ++s_native_calloc_calls;
    if (s_fail_native_calloc) {
        --s_fail_native_calloc;
        return NULL;
    }
    return calloc(count, size);
}

void *oracle_native_realloc(void *pointer, size_t size)
{
    ++s_native_realloc_calls;
    if (s_force_native_realloc_same) {
        --s_force_native_realloc_same;
        if (pointer && size == 1U)
            return pointer;
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_native_realloc) {
        --s_fail_native_realloc;
        return NULL;
    }
    return realloc(pointer, size);
}

void oracle_native_free(void *pointer)
{
    ++s_native_free_calls;
    if (pointer == s_static_canary_allocation) {
        if (!s_static_canary_live) {
            ++s_bad_api_calls;
            return;
        }
        s_static_canary_live = 0;
        return;
    }
    free(pointer);
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind;
    (void)address;
    (void)size;
    guest_fault(c, pc, "overflow router oracle stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(
        c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
}

static void oracle_clear_injections(void)
{
    s_fail_overflow_memblock = 0U;
    s_fail_overflow_free = 0U;
    s_fail_ledger_free = 0U;
    s_fail_native_malloc = 0U;
    s_fail_native_calloc = 0U;
    s_fail_native_realloc = 0U;
    s_force_native_realloc_same = 0U;
    s_fail_mspace_malloc = 0U;
    s_fail_mspace_calloc = 0U;
    s_fail_mspace_realloc = 0U;
    s_misalign_mspace_malloc = 0U;
    s_misalign_mspace_realloc = 0U;
    s_out_of_range_mspace_realloc = 0U;
    s_use_static_canary_malloc = 0U;
    s_corrupt_room_raw_snapshot_match = 0U;
}

static int oracle_reset(void)
{
    oracle_clear_injections();
    if (!isaac_vita_guest_heap_test_storage_reset())
        return 0;
    return oracle_live_memblocks() == 0U && !s_mspace.live &&
        !s_static_canary_live;
}

static int oracle_init(void)
{
    return isaac_vita_guest_heap_test_init(
        0x82ad1560U, 0x05100000U, 0x85000000U, 0x85400000U);
}

static int oracle_telemetry_invariants(
    const isaac_vita_guest_heap_telemetry_snapshot *snapshot)
{
    if (!snapshot || !snapshot->accounting_valid ||
        snapshot->counter_saturated ||
        snapshot->pool_allocations < snapshot->pool_frees ||
        snapshot->pool_allocations - snapshot->pool_frees !=
            snapshot->owned_live_count ||
        snapshot->peak_live_count < snapshot->owned_live_count ||
        snapshot->peak_requested_bytes <
            snapshot->owned_requested_bytes ||
        snapshot->native_to_pool > snapshot->pool_allocations ||
        snapshot->pool_to_native > snapshot->pool_frees ||
        snapshot->consumed_terminal >
            snapshot->pool_frees - snapshot->pool_to_native ||
        snapshot->owned_live_count >
            UINT32_MAX - snapshot->raw_stranded_count ||
        snapshot->owned_live_count + snapshot->raw_stranded_count >
            UINT32_MAX - snapshot->raw_internal_live_count ||
        snapshot->raw_live_count !=
            snapshot->owned_live_count + snapshot->raw_stranded_count +
                snapshot->raw_internal_live_count)
        return 0;
    return 1;
}

static void oracle_force_process_cleanup(void)
{
    size_t index;

    memset(&s_mspace, 0, sizeof s_mspace);
    for (index = 0U; index < ORACLE_MEMBLOCK_SLOTS; ++index) {
        if (!s_memblocks[index].live)
            continue;
        memset(&s_memblocks[index], 0, sizeof s_memblocks[index]);
    }
    s_static_canary_live = 0;
}

static int test_ranges_and_setup(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;
    isaac_vita_guest_heap_test_result result;
    isaac_vita_guest_heap_test_storage_state storage;
    const char *reason = NULL;
    size_t request = 0U;
    size_t native_calls;

    CHECK(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY == 524288U);
    CHECK(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR == 3U);
    CHECK(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR == 4U);
    CHECK(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT == 393216U);
    CHECK(ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_BYTES == 8U);
    CHECK(ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_TABLE_BYTES == 0x00400000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_FLOOR_PHASE_BYTES == 0x00080000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES == 0x00010000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_LEDGER_USABLE_BYTES == 0x00490000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_LEDGER_REQUEST_BYTES == 0x00491000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_LEDGER_RETAINED_BYTES == 0x00492000U);
    CHECK(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES == 0x007d5000U);
    CHECK(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES == 0x007d6000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_TOTAL_REQUEST_BYTES == 0x00c66000U);
    CHECK(ISAAC_VITA_GUEST_HEAP_TOTAL_RETAINED_BYTES == 0x00c68000U);
    CHECK(isaac_vita_heap_ledger_memblock_layout(0x00490000U, &request) &&
          request == 0x00491000U);
    CHECK(isaac_vita_guest_heap_test_forbidden_ranges(
              0x82ad1560U, 0x05100000U,
              0x85000000U, 0x85400000U, &ranges));
    CHECK(ranges.retained_newlib.begin == 0x82ad1560U &&
          ranges.retained_newlib.end == 0x87cd2000U &&
          ranges.fixed_image.begin == 0x98000000U &&
          ranges.fixed_image.end == 0x9885f000U &&
          ranges.guest_stack.begin == 0x85000000U &&
          ranges.guest_stack.end == 0x85400000U);
    CHECK(!isaac_vita_guest_heap_test_forbidden_ranges(
              0x82ad1560U, 0x15000000U,
              0x85000000U, 0x85400000U, &ranges));
    CHECK(!isaac_vita_guest_heap_test_forbidden_ranges(
              0x82ad1560U, 0x15500000U,
              0x85000000U, 0x85400000U, &ranges));
    CHECK(!isaac_vita_guest_heap_test_forbidden_ranges(
              UINTPTR_MAX - 0x1000U, 0x2000U,
              0x85000000U, 0x85400000U, &ranges));
    CHECK(isaac_vita_guest_heap_test_forbidden_ranges(
              0x82000000U, 0x05100000U,
              0x85000000U, 0x85400000U, &ranges) &&
          ranges.retained_newlib.end == 0x87200000U);
    CHECK(!isaac_vita_guest_heap_test_forbidden_ranges(
              UINTPTR_MAX - 0x2000U, 0x1000U,
              0x85000000U, 0x85400000U, &ranges));
    CHECK(!isaac_vita_guest_heap_test_forbidden_ranges(
              0x82ad1560U, 0x05100000U,
              0x85000000U, 0x85000000U, &ranges));
    CHECK(!isaac_vita_guest_heap_test_forbidden_ranges(
              0x82ad1560U, 0x05100000U,
              0x98800000U, 0x98900000U, &ranges));

    native_calls = s_native_malloc_calls;
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(
              32U, &result, &reason) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          strcmp(reason, "overflow-mspace-not-initialized") == 0 &&
          s_native_malloc_calls == native_calls &&
          isaac_vita_guest_heap_terminal());
    CHECK(!isaac_vita_guest_heap_test_init(
              0x82ad1560U, 0x04000000U,
              0x85000000U, 0x85400000U) &&
          s_alloc_event_count == 0U);
    CHECK(!oracle_init());
    CHECK(oracle_reset());
    CHECK(!isaac_vita_guest_heap_terminal());

    s_alloc_event_count = 0U;
    s_fail_overflow_memblock = 1U;
    CHECK(!oracle_init());
    CHECK(s_alloc_event_count == 2U &&
          s_alloc_kinds[0] == ORACLE_ALLOC_LEDGER &&
          s_alloc_kinds[1] == ORACLE_ALLOC_OVERFLOW &&
          s_alloc_sizes[1] == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&storage) &&
          storage.capacity ==
              ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY &&
          isaac_vita_guest_heap_test_fixed_capacity());
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(
              32U, &result, &reason) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          isaac_vita_guest_heap_terminal());
    CHECK(oracle_reset());

    s_alloc_event_count = 0U;
    CHECK(oracle_init());
    CHECK(s_alloc_event_count == 2U &&
          s_alloc_kinds[0] == ORACLE_ALLOC_LEDGER &&
          s_alloc_kinds[1] == ORACLE_ALLOC_OVERFLOW &&
          s_alloc_sizes[0] == ORACLE_HOST_LEDGER_REQUEST_BYTES &&
          s_alloc_sizes[1] == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&storage) &&
          storage.capacity ==
              ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY &&
          storage.usable_bytes == ORACLE_HOST_LEDGER_USABLE_BYTES &&
          storage.block_bytes == ORACLE_HOST_LEDGER_REQUEST_BYTES &&
          isaac_vita_guest_heap_test_fixed_capacity());
    CHECK(!isaac_vita_guest_heap_terminal());
    CHECK(oracle_reset());
    return 0;
}

static int test_domains_leases_and_realloc(void)
{
    isaac_vita_guest_heap_test_result result;
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    unsigned char *native;
    unsigned char *overflow;
    unsigned char *replacement;
    int foreign_object = 0;
    uintptr_t allocation_base;
    uintptr_t old_address;
    uint32_t lease;
    int valid_owner;
    size_t index;
    size_t native_frees;
    size_t mspace_frees;
    size_t native_calloc_calls;
    size_t mspace_calloc_calls;
    size_t native_malloc_calls;
    size_t native_realloc_calls;
    size_t mspace_malloc_calls;
    size_t mspace_realloc_calls;
    size_t live_count;
    const char *reason = NULL;

    CHECK(oracle_init());
    native_calloc_calls = s_native_calloc_calls;
    mspace_calloc_calls = s_mspace_calloc_calls;
    CHECK(!isaac_vita_guest_heap_test_calloc_ex(
              SIZE_MAX, 2U, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          s_native_calloc_calls == native_calloc_calls &&
          s_mspace_calloc_calls == mspace_calloc_calls);

    native_calloc_calls = s_native_calloc_calls;
    mspace_calloc_calls = s_mspace_calloc_calls;
    replacement = isaac_vita_guest_heap_test_calloc_ex(4U, 16U, &result);
    CHECK(replacement && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_owns(replacement) &&
          s_native_calloc_calls == native_calloc_calls + 1U &&
          s_mspace_calloc_calls == mspace_calloc_calls);
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0U);
    CHECK(isaac_vita_guest_heap_test_free_ex(replacement, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK);

    native_calloc_calls = s_native_calloc_calls;
    mspace_calloc_calls = s_mspace_calloc_calls;
    s_fail_native_calloc = 1U;
    replacement = isaac_vita_guest_heap_test_calloc_ex(8U, 8U, &result);
    CHECK(replacement && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_owns(replacement) &&
          s_native_calloc_calls == native_calloc_calls + 1U &&
          s_mspace_calloc_calls == mspace_calloc_calls + 1U);
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0U);
    CHECK(isaac_vita_guest_heap_test_free_ex(replacement, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK);

    valid_owner = 1;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              &foreign_object, 32U, &valid_owner, &result) &&
          !valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(!isaac_vita_guest_heap_test_free_ex(&foreign_object, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);

    replacement = isaac_vita_guest_heap_test_realloc_ex(
        NULL, 48U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_owns(replacement));
    CHECK(isaac_vita_guest_heap_test_free_ex(replacement, &result));

    live_count = isaac_vita_guest_heap_test_live_count();
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    s_fail_native_malloc = 1U;
    s_fail_mspace_malloc = 1U;
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(96U, &result, &reason) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          strcmp(reason, "native-malloc-failed") == 0 &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls + 1U &&
          isaac_vita_guest_heap_test_live_count() == live_count &&
          !isaac_vita_guest_heap_terminal());

    native = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(native && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_guest_heap_owns(native));
    memset(native, 0x31, 64U);
    s_fail_native_malloc = 1U;
    overflow = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(overflow && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(overflow) &&
          isaac_vita_guest_heap_owns(overflow));
    memset(overflow, 0x52, 64U);

    native_frees = s_native_free_calls;
    mspace_frees = s_mspace_free_calls;
    CHECK(!isaac_vita_guest_heap_test_free_ex(native + 1U, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN &&
          s_native_free_calls == native_frees &&
          s_mspace_free_calls == mspace_frees);
    CHECK(!isaac_vita_guest_heap_test_free_ex(overflow + 1U, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN &&
          s_native_free_calls == native_frees &&
          s_mspace_free_calls == mspace_frees);

    lease = isaac_vita_guest_heap_lease_containing(
        overflow + 4U, 8U, &allocation_base);
    CHECK(lease && allocation_base == (uintptr_t)overflow);
    valid_owner = 1;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              overflow, 80U, &valid_owner, &result) &&
          !valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(!isaac_vita_guest_heap_test_free_ex(overflow, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(isaac_vita_guest_heap_lease_release(lease));

    mspace_malloc_calls = s_mspace_malloc_calls;
    mspace_realloc_calls = s_mspace_realloc_calls;
    native_realloc_calls = s_native_realloc_calls;
    old_address = (uintptr_t)native;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        native, 96U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_owns(replacement) &&
          s_native_realloc_calls == native_realloc_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls &&
          s_mspace_realloc_calls == mspace_realloc_calls);
    if ((uintptr_t)replacement != old_address)
        CHECK(!isaac_vita_guest_heap_owns((void *)old_address));
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0x31U);
    native = replacement;

    native_malloc_calls = s_native_malloc_calls;
    native_realloc_calls = s_native_realloc_calls;
    mspace_realloc_calls = s_mspace_realloc_calls;
    old_address = (uintptr_t)overflow;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        overflow, 96U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(replacement) &&
          !isaac_vita_guest_heap_owns((void *)old_address) &&
          s_native_malloc_calls == native_malloc_calls &&
          s_native_realloc_calls == native_realloc_calls &&
          s_mspace_realloc_calls == mspace_realloc_calls + 1U);
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0x52U);
    overflow = replacement;

    s_fail_native_realloc = 1U;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        native, 128U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(replacement) &&
          !isaac_vita_guest_heap_owns(native));
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0x31U);
    native = replacement;

    s_fail_mspace_realloc = 1U;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        native, 80U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          !isaac_vita_guest_heap_owns(native));
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0x31U);
    native = replacement;

    s_fail_native_realloc = 1U;
    s_fail_mspace_malloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              native, 256U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          isaac_vita_guest_heap_owns(native));
    for (index = 0U; index < 64U; ++index)
        CHECK(native[index] == 0x31U);

    s_fail_mspace_realloc = 1U;
    s_fail_native_malloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              overflow, 256U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          isaac_vita_guest_heap_owns(overflow));
    for (index = 0U; index < 64U; ++index)
        CHECK(overflow[index] == 0x52U);

    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              overflow, 0U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_guest_heap_owns(overflow));
    CHECK(!isaac_vita_guest_heap_test_free_ex(overflow, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(isaac_vita_guest_heap_test_free_ex(native, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK);
    CHECK(!isaac_vita_guest_heap_test_free_ex(native, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.live_count == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_floor_lifetime_migration_preservation(void)
{
    isaac_vita_guest_heap_floor_lifetime_snapshot floor;
    isaac_vita_guest_heap_test_result result;
    unsigned char *pointer;
    unsigned char *replacement;
    void *retired = NULL;
    int valid_owner = 0;

    CHECK(oracle_init());
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_bootstrap(
              ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
    pointer = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(pointer && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(pointer));
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_phase_set(
              ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD));

    /* Native same-address replacement occurs under a different runtime
     * phase, so rejuvenation would move its birth bytes to room-load. */
    s_force_native_realloc_same = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pointer, 1U, &valid_owner, &result);
    CHECK(replacement == pointer && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.total_count == 1U && floor.current_count == 1U &&
          floor.current_requested_bytes == 1U &&
          floor.phase_count[0] == 1U && floor.phase_count[1] == 0U &&
          floor.phase_requested_bytes[0] == 1U &&
          floor.phase_requested_bytes[1] == 0U);

    replacement = isaac_vita_guest_heap_test_force_move(
        pointer, 2U, &retired);
    CHECK(replacement && retired == pointer &&
          isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.current_count == 1U &&
          floor.current_requested_bytes == 2U &&
          floor.phase_count[0] == 1U && floor.phase_count[1] == 0U &&
          floor.phase_requested_bytes[0] == 2U);
    free(retired);
    pointer = replacement;

    /* Force the real native->pool and pool->native migration branches. */
    s_fail_native_realloc = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pointer, 3U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.current_count == 1U &&
          floor.current_requested_bytes == 3U &&
          floor.phase_count[0] == 1U && floor.phase_count[1] == 0U &&
          floor.phase_requested_bytes[0] == 3U);
    pointer = replacement;
    s_fail_mspace_realloc = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pointer, 4U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.current_count == 1U &&
          floor.current_requested_bytes == 4U &&
          floor.phase_count[0] == 1U && floor.phase_count[1] == 0U &&
          floor.phase_requested_bytes[0] == 4U);
    pointer = replacement;

    CHECK(isaac_vita_guest_heap_test_floor_lifetime_rollover(
              ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
    CHECK(isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.epoch == 2U && floor.prior_count == 1U &&
          floor.prior_requested_bytes == 4U &&
          floor.current_count == 0U &&
          floor.phase_count[0] == 0U && floor.phase_count[1] == 0U &&
          floor.phase_count[2] == 0U);

    s_fail_native_realloc = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pointer, 5U, &valid_owner, &result);
    CHECK(replacement &&
          isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.prior_count == 1U && floor.prior_requested_bytes == 5U &&
          floor.current_count == 0U && floor.phase_count[0] == 0U);
    pointer = replacement;
    s_fail_mspace_realloc = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pointer, 6U, &valid_owner, &result);
    CHECK(replacement &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_floor_lifetime_snapshot_get(&floor) &&
          floor.prior_count == 1U && floor.prior_requested_bytes == 6U &&
          floor.current_count == 0U && floor.phase_count[0] == 0U);
    CHECK(isaac_vita_guest_heap_test_free_ex(replacement, &result));
    CHECK(oracle_reset());
    return 0;
}

static int test_floor_lifetime_slab_move_categories(void)
{
    isaac_vita_guest_heap_floor_lifetime_snapshot heap_floor;
    isaac_vita_room_entry_slab_floor_lifetime_snapshot slab_floor;
    isaac_vita_guest_heap_test_slab_move_floor_receipt receipt;
    CPU cpu;
    unsigned char *pointer;
    unsigned char *replacement;
    uint32_t inherited;
    unsigned category;
    int valid_owner;

    for (category = 0U; category < 3U; ++category) {
        CHECK(oracle_init());
        if (category != 0U)
            CHECK(isaac_vita_guest_heap_test_floor_lifetime_bootstrap(
                      ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
        oracle_prepare_room_malloc_import(&cpu);
        CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
              !cpu.fault && cpu.eax != 0U);
        pointer = (unsigned char *)(uintptr_t)cpu.eax;
        if (category == 0U)
            CHECK(isaac_vita_guest_heap_test_floor_lifetime_bootstrap(
                      ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
        else if (category == 2U)
            CHECK(isaac_vita_guest_heap_test_floor_lifetime_rollover(
                      ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
        CHECK(isaac_vita_guest_heap_test_floor_lifetime_phase_set(
                  ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD));

        inherited = category == 0U ? 0U :
            ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT |
                (category == 1U ?
                    ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT : 0U);
        isaac_vita_guest_heap_test_slab_move_floor_receipt_reset();
        s_use_static_canary_malloc = 1U;
        valid_owner = 0;
        replacement = isaac_vita_guest_realloc(pointer, 8U, &valid_owner);
        CHECK(valid_owner && replacement == s_static_canary_allocation &&
              !isaac_vita_guest_heap_owns(pointer) &&
              isaac_vita_guest_heap_owns(replacement));
        CHECK(isaac_vita_guest_heap_test_slab_move_floor_receipt_snapshot(
                  &receipt) &&
              receipt.attempts == 1U &&
              receipt.replacement_original_token ==
                  (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD |
                   ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT) &&
              receipt.inherited_token == inherited &&
              receipt.replacement_final_token == inherited &&
              receipt.retag_succeeded == 1U &&
              receipt.commit_succeeded == 1U &&
              receipt.rollback_attempted == 0U &&
              receipt.rollback_succeeded == 0U);
        CHECK(isaac_vita_guest_heap_floor_lifetime_snapshot_get(
                  &heap_floor) &&
              heap_floor.total_count == 1U &&
              heap_floor.total_requested_bytes == 8U &&
              heap_floor.unscoped_count == (category == 0U ? 1U : 0U) &&
              heap_floor.prior_count == (category == 2U ? 1U : 0U) &&
              heap_floor.current_count == (category == 1U ? 1U : 0U) &&
              heap_floor.phase_count[0] == (category == 1U ? 1U : 0U) &&
              heap_floor.phase_count[1] == 0U &&
              heap_floor.phase_count[2] == 0U &&
              heap_floor.phase_requested_bytes[0] ==
                  (category == 1U ? 8U : 0U));
        CHECK(isaac_vita_guest_heap_test_room_floor_lifetime_snapshot(
                  &slab_floor) &&
              slab_floor.live_slots == 0U &&
              slab_floor.unscoped_slots == 0U &&
              slab_floor.prior_slots == 0U &&
              slab_floor.current_slots == 0U &&
              slab_floor.level_init_slots == 0U &&
              slab_floor.room_load_slots == 0U &&
              slab_floor.play_slots == 0U);
        CHECK(isaac_vita_guest_free(replacement) &&
              !s_static_canary_live && oracle_reset());
    }
    return 0;
}

static int test_telemetry_success_and_rollback(void)
{
    isaac_vita_guest_heap_telemetry_snapshot initial;
    isaac_vita_guest_heap_telemetry_snapshot before;
    isaac_vita_guest_heap_telemetry_snapshot snapshot;
    isaac_vita_guest_heap_test_result result;
    unsigned char *native;
    unsigned char *native_calloc_zero;
    unsigned char *native_zero;
    unsigned char *pool_malloc;
    unsigned char *pool_calloc;
    unsigned char *replacement;
    const char *first_use_log;
    const char *final_log;
    size_t log_begin = s_log_count;
    size_t log_count;
    size_t raw_snapshot_calls;
    size_t index;
    int valid_owner;

    CHECK(oracle_init());
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&initial) &&
          oracle_telemetry_invariants(&initial) &&
          initial.sequence == 0U && initial.owned_live_count == 0U &&
          initial.owned_requested_bytes == 0U &&
          initial.peak_live_count == 0U &&
          initial.peak_requested_bytes == 0U &&
          initial.pool_allocations == 0U && initial.pool_frees == 0U &&
          initial.pool_reallocations == 0U &&
          initial.native_failures == 0U && initial.pool_failures == 0U &&
          initial.native_to_pool == 0U && initial.pool_to_native == 0U &&
          initial.consumed_terminal == 0U && initial.raw_live_count == 0U &&
          initial.raw_stranded_count == 0U &&
          initial.mspace_state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY &&
          initial.terminal == 0U);

    log_count = s_log_count;
    raw_snapshot_calls = s_raw_snapshot_calls;
    native_zero = isaac_vita_guest_heap_test_malloc_ex(16U, &result, NULL);
    CHECK(native_zero && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(native_zero) &&
          s_log_count == log_count &&
          s_raw_snapshot_calls == raw_snapshot_calls);
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        native_zero, 24U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          s_log_count == log_count &&
          s_raw_snapshot_calls == raw_snapshot_calls);
    native_zero = replacement;
    CHECK(isaac_vita_guest_heap_test_free_ex(native_zero, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          s_log_count == log_count &&
          s_raw_snapshot_calls == raw_snapshot_calls);
    native_calloc_zero = isaac_vita_guest_heap_test_calloc_ex(
        1U, 16U, &result);
    CHECK(native_calloc_zero && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(native_calloc_zero) &&
          isaac_vita_guest_heap_test_free_ex(native_calloc_zero, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          s_log_count == log_count &&
          s_raw_snapshot_calls == raw_snapshot_calls);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          memcmp(&snapshot, &initial, sizeof snapshot) == 0);

    s_fail_native_malloc = 1U;
    pool_malloc = isaac_vita_guest_heap_test_malloc_ex(32U, &result, NULL);
    CHECK(pool_malloc && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(pool_malloc));
    s_fail_native_calloc = 1U;
    pool_calloc = isaac_vita_guest_heap_test_calloc_ex(2U, 24U, &result);
    CHECK(pool_calloc && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(pool_calloc));
    for (index = 0U; index < 48U; ++index)
        CHECK(pool_calloc[index] == 0U);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          oracle_telemetry_invariants(&snapshot) &&
          snapshot.sequence == 1U && snapshot.owned_live_count == 2U &&
          snapshot.owned_requested_bytes == 80U &&
          snapshot.peak_live_count == 2U &&
          snapshot.peak_requested_bytes == 80U &&
          snapshot.pool_allocations == 2U && snapshot.pool_frees == 0U &&
          snapshot.pool_reallocations == 0U &&
          snapshot.native_failures == 2U && snapshot.pool_failures == 0U &&
          snapshot.raw_live_count == 2U &&
          snapshot.raw_stranded_count == 0U);

    before = snapshot;
    valid_owner = 1;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              pool_malloc + 1U, 40U, &valid_owner, &result) &&
          !valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(!isaac_vita_guest_heap_test_free_ex(pool_calloc + 1U, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          memcmp(&snapshot, &before, sizeof snapshot) == 0);
    CHECK(!isaac_vita_guest_heap_test_storage_reset());
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          memcmp(&snapshot, &before, sizeof snapshot) == 0);

    native = isaac_vita_guest_heap_test_malloc_ex(40U, &result, NULL);
    CHECK(native && !isaac_vita_heap_overflow_mspace_contains(native));
    s_fail_native_realloc = 1U;
    s_fail_mspace_malloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              native, 56U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          isaac_vita_guest_heap_owns(native));
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          oracle_telemetry_invariants(&snapshot) &&
          snapshot.owned_live_count == 2U &&
          snapshot.owned_requested_bytes == 80U &&
          snapshot.pool_allocations == 2U && snapshot.pool_frees == 0U &&
          snapshot.pool_reallocations == 0U &&
          snapshot.native_failures == 3U && snapshot.pool_failures == 1U);

    s_fail_mspace_realloc = 1U;
    s_fail_native_malloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              pool_malloc, 60U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          isaac_vita_guest_heap_owns(pool_malloc));
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          oracle_telemetry_invariants(&snapshot) &&
          snapshot.owned_live_count == 2U &&
          snapshot.owned_requested_bytes == 80U &&
          snapshot.pool_allocations == 2U && snapshot.pool_frees == 0U &&
          snapshot.pool_reallocations == 0U &&
          snapshot.native_failures == 4U && snapshot.pool_failures == 2U);

    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pool_malloc, 64U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(replacement));
    pool_malloc = replacement;
    s_fail_native_realloc = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        native, 80U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          isaac_vita_heap_overflow_mspace_contains(replacement));
    native = replacement;
    s_fail_mspace_realloc = 1U;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pool_calloc, 72U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement));
    pool_calloc = replacement;
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          oracle_telemetry_invariants(&snapshot) &&
          snapshot.sequence == 5U && snapshot.owned_live_count == 2U &&
          snapshot.owned_requested_bytes == 144U &&
          snapshot.peak_live_count == 3U &&
          snapshot.peak_requested_bytes == 192U &&
          snapshot.pool_allocations == 3U && snapshot.pool_frees == 1U &&
          snapshot.pool_reallocations == 1U &&
          snapshot.native_failures == 5U && snapshot.pool_failures == 3U &&
          snapshot.native_to_pool == 1U && snapshot.pool_to_native == 1U &&
          snapshot.raw_live_count == 2U &&
          snapshot.raw_stranded_count == 0U);

    CHECK(isaac_vita_guest_heap_test_free_ex(pool_malloc, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK);
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              native, 0U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_OK);
    CHECK(isaac_vita_guest_heap_test_free_ex(pool_calloc, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          oracle_telemetry_invariants(&snapshot) &&
          snapshot.owned_live_count == 0U &&
          snapshot.owned_requested_bytes == 0U &&
          snapshot.pool_allocations == 3U && snapshot.pool_frees == 3U &&
          snapshot.pool_reallocations == 1U &&
          snapshot.native_failures == 5U && snapshot.pool_failures == 3U &&
          snapshot.native_to_pool == 1U && snapshot.pool_to_native == 1U &&
          snapshot.consumed_terminal == 0U && snapshot.raw_live_count == 0U &&
          snapshot.raw_stranded_count == 0U && snapshot.terminal == 0U);

    isaac_vita_guest_heap_telemetry_log_final();
    isaac_vita_guest_heap_telemetry_log_final();
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          snapshot.sequence == 6U && oracle_telemetry_invariants(&snapshot));
    CHECK(oracle_log_edge_count(log_begin, "first-use") == 1U &&
          oracle_log_edge_count(log_begin, "first-free") == 1U &&
          oracle_log_edge_count(log_begin, "first-realloc") == 1U &&
          oracle_log_edge_count(log_begin, "native-to-pool") == 1U &&
          oracle_log_edge_count(log_begin, "pool-to-native") == 1U &&
          oracle_log_edge_count(log_begin, "pool-failure") == 1U &&
          oracle_log_edge_count(log_begin, "terminal") == 0U &&
          oracle_log_edge_count(log_begin, "final") == 1U &&
          s_log_truncations == 0U && s_log_under_lock == 0U &&
          s_log_dropped == 0U);
    first_use_log = oracle_log_edge_line(log_begin, "first-use");
    final_log = oracle_log_edge_line(log_begin, "final");
    CHECK(first_use_log &&
          strstr(first_use_log,
                 "q=1 op=1 req=32 live=1/32 peak=1/32 a/f/r=1/0/0") &&
          strstr(first_use_log,
                 "n2p/p2n=0/0 nf/pf=1/0 cons=0 raw/str/int=1/0/0/0") &&
          strstr(first_use_log, "state=1 term=0 valid/sat=1/0") &&
          final_log &&
          strstr(final_log,
                 "q=6 op=0 req=0 live=0/0 peak=3/192 a/f/r=3/3/1") &&
          strstr(final_log,
                 "n2p/p2n=1/1 nf/pf=5/3 cons=0 raw/str/int=0/0/0/0") &&
          strstr(final_log, "state=1 term=0 valid/sat=1/0"));

    CHECK(oracle_reset());
    CHECK(!isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot));
    CHECK(oracle_init());
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot) &&
          oracle_telemetry_invariants(&snapshot) &&
          snapshot.sequence == 0U && snapshot.pool_allocations == 0U &&
          snapshot.pool_frees == 0U && snapshot.native_failures == 0U &&
          snapshot.pool_failures == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_drain_only_statuses(void)
{
    isaac_vita_guest_heap_test_result result;
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    CPU cpu;
    unsigned char *first;
    unsigned char *second;
    unsigned char *replacement;
    int valid_owner;
    size_t native_calls;
    size_t native_calloc_calls;
    size_t native_realloc_calls;
    size_t index;
    size_t log_begin = s_log_count;
    const char *terminal_log;

    CHECK(oracle_init());
    s_fail_native_malloc = 1U;
    s_misalign_mspace_malloc = 1U;
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(
              32U, &result, NULL) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          isaac_vita_guest_heap_test_live_count() == 0U &&
          isaac_vita_guest_heap_terminal());
    native_calls = s_native_malloc_calls;
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(16U, &result, NULL) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          s_native_malloc_calls == native_calls);
    CHECK((uintptr_t)s_import_frame <= UINT32_MAX);
    oracle_prepare_heap_import(&cpu, 16U, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          cpu.fault &&
          cpu.esp == (uint32_t)(uintptr_t)s_import_frame &&
          s_native_malloc_calls == native_calls);
    native_calloc_calls = s_native_calloc_calls;
    oracle_prepare_heap_import(&cpu, 2U, 16U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_CALLOC_NAME) &&
          cpu.fault &&
          cpu.esp == (uint32_t)(uintptr_t)s_import_frame &&
          s_native_calloc_calls == native_calloc_calls);
    native_realloc_calls = s_native_realloc_calls;
    oracle_prepare_heap_import(&cpu, 0U, 16U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          cpu.fault &&
          cpu.esp == (uint32_t)(uintptr_t)s_import_frame &&
          s_native_malloc_calls == native_calls &&
          s_native_realloc_calls == native_realloc_calls);
    oracle_prepare_heap_import(&cpu, 0U, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          cpu.fault &&
          cpu.esp == (uint32_t)(uintptr_t)s_import_frame);
    terminal_log = oracle_log_edge_line(log_begin, "terminal");
    CHECK(oracle_log_edge_count(log_begin, "pool-failure") == 1U &&
          oracle_log_edge_count(log_begin, "terminal") == 1U &&
          terminal_log &&
          strstr(terminal_log,
                 "q=1 op=1 req=32 live=0/0 peak=0/0 a/f/r=0/0/0") &&
          strstr(terminal_log,
                 "n2p/p2n=0/0 nf/pf=1/1 cons=0 raw/str/int=0/0/0/0") &&
          strstr(terminal_log, "state=4 term=1 valid/sat=1/0"));
    CHECK(oracle_reset());
    CHECK(!isaac_vita_guest_heap_terminal());

    CHECK(oracle_init());
    s_fail_native_malloc = 1U;
    first = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    s_fail_native_malloc = 1U;
    second = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(first && second);
    memset(first, 0x74, 64U);
    s_fail_native_malloc = 1U;
    s_misalign_mspace_malloc = 1U;
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(32U, &result, NULL) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          isaac_vita_guest_heap_terminal());
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        first, 96U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          isaac_vita_guest_heap_terminal());
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == 0x74U);
    CHECK(isaac_vita_guest_heap_test_free_ex(second, &result));
    CHECK(isaac_vita_guest_heap_test_free_ex(replacement, &result));
    CHECK(oracle_reset());

    CHECK(oracle_init());
    s_fail_native_malloc = 1U;
    first = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(first);
    memset(first, 0x29, 64U);
    s_misalign_mspace_realloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              first, 128U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          !isaac_vita_guest_heap_owns(first) &&
          isaac_vita_guest_heap_test_live_count() == 0U &&
          isaac_vita_guest_heap_terminal());
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          telemetry.owned_live_count == 0U &&
          telemetry.owned_requested_bytes == 0U &&
          telemetry.pool_allocations == 1U &&
          telemetry.pool_frees == 1U &&
          telemetry.consumed_terminal == 1U &&
          telemetry.raw_live_count == 0U &&
          telemetry.raw_stranded_count == 0U &&
          telemetry.terminal == 1U &&
          telemetry.accounting_valid == 0U &&
          telemetry.counter_saturated == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_ready_raw_terminal_latch(void)
{
    isaac_vita_guest_heap_test_result result;
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    void *pointer;
    size_t native_free_calls;
    size_t mspace_free_calls;

    CHECK(oracle_init());
    pointer = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(pointer && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_heap_overflow_mspace_contains(pointer));
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY);
    CHECK(isaac_vita_guest_heap_test_corrupt_no_empty());
    native_free_calls = s_native_free_calls;
    mspace_free_calls = s_mspace_free_calls;
    CHECK(!isaac_vita_guest_heap_test_free_ex(pointer, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          isaac_vita_guest_heap_terminal() &&
          isaac_vita_guest_heap_owns(pointer) &&
          s_native_free_calls == native_free_calls &&
          s_mspace_free_calls == mspace_free_calls);
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY);
    CHECK(isaac_vita_guest_heap_test_restore_empty());
    CHECK(isaac_vita_guest_heap_test_free_ex(pointer, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          !isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_terminal());
    CHECK(oracle_reset());
    CHECK(!isaac_vita_guest_heap_terminal());
    return 0;
}

static int test_fixed_capacity(void)
{
    const size_t limit =
        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT;
    isaac_vita_guest_heap_test_result result;
    isaac_vita_guest_heap_test_probe_stats fill_stats;
    isaac_vita_guest_heap_test_probe_stats cleanup_stats;
    isaac_vita_guest_heap_test_storage_state initial_storage;
    isaac_vita_guest_heap_test_storage_state final_storage;
    const char *reason = NULL;
    void **pointers;
    void *retired = NULL;
    void *replacement;
    void *old_pointer;
    size_t native_malloc_calls;
    size_t native_calloc_calls;
    size_t native_realloc_calls;
    size_t native_free_calls;
    size_t mspace_malloc_calls;
    size_t mspace_calloc_calls;
    size_t mspace_realloc_calls;
    size_t mspace_free_calls;
    size_t realloc_moves;
    size_t index;
    clock_t fill_begin;
    clock_t fill_end;
    clock_t cleanup_begin;
    clock_t cleanup_end;
    int valid_owner;

    s_alloc_event_count = 0U;
    CHECK(isaac_vita_guest_heap_test_backshift_enabled());
    CHECK(isaac_vita_guest_heap_test_backshift_oracle());
    CHECK(oracle_init());
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&initial_storage) &&
          initial_storage.capacity ==
              ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY &&
          initial_storage.tombstones == 0U);
    pointers = (void **)malloc(limit * sizeof *pointers);
    CHECK(pointers != NULL);
    isaac_vita_guest_heap_test_probe_stats_reset();
    fill_begin = clock();
    for (index = 0U; index < limit; ++index) {
        pointers[index] = isaac_vita_guest_malloc(1U);
        CHECK(pointers[index] != NULL);
    }
    fill_end = clock();
    CHECK(isaac_vita_guest_heap_test_live_count() == limit &&
          isaac_vita_guest_heap_test_capacity() ==
              ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY &&
          isaac_vita_guest_heap_test_next_rehash_bytes() == SIZE_MAX);
    CHECK(isaac_vita_guest_heap_test_probe_stats_snapshot(&fill_stats) &&
          fill_stats.lock_acquisitions >= limit &&
          fill_stats.lock_spins == 0U &&
          fill_stats.find_calls >= limit &&
          fill_stats.find_probes >= fill_stats.find_calls &&
          fill_stats.find_max_probes > 0U);
    printf(
        "Vita heap fixed ledger probe/lock receipt: phase=fill "
        "live=%zu capacity=%u load=%u/%u clock_ticks=%lld clock_hz=%ld "
        "locks=%zu spins=%zu find_calls=%zu find_probes=%zu find_max=%zu\n",
        limit, ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY,
        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR,
        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR,
        (long long)(fill_end - fill_begin), (long)CLOCKS_PER_SEC,
        fill_stats.lock_acquisitions, fill_stats.lock_spins, fill_stats.find_calls,
        fill_stats.find_probes, fill_stats.find_max_probes);

    native_malloc_calls = s_native_malloc_calls;
    native_calloc_calls = s_native_calloc_calls;
    native_realloc_calls = s_native_realloc_calls;
    native_free_calls = s_native_free_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    mspace_calloc_calls = s_mspace_calloc_calls;
    mspace_realloc_calls = s_mspace_realloc_calls;
    mspace_free_calls = s_mspace_free_calls;
    CHECK(!isaac_vita_guest_heap_test_malloc_ex(1U, &result, &reason) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          strcmp(reason, "ledger-reserve-failed") == 0 &&
          s_native_malloc_calls == native_malloc_calls &&
          s_native_calloc_calls == native_calloc_calls &&
          s_native_realloc_calls == native_realloc_calls &&
          s_native_free_calls == native_free_calls &&
          s_mspace_malloc_calls == mspace_malloc_calls &&
          s_mspace_calloc_calls == mspace_calloc_calls &&
          s_mspace_realloc_calls == mspace_realloc_calls &&
          s_mspace_free_calls == mspace_free_calls);
    native_malloc_calls = s_native_malloc_calls;
    native_calloc_calls = s_native_calloc_calls;
    native_realloc_calls = s_native_realloc_calls;
    native_free_calls = s_native_free_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    mspace_calloc_calls = s_mspace_calloc_calls;
    mspace_realloc_calls = s_mspace_realloc_calls;
    mspace_free_calls = s_mspace_free_calls;
    CHECK(!isaac_vita_guest_heap_test_calloc_ex(1U, 1U, &result) &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
          s_native_malloc_calls == native_malloc_calls &&
          s_native_calloc_calls == native_calloc_calls &&
          s_native_realloc_calls == native_realloc_calls &&
          s_native_free_calls == native_free_calls &&
          s_mspace_malloc_calls == mspace_malloc_calls &&
          s_mspace_calloc_calls == mspace_calloc_calls &&
          s_mspace_realloc_calls == mspace_realloc_calls &&
          s_mspace_free_calls == mspace_free_calls &&
          isaac_vita_guest_heap_test_live_count() == limit &&
          !isaac_vita_guest_heap_terminal());

    native_free_calls = s_native_free_calls;
    CHECK(isaac_vita_guest_free(pointers[1]) &&
          s_native_free_calls == native_free_calls + 1U &&
          isaac_vita_guest_heap_test_live_count() == limit - 1U);
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    replacement = isaac_vita_guest_heap_test_malloc_ex(
        1U, &result, &reason);
    CHECK(replacement && result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls &&
          isaac_vita_guest_heap_test_live_count() == limit &&
          isaac_vita_guest_heap_owns(replacement));
    pointers[1] = replacement;

    native_malloc_calls = s_native_malloc_calls;
    native_calloc_calls = s_native_calloc_calls;
    native_realloc_calls = s_native_realloc_calls;
    native_free_calls = s_native_free_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    mspace_calloc_calls = s_mspace_calloc_calls;
    mspace_realloc_calls = s_mspace_realloc_calls;
    mspace_free_calls = s_mspace_free_calls;
    s_force_native_realloc_same = 1U;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        pointers[2], 1U, &valid_owner, &result);
    CHECK(replacement == pointers[2] && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          s_force_native_realloc_same == 0U &&
          s_native_malloc_calls == native_malloc_calls &&
          s_native_calloc_calls == native_calloc_calls &&
          s_native_realloc_calls == native_realloc_calls + 1U &&
          s_native_free_calls == native_free_calls &&
          s_mspace_malloc_calls == mspace_malloc_calls &&
          s_mspace_calloc_calls == mspace_calloc_calls &&
          s_mspace_realloc_calls == mspace_realloc_calls &&
          s_mspace_free_calls == mspace_free_calls &&
          isaac_vita_guest_heap_test_live_count() == limit &&
          isaac_vita_guest_heap_owns(replacement));

    realloc_moves = isaac_vita_guest_heap_test_realloc_moves();
    replacement = isaac_vita_guest_heap_test_force_move(
        pointers[0], 2U, &retired);
    CHECK(replacement && retired == pointers[0] &&
          isaac_vita_guest_heap_test_live_count() == limit &&
          !isaac_vita_guest_heap_owns(retired) &&
          isaac_vita_guest_heap_owns(replacement) &&
          isaac_vita_guest_heap_test_realloc_moves() == realloc_moves + 1U);
    free(retired);
    retired = NULL;
    pointers[0] = replacement;

    old_pointer = pointers[3];
    native_realloc_calls = s_native_realloc_calls;
    native_free_calls = s_native_free_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    s_fail_native_realloc = 1U;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        old_pointer, 2U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          s_fail_native_realloc == 0U &&
          s_native_realloc_calls == native_realloc_calls + 1U &&
          s_native_free_calls == native_free_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls + 1U &&
          isaac_vita_heap_overflow_mspace_contains(replacement) &&
          !isaac_vita_guest_heap_owns(old_pointer) &&
          isaac_vita_guest_heap_owns(replacement) &&
          isaac_vita_guest_heap_test_live_count() == limit);
    pointers[3] = replacement;

    old_pointer = pointers[3];
    native_malloc_calls = s_native_malloc_calls;
    mspace_realloc_calls = s_mspace_realloc_calls;
    mspace_free_calls = s_mspace_free_calls;
    s_fail_mspace_realloc = 1U;
    valid_owner = 0;
    replacement = isaac_vita_guest_heap_test_realloc_ex(
        old_pointer, 3U, &valid_owner, &result);
    CHECK(replacement && valid_owner &&
          result == ISAAC_VITA_GUEST_HEAP_TEST_OK &&
          s_fail_mspace_realloc == 0U &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_mspace_realloc_calls == mspace_realloc_calls + 1U &&
          s_mspace_free_calls == mspace_free_calls + 1U &&
          !isaac_vita_heap_overflow_mspace_contains(replacement) &&
          !isaac_vita_guest_heap_owns(old_pointer) &&
          isaac_vita_guest_heap_owns(replacement) &&
          isaac_vita_guest_heap_test_live_count() == limit);
    pointers[3] = replacement;

    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&final_storage) &&
          final_storage.uid == initial_storage.uid &&
          final_storage.capacity == initial_storage.capacity &&
          final_storage.tombstones == 0U &&
          s_alloc_event_count == 2U);

    isaac_vita_guest_heap_test_probe_stats_reset();
    cleanup_begin = clock();
    for (index = 0U; index < limit; ++index)
        CHECK(isaac_vita_guest_free(pointers[index]));
    cleanup_end = clock();
    CHECK(isaac_vita_guest_heap_test_live_count() == 0U);
    CHECK(isaac_vita_guest_heap_test_probe_stats_snapshot(&cleanup_stats) &&
          cleanup_stats.lock_acquisitions >= limit &&
          cleanup_stats.lock_spins == 0U &&
          cleanup_stats.find_calls >= limit &&
          cleanup_stats.find_probes >= cleanup_stats.find_calls &&
          cleanup_stats.find_max_probes > 0U);
    printf(
        "Vita heap fixed ledger probe/lock receipt: phase=cleanup "
        "live=0 capacity=%u load=%u/%u clock_ticks=%lld clock_hz=%ld "
        "locks=%zu spins=%zu find_calls=%zu find_probes=%zu find_max=%zu\n",
        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY,
        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR,
        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR,
        (long long)(cleanup_end - cleanup_begin),
        (long)CLOCKS_PER_SEC,
        cleanup_stats.lock_acquisitions, cleanup_stats.lock_spins,
        cleanup_stats.find_calls, cleanup_stats.find_probes,
        cleanup_stats.find_max_probes);
    free(pointers);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&final_storage) &&
          final_storage.uid == initial_storage.uid &&
          final_storage.capacity == initial_storage.capacity &&
          final_storage.tombstones == 0U &&
          s_alloc_event_count == 2U);
    CHECK(oracle_reset());
    return 0;
}

static int test_consumed_stranded_and_p2n_free_failure_last(void)
{
    isaac_vita_guest_heap_test_result result;
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    unsigned char *first;
    unsigned char *second;
    void *orphan_native;
    int valid_owner;

    CHECK(oracle_init());
    s_fail_native_malloc = 1U;
    first = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    s_fail_native_malloc = 1U;
    second = isaac_vita_guest_heap_test_malloc_ex(64U, &result, NULL);
    CHECK(first && second &&
          isaac_vita_heap_overflow_mspace_contains(first) &&
          isaac_vita_heap_overflow_mspace_contains(second));

    s_out_of_range_mspace_realloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              first, 128U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          !isaac_vita_guest_heap_owns(first) &&
          isaac_vita_guest_heap_terminal());
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          telemetry.owned_live_count == 1U &&
          telemetry.owned_requested_bytes == 64U &&
          telemetry.pool_allocations == 2U &&
          telemetry.pool_frees == 1U &&
          telemetry.pool_to_native == 0U &&
          telemetry.consumed_terminal == 1U &&
          telemetry.raw_live_count == 2U &&
          telemetry.raw_stranded_count == 1U &&
          telemetry.mspace_state ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY &&
          telemetry.terminal == 1U && telemetry.accounting_valid == 0U &&
          telemetry.pool_allocations - telemetry.pool_frees ==
              telemetry.owned_live_count &&
          telemetry.raw_live_count ==
              telemetry.owned_live_count + telemetry.raw_stranded_count);

    s_last_native_malloc_result = NULL;
    isaac_vita_guest_heap_test_fail_next_raw_free();
    valid_owner = 0;
    CHECK(!isaac_vita_guest_heap_test_realloc_ex(
              second, 96U, &valid_owner, &result) &&
          valid_owner && result == ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL &&
          !isaac_vita_guest_heap_owns(second));
    orphan_native = s_last_native_malloc_result;
    CHECK(orphan_native && isaac_vita_guest_heap_owns(orphan_native) &&
          isaac_vita_guest_heap_test_live_count() == 1U);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          telemetry.owned_live_count == 0U &&
          telemetry.owned_requested_bytes == 0U &&
          telemetry.pool_allocations == 2U &&
          telemetry.pool_frees == 2U &&
          telemetry.pool_to_native == 1U &&
          telemetry.consumed_terminal == 1U &&
          telemetry.raw_live_count == 2U &&
          telemetry.raw_stranded_count == 1U &&
          telemetry.terminal == 1U && telemetry.accounting_valid == 0U &&
          telemetry.pool_allocations - telemetry.pool_frees ==
              telemetry.owned_live_count &&
          telemetry.raw_live_count !=
              telemetry.owned_live_count + telemetry.raw_stranded_count);

    free(orphan_native);
    oracle_force_process_cleanup();
    return 0;
}

static int test_room_entry_integrated_routes(void)
{
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    isaac_vita_room_entry_slab_snapshot room;
    CPU cpu;
    unsigned char *pointer;
    unsigned char *replacement;
    uintptr_t allocation_base = 0U;
    uint32_t token;
    size_t native_malloc_calls;
    size_t mspace_malloc_calls;
    size_t index;
    int valid_owner = 0;

    CHECK(oracle_init());
    CHECK((uintptr_t)s_import_frame <= UINT32_MAX - sizeof s_import_frame &&
          (uintptr_t)s_oracle_overflow_block <= UINT32_MAX -
              sizeof s_oracle_overflow_block &&
          (uintptr_t)s_static_canary_allocation <= UINT32_MAX -
              sizeof s_static_canary_allocation);

    /* Exact size without the exact RoomConfig owner chain stays generic and
     * must not acquire or publish a slab page. */
    s_use_static_canary_malloc = 1U;
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    oracle_prepare_room_malloc_import(&cpu);
    s_import_frame[3] = ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA + 1U;
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax ==
              (uint32_t)(uintptr_t)s_static_canary_allocation &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls &&
          isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.pages == 0U && room.live_slots == 0U &&
          room.raw_internal_live == 0U &&
          isaac_vita_guest_heap_owns(s_static_canary_allocation));
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)s_static_canary_allocation, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          !cpu.fault && !s_static_canary_live);

    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    CHECK(isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_test_live_count() == 0U &&
          isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.pages == 1U && room.live_slots == 1U &&
          room.raw_internal_live == 1U &&
          room.raw_internal_requested ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES &&
          isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          oracle_telemetry_invariants(&telemetry) &&
          telemetry.raw_live_count == 1U &&
          telemetry.raw_internal_live_count == 1U &&
          telemetry.raw_internal_requested_bytes ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);

    token = isaac_vita_guest_heap_lease_containing(
        pointer + 7U, 5U, &allocation_base);
    CHECK(token && allocation_base == (uintptr_t)pointer);
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          cpu.fault && isaac_vita_guest_heap_owns(pointer));
    CHECK(isaac_vita_guest_heap_lease_release(token));
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          !cpu.fault && !isaac_vita_guest_heap_owns(pointer));

    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          !cpu.fault && !isaac_vita_guest_heap_owns(pointer));

    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    CHECK(isaac_vita_guest_free(pointer) &&
          !isaac_vita_guest_heap_owns(pointer));

    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    oracle_prepare_bound_heap_import(
        &cpu, UINT32_C(0x0badc0de), (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          !cpu.fault && cpu.eax == 0U &&
          !isaac_vita_guest_heap_owns(pointer));

    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    oracle_prepare_bound_heap_import(
        &cpu, UINT32_C(0x0badc0de), (uint32_t)(uintptr_t)pointer,
        ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          !cpu.fault && cpu.eax == (uint32_t)(uintptr_t)pointer &&
          isaac_vita_guest_heap_owns(pointer));
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        pointer[index] = (unsigned char)(0x40U + index);
    s_use_static_canary_malloc = 1U;
    oracle_prepare_bound_heap_import(
        &cpu, UINT32_C(0x0badc0de), (uint32_t)(uintptr_t)pointer, 8U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    replacement = (unsigned char *)(uintptr_t)cpu.eax;
    CHECK(replacement == s_static_canary_allocation &&
          !isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_owns(replacement));
    for (index = 0U; index < 8U; ++index)
        CHECK(replacement[index] == (unsigned char)(0x40U + index));
    for (index = 8U; index < sizeof s_static_canary_allocation; ++index)
        CHECK(replacement[index] == 0xccU);
    token = isaac_vita_guest_heap_lease_containing(
        replacement + 7U, 1U, &allocation_base);
    CHECK(token && allocation_base == (uintptr_t)replacement &&
          isaac_vita_guest_heap_lease_release(token));
    CHECK(!isaac_vita_guest_heap_lease_containing(
              replacement + 8U, 1U, &allocation_base));
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.live_slots == 0U && room.moving_slots == 0U &&
          room.move_attempts == 1U && room.moves == 1U &&
          room.known_normal_frees == 1U &&
          room.known_stage_frees == 1U && room.unexpected_frees == 1U);

    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)replacement, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          !cpu.fault && !s_static_canary_live &&
          !isaac_vita_guest_heap_owns(replacement));
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          cpu.fault);

    /* The public ownership boundary must use the same slab transaction. */
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        pointer[index] = (unsigned char)(0x70U + index);
    s_use_static_canary_malloc = 1U;
    replacement = isaac_vita_guest_realloc(pointer, 8U, &valid_owner);
    CHECK(valid_owner && replacement == s_static_canary_allocation &&
          !isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_owns(replacement));
    for (index = 0U; index < 8U; ++index)
        CHECK(replacement[index] == (unsigned char)(0x70U + index));
    for (index = 8U; index < sizeof s_static_canary_allocation; ++index)
        CHECK(replacement[index] == 0xccU);
    CHECK(isaac_vita_guest_free(replacement) && !s_static_canary_live);

    /* Ordinary OOM while allocating a generic MOVE replacement cancels the
     * moving bit and preserves the exact slab allocation byte-for-byte. */
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        pointer[index] = (unsigned char)(0xa0U + index);
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    s_fail_native_malloc = 1U;
    s_fail_mspace_malloc = 1U;
    oracle_prepare_bound_heap_import(
        &cpu, UINT32_C(0x0badc0de), (uint32_t)(uintptr_t)pointer, 8U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          !cpu.fault && cpu.eax == 0U &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls + 1U &&
          !isaac_vita_guest_heap_terminal() &&
          isaac_vita_guest_heap_owns(pointer));
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        CHECK(pointer[index] == (unsigned char)(0xa0U + index));
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.live_slots == 1U && room.moving_slots == 0U &&
          room.move_attempts == room.moves + 1U);
    CHECK(isaac_vita_guest_free(pointer) &&
          !isaac_vita_guest_heap_owns(pointer));

    CHECK(oracle_reset());
    return 0;
}

static int test_room_entry_external_integrated_routes(void)
{
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    isaac_vita_room_entry_slab_snapshot room;
    CPU cpu;
    unsigned char *pointer;
    unsigned char *replacement;
    uintptr_t allocation_base = 0U;
    uint32_t token;
    size_t index;
    size_t native_malloc_calls;
    size_t mspace_malloc_calls;
    size_t external_alloc_calls = s_external_alloc_calls;
    size_t external_get_base_calls = s_external_get_base_calls;
    size_t external_free_calls = s_external_free_calls;
    int valid_owner = 0;

    CHECK(oracle_init());
    CHECK((uintptr_t)s_oracle_room_external_blocks <=
              UINT32_MAX - sizeof s_oracle_room_external_blocks);

    /* Force only the raw backing attempt to miss.  The exact owner must then
     * publish an EXTERNAL page without entering the generic guest ledger. */
    s_fail_mspace_malloc = 1U;
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    CHECK(isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_test_live_count() == 0U &&
          isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.pages == 1U && room.raw_pages == 0U &&
          room.external_pages == 1U && room.external_chunks == 1U &&
          room.live_slots == 1U && room.raw_internal_live == 0U &&
          room.external_requested ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES &&
          s_external_alloc_calls == external_alloc_calls + 1U &&
          s_external_get_base_calls == external_get_base_calls + 1U &&
          s_external_free_calls == external_free_calls &&
          isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          oracle_telemetry_invariants(&telemetry) &&
          telemetry.raw_live_count == 0U &&
          telemetry.raw_internal_live_count == 0U);

    token = isaac_vita_guest_heap_lease_containing(
        pointer + 7U, 5U, &allocation_base);
    CHECK(token && allocation_base == (uintptr_t)pointer);
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          cpu.fault && isaac_vita_guest_heap_owns(pointer));
    CHECK(isaac_vita_guest_heap_lease_release(token));
    CHECK(!isaac_vita_guest_heap_lease_containing(
              pointer + ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, 1U,
              &allocation_base));
    CHECK(!isaac_vita_guest_free(pointer + 1U) &&
          isaac_vita_guest_heap_owns(pointer));
    oracle_prepare_bound_heap_import(
        &cpu, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        (uint32_t)(uintptr_t)pointer, 0U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_FREE_NAME) &&
          !cpu.fault && !isaac_vita_guest_heap_owns(pointer));
    CHECK(!isaac_vita_guest_free(pointer));

    /* The imported realloc boundary shares the same EXTERNAL transaction and
     * must copy only eight bytes into the guarded generic replacement. */
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        pointer[index] = (unsigned char)(0x20U + index);
    s_use_static_canary_malloc = 1U;
    oracle_prepare_bound_heap_import(
        &cpu, UINT32_C(0x0badc0de), (uint32_t)(uintptr_t)pointer, 8U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    replacement = (unsigned char *)(uintptr_t)cpu.eax;
    CHECK(replacement == s_static_canary_allocation &&
          !isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_owns(replacement));
    for (index = 0U; index < 8U; ++index)
        CHECK(replacement[index] == (unsigned char)(0x20U + index));
    for (index = 8U; index < sizeof s_static_canary_allocation; ++index)
        CHECK(replacement[index] == 0xccU);
    CHECK(isaac_vita_guest_free(replacement) && !s_static_canary_live);

    /* Generic replacement OOM cancels MOVE and keeps the external slot live,
     * exact and byte-identical. */
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (unsigned char *)(uintptr_t)cpu.eax;
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        pointer[index] = (unsigned char)(0x90U + index);
    valid_owner = 0;
    replacement = isaac_vita_guest_realloc(
        pointer, ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, &valid_owner);
    CHECK(valid_owner && replacement == pointer &&
          isaac_vita_guest_heap_owns(pointer));
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    s_fail_native_malloc = 1U;
    s_fail_mspace_malloc = 1U;
    valid_owner = 0;
    CHECK(!isaac_vita_guest_realloc(pointer, 8U, &valid_owner) &&
          valid_owner && !isaac_vita_guest_heap_terminal() &&
          isaac_vita_guest_heap_owns(pointer) &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_mspace_malloc_calls == mspace_malloc_calls + 1U);
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        CHECK(pointer[index] == (unsigned char)(0x90U + index));
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.raw_pages == 0U && room.external_pages == 1U &&
          room.live_slots == 1U && room.moving_slots == 0U &&
          room.move_attempts == room.moves + 1U);
    CHECK(isaac_vita_guest_free(pointer) &&
          !isaac_vita_guest_heap_owns(pointer));

    CHECK(oracle_reset());
    CHECK(s_external_free_calls == external_free_calls + 1U);
    return 0;
}

static int test_room_reset_outer_release_retry(void)
{
    isaac_vita_room_entry_slab_snapshot room;
    CPU cpu;
    size_t external_frees;
    size_t ledger_frees;
    size_t mspace_frees;
    size_t overflow_frees;
    uint32_t index;

    CHECK(oracle_init());
    for (index = 0U; index < ORACLE_ROOM_RESET_SLOTS; ++index) {
        if (index == ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE)
            s_fail_mspace_malloc = 1U;
        oracle_prepare_room_malloc_import(&cpu);
        CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
              !cpu.fault && cpu.eax != 0U);
        s_room_reset_pointers[index] = (void *)(uintptr_t)cpu.eax;
    }
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.raw_pages == 1U && room.external_pages == 1U &&
          room.external_chunks == 1U &&
          room.live_slots == ORACLE_ROOM_RESET_SLOTS);
    for (index = 0U; index < ORACLE_ROOM_RESET_SLOTS; ++index)
        CHECK(isaac_vita_guest_free(s_room_reset_pointers[index]));

    external_frees = s_external_free_calls;
    ledger_frees = s_ledger_free_calls;
    mspace_frees = s_mspace_free_calls;
    overflow_frees = s_overflow_free_calls;
    s_fail_overflow_free = 1U;
    s_fail_ledger_free = 1U;

    /* Slab teardown succeeds once; the raw backing receipt then fails. */
    CHECK(!isaac_vita_guest_heap_test_storage_reset());
    CHECK(s_external_free_calls == external_frees + 1U &&
          s_mspace_free_calls == mspace_frees + 1U &&
          s_overflow_free_calls == overflow_frees + 1U &&
          s_ledger_free_calls == ledger_frees);

    /* The clean-zero slab/external phase is idempotent.  Retry releases the
     * exact raw receipt once, then stops on the independently retained ledger. */
    CHECK(!isaac_vita_guest_heap_test_storage_reset());
    CHECK(s_external_free_calls == external_frees + 1U &&
          s_mspace_free_calls == mspace_frees + 1U &&
          s_overflow_free_calls == overflow_frees + 2U &&
          s_ledger_free_calls == ledger_frees + 1U);

    CHECK(isaac_vita_guest_heap_test_storage_reset());
    CHECK(s_external_free_calls == external_frees + 1U &&
          s_mspace_free_calls == mspace_frees + 1U &&
          s_overflow_free_calls == overflow_frees + 2U &&
          s_ledger_free_calls == ledger_frees + 2U &&
          oracle_live_memblocks() == 0U && !s_mspace.live);
    return 0;
}

static int test_room_commit_terminal_mode(void)
{
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    isaac_vita_guest_heap_floor_lifetime_snapshot heap_floor;
    isaac_vita_room_entry_slab_floor_lifetime_snapshot slab_floor;
    isaac_vita_guest_heap_test_slab_move_floor_receipt receipt;
    isaac_vita_room_entry_slab_snapshot room;
    CPU cpu;
    void *sentinel;
    void *pointer;
    size_t native_malloc_calls;
    size_t native_free_calls;

    CHECK(oracle_init());
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_bootstrap(
              ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
    sentinel = isaac_vita_guest_malloc(32U);
    CHECK(sentinel && isaac_vita_guest_heap_owns(sentinel));
    s_fail_mspace_malloc = 1U;
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (void *)(uintptr_t)cpu.eax;
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_phase_set(
              ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD));
    s_use_static_canary_malloc = 1U;
    native_malloc_calls = s_native_malloc_calls;
    native_free_calls = s_native_free_calls;
    isaac_vita_guest_heap_test_slab_move_floor_receipt_reset();
    isaac_vita_guest_heap_test_room_fail_next_commit();
    oracle_prepare_bound_heap_import(
        &cpu, UINT32_C(0x0badc0de), (uint32_t)(uintptr_t)pointer, 8U);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_REALLOC_NAME) &&
          cpu.fault && isaac_vita_guest_heap_terminal() &&
          s_native_malloc_calls == native_malloc_calls + 1U &&
          s_native_free_calls == native_free_calls + 1U &&
          !s_static_canary_live &&
          !isaac_vita_guest_heap_owns(s_static_canary_allocation) &&
          isaac_vita_guest_heap_owns(pointer));
    CHECK(isaac_vita_guest_heap_test_slab_move_floor_receipt_snapshot(
              &receipt) &&
          receipt.attempts == 1U &&
          receipt.replacement_original_token ==
              (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD |
               ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT) &&
          receipt.inherited_token ==
              (ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT |
               ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT) &&
          receipt.replacement_final_token ==
              receipt.replacement_original_token &&
          receipt.retag_succeeded == 1U &&
          receipt.commit_succeeded == 0U &&
          receipt.rollback_attempted == 1U &&
          receipt.rollback_succeeded == 1U);
    CHECK(isaac_vita_guest_heap_floor_lifetime_snapshot_get(&heap_floor) &&
          heap_floor.terminal == 1U &&
          heap_floor.total_count == 1U &&
          heap_floor.current_count == 1U &&
          heap_floor.current_requested_bytes == 32U &&
          heap_floor.phase_count[0] == 1U &&
          heap_floor.phase_count[1] == 0U &&
          heap_floor.phase_requested_bytes[0] == 32U &&
          heap_floor.phase_requested_bytes[1] == 0U);
    CHECK(isaac_vita_guest_heap_test_room_floor_lifetime_snapshot(
              &slab_floor) &&
          slab_floor.terminal == 1U && slab_floor.live_slots == 1U &&
          slab_floor.current_slots == 1U &&
          slab_floor.level_init_slots == 1U &&
          slab_floor.room_load_slots == 0U);
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.terminal && room.live_slots == 1U &&
          room.moving_slots == 1U && room.moves == 0U &&
          room.raw_pages == 0U && room.external_pages == 1U &&
          room.external_chunks == 1U);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          telemetry.terminal && oracle_telemetry_invariants(&telemetry) &&
          telemetry.raw_live_count == 0U &&
          telemetry.raw_internal_live_count == 0U &&
          telemetry.raw_internal_requested_bytes == 0U);
    native_malloc_calls = s_native_malloc_calls;
    CHECK(!isaac_vita_guest_malloc(16U) &&
          s_native_malloc_calls == native_malloc_calls);
    /* Pointer APIs classify before consulting slab terminal, so unrelated
     * generic unwind remains legal after the terminal latch. */
    CHECK(isaac_vita_guest_free(sentinel) &&
          !isaac_vita_guest_heap_owns(sentinel));
    CHECK(!isaac_vita_guest_heap_test_storage_reset());
    oracle_force_process_cleanup();
    CHECK(oracle_live_memblocks() == 0U && !s_static_canary_live);
    return 0;
}

static int test_room_invariant_terminal_mode(void)
{
    CPU cpu;
    void *pointer;
    size_t native_malloc_calls;
    size_t mspace_malloc_calls;

    CHECK(oracle_init());
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          !cpu.fault && cpu.eax != 0U);
    pointer = (void *)(uintptr_t)cpu.eax;
    isaac_vita_guest_heap_test_room_corrupt_nonfull();
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          cpu.fault && cpu.eax == 0U &&
          isaac_vita_guest_heap_terminal() &&
          isaac_vita_guest_heap_owns(pointer) &&
          s_native_malloc_calls == native_malloc_calls &&
          s_mspace_malloc_calls == mspace_malloc_calls);
    native_malloc_calls = s_native_malloc_calls;
    CHECK(!isaac_vita_guest_malloc(16U) &&
          s_native_malloc_calls == native_malloc_calls);
    oracle_force_process_cleanup();
    return 0;
}

static int test_room_cold_invariant_terminal_mode(void)
{
    CPU cpu;
    isaac_vita_room_entry_slab_snapshot room;
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    size_t native_malloc_calls;
    size_t mspace_malloc_calls;

    CHECK(oracle_init());
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    /* The first matching raw snapshot validates page acquisition.  Corrupt
     * only the following cold publication preflight, while all fast slab
     * invariants remain valid; transactional rollback must release backing. */
    s_corrupt_room_raw_snapshot_match = 2U;
    oracle_prepare_room_malloc_import(&cpu);
    CHECK(isaac_vita_heap_import(&cpu, ISAAC_VITA_HEAP_MALLOC_NAME) &&
          cpu.fault && cpu.eax == 0U &&
          s_corrupt_room_raw_snapshot_match == 0U &&
          isaac_vita_guest_heap_terminal() &&
          s_native_malloc_calls == native_malloc_calls &&
          s_mspace_malloc_calls == mspace_malloc_calls + 1U);
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&room) &&
          room.terminal && room.pages == 0U && room.live_slots == 0U &&
          room.moving_slots == 0U && room.allocations == 0U &&
          room.fallbacks == 0U && room.raw_internal_live == 0U &&
          room.raw_internal_requested == 0U);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry) &&
          telemetry.terminal && oracle_telemetry_invariants(&telemetry) &&
          telemetry.raw_live_count == 0U &&
          telemetry.raw_internal_live_count == 0U &&
          telemetry.raw_internal_requested_bytes == 0U);
    native_malloc_calls = s_native_malloc_calls;
    mspace_malloc_calls = s_mspace_malloc_calls;
    CHECK(!isaac_vita_guest_malloc(16U) &&
          s_native_malloc_calls == native_malloc_calls &&
          s_mspace_malloc_calls == mspace_malloc_calls);
    oracle_force_process_cleanup();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--room-commit-terminal") == 0) {
        CHECK(test_room_commit_terminal_mode() == 0);
        CHECK(s_bad_api_calls == 0U);
        puts("Vita RoomConfig Entry commit-terminal router oracle: PASS");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--room-invariant-terminal") == 0) {
        CHECK(test_room_invariant_terminal_mode() == 0);
        CHECK(s_bad_api_calls == 0U);
        puts("Vita RoomConfig Entry invariant-terminal router oracle: PASS");
        return 0;
    }
    if (argc == 2 &&
        strcmp(argv[1], "--room-cold-invariant-terminal") == 0) {
        CHECK(test_room_cold_invariant_terminal_mode() == 0);
        CHECK(s_bad_api_calls == 0U);
        puts("Vita RoomConfig Entry cold-invariant terminal router oracle: PASS");
        return 0;
    }
    CHECK(argc == 1);
    CHECK(test_ranges_and_setup() == 0);
    CHECK(test_room_entry_integrated_routes() == 0);
    CHECK(test_room_entry_external_integrated_routes() == 0);
    CHECK(test_floor_lifetime_slab_move_categories() == 0);
    CHECK(test_room_reset_outer_release_retry() == 0);
    CHECK(test_floor_lifetime_migration_preservation() == 0);
    CHECK(test_domains_leases_and_realloc() == 0);
    CHECK(test_ready_raw_terminal_latch() == 0);
    CHECK(test_drain_only_statuses() == 0);
    CHECK(test_telemetry_success_and_rollback() == 0);
    CHECK(test_fixed_capacity() == 0);
    CHECK(test_consumed_stranded_and_p2n_free_failure_last() == 0);
    CHECK(s_bad_api_calls == 0U);
    CHECK(oracle_live_memblocks() == 0U);
    CHECK(s_log_truncations == 0U && s_log_under_lock == 0U &&
          s_log_dropped == 0U);
    puts("Vita heap overflow integrated router oracle: PASS");
    return 0;
}
