/* Host oracle for host_vita_update_probe.c (test_vita_update_probe.py).
 * Stubs the clock (+STEP us per read) and every __real_ body; checks that a
 * seam calls its body exactly once with the same CPU pointer, and pins the
 * counters: entries, the timing rule (first 64 of a window, then 1 in 64),
 * no double timing on re-entry, nested-span counts, sums, the empty bracket
 * cadence, and take/discard. */
#include "host_vita_update_probe.h"
#include "guest.h"

#include <stdio.h>
#include <string.h>

static uint64_t s_clock;
static uint64_t s_step = 1U;
uint64_t sceKernelGetProcessTimeWide(void)
{
    s_clock += s_step;
    return s_clock;
}

/* The dispatcher's import census (guest.c in production). */
uint32_t g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
#define NOTE(id, n) (g_guest_phase_profile_import_calls[(id)] += (n))

static unsigned s_body_calls[ISAAC_VITA_UPDATE_PROBES];
static CPU *s_last_cpu;
static unsigned s_recurse;
static int s_fail;

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); s_fail = 1; } } while (0)

#define DECL(r) void __wrap_sub_##r(CPU *c);
ISAAC_VITA_UPDATE_PROBE_LIST(DECL)
#undef DECL

#define IDX(r) I_##r,
enum { ISAAC_VITA_UPDATE_PROBE_LIST(IDX) I_END };
#undef IDX

/* Game::Update calls Room-level 0048a3a0 twice; 0048a3a0 calls the player
 * update once; 00003b00 recurses into itself s_recurse levels; the others
 * are leaves.  Every body burns 10 clock reads' worth of "work". */
static void body_common(unsigned i, CPU *c)
{
    ++s_body_calls[i];
    s_last_cpu = c;
    s_clock += 10U * s_step;
}

void __real_sub_002cdcf0(CPU *c)
{
    body_common(I_002cdcf0, c);
    __wrap_sub_0048a3a0(c);
    __wrap_sub_0048a3a0(c);
}
void __real_sub_0048a3a0(CPU *c)
{
    body_common(I_0048a3a0, c);
    __wrap_sub_0034b8d0(c);
}
void __real_sub_00003b00(CPU *c)
{
    body_common(I_00003b00, c);
    if (s_recurse) {
        --s_recurse;
        __wrap_sub_00003b00(c);
    }
}
/* GetMountedFilePath stand-in: 2 malloc, 1 free, 1 _access, 1 fread,
 * 1 EnterCriticalSection, 1 strncpy_s, and an import outside every group
 * (floor, 313).  The anm2 lookup stand-in adds a calloc and calls it. */
void __real_sub_00563120(CPU *c)
{
    body_common(I_00563120, c);
    NOTE(307, 2); NOTE(305, 1); NOTE(303, 1); NOTE(359, 1); NOTE(61, 1);
    NOTE(383, 1); NOTE(313, 5);
}
void __real_sub_00008be0(CPU *c)
{
    body_common(I_00008be0, c);
    NOTE(306, 1);
    __wrap_sub_00563120(c);
}
#define LEAF(r) void __real_sub_##r(CPU *c) { body_common(I_##r, c); }
/* Leaves: every listed function except the three above. */
#define LEAVES(X)                                                           \
    X(00263d10) X(005027e0) X(002c40e0) X(00484e40) X(0053bce0) X(004b3680) \
    X(004f0010) X(003f5950) X(003ebfd0) X(004e55f0) X(0050d5b0) X(003f1de0) \
    X(00313840) X(005178e0) X(005163a0) X(004894c0) X(003e9650) X(002d0aa0) \
    X(003bf630) X(0051a7e0) X(00013480) X(00311e60) X(00313a20) X(002ccfa0) \
    X(00488de0) X(0034b8d0) X(0029b140) X(001bdf00) X(001fe830) X(0022f6f0) \
    X(002b9ed0) X(0027ae90) X(00242990) X(004d0aa0) X(004ba640) X(00239870) \
    X(002860a0) X(00016f10) X(00003740) X(0040eef0) X(005637e0) X(00011fb0) \
    X(00289cf0) X(00289980) X(001b79c0) X(001b7ec7) X(002819e0) X(00007160) \
    X(000073a0) X(00007de0) X(00007c80) X(000054f0) X(004b3340) \
    X(00470460) X(003fccf0) X(003ffe10) X(001eeac0)
LEAVES(LEAF)

_Static_assert(ISAAC_VITA_UPDATE_PROBES == 62, "62 probes");
static IsaacVitaUpdateProbe s_take;

static const IsaacVitaUpdateProbeRow *row(unsigned i) { return &s_take.row[i]; }

int main(void)
{
    static CPU cpu;
    /* Guest == host addresses (x86 oracle): the Lua argument block that
     * 0x40eef0 reads at [ecx + 4], and Effect::Init's stack at entry:
     * [esp] return word, type, variant, subtype, seed. */
    static uint32_t lua_args[3] = {0x1234U, 1U, 0xffffffffU};
    static uint32_t init_stack[5] = {0x001b0000U, 1000U, 0x2cU, 5U, 77U};
    unsigned i, k;
    unsigned leaves = 0;

    cpu.ecx = (uint32_t)(uintptr_t)lua_args;
    cpu.esp = (uint32_t)(uintptr_t)init_stack;

#define COUNT_LEAF(r) ++leaves;
    LEAVES(COUNT_LEAF)
#undef COUNT_LEAF
    CHECK(leaves + 5U == ISAAC_VITA_UPDATE_PROBES);

    /* Window 0 is discarded (loading). */
    __wrap_sub_00263d10(&cpu);
    isaac_vita_update_probe_take(NULL);
    memset(s_body_calls, 0, sizeof s_body_calls);

    /* 1. One leaf, 200 entries: timed = first 64, then ordinals 64,128,192. */
    for (k = 0; k < 200U; ++k)
        __wrap_sub_00016f10(&cpu);
    CHECK(s_body_calls[I_00016f10] == 200U && s_last_cpu == &cpu);
    /* 2. Nested tree, 3 ticks: Game -> 2x Room -> Player. */
    for (k = 0; k < 3U; ++k)
        __wrap_sub_002cdcf0(&cpu);
    /* 3. Recursion: AdvancePosition re-entered 4 deep, once. */
    s_recurse = 4U;
    __wrap_sub_00003b00(&cpu);
    CHECK(s_body_calls[I_00003b00] == 5U);
    /* 4. Every leaf seam once. */
#define CALL_LEAF(r) __wrap_sub_##r(&cpu);
    LEAVES(CALL_LEAF)
#undef CALL_LEAF
    /* Composition: the anm2 lookup once (nesting GetMountedFilePath), then
     * census traffic outside every span, which must not be counted. */
    __wrap_sub_00008be0(&cpu);
    NOTE(307, 100); NOTE(303, 100);
    isaac_vita_update_probe_take(&s_take);
    {
        const IsaacVitaUpdateProbeComposed *mp = &s_take.composed[0];
        const IsaacVitaUpdateProbeComposed *an = &s_take.composed[1];
        CHECK(mp->rva == 0x00563120U && mp->t == 1U && mp->us == 11U &&
              mp->calls[0] == 3U && mp->calls[1] == 1U && mp->calls[2] == 1U &&
              mp->calls[3] == 1U && mp->calls[4] == 1U);
        CHECK(an->rva == 0x00008be0U && an->t == 1U && an->us == 23U &&
              an->calls[0] == 4U && an->calls[1] == 1U && an->calls[2] == 1U &&
              an->calls[3] == 1U && an->calls[4] == 1U);
        CHECK(s_take.composed[5].rva == 0x001b79c0U && s_take.composed[5].t == 1U &&
              s_take.composed[5].calls[0] == 0U);
        CHECK(s_take.composed[7].rva == 0x002860a0U && s_take.composed[7].t == 1U);
    }

    for (i = 0; i < ISAAC_VITA_UPDATE_PROBES; ++i)
        CHECK(row(i)->n == s_body_calls[i]);
    CHECK(row(I_00016f10)->rva == 0x00016f10U);
    CHECK(row(I_00016f10)->n == 201U && row(I_00016f10)->t == 64U + 3U + 0U);
    /* the 201st (ordinal 200) is not a multiple of 64: untimed */
    /* Leaf span = 10 body steps + the closing read = 11 us; nothing inside. */
    CHECK(row(I_00016f10)->us == 67U * 11U && row(I_00016f10)->in == 0U &&
          row(I_00016f10)->mx == 11U);
    CHECK(row(I_002cdcf0)->n == 3U && row(I_002cdcf0)->t == 3U);
    CHECK(row(I_0048a3a0)->n == 6U && row(I_0048a3a0)->t == 6U);
    /* player: 6 from Room + 1 leaf call */
    CHECK(row(I_0034b8d0)->n == 7U && row(I_0034b8d0)->t == 7U);
    /* Room span: own 10 + player (2 reads + 10) + its read = 23; player
     * span complete inside: in = 1 each. */
    CHECK(row(I_0048a3a0)->us == 6U * 23U && row(I_0048a3a0)->in == 6U);
    /* Game: own 10 + 2 * (1 + 23) + 1 = 59; in = 2 Rooms + 2 players. */
    CHECK(row(I_002cdcf0)->us == 3U * 59U && row(I_002cdcf0)->in == 12U &&
          row(I_002cdcf0)->mx == 59U);
    /* Recursion: 5 seam entries, only the outer one timed: own 10 + four
     * untimed inner bodies of 10 + the closing read. */
    CHECK(row(I_00003b00)->n == 5U && row(I_00003b00)->t == 1U);
    CHECK(row(I_00003b00)->us == 51U && row(I_00003b00)->in == 0U);
    /* Empty bracket: entry ordinals 512, 1536 ... (1-based count & 1023). */
    {
        unsigned entries = 0;
        for (i = 0; i < ISAAC_VITA_UPDATE_PROBES; ++i)
            entries += row(i)->n;
        CHECK(entries == 200U + 15U + 5U + 59U);
        CHECK(s_take.z_n == 0U);
    }
    /* Keyed probes: the window-0 discard dropped nothing keyed (no keyed
     * call); step 4 called each keyed probe once. */
    CHECK(s_take.keyed[0].rva == 0x0040eef0U && s_take.keyed[0].distinct == 1U &&
          s_take.keyed[0].top[0].key == 1U && s_take.keyed[0].top[0].n == 1U &&
          s_take.keyed[0].top[0].us == 11U && s_take.keyed[0].top[1].n == 0U);
    CHECK(s_take.keyed[1].rva == 0x001b79c0U && s_take.keyed[1].distinct == 1U &&
          s_take.keyed[1].top[0].key == 0x002c0005U &&
          s_take.keyed[1].top[0].us == 11U && s_take.keyed[1].overflow == 0U);
    /* take zeroes */
    isaac_vita_update_probe_take(&s_take);
    CHECK(s_take.keyed[0].distinct == 0U && s_take.keyed[0].top[0].n == 0U &&
          s_take.keyed[1].distinct == 0U);
    CHECK(s_take.composed[0].t == 0U && s_take.composed[1].calls[0] == 0U &&
          s_take.composed[0].rva == 0x00563120U);
    for (i = 0; i < ISAAC_VITA_UPDATE_PROBES; ++i)
        CHECK(row(i)->n == 0U && row(i)->t == 0U && row(i)->us == 0U &&
              row(i)->in == 0U && row(i)->mx == 0U && row(i)->rva != 0U);
    CHECK(s_take.z_n == 0U && s_take.z_us == 0U);

    /* 5. Empty bracket cadence: 1024 entries of one leaf -> exactly one
     * bracket of 1 us (two reads, step 1). */
    for (k = 0; k < 1024U; ++k)
        __wrap_sub_00011fb0(&cpu);
    isaac_vita_update_probe_take(&s_take);
    CHECK(s_take.z_n == 1U && s_take.z_us == 1U);
    CHECK(row(I_00011fb0)->t == 64U + 15U);
    /* 6. The bracket falls on the 512th entry, not the 511th. */
    for (k = 0; k < 511U; ++k)
        __wrap_sub_00011fb0(&cpu);
    isaac_vita_update_probe_take(&s_take);
    CHECK(s_take.z_n == 0U);
    for (k = 0; k < 512U; ++k)
        __wrap_sub_00011fb0(&cpu);
    isaac_vita_update_probe_take(&s_take);
    CHECK(s_take.z_n == 1U);

    /* 7. Keyed probes time every entry: 3 x id 1, 105 x id 2. */
    lua_args[1] = 1U;
    for (k = 0; k < 3U; ++k)
        __wrap_sub_0040eef0(&cpu);
    lua_args[1] = 2U;
    for (k = 0; k < 105U; ++k)
        __wrap_sub_0040eef0(&cpu);
    /* 70 distinct Init keys: 64 slots fill, 6 overflow; subtype masked. */
    for (k = 0; k < 70U; ++k) {
        init_stack[2] = k;
        init_stack[3] = 0x12345U;
        __wrap_sub_001b79c0(&cpu);
    }
    isaac_vita_update_probe_take(&s_take);
    CHECK(row(I_0040eef0)->n == 108U && row(I_0040eef0)->t == 108U);
    CHECK(s_take.keyed[0].distinct == 2U && s_take.keyed[0].overflow == 0U &&
          s_take.keyed[0].top[0].key == 2U && s_take.keyed[0].top[0].n == 105U &&
          s_take.keyed[0].top[0].us == 105U * 11U &&
          s_take.keyed[0].top[1].key == 1U && s_take.keyed[0].top[1].n == 3U &&
          s_take.keyed[0].top[1].us == 33U && s_take.keyed[0].top[2].n == 0U);
    CHECK(row(I_001b79c0)->n == 70U && row(I_001b79c0)->t == 70U);
    CHECK(s_take.keyed[1].distinct == 64U && s_take.keyed[1].overflow == 6U);
    for (i = 0; i < ISAAC_VITA_UPDATE_PROBE_TOP_KEYS; ++i)
        CHECK(s_take.keyed[1].top[i].n == 1U && s_take.keyed[1].top[i].us == 11U &&
              (s_take.keyed[1].top[i].key & 0xffffU) == 0x2345U &&
              (s_take.keyed[1].top[i].key >> 16) < 70U);
    /* 8. Ranking by us, and a 0 us span still lands in the table. */
    lua_args[1] = 7U;
    __wrap_sub_0040eef0(&cpu);
    s_step = 0U;
    lua_args[1] = 9U;
    __wrap_sub_0040eef0(&cpu);
    s_step = 1U;
    s_recurse = 0U;
    lua_args[1] = 8U;
    __wrap_sub_0040eef0(&cpu);
    __wrap_sub_0040eef0(&cpu);
    isaac_vita_update_probe_take(&s_take);
    CHECK(s_take.keyed[0].distinct == 3U &&
          s_take.keyed[0].top[0].key == 8U && s_take.keyed[0].top[0].us == 22U &&
          s_take.keyed[0].top[1].key == 7U && s_take.keyed[0].top[1].us == 11U &&
          s_take.keyed[0].top[2].key == 9U && s_take.keyed[0].top[2].n == 1U &&
          s_take.keyed[0].top[2].us == 0U);

    if (s_fail)
        return 1;
    printf("Vita update probe oracle: PASS; probes=%u\n",
           (unsigned)ISAAC_VITA_UPDATE_PROBES);
    return 0;
}
