#include "host_vita_memset_thunk_direct.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                           \
    ++s_checks;                                                         \
    if (!(condition)) {                                                 \
        fprintf(stderr, "memset-thunk oracle failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition);                        \
        return 1;                                                       \
    }                                                                   \
} while (0)

enum {
    MEMSET_IAT_RVA = 0x00606484U,
    MEMSET_IMPORT_ID = 280U,
    MEMSET_MEMORY_INDEX = 2U
};

static unsigned char s_import_coverage[413];
static unsigned s_checks;
static unsigned s_handler_calls;
static uint32_t s_handler_index;
static unsigned s_faults;
static int s_handler_accept;
static int s_handler_fault;

unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_imports = s_import_coverage;
unsigned char *g_guest_coverage_cases;
unsigned g_host_import_calls;
GuestPhaseProfileCounters g_guest_phase_profile_counters;
int g_isaac_vita_import_ids_ready;

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_faults;
    c->fault = what;
    c->fault_addr = address;
}

int isaac_vita_memory_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count)
{
    if (!s_handler_accept)
        return 0;
    ++s_handler_calls;
    s_handler_index = index;
    if (call_count)
        ++*call_count;
    if (s_handler_fault) {
        guest_fault(c, 0xface0002U, "injected memset fault");
        return 1;
    }
    c->eax = 0x0badc0deU;
    c->esp += 4U; /* the real cdecl handler consumes only the return word */
    return 1;
}

static void fixture(CPU *c)
{
    memset(c, 0xa5, sizeof *c);
    memset(s_import_coverage, 0, sizeof s_import_coverage);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    c->eax = 0x11111111U;
    c->ecx = 0x22222222U;
    c->edx = 0x33333333U;
    c->esp = 0x21004000U;
    c->fault = NULL;
    c->fault_addr = 0U;
    g_isaac_vita_import_ids_ready = 1;
    g_host_import_calls = 0U;
    s_handler_calls = 0U;
    s_handler_index = UINT32_MAX;
    s_faults = 0U;
    s_handler_accept = 1;
    s_handler_fault = 0;
}

static int expect_handled(uint32_t target)
{
    CPU c;
    uint32_t entry;

    fixture(&c);
    entry = c.esp;
    CHECK(isaac_vita_memset_thunk_direct_try(&c, target) == 1);
    CHECK(c.eax == 0x0badc0deU && c.esp == entry + 4U);
    CHECK(c.ecx == 0x22222222U && c.edx == 0x33333333U);
    CHECK(c.fault == NULL && c.fault_addr == 0U && s_faults == 0U);
    CHECK(s_handler_calls == 1U &&
          s_handler_index == MEMSET_MEMORY_INDEX &&
          g_host_import_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_lookups == 0U &&
          g_guest_phase_profile_counters.lookup_iterations == 0U);
    CHECK(s_import_coverage[MEMSET_IMPORT_ID] == 1U);
    return 0;
}

static int expect_rejected(uint32_t target, int ready)
{
    CPU c;
    CPU before;
    unsigned char coverage_before[sizeof s_import_coverage];

    fixture(&c);
    g_isaac_vita_import_ids_ready = ready;
    before = c;
    memcpy(coverage_before, s_import_coverage, sizeof coverage_before);
    CHECK(isaac_vita_memset_thunk_direct_try(&c, target) == 0);
    CHECK(memcmp(&c, &before, sizeof c) == 0);
    CHECK(memcmp(s_import_coverage, coverage_before,
                 sizeof coverage_before) == 0);
    CHECK(s_handler_calls == 0U && g_host_import_calls == 0U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 0U &&
          g_guest_phase_profile_counters.guest_lookups == 0U &&
          g_guest_phase_profile_counters.lookup_iterations == 0U);
    CHECK(s_faults == 0U);
    return 0;
}

static int expect_handler_reject_fault(void)
{
    CPU c;

    fixture(&c);
    s_handler_accept = 0;
    CHECK(isaac_vita_memset_thunk_direct_try(&c, MEMSET_IAT_RVA) == 1);
    CHECK(s_handler_calls == 0U && g_host_import_calls == 0U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(s_import_coverage[MEMSET_IMPORT_ID] == 1U);
    CHECK(s_faults == 1U && c.fault != NULL &&
          c.fault_addr == MEMSET_IAT_RVA);
    return 0;
}

static int expect_returning_handler_fault(void)
{
    CPU c;
    uint32_t entry;

    fixture(&c);
    entry = c.esp;
    s_handler_fault = 1;
    CHECK(isaac_vita_memset_thunk_direct_try(
              &c, GUEST_IMAGE_BASE + MEMSET_IAT_RVA) == 1);
    CHECK(s_handler_calls == 1U &&
          s_handler_index == MEMSET_MEMORY_INDEX &&
          g_host_import_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(s_import_coverage[MEMSET_IMPORT_ID] == 1U);
    CHECK(s_faults == 1U && c.fault != NULL &&
          c.fault_addr == 0xface0002U && c.esp == entry);
    return 0;
}

int main(void)
{
    CHECK(expect_handled(MEMSET_IAT_RVA) == 0);
    CHECK(expect_handled(GUEST_IMAGE_BASE + MEMSET_IAT_RVA) == 0);
    CHECK(expect_rejected(0x12345678U, 1) == 0);
    CHECK(expect_rejected(MEMSET_IAT_RVA, 0) == 0);
    CHECK(expect_handler_reject_fault() == 0);
    CHECK(expect_returning_handler_fault() == 0);
    CHECK(isaac_vita_memset_thunk_direct_try(NULL, MEMSET_IAT_RVA) == 0);
    printf("Vita memset thunk oracle: PASS; checks=%u; "
           "handled=2; rejected=3; hostile=2\n", s_checks);
    return 0;
}
