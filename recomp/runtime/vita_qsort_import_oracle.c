/* Executable host oracle and link-only ARM oracle for the production Vita
 * qsort import.  The host build is non-PIE so its static guest arena is below
 * 4 GiB and exercises the same identity-mapped ld/st helpers as Vita.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "host_vita_crt.h"

enum {
    COMPARE_LIVE = 0x10002000U,
    COMPARE_WIDE = 0x10002004U,
    COMPARE_NESTED = 0x10002008U,
    COMPARE_BAD_STACK = 0x1000200cU,
    COMPARE_FAULT = 0x10002010U,
    IMPORT_RETURN = 0xabc06678U,
    NESTED_RETURN = 0xabc02008U,
    LIVE_COMPARISON_COUNT = 24U,
    WIDE_COMPARISON_COUNT = 27U,
    NESTED_COMPARISON_COUNT = 3U
};

typedef struct qsort_evidence {
    uint32_t owner_rva;
    uint32_t call_rva;
    uint32_t return_rva;
    uint32_t comparator_push_rva;
    uint32_t comparator_rva;
    uint32_t width;
} qsort_evidence;

#define ISAAC_QSORT_EVIDENCE(owner, call, ret, push, comparator, width) \
    { owner, call, ret, push, comparator, width },
static const qsort_evidence s_evidence[] = {
    ISAAC_VITA_CRT_QSORT_CALL_SITES(ISAAC_QSORT_EVIDENCE)
};
#undef ISAAC_QSORT_EVIDENCE

typedef struct wide_record {
    int32_t key;
    uint32_t id;
    uint8_t payload[24];
} wide_record;

typedef struct live_arena {
    uint32_t before;
    int32_t values[8];
    uint32_t after;
} live_arena;

typedef struct wide_arena {
    uint32_t before;
    wide_record values[8];
    uint32_t after;
} wide_arena;

typedef struct nested_arena {
    uint32_t before;
    int32_t values[3];
    uint32_t after;
} nested_arena;

_Static_assert(sizeof(wide_record) == 32U,
               "wide qsort record stopped matching the measured width");
_Static_assert(sizeof s_evidence / sizeof s_evidence[0] ==
                   ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT,
               "qsort evidence lost a physical call site");

_Alignas(16) static uint32_t s_guest_stack[1024];
_Alignas(16) static live_arena s_live;
_Alignas(16) static wide_arena s_wide;
_Alignas(16) static nested_arena s_nested;
_Alignas(16) static char s_strdup_source[8];
_Alignas(16) static unsigned char s_strdup_allocation[32];

static unsigned s_live_comparisons;
static unsigned s_wide_comparisons;
static unsigned s_nested_comparisons;
static int s_bad_pointer;
static int s_nested_started;
static int s_guest_heap_terminal;
static int s_guest_heap_malloc_fail;
static int s_guest_heap_bad_free;
static unsigned s_guest_heap_malloc_calls;
static unsigned s_guest_heap_free_calls;

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT; ++i) {
        const uint32_t fields[] = {
            s_evidence[i].owner_rva,
            s_evidence[i].call_rva,
            s_evidence[i].return_rva,
            s_evidence[i].comparator_push_rva,
            s_evidence[i].comparator_rva,
            s_evidence[i].width
        };
        uint32_t field;

        for (field = 0U; field < sizeof fields / sizeof fields[0]; ++field) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash,
                                 (uint8_t)(fields[field] >> shift));
        }
    }
    return hash;
}

static int evidence_valid(void)
{
    uint32_t unique_comparators = 0U;
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT; ++i) {
        uint32_t j;
        int first = 1;

        if (!s_evidence[i].owner_rva ||
            !s_evidence[i].comparator_push_rva ||
            s_evidence[i].return_rva - s_evidence[i].call_rva != 6U ||
            !s_evidence[i].comparator_rva ||
            (s_evidence[i].width != 4U && s_evidence[i].width != 32U))
            return 0;
        for (j = 0U; j < i; ++j) {
            if (s_evidence[j].call_rva == s_evidence[i].call_rva)
                return 0;
            if (s_evidence[j].comparator_rva ==
                s_evidence[i].comparator_rva)
                first = 0;
        }
        if (first)
            ++unique_comparators;
    }
    return strcmp(ISAAC_VITA_CRT_QSORT_NAME,
                  "api-ms-win-crt-utility-l1-1-0.dll!qsort") == 0 &&
           ISAAC_VITA_CRT_QSORT_IAT_RVA == 0x00606678U &&
           ISAAC_VITA_CRT_QSORT_IAT_VA == 0x98606678U &&
           ISAAC_VITA_CRT_QSORT_DIRECT_CALL_COUNT == 7U &&
           ISAAC_VITA_CRT_QSORT_REGISTER_LOAD_COUNT == 0U &&
           ISAAC_VITA_CRT_QSORT_REGISTER_CALL_COUNT == 0U &&
           unique_comparators == ISAAC_VITA_CRT_QSORT_COMPARATOR_COUNT &&
           evidence_hash() == ISAAC_VITA_CRT_QSORT_EVIDENCE_FNV64 &&
           s_evidence[1].owner_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_OWNER_RVA &&
           s_evidence[1].call_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_CALL_RVA &&
           s_evidence[1].return_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_RETURN_RVA &&
           s_evidence[1].comparator_push_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_PUSH_RVA &&
           s_evidence[1].comparator_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_COMPARATOR_RVA &&
           s_evidence[1].width == ISAAC_VITA_CRT_QSORT_LIVE_WIDTH &&
           ISAAC_VITA_CRT_QSORT_LIVE_GENERATED_UNIT == 157U;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    if (!c->fault) {
        c->fault_addr = address;
        c->fault = what;
    }
}

void *isaac_vita_guest_malloc(size_t size)
{
    ++s_guest_heap_malloc_calls;
    if (s_guest_heap_malloc_fail || size > sizeof s_strdup_allocation)
        return NULL;
    return s_strdup_allocation;
}

int isaac_vita_guest_free(void *pointer)
{
    ++s_guest_heap_free_calls;
    if (pointer != s_strdup_allocation) {
        s_guest_heap_bad_free = 1;
        return 0;
    }
    return 1;
}

int isaac_vita_guest_heap_terminal(void)
{
    return s_guest_heap_terminal;
}

static int compare_pointer_valid(uint32_t pointer, uint32_t base,
                                 uint32_t count, uint32_t width)
{
    uint32_t end = base + count * width;
    return pointer >= base && pointer < end &&
           (pointer - base) % width == 0U;
}

static void run_nested_sort(CPU *__restrict c, uint32_t outer_esp)
{
    uint32_t nested_base = pointer32(s_nested.values);

    gpush(c, COMPARE_NESTED);
    gpush(c, 4U);
    gpush(c, 3U);
    gpush(c, nested_base);
    gpush(c, NESTED_RETURN);
    isaac_vita_crt_qsort(c);
    if (c->fault)
        return;
    if (c->esp != outer_esp - 16U) {
        guest_fault(c, c->esp,
                    "nested qsort did not preserve its cdecl arguments");
        return;
    }
    c->esp += 16U;                 /* nested translated caller cleanup */
    if (c->esp != outer_esp)
        guest_fault(c, c->esp, "nested qsort escaped comparator frame");
}

void guest_call(CPU *__restrict c, uint32_t target)
{
    uint32_t entry = c->esp;
    uint32_t left;
    uint32_t right;
    uint32_t base;
    uint32_t count;
    uint32_t width;
    int32_t left_key;
    int32_t right_key;

    if (ld32(entry) != ISAAC_VITA_CRT_QSORT_CALLBACK_RETURN) {
        guest_fault(c, target, "qsort comparator return sentinel mismatch");
        return;
    }
    if (target == COMPARE_BAD_STACK) {
        /* Deliberately behave like stdcall and pop the two cdecl arguments. */
        c->esp += 12U;
        c->eax = 0U;
        return;
    }
    if (target == COMPARE_FAULT) {
        guest_fault(c, target, "oracle comparator fault");
        return;
    }

    left = ld32(entry + 4U);
    right = ld32(entry + 8U);
    switch (target) {
    case COMPARE_LIVE:
        base = pointer32(s_live.values);
        count = 8U;
        width = 4U;
        ++s_live_comparisons;
        break;
    case COMPARE_WIDE:
        base = pointer32(s_wide.values);
        count = 8U;
        width = 32U;
        ++s_wide_comparisons;
        break;
    case COMPARE_NESTED:
        base = pointer32(s_nested.values);
        count = 3U;
        width = 4U;
        ++s_nested_comparisons;
        break;
    default:
        guest_fault(c, target, "oracle received unknown comparator");
        return;
    }
    if (!compare_pointer_valid(left, base, count, width) ||
        !compare_pointer_valid(right, base, count, width)) {
        s_bad_pointer = 1;
        guest_fault(c, target, "qsort passed an invalid element pointer");
        return;
    }

    left_key = (int32_t)ld32(left);
    right_key = (int32_t)ld32(right);
    if (target == COMPARE_LIVE && !s_nested_started) {
        s_nested_started = 1;
        run_nested_sort(c, entry);
        if (c->fault)
            return;
    }
    (void)gpop(c);                  /* comparator RET; qsort cleans arguments */
    c->eax = (uint32_t)((left_key > right_key) - (left_key < right_key));
}

static uint32_t prepare_call(CPU *c, uint32_t base, uint32_t count,
                             uint32_t width, uint32_t comparator)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_guest_stack, 0xcc, sizeof s_guest_stack);
    esp = pointer32(&s_guest_stack[512]);
    st32(esp, IMPORT_RETURN);
    st32(esp + 4U, base);
    st32(esp + 8U, count);
    st32(esp + 12U, width);
    st32(esp + 16U, comparator);
    c->esp = esp;
    c->eax = 0x5aa55aa5U;
    return esp;
}

static int arguments_unchanged(uint32_t esp, uint32_t base, uint32_t count,
                               uint32_t width, uint32_t comparator)
{
    return ld32(esp) == IMPORT_RETURN && ld32(esp + 4U) == base &&
           ld32(esp + 8U) == count && ld32(esp + 12U) == width &&
           ld32(esp + 16U) == comparator;
}

static int expect_fault(uint32_t base, uint32_t count, uint32_t width,
                        uint32_t comparator, uint32_t address,
                        const char *what, int32_t esp_delta)
{
    CPU c;
    uint32_t esp = prepare_call(&c, base, count, width, comparator);

    isaac_vita_crt_qsort(&c);
    return c.fault && strcmp(c.fault, what) == 0 &&
           c.fault_addr == address &&
           c.esp == (uint32_t)(esp + (uint32_t)esp_delta) &&
           arguments_unchanged(esp, base, count, width, comparator);
}

static int run_oracle(void)
{
    static const int32_t live_input[8] = { 5, -3, 4, 0, 7, 9, -1, 2 };
    static const int32_t live_sorted[8] = { -3, -1, 0, 2, 4, 5, 7, 9 };
    static const int32_t wide_input[8] = { 5, -3, 5, 0, -3, 9, -1, 0 };
    static const int32_t wide_sorted[8] = { -3, -3, -1, 0, 0, 5, 5, 9 };
    CPU c;
    uint32_t esp;
    uint32_t i;
    uint32_t seen = 0U;

    if (!pointer_fits(s_guest_stack) || !pointer_fits(&s_live) ||
        !pointer_fits(&s_wide) || !pointer_fits(&s_nested) ||
        !pointer_fits(s_strdup_source) ||
        !pointer_fits(s_strdup_allocation))
        return 1;
    if (!evidence_valid())
        return 2;

    s_live_comparisons = s_wide_comparisons = s_nested_comparisons = 0U;
    s_bad_pointer = s_nested_started = 0;
    esp = prepare_call(&c, 0U, 0U, 0U, 0U);
    isaac_vita_crt_qsort(&c);
    if (c.fault || c.esp != esp + 4U || c.eax != 0x5aa55aa5U ||
        !arguments_unchanged(esp, 0U, 0U, 0U, 0U) ||
        s_live_comparisons || s_wide_comparisons || s_nested_comparisons)
        return 3;

    memset(&s_live, 0, sizeof s_live);
    s_live.before = 0x13579bdfU;
    s_live.after = 0x2468ace0U;
    for (i = 0U; i < 8U; ++i)
        s_live.values[i] = live_input[i];
    esp = prepare_call(&c, pointer32(s_live.values), 1U, 4U, COMPARE_LIVE);
    isaac_vita_crt_qsort(&c);
    if (c.fault || c.esp != esp + 4U || c.eax != 0x5aa55aa5U ||
        !arguments_unchanged(esp, pointer32(s_live.values), 1U, 4U,
                             COMPARE_LIVE) ||
        s_live.values[0] != live_input[0] || s_live_comparisons)
        return 4;

    s_nested.before = 0x11223344U;
    s_nested.values[0] = 3;
    s_nested.values[1] = -2;
    s_nested.values[2] = 1;
    s_nested.after = 0x55667788U;
    esp = prepare_call(&c, pointer32(s_live.values), 8U, 4U, COMPARE_LIVE);
    isaac_vita_crt_qsort(&c);
    if (c.fault || c.esp != esp + 4U || !s_nested_started || s_bad_pointer ||
        !arguments_unchanged(esp, pointer32(s_live.values), 8U, 4U,
                             COMPARE_LIVE) ||
        s_live_comparisons != LIVE_COMPARISON_COUNT ||
        s_nested_comparisons != NESTED_COMPARISON_COUNT ||
        s_live.before != 0x13579bdfU || s_live.after != 0x2468ace0U ||
        s_nested.before != 0x11223344U || s_nested.after != 0x55667788U ||
        s_nested.values[0] != -2 || s_nested.values[1] != 1 ||
        s_nested.values[2] != 3)
        return 5;
    for (i = 0U; i < 8U; ++i)
        if (s_live.values[i] != live_sorted[i])
            return 6;

    memset(&s_wide, 0, sizeof s_wide);
    s_wide.before = 0xa1b2c3d4U;
    s_wide.after = 0x4d3c2b1aU;
    for (i = 0U; i < 8U; ++i) {
        uint32_t j;
        s_wide.values[i].key = wide_input[i];
        s_wide.values[i].id = i;
        for (j = 0U; j < sizeof s_wide.values[i].payload; ++j)
            s_wide.values[i].payload[j] = (uint8_t)(i * 17U + j);
    }
    esp = prepare_call(&c, pointer32(s_wide.values), 8U, 32U, COMPARE_WIDE);
    isaac_vita_crt_qsort(&c);
    if (c.fault || c.esp != esp + 4U ||
        !arguments_unchanged(esp, pointer32(s_wide.values), 8U, 32U,
                             COMPARE_WIDE) ||
        s_wide_comparisons != WIDE_COMPARISON_COUNT ||
        s_wide.before != 0xa1b2c3d4U || s_wide.after != 0x4d3c2b1aU)
        return 7;
    for (i = 0U; i < 8U; ++i) {
        uint32_t id = s_wide.values[i].id;
        uint32_t j;
        if (s_wide.values[i].key != wide_sorted[i] || id >= 8U ||
            (id < 8U && (seen & (1U << id))) ||
            (id < 8U && wide_input[id] != s_wide.values[i].key))
            return 8;
        seen |= 1U << id;
        for (j = 0U; j < sizeof s_wide.values[i].payload; ++j)
            if (s_wide.values[i].payload[j] != (uint8_t)(id * 17U + j))
                return 9;
    }
    if (seen != 0xffU)
        return 10;

    if (!expect_fault(0U, 2U, 4U, COMPARE_LIVE, 0U,
                      "qsort received a null base", 0) ||
        !expect_fault(pointer32(s_live.values), 2U, 0U, COMPARE_LIVE, 0U,
                      "qsort received a zero element width", 0) ||
        !expect_fault(pointer32(s_live.values), 2U, 4U, 0U, 0U,
                      "qsort received a null comparator", 0) ||
        !expect_fault(pointer32(s_live.values), 0x80000000U, 8U,
                      COMPARE_LIVE, pointer32(s_live.values),
                      "qsort guest range overflows 32-bit address space", 0) ||
        !expect_fault(0xfffffffcU, 2U, 4U, COMPARE_LIVE, 0xfffffffcU,
                      "qsort guest range overflows 32-bit address space", 0))
        return 11;
    if (!expect_fault(pointer32(s_live.values), 2U, 4U,
                      COMPARE_BAD_STACK, COMPARE_BAD_STACK,
                      "qsort comparator did not preserve its cdecl stack", 0))
        return 12;
    if (!expect_fault(pointer32(s_live.values), 2U, 4U, COMPARE_FAULT,
                      COMPARE_FAULT, "oracle comparator fault", -12))
        return 13;
    if (!expect_fault(pointer32(s_live.values), 2U, 4U, 0x10002ffcU,
                      0x10002ffcU, "oracle received unknown comparator", -12))
        return 14;

    /* Execute the production `_strdup` handler against a durable heap
     * terminal.  Even a masked allocation success must be cleaned up and
     * fault without consuming RET; ordinary NULL remains a cdecl return. */
    strcpy(s_strdup_source, "x");
    s_guest_heap_terminal = 1;
    s_guest_heap_malloc_fail = 0;
    s_guest_heap_bad_free = 0;
    s_guest_heap_malloc_calls = 0U;
    s_guest_heap_free_calls = 0U;
    esp = prepare_call(&c, pointer32(s_strdup_source), 0U, 0U, 0U);
    isaac_vita_crt_strdup(&c);
    if (!c.fault ||
        strcmp(c.fault, "_strdup guest heap is terminal") != 0 ||
        c.fault_addr != pointer32(s_strdup_source) || c.esp != esp ||
        ld32(esp) != IMPORT_RETURN ||
        ld32(esp + 4U) != pointer32(s_strdup_source) ||
        s_guest_heap_malloc_calls != 1U ||
        s_guest_heap_free_calls != 1U || s_guest_heap_bad_free)
        return 15;

    s_guest_heap_malloc_fail = 1;
    esp = prepare_call(&c, pointer32(s_strdup_source), 0U, 0U, 0U);
    isaac_vita_crt_strdup(&c);
    if (!c.fault ||
        strcmp(c.fault, "_strdup guest heap is terminal") != 0 ||
        c.fault_addr != pointer32(s_strdup_source) || c.esp != esp ||
        s_guest_heap_malloc_calls != 2U ||
        s_guest_heap_free_calls != 1U || s_guest_heap_bad_free)
        return 16;

    s_guest_heap_terminal = 0;
    esp = prepare_call(&c, pointer32(s_strdup_source), 0U, 0U, 0U);
    isaac_vita_crt_strdup(&c);
    if (c.fault || c.esp != esp + 4U || c.eax != 0U ||
        ld32(esp + 4U) != pointer32(s_strdup_source) ||
        s_guest_heap_malloc_calls != 3U ||
        s_guest_heap_free_calls != 1U || s_guest_heap_bad_free)
        return 17;

    printf("Vita qsort/strdup production-handler oracle: PASS "
           "(live=%u wide=%u nested=%u comparisons; terminal cleanup=%u)\n",
           s_live_comparisons, s_wide_comparisons, s_nested_comparisons,
           s_guest_heap_free_calls);
    return 0;
}

int main(void)
{
    int result = run_oracle();
    if (result)
        fprintf(stderr, "Vita qsort oracle failure: %d\n", result);
    return result;
}
