#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ORACLE_ENABLE_POLICY
#define ORACLE_ENABLE_POLICY 0
#endif

#if ORACLE_ENABLE_POLICY
#define ISAAC_VITA_FXLAYERS_NULL_ROLLBACK 1
#endif

#if defined(__GNUC__)
#define ORACLE_UNUSED __attribute__((unused))
#else
#define ORACLE_UNUSED
#endif

static uint32_t ORACLE_UNUSED oracle_read32(uint32_t address);
static uint32_t ORACLE_UNUSED oracle_heap_lease(
    uint32_t address, size_t size, uintptr_t *base_out);
static int ORACLE_UNUSED oracle_heap_release(uint32_t lease);
static void ORACLE_UNUSED oracle_log(const char *format, ...);

#define KAGE_VITA_FX_ROLLBACK_READ32(address) oracle_read32((address))
#define KAGE_VITA_FX_ROLLBACK_HEAP_LEASE(address, size, base_out) \
    oracle_heap_lease((address), (size), (base_out))
#define KAGE_VITA_FX_ROLLBACK_HEAP_RELEASE(lease) \
    oracle_heap_release((lease))
#define KAGE_VITA_FX_ROLLBACK_LOG oracle_log
#include "kage_vita_fx_rollback.c"

typedef struct oracle_word {
    uint32_t address;
    uint32_t value;
} oracle_word;

static oracle_word s_words[16];
static size_t s_word_count;
static uint32_t s_expected_last;
static uint32_t s_expected_lease_range;
static size_t s_expected_lease_size;
static uintptr_t s_expected_allocation_base;
static unsigned s_reads;
static unsigned s_record_reads;
static unsigned s_lease_calls;
static unsigned s_release_calls;
static unsigned s_logs;
static unsigned s_unexpected_reads;
static unsigned s_destructor_calls;
static unsigned s_destructor_bad_args;
static int s_allow_lease;
static int s_allow_release;
static uint32_t s_active_lease;
static uint32_t s_next_lease;
static int s_destructor_fault;
static int s_destructor_jump_armed;
static jmp_buf s_destructor_jump;

#define ORACLE_FRAME 0x10000000U
#define ORACLE_OWNER 0x20000000U
#define ORACLE_BEGIN 0x21000000U
#define ORACLE_END (ORACLE_BEGIN + 2U * 0x4cU)
#define ORACLE_CAP (ORACLE_BEGIN + 4U * 0x4cU)
#define ORACLE_BIG_RAW   0x21000010U
#define ORACLE_BIG_BEGIN 0x21000020U
#define ORACLE_BIG_END (ORACLE_BIG_BEGIN + 2U * 0x4cU)
#define ORACLE_BIG_CAP  (ORACLE_BIG_BEGIN + 64U * 0x4cU)

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void oracle_set32(uint32_t address, uint32_t value)
{
    size_t i;
    for (i = 0; i < s_word_count; ++i) {
        if (s_words[i].address == address) {
            s_words[i].value = value;
            return;
        }
    }
    if (s_word_count < sizeof s_words / sizeof s_words[0]) {
        s_words[s_word_count].address = address;
        s_words[s_word_count].value = value;
        ++s_word_count;
    }
}

static uint32_t oracle_peek32(uint32_t address)
{
    size_t i;
    for (i = 0; i < s_word_count; ++i)
        if (s_words[i].address == address)
            return s_words[i].value;
    return UINT32_MAX;
}

static uint32_t oracle_read32(uint32_t address)
{
    size_t i;
    ++s_reads;
    if (address == s_expected_last + 0x04U ||
        address == s_expected_last + 0x2cU)
        ++s_record_reads;
    for (i = 0; i < s_word_count; ++i)
        if (s_words[i].address == address)
            return s_words[i].value;
    ++s_unexpected_reads;
    return UINT32_MAX;
}

static uint32_t oracle_heap_lease(
    uint32_t address, size_t size, uintptr_t *base_out)
{
    ++s_lease_calls;
    if (base_out)
        *base_out = 0U;
    if (!s_allow_lease || !base_out || s_active_lease ||
        address != s_expected_lease_range ||
        size != s_expected_lease_size)
        return 0U;
    *base_out = s_expected_allocation_base;
    s_active_lease = s_next_lease++;
    return s_active_lease;
}

static int oracle_heap_release(uint32_t lease)
{
    ++s_release_calls;
    if (!s_allow_release || !lease || lease != s_active_lease)
        return 0;
    s_active_lease = 0U;
    return 1;
}

static void oracle_log(const char *format, ...)
{
    va_list arguments;
    ++s_logs;
    va_start(arguments, format);
    va_end(arguments);
}

static void oracle_fixture(CPU *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    memset(s_words, 0, sizeof s_words);
    s_word_count = 0;
    s_expected_last = ORACLE_END - 0x4cU;
    s_expected_lease_range = ORACLE_BEGIN;
    s_expected_lease_size = ORACLE_CAP - ORACLE_BEGIN;
    s_expected_allocation_base = ORACLE_BEGIN;
    s_reads = 0;
    s_record_reads = 0;
    s_lease_calls = 0;
    s_release_calls = 0;
    s_logs = 0;
    s_unexpected_reads = 0;
    s_destructor_calls = 0;
    s_destructor_bad_args = 0;
    s_allow_lease = 1;
    s_allow_release = 1;
    s_active_lease = 0U;
    s_next_lease = 0x12340001U;
    s_destructor_fault = 0;
    s_destructor_jump_armed = 0;

    cpu->ebp = ORACLE_FRAME + 0x1013cU;
    cpu->esi = ORACLE_END;
    cpu->stack_owner = cpu;
    cpu->stack_floor = ORACLE_FRAME - 0x10U;
    cpu->stack_ceiling = ORACLE_FRAME + 0x10U;
    cpu->stack_low_water = cpu->stack_floor;

    oracle_set32(ORACLE_FRAME, ORACLE_END);
    oracle_set32(ORACLE_FRAME + 8U, ORACLE_OWNER);
    oracle_set32(ORACLE_OWNER + 0x18U, ORACLE_BEGIN);
    oracle_set32(ORACLE_OWNER + 0x1cU, ORACLE_END);
    oracle_set32(ORACLE_OWNER + 0x20U, ORACLE_CAP);
    oracle_set32(s_expected_last + 0x04U, 0U);
    oracle_set32(s_expected_last + 0x2cU, 4U);
}

static void oracle_big_fixture(CPU *cpu)
{
    oracle_fixture(cpu);
    s_expected_last = ORACLE_BIG_END - 0x4cU;
    s_expected_lease_range = ORACLE_BIG_BEGIN - 4U;
    s_expected_lease_size = (ORACLE_BIG_CAP - ORACLE_BIG_BEGIN) + 4U;
    s_expected_allocation_base = ORACLE_BIG_RAW;
    cpu->esi = ORACLE_BIG_END;
    oracle_set32(ORACLE_FRAME, ORACLE_BIG_END);
    oracle_set32(ORACLE_OWNER + 0x18U, ORACLE_BIG_BEGIN);
    oracle_set32(ORACLE_OWNER + 0x1cU, ORACLE_BIG_END);
    oracle_set32(ORACLE_OWNER + 0x20U, ORACLE_BIG_CAP);
    oracle_set32(ORACLE_BIG_BEGIN - 4U, ORACLE_BIG_RAW);
    oracle_set32(s_expected_last + 0x04U, 0U);
    oracle_set32(s_expected_last + 0x2cU, 4U);
}

static void oracle_destructor(CPU *cpu, uint32_t last, uint32_t end)
{
    ++s_destructor_calls;
    if (cpu->ecx != last || cpu->edx != end || cpu->esi != end ||
        oracle_peek32(cpu->ebp - 0x1013cU) != end ||
        last != s_expected_last || end != ORACLE_END || s_active_lease)
        ++s_destructor_bad_args;
    if (s_destructor_fault) {
        cpu->fault = "oracle destructor fault";
        if (s_destructor_jump_armed)
            longjmp(s_destructor_jump, 1);
    }
}

/* Host model of the generated seam, used only to prove plan/commit ordering. */
static int oracle_apply(CPU *cpu, uint32_t root_rva)
{
    uint32_t owner;
    uint32_t last;
    uint32_t end;
    int plan = kage_vita_fx_rollback_prepare(
        cpu, root_rva, &owner, &last, &end);
    if (plan <= 0)
        return plan;
    if (s_active_lease)
        return -3;
    cpu->ecx = last;
    cpu->edx = end;
    if (s_destructor_fault) {
        if (setjmp(s_destructor_jump) == 0) {
            s_destructor_jump_armed = 1;
            oracle_destructor(cpu, last, end);
            s_destructor_jump_armed = 0;
            return -4; /* fault injection failed to leave nonlocally */
        }
        s_destructor_jump_armed = 0;
        return -2;
    }
    oracle_destructor(cpu, last, end);
    if (cpu->fault)
        return -2;
    oracle_set32(cpu->ebp - 0x1013cU, last);
    cpu->esi = last;
    oracle_set32(owner + 0x1cU, last);
    kage_vita_fx_rollback_note(root_rva, owner, last, end);
    return 2;
}

static int oracle_off(void)
{
    CPU cpu;
    uint32_t owner = 0xaaaaaaaaU;
    uint32_t last = 0xbbbbbbbbU;
    uint32_t end = 0xccccccccU;

    oracle_fixture(&cpu);
    CHECK(kage_vita_fx_rollback_prepare(
              NULL, 0U, NULL, NULL, NULL) == 0);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == 0);
    kage_vita_fx_rollback_note(0U, 0U, 0U, 0U);
    CHECK(owner == 0xaaaaaaaaU && last == 0xbbbbbbbbU &&
          end == 0xccccccccU);
    CHECK(s_reads == 0U && s_lease_calls == 0U &&
          s_release_calls == 0U && s_logs == 0U);
    CHECK(oracle_apply(&cpu, 0x004e8330U) == 0);
    CHECK(s_destructor_calls == 0U);
    CHECK(oracle_peek32(ORACLE_OWNER + 0x1cU) == ORACLE_END);
    return 0;
}

static int oracle_enabled(void)
{
    CPU cpu;
    CPU before;
    oracle_word words_before[16];
    uint32_t owner;
    uint32_t last;
    uint32_t end;
    size_t words_size;

    oracle_fixture(&cpu);
    before = cpu;
    words_size = s_word_count * sizeof s_words[0];
    memcpy(words_before, s_words, words_size);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == 1);
    CHECK(owner == ORACLE_OWNER && last == s_expected_last &&
          end == ORACLE_END);
    CHECK(memcmp(&cpu, &before, sizeof cpu) == 0);
    CHECK(memcmp(s_words, words_before, words_size) == 0);
    CHECK(s_reads == 7U && s_record_reads == 2U &&
          s_lease_calls == 1U && s_release_calls == 1U &&
          s_active_lease == 0U && s_unexpected_reads == 0U);

    oracle_fixture(&cpu);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e85b3U, &owner, &last, &end) == 1);
    CHECK(last == ORACLE_END - 0x4cU && s_active_lease == 0U);

    /* Capacity 64 crosses the 54-record / 0x1000-byte allocator split. */
    oracle_big_fixture(&cpu);
    before = cpu;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == 1);
    CHECK(owner == ORACLE_OWNER && last == ORACLE_BIG_END - 0x4cU &&
          end == ORACLE_BIG_END);
    CHECK(memcmp(&cpu, &before, sizeof cpu) == 0);
    CHECK(s_reads == 8U && s_record_reads == 2U &&
          s_lease_calls == 1U && s_release_calls == 1U &&
          s_active_lease == 0U && s_unexpected_reads == 0U);

    oracle_fixture(&cpu);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8331U, &owner, &last, &end) == -1);
    CHECK(s_reads == 0U && s_record_reads == 0U && s_lease_calls == 0U);

    oracle_fixture(&cpu);
    cpu.stack_owner = NULL;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_reads == 0U && s_record_reads == 0U);

    oracle_fixture(&cpu);
    oracle_set32(ORACLE_OWNER + 0x18U, ORACLE_END + 4U);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 0U && s_lease_calls == 0U);

    oracle_fixture(&cpu);
    oracle_set32(ORACLE_OWNER + 0x1cU, ORACLE_END - 4U);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 0U && s_lease_calls == 0U);

    oracle_fixture(&cpu);
    oracle_set32(ORACLE_OWNER + 0x20U, ORACLE_END - 0x4cU);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 0U && s_lease_calls == 0U);

    oracle_fixture(&cpu);
    oracle_set32(ORACLE_FRAME, ORACLE_END + 0x4cU);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 0U && s_lease_calls == 0U);

    oracle_fixture(&cpu);
    cpu.esi = ORACLE_END + 0x4cU;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 0U && s_lease_calls == 0U);

    oracle_fixture(&cpu);
    s_allow_lease = 0;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_lease_calls == 1U && s_release_calls == 0U &&
          s_record_reads == 0U);

    oracle_fixture(&cpu);
    s_expected_allocation_base = ORACLE_BEGIN - 8U;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_lease_calls == 1U && s_release_calls == 1U &&
          s_active_lease == 0U && s_record_reads == 0U);

#if UINTPTR_MAX > UINT32_MAX
    oracle_fixture(&cpu);
    s_expected_allocation_base = (uintptr_t)UINT32_MAX + 1U;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_lease_calls == 1U && s_release_calls == 1U &&
          s_active_lease == 0U && s_record_reads == 0U);
#endif

    oracle_big_fixture(&cpu);
    oracle_set32(ORACLE_BIG_BEGIN - 4U, ORACLE_BIG_RAW + 4U);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_reads == 6U && s_record_reads == 0U &&
          s_release_calls == 1U && s_active_lease == 0U);

    oracle_fixture(&cpu);
    oracle_set32(s_expected_last + 0x04U, 0x1234U);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 1U && s_release_calls == 1U &&
          s_active_lease == 0U);

    oracle_fixture(&cpu);
    oracle_set32(s_expected_last + 0x2cU, 3U);
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_record_reads == 2U && s_release_calls == 1U &&
          s_active_lease == 0U);

    oracle_fixture(&cpu);
    s_allow_release = 0;
    CHECK(kage_vita_fx_rollback_prepare(
              &cpu, 0x004e8330U, &owner, &last, &end) == -1);
    CHECK(s_release_calls == 1U && s_active_lease != 0U);

    oracle_fixture(&cpu);
    CHECK(oracle_apply(&cpu, 0x004e8330U) == 2);
    CHECK(s_destructor_calls == 1U && s_destructor_bad_args == 0U);
    CHECK(cpu.ecx == s_expected_last && cpu.edx == ORACLE_END &&
          cpu.esi == s_expected_last);
    CHECK(oracle_peek32(ORACLE_FRAME) == s_expected_last);
    CHECK(oracle_peek32(ORACLE_OWNER + 0x1cU) == s_expected_last);
    CHECK(s_release_calls == 1U && s_active_lease == 0U && s_logs == 1U);

    /* A translated-destructor guest_fault longjmps over the seam.  Prepare
     * has already released its validation lease, so no cleanup is skipped. */
    oracle_fixture(&cpu);
    s_destructor_fault = 1;
    CHECK(oracle_apply(&cpu, 0x004e8330U) == -2);
    CHECK(s_destructor_calls == 1U && s_destructor_bad_args == 0U);
    CHECK(cpu.esi == ORACLE_END);
    CHECK(oracle_peek32(ORACLE_FRAME) == ORACLE_END);
    CHECK(oracle_peek32(ORACLE_OWNER + 0x1cU) == ORACLE_END);
    CHECK(s_release_calls == 1U && s_active_lease == 0U && s_logs == 0U);
    return 0;
}

int main(void)
{
    int result = ORACLE_ENABLE_POLICY ? oracle_enabled() : oracle_off();
    if (result == 0)
        printf("FXLayers rollback oracle %s PASS\n",
               ORACLE_ENABLE_POLICY ? "ON" : "OFF");
    return result;
}
