/* Host oracle for host_vita_uniform_set_receipt.c (ISAAC_VITA_UNIFORM_SET_RECEIPT).
 *
 * Drives the five __wrap_ roots on a synthetic Shader image laid out with
 * the frozen-PE offsets recomp/test_uniform_set_sites.py pins (records at
 * [this+0x10..0x14) stride 0x18, record type at +8, values vector at
 * +0xc..0x10, default index list at [this+0x1c]) and checks every counter
 * the receipt publishes, the "KAGE VITA UNIFORM SET" cadence (binds 1, 2,
 * 4, ...) and that every body runs with the CPU state it was entered with.
 * The image is mmap'ed at a fixed 32-bit address so the runtime's own
 * ld32() (guest address == host address) reads it unmodified; the only
 * stubs are the __real_ bodies, isaac_vita_log and guest_stack_address.
 *
 * Build and run on a 64-bit Linux host with gcc, from the repo root:
 *
 *   gcc -std=gnu11 -O1 -Wall -Wextra -I recomp/runtime \
 *       -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 \
 *       -DGUEST_FLAGS_LOCAL=1 -DGUEST_COVERAGE_HOOKS=0 \
 *       -DISAAC_VITA_UNIFORM_SET_RECEIPT=1 \
 *       recomp/runtime/host_vita_uniform_set_receipt_oracle.c \
 *       recomp/runtime/host_vita_uniform_set_receipt.c -o uniform_receipt_oracle \
 *   && ./uniform_receipt_oracle
 *
 * Expected last line: "Vita uniform set receipt oracle: PASS".  Not wired
 * into CMake: it is a host-only proof, the Vita build never sees it. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "guest.h"
#include "host_vita_uniform_set_receipt.h"

void __wrap_sub_005671d0(CPU *__restrict c);
void __wrap_sub_00567b50(CPU *__restrict c);
void __wrap_sub_00567490(CPU *__restrict c);
void __wrap_sub_00567430(CPU *__restrict c);
void __wrap_sub_005679e0(CPU *__restrict c);

/* ---- stubs ------------------------------------------------------------ */

enum { BODY_BIND, BODY_APPLY, BODY_SET, BODY_SET_INT, BODY_COLLAPSE, BODY_COUNT };
static unsigned s_body_calls[BODY_COUNT];
static uint32_t s_body_ecx[BODY_COUNT], s_body_esp[BODY_COUNT];
static const char *s_body_fault[BODY_COUNT];
static unsigned s_log_calls;
static char s_log_last[512];

static void note_body(int which, CPU *c)
{
    s_body_calls[which]++;
    s_body_ecx[which] = c->ecx;
    s_body_esp[which] = c->esp;
    s_body_fault[which] = c->fault;
}

void __real_sub_005671d0(CPU *__restrict c) { note_body(BODY_BIND, c); }
void __real_sub_00567b50(CPU *__restrict c) { note_body(BODY_APPLY, c); }
void __real_sub_00567490(CPU *__restrict c) { note_body(BODY_SET, c); }
void __real_sub_00567430(CPU *__restrict c) { note_body(BODY_SET_INT, c); }
void __real_sub_005679e0(CPU *__restrict c) { note_body(BODY_COLLAPSE, c); }

void isaac_vita_log(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsnprintf(s_log_last, sizeof s_log_last, format, ap);
    va_end(ap);
    s_log_calls++;
}

/* The receipt only reads [esp+4]; the legacy identity answer is the guest
 * contract for an in-range stack word. */
uint32_t guest_stack_address(CPU *__restrict c, uint32_t address, uint32_t size, uint32_t pc)
{
    (void)c; (void)size; (void)pc;
    return address;
}

/* ---- synthetic guest image ------------------------------------------- */

#define IMG            0x30000000u
#define IMG_SIZE       0x00100000u
#define SHADER         (IMG + 0x100u)
#define SHADER_BAD     (IMG + 0x101u)          /* misaligned this */
#define RECORDS        (IMG + 0x1000u)
#define VALUES         (IMG + 0x10000u)
#define LIST           (IMG + 0x20000u)        /* explicit std::vector<int> */
#define LIST_ITEMS     (IMG + 0x20100u)
#define STACK          (IMG + 0x30000u)

#define RECORD_STRIDE  0x18u
#define OFF_RECORDS_BEGIN 0x10u
#define OFF_RECORDS_END   0x14u
#define OFF_LIST          0x1cu                /* default list: first/last/end */
#define REC_TYPE          0x8u
#define REC_VALUES_BEGIN  0xcu
#define REC_VALUES_END    0x10u
#define REC_VALUES_CAP    0x14u

#define NAME_TRANSFORM  ((uint32_t)GUEST_IMAGE_BASE + 0x744ac0u)
#define NAME_TEXTURE0   ((uint32_t)GUEST_IMAGE_BASE + 0x75e098u)
#define NAME_TEXTURE1   ((uint32_t)GUEST_IMAGE_BASE + 0x75e03cu)
#define NAME_OTHER      ((uint32_t)GUEST_IMAGE_BASE + 0x746e14u)  /* "ChampionColorIn" */

/* ld32/st32 come from guest.h: guest address == host address. */

static unsigned s_failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            s_failures++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
        } \
    } while (0)

static void image_reset(void)
{
    memset((void *)(uintptr_t)IMG, 0, IMG_SIZE);
    st32(SHADER + OFF_RECORDS_BEGIN, RECORDS);
    st32(SHADER + OFF_RECORDS_END, RECORDS);
    st32(SHADER + OFF_LIST, LIST_ITEMS);
    st32(SHADER + OFF_LIST + 4u, LIST_ITEMS);
    st32(SHADER + OFF_LIST + 8u, LIST_ITEMS + 0x80u);
}

/* Appends a record with `type` holding `values` pointers (each value word
 * is a distinct fake heap address) and grows the default index list by one
 * entry pointing at the last value, the way Collapse rebuilds it. */
static void add_record(uint32_t type, uint32_t values)
{
    uint32_t end = *(uint32_t *)(uintptr_t)(SHADER + OFF_RECORDS_END);
    uint32_t index = (end - RECORDS) / RECORD_STRIDE;
    uint32_t array = VALUES + index * 0x100u;
    uint32_t k;

    st32(end + 0u, NAME_OTHER);
    st32(end + 4u, 1u);
    st32(end + REC_TYPE, type);
    st32(end + REC_VALUES_BEGIN, array);
    st32(end + REC_VALUES_END, array + 4u * values);
    st32(end + REC_VALUES_CAP, array + 0x100u);
    for (k = 0; k < values; ++k)
        st32(array + 4u * k, 0x40000000u + index * 0x1000u + k * 0x10u);
    st32(SHADER + OFF_RECORDS_END, end + RECORD_STRIDE);
    {
        uint32_t list_last = *(uint32_t *)(uintptr_t)(SHADER + OFF_LIST + 4u);
        st32(list_last, values ? values - 1u : 0u);
        st32(SHADER + OFF_LIST + 4u, list_last + 4u);
    }
}

static void cpu_enter(CPU *c, uint32_t self, uint32_t stack_word)
{
    memset(c, 0, sizeof *c);
    c->ecx = self;
    c->esp = STACK;
    st32(STACK, 0x00567000u);   /* return address slot, never read */
    st32(STACK + 4u, stack_word);
}

static int same(const IsaacVitaUniformSetCounters *a, const IsaacVitaUniformSetCounters *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void dump(const char *label, const IsaacVitaUniformSetCounters *u)
{
    fprintf(stderr, "  %s: b(n,a,r,rn,it,im)=%u,%u,%u,%u,%u,%u s(n,i,t,x0,x1,o)=%u,%u,%u,%u,%u,%u "
            "c(n,v,f)=%u,%u,%u bad=%u\n", label,
            u->binds, u->applies, u->records_max, u->records_nonempty, u->iterations,
            u->iterations_max, u->sets, u->set_ints, u->name_transform, u->name_texture0,
            u->name_texture1, u->name_other, u->collapses, u->collapse_values, u->frees, u->bad);
}

#define EXPECT_WINDOW(label, ...) do { \
        IsaacVitaUniformSetCounters got, want = { __VA_ARGS__ }; \
        isaac_vita_uniform_set_receipt_take(&got); \
        if (!same(&got, &want)) { \
            s_failures++; \
            fprintf(stderr, "FAIL %s: window counters differ\n", label); \
            dump("got ", &got); dump("want", &want); \
        } \
    } while (0)

static void expect_body(int which, unsigned calls, uint32_t ecx, uint32_t esp, const char *fault)
{
    CHECK(s_body_calls[which] == calls, "body %d ran %u times, want %u", which, s_body_calls[which], calls);
    CHECK(s_body_ecx[which] == ecx, "body %d saw ecx %08x, want %08x", which, s_body_ecx[which], ecx);
    CHECK(s_body_esp[which] == esp, "body %d saw esp %08x, want %08x", which, s_body_esp[which], esp);
    CHECK(s_body_fault[which] == fault, "body %d saw fault %p, want %p", which,
          (const void *)s_body_fault[which], (const void *)fault);
}

int main(void)
{
    CPU c;
    void *map = mmap((void *)(uintptr_t)IMG, IMG_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (map != (void *)(uintptr_t)IMG) {
        fprintf(stderr, "cannot map the synthetic image at %08x\n", IMG);
        return 2;
    }

    /* A: the 1.7.9b steady state -- no records.  Collapse sees nothing,
     * Bind loops zero times, the two sampler sets and the Transform matrix
     * are the whole per-flush uniform traffic. */
    image_reset();
    cpu_enter(&c, SHADER, 0u);
    __wrap_sub_005679e0(&c);
    expect_body(BODY_COLLAPSE, 1, SHADER, STACK, NULL);
    cpu_enter(&c, SHADER, 0u);            /* list NULL -> default [this+0x1c] */
    __wrap_sub_005671d0(&c);
    expect_body(BODY_BIND, 1, SHADER, STACK, NULL);
    CHECK(s_log_calls == 1, "first bind must log once, logged %u", s_log_calls);
    CHECK(strstr(s_log_last, "KAGE VITA UNIFORM SET: binds=1 applies=0 records_max=0 "
                             "records_nonempty=0 it=0 it_max=0 set=0 set_int=0 "
                             "names(t,x0,x1,o)=0,0,0,0 collapses=1 values=0 frees=0 bad=0") != NULL,
          "log text: %s", s_log_last);
    cpu_enter(&c, SHADER, NAME_TEXTURE0);
    __wrap_sub_00567430(&c);
    expect_body(BODY_SET_INT, 1, SHADER, STACK, NULL);
    cpu_enter(&c, SHADER, NAME_TEXTURE1);
    __wrap_sub_00567430(&c);
    cpu_enter(&c, SHADER, NAME_TRANSFORM);
    __wrap_sub_00567490(&c);
    expect_body(BODY_SET, 1, SHADER, STACK, NULL);
    EXPECT_WINDOW("A", .binds = 1, .set_ints = 2, .name_texture0 = 1, .name_texture1 = 1,
                  .sets = 1, .name_transform = 1, .collapses = 1);

    /* B: the reference project's world -- records with stale values.  Two
     * vec records with 3 values (2 frees each), a sampler record with 3
     * values (kept: signed type >= 0x15 skips the free loop), a vec record
     * with 1 value (nothing stale), a record with type -1 (signed < 0x15)
     * and 2 values (1 free).  values = 3+3+3+1+2 = 12, frees = 2+2+0+0+1 = 5. */
    image_reset();
    add_record(4u, 3u);
    add_record(3u, 3u);
    add_record(0x15u, 3u);
    add_record(1u, 1u);
    add_record(0xffffffffu, 2u);
    cpu_enter(&c, SHADER, 0u);
    __wrap_sub_005679e0(&c);
    cpu_enter(&c, SHADER, 0u);            /* default list has 5 entries */
    __wrap_sub_005671d0(&c);
    CHECK(s_log_calls == 2, "second bind must log (2 is a power of two), logged %u", s_log_calls);
    st32(LIST, LIST_ITEMS + 0x40u);       /* explicit list of 2 entries */
    st32(LIST + 4u, LIST_ITEMS + 0x48u);
    st32(LIST + 8u, LIST_ITEMS + 0x80u);
    st32(LIST_ITEMS + 0x40u, 0u);
    st32(LIST_ITEMS + 0x44u, 2u);
    cpu_enter(&c, SHADER, LIST);
    __wrap_sub_00567b50(&c);
    expect_body(BODY_APPLY, 1, SHADER, STACK, NULL);
    cpu_enter(&c, SHADER, NAME_OTHER);
    __wrap_sub_00567490(&c);
    EXPECT_WINDOW("B", .binds = 1, .applies = 1, .records_max = 5, .records_nonempty = 2,
                  .iterations = 7, .iterations_max = 5, .sets = 1, .name_other = 1,
                  .collapses = 1, .collapse_values = 12, .frees = 5);

    /* C: the bound -- 65 empty records is read as 64 and flagged. */
    image_reset();
    {
        unsigned k;
        for (k = 0; k < 65; ++k)
            add_record(4u, 0u);
    }
    cpu_enter(&c, SHADER, 0u);
    __wrap_sub_005679e0(&c);
    EXPECT_WINDOW("C", .records_max = 65, .collapses = 1, .bad = 1);

    /* D: unreadable entry state is counted and the body still runs:
     * misaligned this (Collapse, Bind), a faulted CPU (SetUniform), a NULL
     * stack answer is not reachable through the identity stub so the
     * misaligned-list case stands in for it. */
    image_reset();
    cpu_enter(&c, SHADER_BAD, 0u);
    __wrap_sub_005679e0(&c);
    expect_body(BODY_COLLAPSE, 4, SHADER_BAD, STACK, NULL);
    cpu_enter(&c, SHADER_BAD, 0u);
    __wrap_sub_005671d0(&c);
    CHECK(s_log_calls == 2, "third bind must not log (3 is not a power of two), logged %u", s_log_calls);
    cpu_enter(&c, SHADER, NAME_TRANSFORM);
    c.fault = "oracle";
    __wrap_sub_00567490(&c);
    expect_body(BODY_SET, 3, SHADER, STACK, "oracle");
    cpu_enter(&c, SHADER, LIST + 2u);     /* misaligned list pointer */
    __wrap_sub_00567b50(&c);
    cpu_enter(&c, SHADER, 0u);
    __wrap_sub_005671d0(&c);
    CHECK(s_log_calls == 3, "fourth bind must log (4 is a power of two), logged %u", s_log_calls);
    CHECK(strstr(s_log_last, "binds=4 applies=2 records_max=65 records_nonempty=2 it=7 it_max=5 "
                             "set=3 set_int=2 names(t,x0,x1,o)=1,1,1,1 collapses=4 values=12 "
                             "frees=5 bad=5") != NULL,
          "cumulative log text: %s", s_log_last);
    EXPECT_WINDOW("D", .binds = 2, .applies = 1, .sets = 1, .collapses = 1, .bad = 4);

    /* E: take(NULL) discards a window; the next window starts clean. */
    cpu_enter(&c, SHADER, 0u);
    __wrap_sub_005679e0(&c);
    isaac_vita_uniform_set_receipt_take(NULL);
    EXPECT_WINDOW("E", .binds = 0);

    if (s_failures) {
        fprintf(stderr, "Vita uniform set receipt oracle: %u failure(s)\n", s_failures);
        return 1;
    }
    puts("Vita uniform set receipt oracle: PASS");
    return 0;
}
