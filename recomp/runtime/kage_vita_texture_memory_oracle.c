#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_texture_memory.h"

static struct mallinfo s_heap;
static unsigned s_mallinfo_calls;
static unsigned s_malloc_calls;
static unsigned s_free_calls;
static unsigned s_log_calls;
static size_t s_malloc_sizes[2];
static void *s_free_pointers[2];
static int s_large_probe_succeeds;
static int s_small_probe_succeeds;
static uint8_t s_probe_tokens[2];
static unsigned s_memblock_alloc_calls;
static unsigned s_memblock_get_calls;
static unsigned s_memblock_free_calls;
static size_t s_memblock_sizes[2];
static int32_t s_memblock_alloc_results[2];
static int s_memblock_get_results[2];
static int s_memblock_free_results[2];
static void *s_memblock_bases[2];
#define ORACLE_PRODUCTION_LOG_BODY_BYTES 384U
#define ORACLE_DIAGNOSTIC_LOG_LINES 2U
static char s_logs[ORACLE_DIAGNOSTIC_LOG_LINES]
                  [ORACLE_PRODUCTION_LOG_BODY_BYTES];
static int s_log_lengths[ORACLE_DIAGNOSTIC_LOG_LINES];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "texture-memory oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct mallinfo kage_vita_texture_memory_oracle_mallinfo(void)
{
    ++s_mallinfo_calls;
    return s_heap;
}

void *kage_vita_texture_memory_oracle_malloc(size_t size)
{
    unsigned index = s_malloc_calls++;

    if (index < sizeof s_malloc_sizes / sizeof s_malloc_sizes[0])
        s_malloc_sizes[index] = size;
    if (size == KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST &&
        s_large_probe_succeeds)
        return &s_probe_tokens[0];
    if (size == KAGE_VITA_TEXEL_PROBE_SMALL_REQUEST &&
        s_small_probe_succeeds)
        return &s_probe_tokens[1];
    return NULL;
}

void kage_vita_texture_memory_oracle_free(void *pointer)
{
    unsigned index = s_free_calls++;

    if (index < sizeof s_free_pointers / sizeof s_free_pointers[0])
        s_free_pointers[index] = pointer;
}

int32_t kage_vita_texture_memory_oracle_memblock_alloc(size_t size)
{
    unsigned index = s_memblock_alloc_calls++;

    if (index >= sizeof s_memblock_sizes / sizeof s_memblock_sizes[0])
        return -1;
    s_memblock_sizes[index] = size;
    return s_memblock_alloc_results[index];
}

static int oracle_memblock_slot(int32_t uid)
{
    unsigned index;

    for (index = 0U; index < s_memblock_alloc_calls; ++index)
        if (s_memblock_alloc_results[index] == uid)
            return (int)index;
    return -1;
}

int kage_vita_texture_memory_oracle_memblock_get(int32_t uid, void **base)
{
    int slot = oracle_memblock_slot(uid);

    ++s_memblock_get_calls;
    if (slot < 0 || !base)
        return -1;
    if (s_memblock_get_results[slot] >= 0)
        *base = s_memblock_bases[slot];
    return s_memblock_get_results[slot];
}

int kage_vita_texture_memory_oracle_memblock_free(int32_t uid)
{
    int slot = oracle_memblock_slot(uid);

    ++s_memblock_free_calls;
    if (slot < 0)
        return -1;
    return s_memblock_free_results[slot];
}

void kage_vita_texture_memory_oracle_log(const char *format, ...)
{
    va_list arguments;
    unsigned index = s_log_calls++;

    if (index >= ORACLE_DIAGNOSTIC_LOG_LINES)
        return;
    va_start(arguments, format);
    s_log_lengths[index] = vsnprintf(
        s_logs[index], sizeof s_logs[index], format, arguments);
    va_end(arguments);
}

static int log_line_is_complete(unsigned index)
{
    return index < ORACLE_DIAGNOSTIC_LOG_LINES &&
           s_log_lengths[index] >= 0 &&
           (unsigned)s_log_lengths[index] <
               ORACLE_PRODUCTION_LOG_BODY_BYTES &&
           strlen(s_logs[index]) == (size_t)s_log_lengths[index];
}

static int string_ends_with(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return suffix_length <= text_length &&
           memcmp(text + text_length - suffix_length,
                  suffix, suffix_length) == 0;
}

static void reset_observations(void)
{
    s_mallinfo_calls = 0U;
    s_malloc_calls = 0U;
    s_free_calls = 0U;
    s_log_calls = 0U;
    memset(s_malloc_sizes, 0, sizeof s_malloc_sizes);
    memset(s_free_pointers, 0, sizeof s_free_pointers);
    s_large_probe_succeeds = 0;
    s_small_probe_succeeds = 0;
    s_memblock_alloc_calls = 0U;
    s_memblock_get_calls = 0U;
    s_memblock_free_calls = 0U;
    memset(s_memblock_sizes, 0, sizeof s_memblock_sizes);
    s_memblock_alloc_results[0] = -1;
    s_memblock_alloc_results[1] = -1;
    s_memblock_get_results[0] = -1;
    s_memblock_get_results[1] = -1;
    s_memblock_free_results[0] = -1;
    s_memblock_free_results[1] = -1;
    s_memblock_bases[0] = NULL;
    s_memblock_bases[1] = NULL;
    memset(s_logs, 0, sizeof s_logs);
    memset(s_log_lengths, 0, sizeof s_log_lengths);
}

int main(void)
{
    uint8_t flag;

    s_heap.arena = 82825216;
    s_heap.uordblks = 81035320;
    s_heap.fordblks = 1789896;
    s_heap.ordblks = 596;

    kage_vita_texel_oom_diagnostic_oracle_reset();
    reset_observations();
    s_large_probe_succeeds = 1;
    s_memblock_alloc_results[0] = 0x2345;
    s_memblock_alloc_results[1] = 0x3456;
    s_memblock_get_results[0] = 0;
    s_memblock_get_results[1] = 0;
    s_memblock_free_results[0] = 0;
    s_memblock_free_results[1] = 0;
    s_memblock_bases[0] = (void *)(uintptr_t)0x70000000U;
    s_memblock_bases[1] = (void *)(uintptr_t)0x71000000U;
    kage_vita_texel_oom_diagnostic(
        KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST,
        KAGE_VITA_TEXEL_LOADER_RETURN_0, 2U,
        "native-malloc-failed",
        78000000U, 8520704U, 321U, 1,
        0x82a00000U, 0x82e00000U);
    CHECK(s_mallinfo_calls == 1U && s_malloc_calls == 2U &&
          s_free_calls == 1U && s_log_calls == 2U);
    CHECK(log_line_is_complete(0U) && log_line_is_complete(1U));
    CHECK(s_malloc_sizes[0] == KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST &&
          s_malloc_sizes[1] == KAGE_VITA_TEXEL_PROBE_SMALL_REQUEST);
    CHECK(s_free_pointers[0] == &s_probe_tokens[0]);
    CHECK(s_memblock_alloc_calls == 2U && s_memblock_get_calls == 2U &&
          s_memblock_free_calls == 2U &&
          s_memblock_sizes[0] == KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES &&
          s_memblock_sizes[1] == KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES);
    CHECK(strstr(s_logs[0],
                 "event=1 request=4660224 owner=0x005a0857 ") != NULL);
    CHECK(strstr(s_logs[0],
                 "chain_depth=2 failure=native-malloc-failed "
                 "part=allocator") != NULL);
    CHECK(strstr(s_logs[0],
                 "arena=82825216 allocated=81035320") != NULL);
    CHECK(strstr(s_logs[0], "free=1789896 chunks=596") != NULL);
    CHECK(strstr(s_logs[0],
                 "ledger_live_requested=78000000 ") != NULL);
    CHECK(strstr(s_logs[0],
                 "ledger_largest_live_request=8520704 ") != NULL);
    CHECK(strstr(s_logs[0],
                 "ledger_live_count=321 ledger_requested_exact=yes") != NULL);
    CHECK(string_ends_with(
              s_logs[0],
              "probe_4660224=PASS probe_2097152=FAIL"));
    CHECK(strstr(s_logs[1],
                 "event=1 request=4660224 owner=0x005a0857 ") != NULL);
    CHECK(strstr(s_logs[1],
                 "chain_depth=2 failure=native-malloc-failed "
                 "part=memblocks") != NULL);
    CHECK(strstr(s_logs[1],
                 "memblock_00801000=PASS uid=0x00002345") != NULL);
    CHECK(strstr(s_logs[1],
                 "get=0x00000000 free=0x00000000 ") != NULL);
    CHECK(strstr(s_logs[1],
                 "base=0x70000000 end=0x70801000") != NULL);
    CHECK(strstr(s_logs[1],
                 "memblock_00bd6000=PASS uid=0x00003456") != NULL);
    CHECK(string_ends_with(
              s_logs[1],
              "base=0x71000000 end=0x71bd6000"));
    kage_vita_texel_oom_diagnostic(
        KAGE_VITA_TEXEL_PROBE_SMALL_REQUEST,
        KAGE_VITA_TEXEL_LOADER_RETURN_1, 3U,
        "native-malloc-failed", 1U, 1U, 1U, 1,
        0x82a00000U, 0x82e00000U);
    CHECK(s_mallinfo_calls == 1U && s_malloc_calls == 2U &&
          s_free_calls == 1U && s_log_calls == 2U);

    kage_vita_texel_oom_diagnostic_oracle_reset();
    reset_observations();
    s_memblock_alloc_results[0] = -1;
    s_memblock_alloc_results[1] = 0x3456;
    s_memblock_get_results[1] = 0;
    s_memblock_free_results[1] = 0;
    s_memblock_bases[1] = (void *)(uintptr_t)0x71000000U;
    kage_vita_texel_oom_diagnostic(
        KAGE_VITA_TEXEL_PROBE_SMALL_REQUEST,
        KAGE_VITA_TEXEL_LOADER_RETURN_1, 3U,
        "native-malloc-failed", 1U, 1U, 1U, 1,
        0x82a00000U, 0x82e00000U);
    CHECK(s_memblock_alloc_calls == 2U && s_memblock_get_calls == 1U &&
          s_memblock_free_calls == 1U);
    CHECK(strstr(s_logs[1],
                 "memblock_00801000=alloc-failed uid=0xffffffff") != NULL);
    CHECK(strstr(s_logs[1],
                 "memblock_00bd6000=PASS uid=0x00003456") != NULL);

    kage_vita_texel_oom_diagnostic_oracle_reset();
    reset_observations();
    s_heap.arena = UINT32_MAX;
    s_heap.uordblks = UINT32_MAX;
    s_heap.fordblks = UINT32_MAX;
    s_heap.ordblks = UINT32_MAX;
    s_memblock_alloc_results[0] = INT32_MAX;
    s_memblock_get_results[0] = 0;
    s_memblock_free_results[0] = -1;
    s_memblock_bases[0] = (void *)(uintptr_t)0x70000000U;
    kage_vita_texel_oom_diagnostic(
        (size_t)UINT32_MAX, UINT32_MAX, UINT32_MAX,
        "ledger-reserve-failed",
        (size_t)UINT32_MAX, (size_t)UINT32_MAX,
        (size_t)UINT32_MAX, 1,
        0x82a00000U, 0x82e00000U);
    CHECK(s_mallinfo_calls == 1U && s_malloc_calls == 2U &&
          s_free_calls == 0U && s_log_calls == 2U);
    CHECK(s_memblock_alloc_calls == 1U && s_memblock_get_calls == 1U &&
          s_memblock_free_calls == 1U);
    CHECK(log_line_is_complete(0U) && log_line_is_complete(1U));
    CHECK(s_log_lengths[0] == 371 && s_log_lengths[1] == 362);
    CHECK(strstr(s_logs[0],
                 "event=1 request=4294967295 owner=0xffffffff ") != NULL);
    CHECK(strstr(s_logs[0],
                 "chain_depth=4294967295 failure=ledger-reserve-failed "
                 "part=allocator") != NULL);
    CHECK(string_ends_with(
              s_logs[0],
              "probe_4660224=FAIL probe_2097152=FAIL"));
    CHECK(strstr(s_logs[1],
                 "event=1 request=4294967295 owner=0xffffffff ") != NULL);
    CHECK(strstr(s_logs[1],
                 "chain_depth=4294967295 failure=ledger-reserve-failed "
                 "part=memblocks") != NULL);
    CHECK(strstr(s_logs[1],
                 "memblock_00801000=free-failed uid=0x7fffffff") != NULL);
    CHECK(string_ends_with(
              s_logs[1],
              "memblock_00bd6000=skipped-after-free-failure "
              "uid=0xffffffff get=0xffffffff free=0xffffffff "
              "base=0x00000000 end=0x00000000"));

    reset_observations();
    flag = 0U;
    CHECK(!kage_vita_texture_align8_policy_oracle_apply(&flag, 0));
    CHECK(flag == 0U && s_log_calls == 1U &&
          strstr(s_logs[0], "absent") != NULL);

    reset_observations();
    flag = 2U;
    CHECK(!kage_vita_texture_align8_policy_oracle_apply(&flag, 1));
    CHECK(flag == 2U && s_log_calls == 1U &&
          strstr(s_logs[0], "value=2") != NULL);

    reset_observations();
    flag = 0U;
    CHECK(kage_vita_texture_align8_policy_oracle_apply(&flag, 1));
    CHECK(flag == 1U && s_log_calls == 1U &&
          strstr(s_logs[0], "previous=0 active=1") != NULL);
    CHECK(kage_vita_texture_align8_policy_oracle_apply(&flag, 1));
    CHECK(flag == 1U && s_log_calls == 2U &&
          strstr(s_logs[1], "previous=1 active=1") != NULL);

    puts("Vita KAGE texture memory oracle: PASS");
    return 0;
}
