/* Host oracle for ISAAC_VITA_BACKDROP_WATERGFX_GUARD (MSVC x86: guest
 * address == host address, as on the Vita).  Driven by
 * recomp/test_backdrop_watergfx_guard.py, which extracts the two generated
 * seams verbatim from the Vita-base rendering into a harness TU compiled in
 * both GPR spellings (watergfx_owner_gpr/_mem, watergfx_loader_gpr/_mem).
 *
 * ORACLE_GUARD_ON=1: the harness TUs see __vita__ and the knob, and this TU
 * links the real helper with its log redirected here.  ORACLE_GUARD_ON=0:
 * the same extracted text with the knob undefined must behave exactly like
 * the frozen code (fall through, no log).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

#ifndef ORACLE_GUARD_ON
#error "define ORACLE_GUARD_ON to 0 or 1"
#endif

#define ORACLE_LOG_LINES 256u
static char s_log[ORACLE_LOG_LINES][512];
static unsigned s_log_count;

#if ORACLE_GUARD_ON
static void oracle_log(const char *format, ...)
{
    va_list args;
    if (s_log_count >= ORACLE_LOG_LINES) {
        ++s_log_count;
        return;
    }
    va_start(args, format);
    vsnprintf(s_log[s_log_count], sizeof(s_log[0]), format, args);
    va_end(args);
    ++s_log_count;
}
#define KAGE_VITA_BACKDROP_WATERGFX_LOG oracle_log
#define ISAAC_VITA_BACKDROP_WATERGFX_GUARD 1
#include "kage_vita_backdrop_watergfx.c"
#endif

int watergfx_owner_gpr(CPU *__restrict c);
int watergfx_owner_mem(CPU *__restrict c);
int watergfx_loader_gpr(CPU *__restrict c);
int watergfx_loader_mem(CPU *__restrict c);

typedef int (*harness_fn)(CPU *__restrict c);

static unsigned s_failures;
static unsigned s_checks;

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        ++s_failures; \
        printf("FAIL %s line %d: %s\n", s_mode, __LINE__, #condition); \
    } \
} while (0)

static const char *s_mode = "?";

/* Backdrop object: id at +0x150, per-id std::string at
 * +0x170 + id*0x13c (size +0x10, capacity +0x14, inline while < 0x10). */
static uint32_t s_backdrop_storage[0x20000u / 4u];
static uint32_t s_frame_storage[64];
static char s_heap_names[8][64];

static uint32_t addr(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint8_t *backdrop_bytes(void)
{
    return (uint8_t *)s_backdrop_storage;
}

static void put32(uint8_t *base, uint32_t offset, uint32_t value)
{
    memcpy(base + offset, &value, 4u);
}

static void set_backdrop(uint32_t id, uint32_t holder, const char *name,
                         uint32_t size_override, uint32_t capacity_override,
                         unsigned heap_slot)
{
    uint8_t *base = backdrop_bytes();
    uint32_t string = 0x170u + id * 0x13cu;
    size_t length = strlen(name);

    memset(base + string, 0xcc, 0x18u);
    put32(base, 0x150u, id);
    put32(base, 0x2cu, holder);
    if (length < 16u) {
        memcpy(base + string, name, length + 1u);
        put32(base, string + 0x10u, (uint32_t)length);
        put32(base, string + 0x14u, 15u);
    } else {
        strcpy(s_heap_names[heap_slot], name);
        put32(base, string, addr(s_heap_names[heap_slot]));
        put32(base, string + 0x10u, (uint32_t)length);
        put32(base, string + 0x14u, 31u);
    }
    if (size_override != 0xffffffffu)
        put32(base, string + 0x10u, size_override);
    if (capacity_override != 0xffffffffu)
        put32(base, string + 0x14u, capacity_override);
}

static CPU make_cpu(void)
{
    CPU c;
    memset(&c, 0, sizeof(c));
    c.eax = 0xa1a1a1a1u;
    c.ecx = 0xc1c1c1c1u;
    c.edx = 0xd1d1d1d1u;
    c.ebx = 0xb1b1b1b1u;
    c.esp = 0x0badf00cu;
    c.ebp = addr(&s_frame_storage[16]);
    c.esi = addr(s_backdrop_storage);
    c.edi = 0xe1e1e1e1u;
    return c;
}

static int others_unchanged(const CPU *before, const CPU *after,
                            int ecx_may_change, int esi_may_change)
{
    return before->eax == after->eax && before->edx == after->edx &&
           before->ebx == after->ebx && before->esp == after->esp &&
           before->ebp == after->ebp && before->edi == after->edi &&
           (ecx_may_change || before->ecx == after->ecx) &&
           (esi_may_change || before->esi == after->esi);
}

static int last_log_is(const char *expected)
{
    return s_log_count > 0u && s_log_count <= ORACLE_LOG_LINES &&
           strcmp(s_log[s_log_count - 1u], expected) == 0;
}

static void run_owner(harness_fn owner)
{
    CPU before, c;
    unsigned logs;

    /* Live holder: the original pushes/call follow, nothing else moves. */
    set_backdrop(30u, 0x12345678u, "gfx/backdrop/water_blood.png",
                 0xffffffffu, 0xffffffffu, 0u);
    c = make_cpu();
    before = c;
    logs = s_log_count;
    CHECK(owner(&c) == 0);
    CHECK(c.ecx == 0x12345678u);
    CHECK(others_unchanged(&before, &c, 1, 0));
    CHECK(s_log_count == logs);

    /* NULL holder, heap string (rc3: backdrop 30, water_blood.png). */
    set_backdrop(30u, 0u, "gfx/backdrop/water_blood.png",
                 0xffffffffu, 0xffffffffu, 0u);
    c = make_cpu();
    before = c;
    logs = s_log_count;
#if ORACLE_GUARD_ON
    CHECK(owner(&c) == 1);            /* rejoined at L_00498cec */
    CHECK(c.ecx == 30u);              /* the replicated 00498ce6 load */
    CHECK(others_unchanged(&before, &c, 1, 0));   /* ESP: post-ret-8 value */
#else
    CHECK(owner(&c) == 0);            /* frozen: falls into the fault */
    CHECK(c.ecx == 0u);
    CHECK(others_unchanged(&before, &c, 1, 0));
#endif
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(last_log_is("KAGE VITA BACKDROP WATERGFX: id=30 "
                      "name=gfx/backdrop/water_blood.png NULL -> skipped"));
#endif

    /* Same id again: skipped, logged once per id. */
    c = make_cpu();
    logs = s_log_count;
    CHECK(owner(&c) == ORACLE_GUARD_ON);
    CHECK(s_log_count == logs);

    /* Inline (SSO) string, Flooded Caves id 6. */
    set_backdrop(6u, 0u, "water.png", 0xffffffffu, 0xffffffffu, 1u);
    c = make_cpu();
    before = c;
    logs = s_log_count;
    CHECK(owner(&c) == ORACLE_GUARD_ON);
    CHECK(others_unchanged(&before, &c, 1, 0));
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(c.ecx == 6u);
    CHECK(last_log_is("KAGE VITA BACKDROP WATERGFX: id=6 name=water.png "
                      "NULL -> skipped"));
#endif

    /* Broken string header (size > capacity): still skipped, name "?". */
    set_backdrop(7u, 0u, "water.png", 40u, 15u, 2u);
    c = make_cpu();
    logs = s_log_count;
    CHECK(owner(&c) == ORACLE_GUARD_ON);
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(c.ecx == 7u);
    CHECK(last_log_is("KAGE VITA BACKDROP WATERGFX: id=7 name=? NULL -> skipped"));
#endif

    /* Ids past the tracked range share one report. */
    set_backdrop(300u, 0u, "water_x.png", 0xffffffffu, 0xffffffffu, 3u);
    c = make_cpu();
    logs = s_log_count;
    CHECK(owner(&c) == ORACLE_GUARD_ON);
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(c.ecx == 300u);
    CHECK(last_log_is("KAGE VITA BACKDROP WATERGFX: id=300 name=water_x.png "
                      "NULL -> skipped"));
#endif
    set_backdrop(301u, 0u, "water_y.png", 0xffffffffu, 0xffffffffu, 4u);
    c = make_cpu();
    logs = s_log_count;
    CHECK(owner(&c) == ORACLE_GUARD_ON);
    CHECK(s_log_count == logs);
#if ORACLE_GUARD_ON
    CHECK(c.ecx == 301u);
#endif
}

static void run_loader(harness_fn loader)
{
    static const char path_a[] =
        "mods/timemachine_2617557401/resources/gfx/backdrop/water_blood.png";
    static const char path_b[] = "gfx/backdrop/water.png";
    static char long_path[400];
    static char paths[70][24];
    CPU before, c;
    unsigned logs;
    unsigned i;

    s_frame_storage[16 + 2] = 0x5eed0008u;   /* [ebp + 8]: result pair */

    c = make_cpu();
    c.edi = addr(path_a);
    before = c;
    logs = s_log_count;
    CHECK(loader(&c) == 0);
    CHECK(c.esi == 0x5eed0008u);          /* 0056c834 itself still ran */
    CHECK(others_unchanged(&before, &c, 0, 1));
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(last_log_is("image load: resolve failed for \"mods/timemachine_"
                      "2617557401/resources/gfx/backdrop/water_blood.png\""));
#endif

    /* Same path: once per distinct path. */
    c = make_cpu();
    c.edi = addr(path_a);
    logs = s_log_count;
    CHECK(loader(&c) == 0);
    CHECK(s_log_count == logs);

    c = make_cpu();
    c.edi = addr(path_b);
    logs = s_log_count;
    CHECK(loader(&c) == 0);
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(last_log_is("image load: resolve failed for \"gfx/backdrop/water.png\""));
#endif

    /* NULL path pointer. */
    c = make_cpu();
    c.edi = 0u;
    logs = s_log_count;
    CHECK(loader(&c) == 0);
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    CHECK(last_log_is("image load: resolve failed for \"?\""));
#endif

    /* Long path: bounded to 255 bytes. */
    memset(long_path, 'a', sizeof(long_path) - 1u);
    long_path[sizeof(long_path) - 1u] = '\0';
    c = make_cpu();
    c.edi = addr(long_path);
    logs = s_log_count;
    CHECK(loader(&c) == 0);
    CHECK(s_log_count == logs + (unsigned)ORACLE_GUARD_ON);
#if ORACLE_GUARD_ON
    {
        const char *line = s_log[s_log_count - 1u];
        const char *open = strchr(line, '"');
        const char *close = open ? strchr(open + 1, '"') : NULL;
        CHECK(open != NULL && close != NULL && close - open - 1 == 255);
    }
#endif

    /* Fill the 64-entry table (4 used above), then the bounded tail logs
     * the 1st, 2nd, 4th failure only. */
    for (i = 0u; i < 70u; ++i) {
        snprintf(paths[i], sizeof(paths[i]), "gfx/missing_%02u.png", i);
        c = make_cpu();
        c.edi = addr(paths[i]);
        logs = s_log_count;
        CHECK(loader(&c) == 0);
#if ORACLE_GUARD_ON
        if (i < 60u) {
            CHECK(s_log_count == logs + 1u);
        } else {
            unsigned untracked = i - 59u;
            unsigned expect = (untracked & (untracked - 1u)) == 0u;
            CHECK(s_log_count == logs + expect);
            if (expect) {
                char expected[160];
                snprintf(expected, sizeof(expected),
                         "image load: resolve failed for \"%s\" "
                         "(untracked=%u after 64 distinct paths)",
                         paths[i], untracked);
                CHECK(last_log_is(expected));
            }
        }
#else
        CHECK(s_log_count == logs);
#endif
    }
}

int main(void)
{
    s_mode = "gpr";
    run_owner(watergfx_owner_gpr);
    run_loader(watergfx_loader_gpr);
#if ORACLE_GUARD_ON
    /* Both spellings share the helper's once-only state; reset it so the
     * memory-mode pass sees the same first-occurrence logs. */
    memset(s_logged_ids, 0, sizeof(s_logged_ids));
    s_logged_untracked_id = 0u;
    memset(s_path_hashes, 0, sizeof(s_path_hashes));
    s_path_count = 0u;
    s_untracked_failures = 0u;
#endif
    s_mode = "mem";
    run_owner(watergfx_owner_mem);
    run_loader(watergfx_loader_mem);
    printf("watergfx oracle guard=%d: checks=%u failures=%u logs=%u\n",
           ORACLE_GUARD_ON, s_checks, s_failures, s_log_count);
    return s_failures == 0u ? 0 : 1;
}
