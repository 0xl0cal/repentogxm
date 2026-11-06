/* Differential oracle of host_vita_render_chain_native.c.
 *
 * test_vita_render_chain_native.py extracts from the frozen corpus the
 * translated sub_0025af60, sub_0055f610 and sub_0055f630 and the nine size
 * getter leaves, links them here with sub_0025af60/sub_0055f610 renamed
 * real_* (the --wrap view), registers the leaves and sub_0055f630 at their
 * real VAs, and runs every case twice from one snapshot: the translated body
 * and the wrapper.  Compared: a trace of every callee entry (the full
 * register file, xmm0/xmm1 and the argument/frame words it reads), the whole
 * CPU struct after the return, and every byte of the world (guest stack, the
 * object arena, the image pages, the TIB head).  Stand-ins: sub_0055ebd0 and
 * sub_0055f990 (direct callees), DecrementReference, the release observer,
 * a foreign getter and a foreign Render target; "hostile" cases make them
 * clobber callee-saved registers.  Executed on a 32-bit identity-mapped
 * host (MSVC x86, /LARGEADDRESSAWARE). */
#include "guest.c"

#include <stdarg.h>
#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_render_chain_native_oracle requires a 32-bit host
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))

/* ---- stubs owned by other Vita runtime units (as the G FUEL oracle) ---- */
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

/* ---- translated bodies and the wrapper ----------------------------------- */
void real_sub_0025af60(CPU *__restrict c);
void real_sub_0055f610(CPU *__restrict c);
void sub_0055f630(CPU *__restrict c);
void sub_005a0bb0(CPU *__restrict c);
void sub_005a0bc0(CPU *__restrict c);
void sub_005a0df0(CPU *__restrict c);
void sub_005a0e00(CPU *__restrict c);
void sub_005a1a70(CPU *__restrict c);
void sub_005a1a80(CPU *__restrict c);
void sub_005a1f10(CPU *__restrict c);
void sub_005a1f20(CPU *__restrict c);
void sub_005b6270(CPU *__restrict c);

void oracle_real_fill(CPU *__restrict c);
void oracle_real_render(CPU *__restrict c);
#define __real_sub_0025af60 oracle_real_fill
#define __real_sub_0055f610 oracle_real_render
#include "host_vita_render_chain_native.c"
#undef __real_sub_0025af60
#undef __real_sub_0055f610

static unsigned s_real_calls;
static int s_suppress_real;      /* decline probes: the body would fault */

void oracle_real_fill(CPU *__restrict c)
{
    ++s_real_calls;
    if (!s_suppress_real)
        real_sub_0025af60(c);
}

void oracle_real_render(CPU *__restrict c)
{
    ++s_real_calls;
    if (!s_suppress_real)
        real_sub_0055f610(c);
}

/* ---- trace ---------------------------------------------------------------- */
static char s_trace[1 << 16];
static size_t s_trace_len;
static int s_hostile;
static uint32_t s_seed_word;     /* per-case value the stand-ins fold in */

static void trace(const char *format, ...)
{
    va_list a;
    int n;
    va_start(a, format);
    n = vsnprintf(s_trace + s_trace_len, sizeof s_trace - s_trace_len, format, a);
    va_end(a);
    if (n > 0 && (size_t)n < sizeof s_trace - s_trace_len)
        s_trace_len += (size_t)n;
}

static void trace_cpu(const char *tag, const CPU *c)
{
    trace("%s eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x "
          "edi=%08x x0=%08x%08x%08x%08x x1=%08x%08x%08x%08x s=%08x,%08x,%08x,"
          "%08x,%08x,%08x,%08x,%08x\n", tag, c->eax, c->ecx, c->edx, c->ebx,
          c->esp, c->ebp, c->esi, c->edi, c->x[0].u32[3], c->x[0].u32[2],
          c->x[0].u32[1], c->x[0].u32[0], c->x[1].u32[3], c->x[1].u32[2],
          c->x[1].u32[1], c->x[1].u32[0], ld32(c->esp), ld32(c->esp + 4u),
          ld32(c->esp + 8u), ld32(c->esp + 12u), ld32(c->esp + 16u),
          ld32(c->esp + 20u), ld32(c->esp + 24u), ld32(c->esp + 28u));
}

static void clobber(CPU *c, uint32_t salt)
{
    c->edx = s_seed_word ^ salt;
    if (s_hostile) {
        c->ebx ^= 0x01010101u * (salt & 0xffu);
        c->edi ^= 0x00ff00ffu ^ salt;
    }
}

/* sub_0055ebd0 (direct, thiscall, ret 8): reads and rewrites the UV copy. */
void sub_0055ebd0(CPU *__restrict c)
{
    uint32_t i;
    trace_cpu("ebd0", c);
    for (i = 0; i < 9u; ++i)
        trace(" uv%u=%08x", i, ld32(c->ecx + 4u * i));
    trace("\n");
    st32(c->ecx + 4u, ld32(c->ecx + 4u) ^ 0x80000000u);
    st32(c->ecx + 0x18u, s_seed_word);
    c->eax = s_seed_word * 3u;
    c->ecx ^= 0x5a5a0000u;
    clobber(c, 0xebd0u);
    (void)gpop(c);
    c->esp += 8u;
}

/* sub_0055f990 (direct, Image::PushQuad, ret 0x18). */
void sub_0055f990(CPU *__restrict c)
{
    uint32_t i, uv = ld32(c->esp + 4u);
    trace_cpu("f990", c);
    for (i = 0; i < 9u; ++i)
        trace(" q%u=%08x", i, ld32(uv + 4u * i));
    trace("\n");
    c->eax = s_seed_word + 0x990u;
    c->ecx = 0x0c0c0c0cu;
    if (s_hostile)
        c->esi ^= 0x77u;   /* pop esi in 0x55f630 must restore it */
    clobber(c, 0xf990u);
    (void)gpop(c);
    c->esp += 0x18u;
}

/* ---- registered stand-ins (reached through guest_call) --------------------- */
#define ORACLE_DECREMENT_VA  0x98500000u
#define ORACLE_OBSERVER_VA   0x98500010u
#define ORACLE_FOREIGN_GET   0x98500020u
#define ORACLE_FOREIGN_RENDER 0x98500030u
static uint32_t s_decrement_result;
static uint8_t s_heap[0x2000];
#define H(off) ((uint32_t)(uintptr_t)(s_heap + (off)))
#define OFF_THIS 0x000u
#define OFF_DEST 0x100u
#define OFF_IMG  0x300u
#define OFF_VT   0x500u
#define OFF_CNT  0x600u
#define OFF_CVT  0x640u
#define OFF_UV   0x700u

static void oracle_decrement(CPU *__restrict c)
{
    trace_cpu("dec", c);
    c->eax = s_decrement_result;
    clobber(c, 0xdecu);
    (void)gpop(c);
}

static void oracle_observer(CPU *__restrict c)
{
    uint32_t p = ld32(c->esp + 4u);
    trace_cpu("obs", c);
    trace(" sp=%08x,%08x\n", ld32(p), ld32(p + 4u));
    c->eax = s_seed_word ^ 0x0b5u;
    c->ecx = 0x13131313u;
    clobber(c, 0x0b5u);
    (void)gpop(c);
}

static void oracle_foreign_get(CPU *__restrict c)
{
    trace_cpu("fget", c);
    c->eax = s_seed_word * 7u;        /* any sign, any size */
    st32(H(OFF_DEST) + 0x40u, c->eax); /* a visible side effect */
    c->ecx = 0u;                       /* caller-saved: the fill re-reads */
    c->x[1].u32[0] ^= 0x00400000u;     /* xmm1 is volatile too */
    c->x[1].u32[2] = s_seed_word;
    clobber(c, 0xf6e7u);
    (void)gpop(c);
}

static void oracle_foreign_render(CPU *__restrict c)
{
    trace_cpu("frnd", c);
    c->eax = s_seed_word ^ 0x4e4du;
    clobber(c, 0x4e4du);
    (void)gpop(c);
    c->esp += 0x18u;
}

static const uint32_t s_reg_addrs[] = {
    ORACLE_DECREMENT_VA, ORACLE_OBSERVER_VA, ORACLE_FOREIGN_GET,
    ORACLE_FOREIGN_RENDER, 0x9855f630u, 0x985a0bb0u, 0x985a0bc0u,
    0x985a0df0u, 0x985a0e00u, 0x985a1a70u, 0x985a1a80u, 0x985a1f10u,
    0x985a1f20u, 0x985b6270u,
};
static const guest_fn s_reg_fns[] = {
    oracle_decrement, oracle_observer, oracle_foreign_get,
    oracle_foreign_render, sub_0055f630, sub_005a0bb0, sub_005a0bc0,
    sub_005a0df0, sub_005a0e00, sub_005a1a70, sub_005a1a80, sub_005a1f10,
    sub_005a1f20, sub_005b6270,
};
/* index 0..8 = the nine leaves, 9 = foreign */
static const uint32_t s_getters[10] = {
    0x985a0bb0u, 0x985a0bc0u, 0x985a0df0u, 0x985a0e00u, 0x985a1a70u,
    0x985a1a80u, 0x985a1f10u, 0x985a1f20u, 0x985b6270u, ORACLE_FOREIGN_GET,
};

/* ---- world ------------------------------------------------------------------ */
#define ORACLE_PAGE 0x10000u
static const uint32_t s_pages[] = { 0x00760000u, 0x007a0000u, 0x007f0000u };
#define STACK_WORDS 2048u
static uint32_t s_stack[STACK_WORDS];
static CPU s_cpu;

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

static uint32_t s_lcg = 0x20261016u;
static uint32_t lcg(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return (s_lcg >> 16) | (s_lcg << 16);
}

/* Random float bits with the specials over-represented. */
static uint32_t fbits(void)
{
    static const uint32_t special[] = { 0x7fc00000u, 0xffc00001u, 0x7f800000u,
                                        0x00000000u, 0x80000000u, 0x3f800000u,
                                        0x00000001u, 0x7f7fffffu };
    uint32_t r = lcg();
    return (r & 7u) == 0u ? special[(r >> 3) & 7u] : lcg();
}

static void fill_random(uint32_t at, uint32_t bytes)
{
    uint32_t i;
    for (i = 0; i < bytes; i += 4u)
        st32(at + i, fbits());
}

static int commit_page(uint32_t base)
{
    void *r = VirtualAlloc((LPVOID)(uintptr_t)base, ORACLE_PAGE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return r != NULL && (uintptr_t)r == base;
}

static void set_globals(void)
{
    static const uint32_t table[4] = { 0u, 0u, 0u, 0x41f00000u }; /* 0, 2^32 */
    uint32_t i;
    for (i = 0; i < 4u; ++i)
        st32(VA(0x0076c940u) + 4u * i, table[i]);
    if ((lcg() & 7u) == 0u) {               /* hostile table */
        st32(VA(0x0076c940u) + 4u, fbits());
        st32(VA(0x0076c940u) + 12u, fbits());
    }
    st32(VA(0x0076a614u), fbits());
    st32(VA(0x007aa3b4u), lcg());
    st32(VA(0x007fedf0u), fbits());
    st32(VA(0x007fedf4u), fbits());
    st32(VA(0x007fedf8u), fbits());
    st32(VA(0x007fd648u), fbits());
}

static void set_cpu(uint32_t entry_words_above_floor)
{
    uint32_t floor = (uint32_t)(uintptr_t)&s_stack[0];
    uint32_t i;
    guest_cpu_init(&s_cpu);
    if (guest_stack_bind(&s_cpu, floor, floor + 4u * STACK_WORDS)) {
        printf("stack bind failed\n");
        exit(2);
    }
    for (i = 0; i < STACK_WORDS; ++i)
        s_stack[i] = lcg();
    s_cpu.eax = lcg();
    s_cpu.ebx = lcg();
    s_cpu.edx = lcg();
    s_cpu.ebp = lcg();
    s_cpu.esi = lcg();
    s_cpu.edi = lcg();
    for (i = 0; i < 4u; ++i) {
        s_cpu.x[0].u32[i] = lcg();
        s_cpu.x[1].u32[i] = lcg();
    }
    s_cpu.esp = floor + 4u * entry_words_above_floor;
    st32(s_cpu.esp, 0x0000470bu);   /* caller's return word */
}

/* ---- run ----------------------------------------------------------------- */
static unsigned s_cases, s_failures, s_declines, s_inlined_cases;

static void compare(const char *name, int decline)
{
    unsigned i;
    if (decline) {
        if (memcmp(&s_cpu, &s_cpu_snap, sizeof s_cpu) != 0)
            printf("FAIL %s: decline touched the CPU\n", name), ++s_failures;
        for (i = 0; i < s_region_count; ++i)
            if (memcmp(s_regions[i].at, s_snap[i], s_regions[i].bytes) != 0)
                printf("FAIL %s: decline touched memory\n", name), ++s_failures;
        return;
    }
    if (memcmp(&s_cpu, &s_cpu_ref, sizeof s_cpu) != 0) {
        printf("FAIL %s: CPU differs\n  ref eax=%08x ecx=%08x edx=%08x ebx=%08x "
               "esp=%08x ebp=%08x esi=%08x edi=%08x x0=%08x x1=%08x\n  nat "
               "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x "
               "edi=%08x x0=%08x x1=%08x\n", name, s_cpu_ref.eax, s_cpu_ref.ecx,
               s_cpu_ref.edx, s_cpu_ref.ebx, s_cpu_ref.esp, s_cpu_ref.ebp,
               s_cpu_ref.esi, s_cpu_ref.edi, s_cpu_ref.x[0].u32[0],
               s_cpu_ref.x[1].u32[0], s_cpu.eax, s_cpu.ecx, s_cpu.edx,
               s_cpu.ebx, s_cpu.esp, s_cpu.ebp, s_cpu.esi, s_cpu.edi,
               s_cpu.x[0].u32[0], s_cpu.x[1].u32[0]);
        ++s_failures;
    }
    for (i = 0; i < s_region_count; ++i)
        if (memcmp(s_regions[i].at, s_ref[i], s_regions[i].bytes) != 0) {
            size_t b = 0;
            while (((uint8_t *)s_regions[i].at)[b] == s_ref[i][b])
                ++b;
            printf("FAIL %s: region %u differs at +0x%x\n", name, i, (unsigned)b);
            ++s_failures;
        }
}

static char s_ref_trace[1 << 16];

static void run(const char *name, int which, int decline)
{
    IsaacVitaRenderChainCounters before = s_counters;
    unsigned real_before;
    size_t ref_len = 0;

    ++s_cases;
    if (getenv("RC_ORACLE_VERBOSE"))
        printf("case %s\n", name);
    snapshot(s_snap, &s_cpu_snap);
    s_trace_len = 0;
    if (!decline) {
        if (which == 0)
            real_sub_0025af60(&s_cpu);
        else
            real_sub_0055f610(&s_cpu);
        ref_len = s_trace_len;
        memcpy(s_ref_trace, s_trace, ref_len);
    }
    snapshot(s_ref, &s_cpu_ref);
    if (getenv("RC_ORACLE_VERBOSE"))
        printf("  reference done\n");
    restore(s_snap, &s_cpu_snap);
    s_trace_len = 0;
    real_before = s_real_calls;
    s_suppress_real = decline;
    if (which == 0)
        __wrap_sub_0025af60(&s_cpu);
    else
        __wrap_sub_0055f610(&s_cpu);
    s_suppress_real = 0;
    if ((s_real_calls - real_before) != (unsigned)(decline != 0))
        printf("FAIL %s: __real_ entered %u times\n", name,
               s_real_calls - real_before), ++s_failures;
    if (which == 0 && (s_counters.fill_calls != before.fill_calls + 1u ||
                       s_counters.fill_declined != before.fill_declined + (decline != 0)))
        printf("FAIL %s: fill counters\n", name), ++s_failures;
    if (which == 1 && (s_counters.render_calls != before.render_calls + 1u ||
                       s_counters.render_declined != before.render_declined + (decline != 0)))
        printf("FAIL %s: render counters\n", name), ++s_failures;
    if (decline)
        ++s_declines;
    if (!decline && (s_trace_len != ref_len || memcmp(s_trace, s_ref_trace, ref_len))) {
        size_t b = 0;
        while (b < ref_len && b < s_trace_len && s_trace[b] == s_ref_trace[b])
            ++b;
        printf("FAIL %s: callee trace differs at %u\n  ref: %.200s\n  nat: %.200s\n",
               name, (unsigned)b, s_ref_trace + (b > 60 ? b - 60 : 0),
               s_trace + (b > 60 ? b - 60 : 0));
        ++s_failures;
    }
    compare(name, decline);
}

/* sub_0025af60 case. */
static void fill_case(unsigned n)
{
    char name[64];
    uint32_t g30 = lcg() % 10u, g34 = lcg() % 10u;
    int img_null = (lcg() % 6u) == 0u, cnt_null = (lcg() % 3u) == 0u;
    int obs_null = (lcg() % 3u) == 0u, alias = (lcg() % 8u) == 0u;
    int decline = (lcg() % 16u) == 0u;
    uint32_t this_at, before_native;

    /* The fill keeps its loop state in ebx/esi/edi across the getter calls:
     * a callee breaking that ABI crashes the x86 too.  Caller-saved only. */
    s_hostile = 0;
    s_seed_word = lcg();
    s_decrement_result = (lcg() & 1u) ? (lcg() & 0xffffff00u) : lcg() | 1u;
    memset(s_heap, 0, sizeof s_heap);
    fill_random(H(0), sizeof s_heap);
    set_globals();
    st32(VA(0x007fd62cu), obs_null ? 0u : ORACLE_OBSERVER_VA);
    /* image: vtable, then fields at +0x84..+0x10c (random, any sign) */
    st32(H(OFF_IMG), H(OFF_VT));
    st32(H(OFF_VT) + 0x30u, s_getters[g30]);
    st32(H(OFF_VT) + 0x34u, s_getters[g34]);
    st32(H(OFF_CNT), H(OFF_CVT));
    st32(H(OFF_CVT) + 0xcu, ORACLE_DECREMENT_VA);
    set_cpu(decline ? 8u + (lcg() % 8u) : 64u + (lcg() % 1800u));
    this_at = alias ? H(OFF_DEST) + 0x24u + 0x58u * (lcg() % 3u) - 0x10u + 4u * (lcg() % 3u)
                    : H(OFF_THIS);
    s_cpu.ecx = this_at;
    st32(s_cpu.esp + 4u, H(OFF_DEST));
    st32(s_cpu.esp + 8u, img_null ? 0u : H(OFF_IMG));
    st32(s_cpu.esp + 12u, cnt_null ? 0u : H(OFF_CNT));
    before_native = s_counters.getters_native;
    snprintf(name, sizeof name, "fill-%u-g%u-%u%s%s%s%s%s", n, g30, g34,
             img_null ? "-noimg" : "", cnt_null ? "-nocnt" : "",
             obs_null ? "-noobs" : "", alias ? "-alias" : "",
             decline ? "-decline" : "");
    run(name, 0, decline);
    if (!decline && !img_null &&
            s_counters.getters_native - before_native !=
            4u * ((g30 < 9u) + (g34 < 9u)))
        printf("FAIL %s: native getter count\n", name), ++s_failures;
}

/* sub_0055f610 case. */
static void render_case(unsigned n)
{
    char name[64];
    int foreign = (lcg() % 4u) == 0u, one = (lcg() & 1u) != 0u;
    int alias = (lcg() % 8u) == 0u, decline = (lcg() % 16u) == 0u;
    uint32_t uv, before_inlined;

    s_hostile = (lcg() & 3u) == 0u;
    s_seed_word = lcg();
    memset(s_heap, 0, sizeof s_heap);
    fill_random(H(0), sizeof s_heap);
    set_globals();
    st32(H(OFF_IMG), H(OFF_VT));
    st32(H(OFF_VT) + 8u, foreign ? ORACLE_FOREIGN_RENDER : 0x9855f630u);
    set_cpu(decline ? 16u + (lcg() % 8u) : 96u + (lcg() % 1800u));
    /* alias: the UV struct sits inside the frames the replay writes */
    uv = alias ? s_cpu.esp - 0x60u + 4u * (lcg() % 16u) : H(OFF_UV);
    st32(uv + 0x20u, one ? 1u : (lcg() & 1u ? 0u : lcg()));
    s_cpu.ecx = H(OFF_IMG);
    st32(s_cpu.esp + 4u, uv);
    st32(s_cpu.esp + 8u, lcg());
    st32(s_cpu.esp + 12u, lcg());
    before_inlined = s_counters.render_inlined;
    snprintf(name, sizeof name, "render-%u%s%s%s%s%s", n,
             foreign ? "-foreign" : "", one ? "-uv1" : "", alias ? "-alias" : "",
             s_hostile ? "-hostile" : "", decline ? "-decline" : "");
    run(name, 1, decline);
    if (!decline && !foreign)
        ++s_inlined_cases;
    if (!decline && (s_counters.render_inlined - before_inlined) != (unsigned)!foreign)
        printf("FAIL %s: inline counter\n", name), ++s_failures;
}

int main(void)
{
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash keeps what was printed */
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        if (!commit_page(VA(s_pages[i]))) {
            fprintf(stderr, "could not commit %08x (/LARGEADDRESSAWARE)\n",
                    VA(s_pages[i]));
            return 2;
        }
    guest_register(s_reg_addrs, s_reg_fns,
                   sizeof s_reg_addrs / sizeof s_reg_addrs[0]);
    (void)guest_fs_base(&s_cpu);                 /* materialise the TIB */
    add_region(s_stack, sizeof s_stack);
    add_region(s_heap, sizeof s_heap);
    for (i = 0; i < sizeof s_pages / sizeof s_pages[0]; ++i)
        add_region((void *)(uintptr_t)VA(s_pages[i]), ORACLE_PAGE);
    add_region(g_tib, 0x40u);

    for (i = 0; i < 3000u; ++i)
        fill_case(i);
    for (i = 0; i < 3000u; ++i)
        render_case(i);
    printf("counters: f=%u fd=%u gn=%u gc=%u rl=%u r=%u ri=%u rd=%u\n",
           s_counters.fill_calls, s_counters.fill_declined,
           s_counters.getters_native, s_counters.getters_called,
           s_counters.releases, s_counters.render_calls,
           s_counters.render_inlined, s_counters.render_declined);
    if (s_failures) {
        printf("Vita render chain oracle: FAIL; failures=%u of %u cases\n",
               s_failures, s_cases);
        return 1;
    }
    printf("Vita render chain oracle: PASS; cases=%u; declines=%u; inlined=%u\n",
           s_cases, s_declines, s_inlined_cases);
    return 0;
}
