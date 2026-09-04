/* Native lifecycle oracle for the exact ANM2 scratch transaction. */
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_anm2_scratch.h"

#define ORACLE_LOG_CAPACITY 24U
#define ORACLE_LOG_BYTES    320U
#define ORACLE_STACK_FLOOR  0x82000000U
#define ORACLE_STACK_CEILING 0x82400000U
#define ORACLE_POST_IMAGE_BASE \
    (ISAAC_VITA_ANM2_GUEST_IMAGE_END + \
     ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES)
#define ORACLE_POST_IMAGE_REQUEST_END \
    (ORACLE_POST_IMAGE_BASE + ISAAC_VITA_ANM2_MEMBLOCK_BYTES)
#define ORACLE_POST_IMAGE_RETAINED_END \
    (ORACLE_POST_IMAGE_BASE + ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX)

_Alignas(ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES)
static unsigned char s_block[ISAAC_VITA_ANM2_MEMBLOCK_BYTES];
static int s_alloc_result;
static int s_get_result;
static int s_free_result;
static size_t s_base_bias;
static uintptr_t s_literal_base;
static int s_block_live;
static unsigned s_alloc_calls;
static unsigned s_get_calls;
static unsigned s_free_calls;
static SceKernelMemBlockType s_last_type;
static SceSize s_last_size;
static SceKernelAllocMemBlockOpt *s_last_option;
static const char *s_last_name;
static unsigned s_log_count;
static char s_logs[ORACLE_LOG_CAPACITY][ORACLE_LOG_BYTES];

static int s_inject_concurrency;
static int s_thread_create_result;
static int s_concurrent_result;
static isaac_vita_anm2_scratch_decision s_concurrent_decision;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ANM2 scratch oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void *concurrent_malloc(void *unused)
{
    (void)unused;
    s_concurrent_result = isaac_vita_anm2_scratch_malloc(
        ISAAC_VITA_ANM2_OWNER_RETURN_1,
        ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING,
        &s_concurrent_decision);
    return NULL;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_alloc_calls;
    s_last_name = name;
    s_last_type = type;
    s_last_size = size;
    s_last_option = option;
    if (s_alloc_result < 0)
        return (SceUID)s_alloc_result;
    if (s_block_live)
        return (SceUID)-0x701;
    s_block_live = 1;

    if (s_inject_concurrency) {
        pthread_t thread;

        s_inject_concurrency = 0;
        memset(&s_concurrent_decision, 0, sizeof s_concurrent_decision);
        s_concurrent_result = 99;
        s_thread_create_result = pthread_create(
            &thread, NULL, concurrent_malloc, NULL);
        if (s_thread_create_result == 0)
            s_thread_create_result = pthread_join(thread, NULL);
    }
    return (SceUID)s_alloc_result;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_get_calls;
    if (uid != (SceUID)s_alloc_result || !s_block_live)
        return -0x702;
    if (s_get_result < 0) {
        if (base)
            *base = NULL;
        return s_get_result;
    }
    if (!base)
        return -0x703;
    *base = s_literal_base
        ? (void *)s_literal_base : s_block + s_base_bias;
    return s_get_result;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_free_calls;
    if (uid != (SceUID)s_alloc_result || !s_block_live)
        return -0x704;
    if (s_free_result >= 0)
        s_block_live = 0;
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

static void reset_observations(void)
{
    s_alloc_calls = 0U;
    s_get_calls = 0U;
    s_free_calls = 0U;
    s_last_type = (SceKernelMemBlockType)0;
    s_last_size = 0U;
    s_last_option = (SceKernelAllocMemBlockOpt *)(uintptr_t)1U;
    s_last_name = NULL;
    s_log_count = 0U;
    memset(s_logs, 0, sizeof s_logs);
    s_thread_create_result = -1;
    s_concurrent_result = 99;
    memset(&s_concurrent_decision, 0, sizeof s_concurrent_decision);
}

static int alloc_site(unsigned index,
                      isaac_vita_anm2_scratch_decision *decision)
{
    static const uint32_t owners[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
        ISAAC_VITA_ANM2_OWNER_RETURN_0,
        ISAAC_VITA_ANM2_OWNER_RETURN_1,
        ISAAC_VITA_ANM2_OWNER_RETURN_2,
        ISAAC_VITA_ANM2_OWNER_RETURN_3,
        ISAAC_VITA_ANM2_OWNER_RETURN_4,
        ISAAC_VITA_ANM2_OWNER_RETURN_5
    };
    size_t size = index < 4U
        ? ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES
        : ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES;

    return isaac_vita_anm2_scratch_malloc(
        owners[index], size, ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING,
        decision);
}

/* Model use of a guest-visible address through the fake memblock's backing
 * store.  This lets the post-image placement case exercise every returned
 * segment without dereferencing an unmapped synthetic 32-bit host address. */
static int use_literal_segment(void *pointer, size_t size,
                               unsigned char value)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = s_literal_base;
    size_t offset;

    if (!base || address < base || address - base > sizeof s_block)
        return 0;
    offset = (size_t)(address - base);
    if (size > sizeof s_block - offset)
        return 0;
    memset(s_block + offset, value, size);
    return s_block[offset] == value && s_block[offset + size - 1U] == value;
}

int main(void)
{
    isaac_vita_anm2_scratch_decision decision;
    isaac_vita_anm2_scratch_snapshot snapshot;
    void *partial[3];
    void *complete[ISAAC_VITA_ANM2_SEGMENT_COUNT];
    static const size_t offsets[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
        0U, 420000U, 840000U, 1260000U, 1680000U, 2220000U
    };
    static const unsigned free_order[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
        4U, 0U, 5U, 1U, 3U, 2U
    };
    unsigned index;
    int result;

    CHECK(ISAAC_VITA_ANM2_PAYLOAD_BYTES ==
              4U * 420000U + 2U * 540000U);
    CHECK(ISAAC_VITA_ANM2_MEMBLOCK_BYTES == 2760704U);
    CHECK(ISAAC_VITA_ANM2_LINK_END == 0x82ad1560U);
    CHECK(ISAAC_VITA_ANM2_HEAP_RETAINED_END == 0x87cd2000U);
    CHECK(ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES == 0x1032e000U);
    CHECK(ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX == 0x002a4000U);
    CHECK(ISAAC_VITA_ANM2_GAP_REMAINING_BYTES == 0x1008a000U);
    CHECK((uint32_t)ISAAC_VITA_ANM2_NO_FREE_PHYSICAL_PAGE ==
          UINT32_C(0x80024302));
    CHECK(((uintptr_t)s_block & 4095U) == 0U);
    CHECK(ORACLE_POST_IMAGE_BASE > ISAAC_VITA_ANM2_GUEST_IMAGE_END);
    CHECK((ORACLE_POST_IMAGE_BASE &
           (ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES - 1U)) == 0U);
    CHECK(ORACLE_POST_IMAGE_REQUEST_END == 0x98b02000U);
    CHECK(ORACLE_POST_IMAGE_RETAINED_END == 0x98b04000U);
    CHECK(ORACLE_POST_IMAGE_RETAINED_END > ORACLE_POST_IMAGE_REQUEST_END);
    CHECK(ORACLE_POST_IMAGE_BASE >= ORACLE_STACK_CEILING ||
          ORACLE_POST_IMAGE_RETAINED_END <= ORACLE_STACK_FLOOR);
    CHECK(ORACLE_POST_IMAGE_BASE >= ISAAC_VITA_ANM2_GUEST_IMAGE_END);

    reset_observations();
    memset(&decision, 0xa5, sizeof decision);
    result = isaac_vita_anm2_scratch_malloc(
        0x0000aa5cU, ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED);
    CHECK(!decision.pointer && !decision.fault &&
          decision.fault_value == 0U && s_alloc_calls == 0U);

    result = isaac_vita_anm2_scratch_malloc(
        ISAAC_VITA_ANM2_OWNER_RETURN_0,
        ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES + 1U,
        ORACLE_STACK_FLOOR, ORACLE_STACK_CEILING, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED);
    CHECK(decision.fault && strstr(decision.fault, "wrong size") &&
          decision.fault_value ==
              ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES + 1U &&
          s_alloc_calls == 0U);

    result = isaac_vita_anm2_scratch_malloc(
        ISAAC_VITA_ANM2_OWNER_RETURN_0,
        ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
        ORACLE_STACK_FLOOR, ORACLE_STACK_FLOOR, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED);
    CHECK(decision.fault && strstr(decision.fault, "stack range") &&
          s_alloc_calls == 0U);

    result = alloc_site(1U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED);
    CHECK(decision.fault && strstr(decision.fault, "start at AA5D") &&
          s_alloc_calls == 0U);

    /* A clean native allocation failure remains a handled NULL result, so
     * operator new owns the unchanged bad_alloc path and no newlib fallback
     * is possible. */
    s_alloc_result = -0x111;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_HANDLED &&
          !decision.pointer && !decision.fault);
    CHECK(s_alloc_calls == 1U && s_get_calls == 0U &&
          s_free_calls == 0U && s_log_count == 1U);
    CHECK(strstr(s_logs[0], "status=alloc-failed") &&
          strstr(s_logs[0], "payload=2760000 block=2760704"));
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == 0U && snapshot.uid == -1 &&
          snapshot.session_id == 0U && snapshot.live_count == 0U);

    /* Get-base failure rolls the complete native transaction back and leaves
     * the first exact site retryable. */
    s_alloc_result = 0x44;
    s_get_result = -0x222;
    s_free_result = 0;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_HANDLED && !decision.pointer);
    CHECK(s_alloc_calls == 2U && s_get_calls == 1U &&
          s_free_calls == 1U && !s_block_live);
    CHECK(strstr(s_logs[1], "status=get-base-failed") &&
          strstr(s_logs[1], "rollback=0x00000000"));

    /* A nonconforming base is never exposed. */
    s_get_result = 0;
    s_base_bias = 1U;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_HANDLED && !decision.pointer);
    CHECK(s_alloc_calls == 3U && s_get_calls == 2U &&
          s_free_calls == 2U && !s_block_live);
    CHECK(strstr(s_logs[2], "status=invalid-base"));

    /* A well-aligned syscall result must still roll back if its full range
     * intersects either frozen PE storage or the active guarded guest stack. */
    s_base_bias = 0U;
    s_literal_base = ISAAC_VITA_ANM2_GUEST_IMAGE_BASE;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_HANDLED && !decision.pointer);
    CHECK(s_alloc_calls == 4U && s_get_calls == 3U &&
          s_free_calls == 3U && !s_block_live);
    CHECK(strstr(s_logs[3], "status=range-overlap") &&
          strstr(s_logs[3], "base=0x98000000") &&
          strstr(s_logs[3], "image=0x98000000..0x9885f000"));

    s_literal_base = ORACLE_STACK_FLOOR + 0x100000U;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_HANDLED && !decision.pointer);
    CHECK(s_alloc_calls == 5U && s_get_calls == 4U &&
          s_free_calls == 4U && !s_block_live);
    CHECK(strstr(s_logs[4], "status=range-overlap") &&
          strstr(s_logs[4], "stack=0x82000000..0x82400000"));

    /* If rollback itself fails, poison the seam permanently rather than
     * allocating a second block or silently using newlib. */
    s_literal_base = 0U;
    s_get_result = -0x333;
    s_free_result = -0x444;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "rollback failed"));
    CHECK(s_block_live && s_alloc_calls == 6U && s_get_calls == 5U &&
          s_free_calls == 5U);
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.poisoned == 1U && snapshot.uid == 0x44 &&
          snapshot.base == 0U);
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "poisoned") &&
          s_alloc_calls == 6U);
    s_free_result = 0;
    CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    CHECK(!s_block_live && s_free_calls == 6U);

    /* Eager ledger/pool blocks may consume the old pre-image gap.  Prove that
     * a page-aligned USER_RW block strictly after the image is a complete,
     * usable allocation: its requested and Vita3K-retained bounds avoid both
     * the frozen image and active stack, all six segments are usable, and all
     * six exact frees are accepted before the retained block is reset. */
    reset_observations();
    s_alloc_result = 0x88;
    s_get_result = 0;
    s_free_result = 0;
    s_base_bias = 0U;
    s_literal_base = ORACLE_POST_IMAGE_BASE;
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        size_t segment_size = index < 4U
            ? ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES
            : ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES;

        CHECK(alloc_site(index, &decision) ==
              ISAAC_VITA_ANM2_SCRATCH_HANDLED);
        complete[index] = decision.pointer;
        CHECK((uintptr_t)complete[index] ==
              (uintptr_t)ORACLE_POST_IMAGE_BASE + offsets[index]);
        CHECK(use_literal_segment(
                  complete[index], segment_size,
                  (unsigned char)(0x31U + index)));
    }
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == (uintptr_t)ORACLE_POST_IMAGE_BASE &&
          snapshot.uid == 0x88 && !snapshot.poisoned &&
          snapshot.live_count == ISAAC_VITA_ANM2_SEGMENT_COUNT &&
          snapshot.acquired_count == ISAAC_VITA_ANM2_SEGMENT_COUNT &&
          snapshot.peak_live == ISAAC_VITA_ANM2_SEGMENT_COUNT);
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        CHECK(isaac_vita_anm2_scratch_free(
                  complete[free_order[index]], &decision) ==
              ISAAC_VITA_ANM2_SCRATCH_HANDLED);
    }
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == (uintptr_t)ORACLE_POST_IMAGE_BASE &&
          snapshot.live_count == 0U &&
          snapshot.freed_count == ISAAC_VITA_ANM2_SEGMENT_COUNT);
    CHECK(s_alloc_calls == 1U && s_get_calls == 1U &&
          s_free_calls == 0U && s_block_live);
    CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    CHECK(!s_block_live && s_free_calls == 1U);

    /* Reserve once.  The fake syscall starts another native thread while the
     * lifecycle lock is held; that thread must reject immediately instead of
     * waiting, allocating, or observing partial state. */
    reset_observations();
    s_alloc_result = 0x1234;
    s_get_result = 0;
    s_free_result = 0;
    s_base_bias = 0U;
    s_literal_base = 0U;
    s_inject_concurrency = 1;
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_HANDLED && decision.pointer);
    partial[0] = decision.pointer;
    CHECK(s_thread_create_result == 0);
    CHECK(s_concurrent_result == ISAAC_VITA_ANM2_SCRATCH_REJECTED);
    CHECK(s_concurrent_decision.fault &&
          strstr(s_concurrent_decision.fault, "concurrent/reentrant"));
    CHECK(s_alloc_calls == 1U && s_get_calls == 1U &&
          s_free_calls == 0U && s_block_live);
    CHECK(s_last_name && strcmp(s_last_name, "isaac_anm2_scratch") == 0);
    CHECK(s_last_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    CHECK(s_last_size == ISAAC_VITA_ANM2_MEMBLOCK_BYTES);
    CHECK(s_last_option == NULL);
    CHECK(((uintptr_t)partial[0] & 15U) == 0U);
    CHECK(s_log_count == 2U);
    CHECK(strstr(s_logs[0], "status=ready") &&
          strstr(s_logs[1], "session: id=1"));

    for (index = 1U; index < 3U; ++index) {
        CHECK(alloc_site(index, &decision) ==
              ISAAC_VITA_ANM2_SCRATCH_HANDLED);
        partial[index] = decision.pointer;
        CHECK((uintptr_t)partial[index] ==
              (uintptr_t)partial[0] + offsets[index]);
        CHECK(((uintptr_t)partial[index] & 15U) == 0U);
    }
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "overlap/reentrancy"));

    /* Interior pointers stay foreign.  Exact bases are freed without a
     * kernel free; a repeated exact base is a loud double-free rejection. */
    result = isaac_vita_anm2_scratch_free(
        (unsigned char *)partial[1] + 16U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED &&
          s_free_calls == 0U);
    CHECK(isaac_vita_anm2_scratch_free(partial[2], &decision) ==
          ISAAC_VITA_ANM2_SCRATCH_HANDLED);
    result = isaac_vita_anm2_scratch_free(partial[2], &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "double free"));
    CHECK(isaac_vita_anm2_scratch_free(partial[1], &decision) ==
          ISAAC_VITA_ANM2_SCRATCH_HANDLED);
    CHECK(isaac_vita_anm2_scratch_free(partial[0], &decision) ==
          ISAAC_VITA_ANM2_SCRATCH_HANDLED);
    CHECK(s_free_calls == 0U && s_log_count == 3U);
    CHECK(strstr(s_logs[2], "acquired=3 freed=3 peak=3 complete=no"));

    result = isaac_vita_anm2_scratch_malloc(
        ISAAC_VITA_ANM2_OWNER_RETURN_0,
        ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,
        ORACLE_STACK_FLOOR + 0x1000U, ORACLE_STACK_CEILING + 0x1000U,
        &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "stack owner changed"));

    /* A second session reuses the exact bases.  An out-of-order exact owner
     * rejects without consuming its segment, after which the valid sequence
     * can continue. */
    CHECK(alloc_site(0U, &decision) ==
          ISAAC_VITA_ANM2_SCRATCH_HANDLED);
    complete[0] = decision.pointer;
    CHECK(complete[0] == partial[0]);
    result = alloc_site(2U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "order drifted"));
    for (index = 1U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        CHECK(alloc_site(index, &decision) ==
              ISAAC_VITA_ANM2_SCRATCH_HANDLED);
        complete[index] = decision.pointer;
        CHECK((uintptr_t)complete[index] ==
              (uintptr_t)complete[0] + offsets[index]);
        CHECK(((uintptr_t)complete[index] & 15U) == 0U);
    }
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        unsigned victim = free_order[index];
        CHECK(isaac_vita_anm2_scratch_free(
                  complete[victim], &decision) ==
              ISAAC_VITA_ANM2_SCRATCH_HANDLED);
    }
    CHECK(s_free_calls == 0U && s_log_count == 5U);
    CHECK(strstr(s_logs[3], "session: id=2"));
    CHECK(strstr(s_logs[4],
                 "acquired=6 freed=6 peak=6 complete=yes"));
    result = isaac_vita_anm2_scratch_free(complete[5], &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "double free"));

    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == (uintptr_t)s_block && snapshot.uid == 0x1234 &&
          !snapshot.poisoned && snapshot.session_id == 2U &&
          snapshot.live_count == 0U && snapshot.acquired_count == 6U &&
          snapshot.freed_count == 6U && snapshot.peak_live == 6U);

    /* Successful session 3 is silent; the next power-of-two session keeps
     * one start and one complete summary.  This pins bounded native-log I/O
     * without hiding an incomplete session. */
    for (unsigned session = 3U; session <= 4U; ++session) {
        unsigned logs_before = s_log_count;

        for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
            CHECK(alloc_site(index, &decision) ==
                  ISAAC_VITA_ANM2_SCRATCH_HANDLED);
            complete[index] = decision.pointer;
        }
        for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
            CHECK(isaac_vita_anm2_scratch_free(
                      complete[index], &decision) ==
                  ISAAC_VITA_ANM2_SCRATCH_HANDLED);
        }
        if (session == 3U) {
            CHECK(s_log_count == logs_before);
        }
        else {
            CHECK(s_log_count == logs_before + 2U);
            CHECK(strstr(s_logs[logs_before], "session: id=4"));
            CHECK(strstr(s_logs[logs_before + 1U],
                         "session=4 acquired=6 freed=6 peak=6 complete=yes"));
        }
    }
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.session_id == 4U && snapshot.live_count == 0U &&
          snapshot.acquired_count == 6U && snapshot.freed_count == 6U);

    /* Test-only reset proves the fake contract has exactly one retained
     * block.  Production intentionally has no shutdown/free edge. */
    CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    CHECK(!s_block_live && s_free_calls == 1U);
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == 0U && snapshot.uid == -1 &&
          !snapshot.guest_heap_fallback &&
          snapshot.session_id == 0U && snapshot.live_count == 0U);

    /* Real hardware can exhaust free physical pages after the ledger, mspace,
     * graphics and asset allocations are already resident.  Only that exact
     * pre-transaction error changes ownership: all six frozen owners continue
     * through the common guest-heap router, no second memblock probe occurs,
     * and foreign/generic pointers remain outside scratch free ownership. */
    reset_observations();
    s_alloc_result = ISAAC_VITA_ANM2_NO_FREE_PHYSICAL_PAGE;
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        result = alloc_site(index, &decision);
        CHECK(result == ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP &&
              !decision.pointer && !decision.fault &&
              decision.fault_value == 0U);
    }
    CHECK(s_alloc_calls == 1U && s_get_calls == 0U &&
          s_free_calls == 0U && !s_block_live && s_log_count == 2U);
    CHECK(strstr(s_logs[0], "status=alloc-failed") &&
          strstr(s_logs[0], "result=0x80024302") &&
          strstr(s_logs[0], "route=guest-heap"));
    CHECK(strstr(s_logs[1], "fallback session: id=1") &&
          strstr(s_logs[1], "route=guest-heap"));
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.base == 0U && snapshot.uid == -1 &&
          !snapshot.poisoned && snapshot.guest_heap_fallback == 1U &&
          snapshot.session_id == 1U && snapshot.live_count == 0U &&
          snapshot.acquired_count == ISAAC_VITA_ANM2_SEGMENT_COUNT &&
          snapshot.freed_count == 0U && snapshot.peak_live == 0U);

    result = alloc_site(1U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_REJECTED &&
          decision.fault && strstr(decision.fault, "restart at AA5D") &&
          s_alloc_calls == 1U);
    result = alloc_site(0U, &decision);
    CHECK(result == ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP &&
          !decision.pointer && !decision.fault && s_alloc_calls == 1U &&
          s_log_count == 3U &&
          strstr(s_logs[2], "fallback session: id=2"));
    CHECK(isaac_vita_anm2_scratch_free(
              (void *)(uintptr_t)0x70000000U, &decision) ==
          ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED);
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(snapshot.guest_heap_fallback == 1U &&
          snapshot.session_id == 2U && snapshot.acquired_count == 1U);
    CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    CHECK(s_free_calls == 0U);
    CHECK(isaac_vita_anm2_scratch_oracle_snapshot(&snapshot));
    CHECK(!snapshot.guest_heap_fallback && snapshot.session_id == 0U &&
          snapshot.acquired_count == 0U);

    puts("Vita ANM2 scratch lifecycle oracle: PASS");
    return 0;
}
