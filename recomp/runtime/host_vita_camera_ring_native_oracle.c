/* Differential oracle of host_vita_camera_ring_native.c.
 *
 * test_vita_camera_ring_native.py links two copies of the generated
 * sub_0049df90: ref_sub_0049df90 (the body with its seam block removed, which
 * the test pins equal to the frozen int3-off body) and seam_sub_0049df90 (the
 * seamed body, guard forced on).  The seam calls isaac_vita_camera_ring_try
 * below, which counts verdicts around the replay (built as its own TU with
 * the production defines and renamed native_camera_ring_try).  Every case
 * builds one world - the four image constants, a ring header and its element
 * array, the out vector, a register file with random xmm lanes and an entry
 * ESP - and runs both bodies under guest_run_until_stop on identical copies,
 * then compares the CPU struct and every byte of the world (stack with guard
 * words, heap, both image pages).  Each case also states the verdict the seam
 * must reach (handled or declined), so a replay that declines everything
 * fails.  Probe cases, whose translated body would fault on the host (null or
 * wrapping pointers), call the replay alone and require an untouched decline.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <math.h>
#include <stddef.h>
#include "host_vita_camera_ring_native.h"
#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_camera_ring_native_oracle requires a 32-bit host
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))
#define RVA_RESET_X 0x00802fecu
#define RVA_RESET_Y 0x00802ff0u
#define RVA_ONE 0x0076a614u
#define RVA_LOW 0x0076ae44u

/* ---- stubs owned by other Vita runtime units (the kage-quad oracle's) - */
unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
int g_isaac_vita_import_ids_ready = 1;
int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    (void)imports;
    (void)count;
    return 1;
}
int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    (void)c;
    (void)import_id;
    return 0;
}
const char *guest_host_import_id_error(void)
{
    return "oracle import-ID error";
}
int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

/* ---- the two bodies and the replay ------------------------------------ */
void ref_sub_0049df90(CPU *__restrict c);
void seam_sub_0049df90(CPU *__restrict c);
int native_camera_ring_try(CPU *__restrict c);

static unsigned s_try_calls, s_try_handled;
int isaac_vita_camera_ring_try(CPU *__restrict c);
int isaac_vita_camera_ring_try(CPU *__restrict c)
{
    int handled = native_camera_ring_try(c);
    ++s_try_calls;
    if (handled)
        ++s_try_handled;
    return handled;
}

/* KAGE InverseLerp logger (cdecl, two words the caller pops). */
static unsigned s_logger_calls;
void sub_0055e330(CPU *__restrict c);
void sub_0055e330(CPU *__restrict c)
{
    ++s_logger_calls;
    c->eax = 0x0bad1065u;
    (void)gpop(c);
}

/* libgcc helpers the clang i686-gnu bodies call for the idiv's int64 / and %
 * (cdecl, edx:eax result: the MSVC ABI for long long). */
long long __divdi3(long long a, long long b);
long long __moddi3(long long a, long long b);
long long __divdi3(long long a, long long b) { return a / b; }
long long __moddi3(long long a, long long b) { return a % b; }

/* The bodies may come from another compiler (clang, GUEST_SSE_LOWER=1):
 * their view of CPU must be this TU's.  Same expressions as LAYOUT in the
 * test script. */
void oracle_bodies_layout(uint32_t *out);
static void oracle_layout(uint32_t *out)
{
    out[0] = (uint32_t)sizeof(CPU);
    out[1] = (uint32_t)sizeof(xmm_t);
    out[2] = (uint32_t)_Alignof(xmm_t);
    out[3] = (uint32_t)offsetof(CPU, eax);
    out[4] = (uint32_t)offsetof(CPU, esp);
    out[5] = (uint32_t)offsetof(CPU, x);
    out[6] = (uint32_t)offsetof(CPU, fl);
    out[7] = (uint32_t)offsetof(CPU, stack_floor);
    out[8] = (uint32_t)offsetof(CPU, stack_owner);
    out[9] = (uint32_t)offsetof(CPU, run_scope);
    out[10] = (uint32_t)offsetof(CPU, fault);
    out[11] = (uint32_t)offsetof(CPU, stop_kind);
}

static void ref_entry(CPU *__restrict c) { ref_sub_0049df90(c); }
static void seam_entry(CPU *__restrict c) { seam_sub_0049df90(c); }

/* ---- the world ------------------------------------------------------- */
#define STACK_WORDS 1024u
#define GUARD_WORDS 64u
#define ABOVE_WORDS 64u
#define HEAP_BYTES 0x10000u
#define RING_AT 0x100u            /* ring header offset in the heap */
#define DATA_AT 0x4000u           /* element array offset (margin below) */
#define MAX_CAP 0x1400u           /* elements that fit above DATA_AT */
#define ORACLE_PAGE 0x10000u

static uint32_t s_stack[GUARD_WORDS + STACK_WORDS + ABOVE_WORDS];
static __declspec(align(16)) uint8_t s_heap[HEAP_BYTES];
static CPU s_cpu;
static const uint32_t s_pages[] = { 0x00760000u, 0x00800000u };

typedef struct region { void *at; size_t bytes; } region;
static region s_regions[8];
static unsigned s_region_count;
static uint8_t *s_snap[8], *s_ref[8];
static CPU s_cpu_snap, s_cpu_ref;

static void add_region(void *at, size_t bytes)
{
    s_regions[s_region_count].at = at;
    s_regions[s_region_count].bytes = bytes;
    s_snap[s_region_count] = (uint8_t *)malloc(bytes);
    s_ref[s_region_count] = (uint8_t *)malloc(bytes);
    ++s_region_count;
}

static void snapshot(uint8_t **to, CPU *cpu_to)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        memcpy(to[i], s_regions[i].at, s_regions[i].bytes);
    *cpu_to = s_cpu;
}

static void restore(uint8_t **from, const CPU *cpu_from)
{
    unsigned i;
    for (i = 0; i < s_region_count; ++i)
        memcpy(s_regions[i].at, from[i], s_regions[i].bytes);
    s_cpu = *cpu_from;
}

static uint32_t s_lcg = 0x2026101cu;
static uint32_t lcg(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg >> 8;
}
static uint32_t lcg32(void) { return (lcg() << 16) ^ lcg(); }

static uint32_t A(const void *p) { return (uint32_t)(uintptr_t)p; }
static uint32_t fbits(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }

/* frozen constants from argv: X0, Y0, ONE, LOW as the PE stores them */
static uint32_t s_frozen[4];

/* ---- one case ---------------------------------------------------------- */
enum { HANDLED = 0, DECLINE = 1, PROBE = 2, LOGGER = 3 };
enum { ELEM_CAMERA, ELEM_WIDE, ELEM_SPECIAL, ELEM_ZERO };

typedef struct ccase {
    const char *name;
    int32_t count, cap, start;
    float frac;
    uint32_t frac_bits;           /* used when use_frac_bits */
    int use_frac_bits;
    uint32_t konst[4];            /* X0, Y0, ONE, LOW */
    int elem;
    int nan_slot;                 /* element index set to a NaN, or -1 */
    int nan_y;                    /* NaN in .y instead of .x */
    uint32_t esp_offset;          /* bytes above the floor */
    int out_mode;                 /* 0 caller frame e+0x30, 1 heap, 2 = out_addr,
                                     3 = e + out_rel */
    uint32_t out_addr;
    int32_t out_rel;
    int ring_mode;                /* 0 heap, 1 the frame word e-0x20, 2 ring_addr */
    uint32_t ring_addr;
    int data_mode;                /* 0 heap, 1 = e + data_rel, 2 = data_addr */
    int32_t data_rel;
    uint32_t data_addr;
    int unbind;
    int fix_regs;                 /* esi/edi/ebx = fixed_regs (ring in frame) */
    uint32_t fixed_regs[3];
    int expect;
} ccase;

static unsigned s_cases, s_failures, s_handled, s_declined, s_probes;
static unsigned s_logger_cases, s_fault_cases, s_wrap_cases, s_steps;

static void fail(const ccase *k, const char *what)
{
    ++s_failures;
    printf("FAIL %s: %s\n", k->name, what);
}

static float elem_value(int kind)
{
    static const uint32_t specials[] = {
        0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x00000001u,
        0x807fffffu, 0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0x00800000u };
    uint32_t b;
    switch (kind) {
    case ELEM_CAMERA:
        return (float)(int32_t)(lcg() % 160000u) / 128.0f - 200.0f;
    case ELEM_WIDE:
        b = lcg32();
        if ((b & 0x7f800000u) == 0x7f800000u)
            b &= 0xbfffffffu;     /* keep it finite, not NaN */
        { float v; memcpy(&v, &b, 4); return v; }
    case ELEM_SPECIAL:
        b = specials[lcg() % (sizeof specials / sizeof specials[0])];
        { float v; memcpy(&v, &b, 4); return v; }
    default:
        return 0.0f;
    }
}

/* ph120.cr totals over every case, summed from per-case takes */
static IsaacVitaCameraRingCounters s_counter_totals;

/* Take this case's counters: exactly one verdict moved, and it agrees with
 * the replay's return value (and, for logger states, names the logger). */
static void check_counters(const ccase *k, int handled)
{
    IsaacVitaCameraRingCounters d;
    uint32_t r, moved = 0U;
    isaac_vita_camera_ring_take(&d);
    for (r = 0; r < ISAAC_VITA_CAMERA_RING_DECLINE_REASONS; ++r) {
        moved += d.declined[r];
        s_counter_totals.declined[r] += d.declined[r];
    }
    s_counter_totals.calls += d.calls;
    s_counter_totals.handled += d.handled;
    s_counter_totals.steps += d.steps;
    if (d.calls != 1U || d.handled + moved != 1U || d.handled != (uint32_t)(handled != 0))
        fail(k, "verdict counters disagree with the verdict");
    if (k->expect == LOGGER && d.declined[ISAAC_VITA_CAMERA_RING_DECLINE_LOGGER] != 1U)
        fail(k, "a logger state was not counted as d(l)");
    if (d.handled && d.steps != (uint32_t)(k->count > 0 ? k->count : 0))
        fail(k, "st is not the replayed step count");
}

static void run_case(const ccase *k)
{
    const uint32_t floor = A(&s_stack[GUARD_WORDS]);
    uint32_t e, ring, data, out, i;
    unsigned calls_before, handled_before, logger_before, logger_ref;
    int stop_ref, stop_seam, handled;

    ++s_cases;
    memset(s_stack, 0xcd, sizeof s_stack);
    memset(s_heap, 0xab, sizeof s_heap);
    st32(VA(RVA_RESET_X), k->konst[0]);
    st32(VA(RVA_RESET_Y), k->konst[1]);
    st32(VA(RVA_ONE), k->konst[2]);
    st32(VA(RVA_LOW), k->konst[3]);

    guest_cpu_init(&s_cpu);
    if (guest_stack_bind(&s_cpu, floor, floor + 4u * STACK_WORDS)) {
        printf("stack bind failed\n");
        exit(2);
    }
    e = floor + k->esp_offset;

    ring = k->ring_mode == 0 ? A(s_heap) + RING_AT :
           k->ring_mode == 1 ? e - 0x20u : k->ring_addr;
    data = k->data_mode == 0 ? A(s_heap) + DATA_AT :
           k->data_mode == 1 ? e + (uint32_t)k->data_rel : k->data_addr;
    out = k->out_mode == 0 ? e + 0x30u :
          k->out_mode == 1 ? A(s_heap) + 0x40u :
          k->out_mode == 2 ? k->out_addr : e + (uint32_t)k->out_rel;

    if (k->data_mode == 0) {
        uint32_t n = k->cap > 0 && (uint32_t)k->cap < MAX_CAP ? (uint32_t)k->cap : 64u;
        for (i = 0; i < n; ++i) {
            stf(data + 8u * i, elem_value(k->elem));
            stf(data + 8u * i + 4u, elem_value(k->elem));
        }
        /* below the array: what a negative remainder reaches */
        for (i = 1; i <= 0x7f0u; ++i) {
            stf(data - 8u * i, elem_value(ELEM_CAMERA));
            stf(data - 8u * i + 4u, elem_value(ELEM_CAMERA));
        }
        if (k->nan_slot >= 0)
            st32(data + 8u * (uint32_t)k->nan_slot + (k->nan_y ? 4u : 0u),
                 (lcg() & 1u) ? 0x7fc01234u : 0xff812345u);
    }
    if (k->ring_mode != 1 || k->fix_regs == 0) {
        /* ring_mode 1 takes count/cap/start from the pushed registers;
         * probe rings (0, wrapping) are never written */
        if (k->ring_mode != 2 || (k->ring_addr != 0u && k->ring_addr < 0xfffffff0u)) {
            st32(ring + 0u, data);
            st32(ring + 4u, (uint32_t)k->cap);
            st32(ring + 8u, (uint32_t)k->count);
            st32(ring + 12u, (uint32_t)k->start);
        }
    } else {
        st32(ring + 0u, data);    /* e-0x20: below the frame, untouched */
    }

    s_cpu.eax = lcg32();
    s_cpu.ebx = lcg32();
    s_cpu.ecx = lcg32();
    s_cpu.edx = lcg32();
    s_cpu.ebp = floor + 4u * STACK_WORDS - 0x40u;
    s_cpu.esi = lcg32();
    s_cpu.edi = lcg32();
    if (k->fix_regs) {
        s_cpu.esi = k->fixed_regs[0];
        s_cpu.edi = k->fixed_regs[1];
        s_cpu.ebx = k->fixed_regs[2];
    }
    s_cpu.esp = e;
    for (i = 0; i < 8u; ++i) {
        s_cpu.x[i].u32[0] = lcg32();
        s_cpu.x[i].u32[1] = lcg32();
        s_cpu.x[i].u32[2] = lcg32();
        s_cpu.x[i].u32[3] = lcg32();
    }
    s_cpu.x[3].u32[0] = k->use_frac_bits ? k->frac_bits : fbits(k->frac);
    st32(e, 0x0049f16fu);                     /* the caller's return word */
    if (k->esp_offset + 12u <= 4u * (STACK_WORDS + ABOVE_WORDS)) {
        st32(e + 4u, out);
        st32(e + 8u, ring);
    }
    if (k->unbind)
        s_cpu.stack_owner = NULL;
    snapshot(s_snap, &s_cpu_snap);

    isaac_vita_camera_ring_take(NULL);
    if (k->expect == PROBE) {
        int probe_handled;
        ++s_probes;
        probe_handled = native_camera_ring_try(&s_cpu);
        check_counters(k, probe_handled);
        if (probe_handled != 0)
            fail(k, "probe was handled");
        if (memcmp(&s_cpu, &s_cpu_snap, sizeof s_cpu) != 0)
            fail(k, "probe decline touched the CPU");
        for (i = 0; i < s_region_count; ++i)
            if (memcmp(s_regions[i].at, s_snap[i], s_regions[i].bytes) != 0)
                fail(k, "probe decline touched memory");
        return;
    }

    logger_before = s_logger_calls;
    stop_ref = guest_run_until_stop(&s_cpu, ref_entry);
    logger_ref = s_logger_calls - logger_before;
    snapshot(s_ref, &s_cpu_ref);

    restore(s_snap, &s_cpu_snap);
    calls_before = s_try_calls;
    handled_before = s_try_handled;
    stop_seam = guest_run_until_stop(&s_cpu, seam_entry);
    handled = s_try_handled != handled_before;
    check_counters(k, handled);

    if (s_try_calls != calls_before + 1u)
        fail(k, "the seam did not call the replay exactly once");
    if (logger_ref) {
        ++s_logger_cases;
        if (handled)
            fail(k, "a logger state was handled");
        if (k->expect != LOGGER)
            fail(k, "unexpected logger call");
    } else if (k->expect == LOGGER) {
        fail(k, "expected the logger");
    }
    if (k->expect == HANDLED && !handled)
        fail(k, "declined a replayable state");
    if ((k->expect == DECLINE || k->expect == LOGGER) && handled)
        fail(k, "handled a state it must decline");
    if (handled) {
        ++s_handled;
        if (k->count > 0)
            s_steps += (unsigned)k->count;
    } else {
        ++s_declined;
    }
    if (stop_ref == GUEST_RUN_FAULT)
        ++s_fault_cases;
    if (stop_ref != stop_seam)
        fail(k, "stop kind differs");
    /* each body carries its own copy of the fault text */
    if (s_cpu.fault && s_cpu_ref.fault && strcmp(s_cpu.fault, s_cpu_ref.fault) == 0)
        s_cpu.fault = s_cpu_ref.fault;
    if (memcmp(&s_cpu, &s_cpu_ref, sizeof s_cpu) != 0) {
        fail(k, "CPU state differs");
        printf("  ref eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
               "esi=%08x edi=%08x\n  nat eax=%08x ecx=%08x edx=%08x ebx=%08x "
               "esp=%08x ebp=%08x esi=%08x edi=%08x\n",
               s_cpu_ref.eax, s_cpu_ref.ecx, s_cpu_ref.edx, s_cpu_ref.ebx,
               s_cpu_ref.esp, s_cpu_ref.ebp, s_cpu_ref.esi, s_cpu_ref.edi,
               s_cpu.eax, s_cpu.ecx, s_cpu.edx, s_cpu.ebx, s_cpu.esp,
               s_cpu.ebp, s_cpu.esi, s_cpu.edi);
        for (i = 0; i < 8u; ++i)
            if (memcmp(&s_cpu.x[i], &s_cpu_ref.x[i], sizeof s_cpu.x[i]) != 0)
                printf("  x%u ref %08x %08x %08x %08x nat %08x %08x %08x %08x\n", i,
                       s_cpu_ref.x[i].u32[0], s_cpu_ref.x[i].u32[1],
                       s_cpu_ref.x[i].u32[2], s_cpu_ref.x[i].u32[3],
                       s_cpu.x[i].u32[0], s_cpu.x[i].u32[1],
                       s_cpu.x[i].u32[2], s_cpu.x[i].u32[3]);
    }
    for (i = 0; i < s_region_count; ++i) {
        if (memcmp(s_regions[i].at, s_ref[i], s_regions[i].bytes) != 0) {
            size_t b;
            for (b = 0; b < s_regions[i].bytes &&
                        ((uint8_t *)s_regions[i].at)[b] == s_ref[i][b]; ++b)
                ;
            printf("FAIL %s: region %u differs at +0x%x (%08x)\n", k->name, i,
                   (unsigned)b, A((uint8_t *)s_regions[i].at + b));
            ++s_failures;
        }
    }
}

static void init_case(ccase *k, const char *name)
{
    memset(k, 0, sizeof *k);
    k->name = name;
    k->count = 300;
    k->cap = 300;
    k->start = 17;
    k->frac = 0.75f;
    memcpy(k->konst, s_frozen, sizeof k->konst);
    k->elem = ELEM_CAMERA;
    k->nan_slot = -1;
    k->esp_offset = 4u * (STACK_WORDS / 2u);
    k->expect = HANDLED;
}

/* ---- frozen bytes ------------------------------------------------------ */
static int parse_frozen(const char *hex)
{
    unsigned i, j;
    if (strlen(hex) != 32u)
        return 0;
    for (i = 0; i < 4u; ++i) {
        uint32_t w = 0;
        for (j = 0; j < 4u; ++j) {       /* little-endian bytes */
            unsigned v = 0, n;
            for (n = 0; n < 2u; ++n) {
                char ch = hex[8u * i + 2u * j + n];
                v <<= 4;
                if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
                else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
                else return 0;
            }
            w |= (uint32_t)v << (8u * j);
        }
        s_frozen[i] = w;
    }
    return 1;
}

static int commit_page(uint32_t base)
{
    void *r = VirtualAlloc((LPVOID)(uintptr_t)base, ORACLE_PAGE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return r != NULL && (uintptr_t)r == base;
}

int main(int argc, char **argv)
{
    static const float fracs[] = { 0.5f, 0.75f, 0.85f, 0.9f };
    static char names[8192][56];
    unsigned name_index = 0, i, round;
    ccase k;

    if (argc != 2 || !parse_frozen(argv[1])) {
        fprintf(stderr, "usage: oracle <32 hex digits: X0 Y0 ONE LOW>\n");
        return 2;
    }
    {
        uint32_t mine[12], theirs[12];
        oracle_layout(mine);
        oracle_bodies_layout(theirs);
        if (memcmp(mine, theirs, sizeof mine) != 0) {
            for (i = 0; i < 12u; ++i)
                fprintf(stderr, "layout[%u] oracle %u bodies %u\n", i, mine[i],
                        theirs[i]);
            return 2;
        }
    }
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        if (!commit_page(VA(s_pages[i]))) {
            fprintf(stderr, "could not commit %08x (/LARGEADDRESSAWARE)\n",
                    VA(s_pages[i]));
            return 2;
        }
    add_region(s_stack, sizeof s_stack);
    add_region(s_heap, sizeof s_heap);
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        add_region((void *)(uintptr_t)VA(s_pages[i]), ORACLE_PAGE);

#define NAME(...) (snprintf(names[name_index % 8192u], sizeof names[0], __VA_ARGS__), \
                   names[name_index++ % 8192u])

    /* the measured world: count = cap = 300 at the callers' fractions */
    for (i = 0; i < 4u; ++i) {
        init_case(&k, NAME("measured-frac-%u", i));
        k.frac = fracs[i];
        k.start = (int32_t)(lcg() % 300u);
        run_case(&k);
    }
    /* count/cap/start classes */
    {
        static const int32_t shapes[][3] = {   /* count, cap, start */
            {1, 1, 0}, {1, 300, 299}, {2, 2, 1}, {5, 300, 298}, {300, 300, 0},
            {300, 300, 299}, {299, 300, 150}, {7, 3, 2}, {50, 7, 6},
            {64, 16, 15}, {600, 300, 299}, {1000, 3, 0}, {3, 1, 0},
            {0x1000, 0x1000, 0xfff}, {40, 0x13ff, 0x13f0},
            {0, 300, 5}, {-1, 300, 5}, {(int32_t)0x80000000, 300, 5},
            {0, 0, -9}, {-7, -3, (int32_t)0x80000000} };
        for (i = 0; i < sizeof shapes / sizeof shapes[0]; ++i) {
            unsigned f;
            for (f = 0; f < 4u; ++f) {
                init_case(&k, NAME("shape-%d-%d-%d-f%u", shapes[i][0], shapes[i][1],
                                   shapes[i][2], f));
                k.count = shapes[i][0];
                k.cap = shapes[i][1];
                k.start = shapes[i][2];
                k.frac = fracs[f];
                run_case(&k);
            }
        }
    }
    /* fraction classes: k >= n - 1, k < 0, k far out, truncation edges */
    {
        static const float specials[] = { 0.0f, -0.0f, 1.0f, 1.0f / 300.0f,
            299.0f / 300.0f, 1.5f, 2.0f, -0.5f, -1.0f, -3.0f, 1e-30f, -1e-30f,
            7158278.0f, -7158278.0f, 1e-45f };
        for (i = 0; i < sizeof specials / sizeof specials[0]; ++i) {
            init_case(&k, NAME("frac-%u", i));
            k.frac = specials[i];
            run_case(&k);
        }
        /* n * frac outside int32 or NaN: declined (cvttss2si) */
        {
            static const uint32_t bad[] = { 0x7fc00000u, 0xffc00001u, 0x7f800000u,
                0xff800000u, 0x7f7fffffu, 0x4f000000u /* 2^31 */ };
            for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
                init_case(&k, NAME("frac-bad-%08x", bad[i]));
                k.use_frac_bits = 1;
                k.frac_bits = bad[i];
                k.expect = DECLINE;
                run_case(&k);
            }
            /* 2^31 / 300 rounds to 7158279 exactly: n*frac = 2147483700 */
            init_case(&k, NAME("frac-just-out"));
            k.frac = 7158279.0f;
            k.expect = DECLINE;
            run_case(&k);
            /* a NaN fraction with n <= 0 is still declined (checked first) */
            init_case(&k, NAME("frac-nan-count-0"));
            k.count = 0;
            k.use_frac_bits = 1;
            k.frac_bits = 0x7fc00000u;
            k.expect = DECLINE;
            run_case(&k);
        }
    }
    /* element classes */
    for (round = 0; round < 3u; ++round) {
        static const int kinds[] = { ELEM_WIDE, ELEM_SPECIAL, ELEM_ZERO };
        for (i = 0; i < 4u; ++i) {
            init_case(&k, NAME("elem-%d-f%u", kinds[round], i));
            k.elem = kinds[round];
            k.frac = fracs[i];
            k.count = 40;
            k.cap = 64;
            k.start = (int32_t)(lcg() % 64u);
            run_case(&k);
        }
    }
    /* NaN elements: a visited slot declines, an unvisited one does not */
    for (i = 0; i < 6u; ++i) {
        init_case(&k, NAME("nan-visited-%u", i));
        k.count = 40;
        k.cap = 64;
        k.start = 50;
        k.nan_slot = (int)((50u + i * 7u) % 64u);
        k.nan_y = (int)(i & 1u);
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("nan-unvisited-%u", i));
        k.count = 40;
        k.cap = 64;
        k.start = 50;
        k.nan_slot = (int)((50u + 40u + i) % 64u);
        k.nan_y = (int)(i & 1u);
        run_case(&k);
    }
    /* NaN constants decline; other constant values replay */
    for (i = 0; i < 4u; ++i) {
        init_case(&k, NAME("nan-const-%u", i));
        k.konst[i] = 0x7fa00000u;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("nan-const-%u-count-0", i));
        k.konst[i] = 0xffc00000u;
        k.count = 0;
        k.expect = DECLINE;
        run_case(&k);
    }
    {
        static const uint32_t alt[][4] = {
            {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u},
            {0x3f800000u, 0xc0000000u, 0x40000000u, 0xc0400000u},
            {0x7f800000u, 0x00000000u, 0x3f800000u, 0xbf800000u},
            {0x00000001u, 0x80000001u, 0x00000000u, 0xbf800000u},
            {0x42c80000u, 0x43480000u, 0xbf800000u, 0x3f000000u} };
        for (i = 0; i < sizeof alt / sizeof alt[0]; ++i) {
            unsigned f;
            for (f = 0; f < 2u; ++f) {
                init_case(&k, NAME("const-alt-%u-n%u", i, f));
                memcpy(k.konst, alt[i], sizeof k.konst);
                k.count = f ? 0 : 30;
                k.cap = 30;
                run_case(&k);
            }
        }
    }
    /* the InverseLerp logger: LOW == k (some i <= k), n + ONE == k (some i > k) */
    {
        init_case(&k, NAME("logger-low"));
        k.konst[3] = 0x00000000u;      /* LOW = 0, k = 0 */
        k.count = 10; k.cap = 10; k.start = 3;
        k.frac = 0.05f;
        k.expect = LOGGER;
        run_case(&k);
        init_case(&k, NAME("logger-low-k5"));
        k.konst[3] = 0x40a00000u;      /* LOW = 5, k = 5 */
        k.count = 10; k.cap = 10;
        k.frac = 0.5f;
        k.expect = LOGGER;
        run_case(&k);
        init_case(&k, NAME("logger-hi"));
        k.konst[2] = 0xc0a00000u;      /* ONE = -5: 10 - 5 == k = 5 */
        k.count = 10; k.cap = 10;
        k.frac = 0.5f;
        k.expect = LOGGER;
        run_case(&k);
        init_case(&k, NAME("logger-hi-k-negative"));
        k.konst[2] = 0xc1a00000u;      /* ONE = -20: 10 - 20 == k = -10 */
        k.count = 10; k.cap = 10;
        k.frac = -1.0f;
        k.expect = LOGGER;
        run_case(&k);
        /* one step either side: no logger */
        init_case(&k, NAME("logger-low-miss"));
        k.konst[3] = 0x3f800000u;      /* LOW = 1, k = 0 */
        k.count = 10; k.cap = 10;
        k.frac = 0.05f;
        run_case(&k);
        init_case(&k, NAME("logger-hi-only-at-last"));
        k.count = 10; k.cap = 10;
        k.konst[2] = 0xbf800000u;      /* ONE = -1: 10 - 1 == 9 == k = n - 1, no i > k */
        k.frac = 0.95f;
        run_case(&k);
        init_case(&k, NAME("logger-hi-k-n-2"));
        k.konst[2] = 0xc0000000u;      /* ONE = -2: 10 - 2 == 8 == k, i = 9 > k */
        k.count = 10; k.cap = 10;
        k.frac = 0.85f;
        k.expect = LOGGER;
        run_case(&k);
    }
    /* ring arithmetic the replay declines */
    {
        init_case(&k, NAME("cap-zero-faults"));
        k.cap = 0;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("cap-negative"));
        k.cap = -16; k.count = 40; k.start = 3;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("start-negative-small"));
        k.cap = 16; k.count = 40; k.start = -5;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("start-negative-beyond-cap"));
        k.cap = 16; k.count = 40; k.start = -19;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("start-overflows"));
        k.cap = 16; k.count = 40; k.start = 0x7ffffff0;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("start-at-int-max-n1"));
        k.cap = 16; k.count = 1; k.start = 0x7fffffff;
        run_case(&k);
        init_case(&k, NAME("start-int-max-n2"));
        k.cap = 16; k.count = 2; k.start = 0x7fffffff;
        k.expect = DECLINE;
        run_case(&k);
    }
    /* stack: exact fit, short, ceiling, above the ceiling, unbound */
    {
        init_case(&k, NAME("stack-exact"));
        k.esp_offset = 0x1cu;
        run_case(&k);
        init_case(&k, NAME("stack-short"));
        k.esp_offset = 0x18u;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("stack-ceiling"));
        k.esp_offset = 4u * STACK_WORDS - 12u;
        k.out_mode = 1;
        run_case(&k);
        init_case(&k, NAME("stack-above-ceiling"));
        k.esp_offset = 4u * STACK_WORDS - 8u;
        k.out_mode = 1;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("stack-unbound"));
        k.unbind = 1;
        k.expect = DECLINE;
        run_case(&k);
        for (i = 0; i < 4u; ++i) {
            init_case(&k, NAME("esp-mod4-%u", i));
            k.esp_offset = 4u * (STACK_WORDS / 2u) + i;
            k.out_mode = 1;
            run_case(&k);
        }
    }
    /* out overlaps: the frame (every word), the ring header, the data, the
     * constants; and the neighbours that do not overlap */
    {
        int32_t rel;
        for (rel = -0x24; rel <= 0x10; rel += 4) {
            init_case(&k, NAME("out-at-e%+d", rel));
            k.out_mode = 3;
            k.out_rel = rel;
            k.expect = rel <= -0x24 || rel >= 0x0c ? HANDLED : DECLINE;
            run_case(&k);
        }
        init_case(&k, NAME("out-on-ring-count"));
        k.konst[0] = 0x00000004u;      /* the body then loops to count 4 */
        k.konst[1] = 0x00000001u;
        k.out_mode = 2;
        k.out_addr = A(s_heap) + RING_AT + 8u;
        k.count = 6; k.cap = 9; k.start = 2;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("out-on-ring-cap"));
        k.konst[0] = 0x00000004u;
        k.konst[1] = 0x00000001u;
        k.out_mode = 2;
        k.out_addr = A(s_heap) + RING_AT + 4u;
        k.count = 6; k.cap = 9; k.start = 2;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("out-below-ring"));
        k.out_mode = 2;
        k.out_addr = A(s_heap) + RING_AT - 8u;
        run_case(&k);
        init_case(&k, NAME("out-above-ring"));
        k.out_mode = 2;
        k.out_addr = A(s_heap) + RING_AT + 16u;
        run_case(&k);
        for (i = 0; i < 3u; ++i) {
            static const int32_t slot[3] = { 0, 21, 63 };
            init_case(&k, NAME("out-in-data-%d", slot[i]));
            k.count = 40; k.cap = 64; k.start = 10;
            k.out_mode = 2;
            k.out_addr = A(s_heap) + DATA_AT + 8u * (uint32_t)slot[i] + (i == 1 ? 4u : 0u);
            k.expect = DECLINE;
            run_case(&k);
        }
        init_case(&k, NAME("out-just-past-data"));
        k.count = 40; k.cap = 64; k.start = 10;
        k.out_mode = 2;
        k.out_addr = A(s_heap) + DATA_AT + 8u * 64u;
        run_case(&k);
        init_case(&k, NAME("out-just-below-data"));
        k.count = 40; k.cap = 64; k.start = 10;
        k.out_mode = 2;
        k.out_addr = A(s_heap) + DATA_AT - 8u;
        run_case(&k);
        {
            static const uint32_t at[] = { RVA_RESET_X - 4u, RVA_RESET_X, RVA_RESET_Y,
                RVA_RESET_Y + 4u, RVA_ONE - 4u, RVA_ONE, RVA_LOW - 7u, RVA_LOW + 3u,
                RVA_RESET_X - 8u, RVA_RESET_Y + 8u, RVA_ONE + 4u, RVA_LOW - 8u };
            /* RESET_Y + 4 is RESET_X + 8: adjacent, not overlapping */
            static const int expect[] = { DECLINE, DECLINE, DECLINE, HANDLED,
                DECLINE, DECLINE, DECLINE, DECLINE, HANDLED, HANDLED, HANDLED,
                HANDLED };
            for (i = 0; i < sizeof at / sizeof at[0]; ++i) {
                init_case(&k, NAME("out-on-image-%08x", at[i]));
                k.out_mode = 2;
                k.out_addr = VA(at[i]);
                k.expect = expect[i];
                run_case(&k);
            }
        }
    }
    /* the ring header inside the frame: count/cap/start are the pushed
     * esi/edi/ebx (count is read after push esi, cap after push edi) */
    {
        init_case(&k, NAME("ring-in-frame"));
        k.ring_mode = 1;
        k.fix_regs = 1;
        k.fixed_regs[0] = 5u;     /* esi -> count */
        k.fixed_regs[1] = 7u;     /* edi -> cap */
        k.fixed_regs[2] = 2u;     /* ebx -> start */
        k.cap = 7;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("ring-just-below-frame"));
        k.ring_mode = 2;
        k.ring_addr = 0;          /* filled below */
        k.esp_offset = 4u * (STACK_WORDS / 2u);
        k.ring_addr = A(&s_stack[GUARD_WORDS]) + k.esp_offset - 0x2cu;
        run_case(&k);
    }
    /* the data range inside the frame or around out */
    {
        init_case(&k, NAME("data-over-frame"));
        k.data_mode = 1;
        k.data_rel = -0x40;
        k.cap = 16; k.count = 16; k.start = 0;
        k.out_mode = 1;
        k.expect = DECLINE;
        run_case(&k);
        init_case(&k, NAME("data-below-frame"));
        k.data_mode = 1;
        k.data_rel = -0x40 - 0x20 * 8;
        k.cap = 32; k.count = 32; k.start = 0;
        k.out_mode = 1;
        run_case(&k);
    }
    /* probes: the translated body would fault on the host */
    {
        init_case(&k, NAME("probe-null-out"));
        k.out_mode = 2; k.out_addr = 0u;
        k.expect = PROBE;
        run_case(&k);
        init_case(&k, NAME("probe-null-ring"));
        k.ring_mode = 2; k.ring_addr = 0u;
        k.expect = PROBE;
        run_case(&k);
        init_case(&k, NAME("probe-out-wraps"));
        k.out_mode = 2; k.out_addr = 0xfffffffcu;
        k.expect = PROBE;
        run_case(&k);
        init_case(&k, NAME("probe-ring-wraps"));
        k.ring_mode = 2; k.ring_addr = 0xfffffff4u;
        k.expect = PROBE;
        run_case(&k);
        init_case(&k, NAME("probe-data-wraps"));
        k.data_mode = 2; k.data_addr = 0xfffff000u;
        k.cap = 0x400;
        k.expect = PROBE;
        run_case(&k);
        init_case(&k, NAME("probe-data-8-past-4g"));
        k.data_mode = 2; k.data_addr = 0xfffff800u;
        k.cap = 0x101;
        k.expect = PROBE;
        run_case(&k);
    }

    /* random worlds at the callers' shapes and beyond */
    for (round = 0; round < 3000u; ++round) {
        init_case(&k, "random");
        k.cap = 1 + (int32_t)(lcg() % (round & 1u ? 300u : 2000u));
        k.count = (int32_t)(lcg() % (unsigned)(2 * k.cap + 2)) - 1;
        k.start = (int32_t)(lcg() % (unsigned)k.cap);
        k.frac = (lcg() & 3u) ? fracs[lcg() & 3u]
                              : (float)(int32_t)(lcg() % 4000u) / 1000.0f - 1.0f;
        k.elem = (lcg() % 8u) == 0 ? ELEM_WIDE : (lcg() % 8u) == 0 ? ELEM_SPECIAL
                                                                   : ELEM_CAMERA;
        k.out_mode = (int)(lcg() & 1u);
        k.esp_offset = 0x40u + 4u * (lcg() % (STACK_WORDS - 0x40u));
        if (k.out_mode == 0 && k.esp_offset + 0x38u > 4u * STACK_WORDS)
            k.out_mode = 1;
        if ((lcg() % 16u) == 0 && k.count > 0) {
            k.nan_slot = (int)(((uint32_t)k.start + lcg() % (uint32_t)k.count) %
                               (uint32_t)k.cap);
            k.nan_y = (int)(lcg() & 1u);
            k.expect = DECLINE;
        }
        run_case(&k);
    }

    if (s_counter_totals.calls != s_try_calls + s_probes ||
            s_counter_totals.handled != s_handled ||
            s_counter_totals.steps != s_steps) {
        ++s_failures;
        printf("FAIL counter totals: n=%u h=%u st=%u\n", s_counter_totals.calls,
               s_counter_totals.handled, s_counter_totals.steps);
    }
    if (s_failures) {
        printf("Vita camera ring oracle: FAIL; failures=%u\n", s_failures);
        return 1;
    }
    printf("counters: n=%u h=%u st=%u d(f,o,r,k,l,x)=%u,%u,%u,%u,%u,%u\n",
           s_counter_totals.calls, s_counter_totals.handled,
           s_counter_totals.steps, s_counter_totals.declined[0],
           s_counter_totals.declined[1], s_counter_totals.declined[2],
           s_counter_totals.declined[3], s_counter_totals.declined[4],
           s_counter_totals.declined[5]);
    printf("Vita camera ring oracle: PASS; cases=%u; handled=%u; declined=%u; "
           "probes=%u; logger=%u; faults=%u; steps=%u\n", s_cases, s_handled,
           s_declined, s_probes, s_logger_cases, s_fault_cases, s_steps);
    (void)s_wrap_cases;
    return 0;
}
