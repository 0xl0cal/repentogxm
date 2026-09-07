/* Host-only opaque-callee differential for the freshly emitted whole owner.
 * Models retain observable call/return/memory effects, not original allocator
 * internals. No generated function is copied into this fixture. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#undef GUEST_GENERATED_STACK_GUARD
#define GUEST_GENERATED_STACK_GUARD 1
#include "guest.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* IMAGE_BYTES is len(Image(...).mem) for the runner's pinned PE. */
enum { ARENA_BYTES = 0x100000, IMAGE_BYTES = 0x85f000,
       CALL_LIMIT = 9000, WRITE_LIMIT = 150000 };
static uint8_t *s_arena, *s_image, *s_saved_arena, *s_saved_image;
static CPU s_cpu, s_final;
static uint32_t s_case, s_pass, s_seed, s_calls, s_writes, s_events;
static uint32_t s_expected_calls, s_expected_writes, s_expected_events;
static uint32_t s_failure, s_mutation, s_seen_mutation, s_checks, s_expected_checks;
static uint32_t s_iteration, s_pairs, s_full, s_errors, s_adversarial;
static uint32_t s_stop, s_expected_stop, s_constructor_count;
static jmp_buf s_jump;
static const char s_failure_text[] = "modeled child failure";
static unsigned char s_coverage_functions[16384], s_coverage_cases[16384];
unsigned char *g_guest_coverage_functions = s_coverage_functions;
unsigned char *g_guest_coverage_cases = s_coverage_cases;
uint32_t g_guest_fs_base_cached;

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "reset oracle line %d: %s case=%u pass=%u call=%u write=%u\n", \
            __LINE__, #x, s_case, s_pass, s_calls, s_writes); exit(1); \
} } while (0)

typedef struct call_event {
    CPU cpu;
    uint32_t rva, exiting, words[5];
} call_event;
typedef struct write_event {
    uint32_t address, bytes;
    uint8_t value[16];
} write_event;
static call_event *s_trace;
static write_event *s_write_trace;
void sub_00520160(CPU *__restrict c);
void room_reset_original(CPU *__restrict c);

static uint32_t at(uint32_t n) { return (uint32_t)(uintptr_t)s_arena + n; }
static int owns(uint32_t a, uint32_t n)
{
    return (a >= at(0) && n <= ARENA_BYTES && a - at(0) <= ARENA_BYTES - n) ||
           (a >= GUEST_IMAGE_BASE && n <= IMAGE_BYTES && a - GUEST_IMAGE_BASE <= IMAGE_BYTES - n);
}
void guest_check(uint32_t a, uint32_t n, int write)
{
    (void)write; REQUIRE(owns(a, n)); ++s_checks;
}
static uint32_t read32(uint32_t a)
{
    uint32_t v; REQUIRE(owns(a, 4)); memcpy(&v, (void *)(uintptr_t)a, 4); return v;
}
static void write_bytes(uint32_t a, const void *p, uint32_t n)
{
    const uint8_t *src = p;
    REQUIRE(owns(a, n));
    for (uint32_t i = 0; i < n; i += 16) {
        uint32_t take = n - i > 16 ? 16 : n - i;
        write_event event; memset(&event, 0, sizeof event);
        event.address = a + i; event.bytes = take; memcpy(event.value, src + i, take);
        REQUIRE(s_writes < WRITE_LIMIT);
        if (!s_pass) s_write_trace[s_writes] = event;
        else REQUIRE(s_writes < s_expected_writes &&
                     memcmp(&event, &s_write_trace[s_writes], sizeof event) == 0);
        ++s_writes;
    }
    memmove((void *)(uintptr_t)a, p, n);
}
#define WRITER(name, type) void reset_##name(uint32_t a, type v) { \
    write_bytes(a, &v, (uint32_t)sizeof v); }
WRITER(st8, uint8_t)
WRITER(st16, uint16_t)
WRITER(st32, uint32_t)
WRITER(st64, uint64_t)
WRITER(stf, float)
WRITER(std_, double)
WRITER(stx, xmm_t)
#undef WRITER
#define READER(name, type) type reset_##name(uint32_t a) { \
    type v; if (!owns(a, sizeof v)) fprintf(stderr, "invalid read %08x size=%u\n", a, (unsigned)sizeof v); \
    REQUIRE(owns(a, sizeof v)); memcpy(&v, (void *)(uintptr_t)a, sizeof v); return v; }
READER(ld8, uint8_t)
READER(ld16, uint16_t)
READER(ld32, uint32_t)
READER(ld64, uint64_t)
READER(ldf, float)
READER(ldd, double)
READER(ldx, xmm_t)
#undef READER

void gpush_generated(CPU *__restrict c, uint32_t v)
{
    REQUIRE(guest_stack_contains(c, c->esp - 4, 4));
    c->esp -= 4; reset_st32(c->esp, v); guest_stack_note_low(c, c->esp);
}
uint32_t gpop_generated(CPU *__restrict c)
{
    REQUIRE(guest_stack_contains(c, c->esp, 4));
    uint32_t v = read32(c->esp); c->esp += 4; return v;
}
int guest_stack_set_generated(CPU *__restrict c, uint32_t a)
{
    REQUIRE(a >= c->stack_floor && a <= c->stack_ceiling);
    c->esp = a; guest_stack_note_low(c, a); return 1;
}
int guest_stack_adjust_generated(CPU *__restrict c, uint32_t n)
{ return guest_stack_set_generated(c, c->esp + n); }
uint32_t guest_stack_address_generated(CPU *__restrict c, uint32_t a, uint32_t n)
{
    REQUIRE(guest_stack_contains(c, a, n)); guest_stack_note_low(c, a); return a;
}
uint32_t guest_fs_base(CPU *__restrict c) { (void)c; return at(0x100); }
void guest_fault(CPU *__restrict c, uint32_t rva, const char *what)
{
    (void)what; c->fault_addr = rva; c->fault = s_failure_text;
}
void guest_int3(CPU *__restrict c, uint32_t rva)
{ guest_fault(c, rva, 0); s_stop = 2; longjmp(s_jump, 1); }
void guest_call(CPU *__restrict c, uint32_t rva)
{ guest_int3(c, rva); }

static void record(CPU *c, uint32_t rva, uint32_t exiting)
{
    call_event event; memset(&event, 0, sizeof event);
    event.cpu = *c; event.rva = rva; event.exiting = exiting;
    for (uint32_t i = 0; i < 5; ++i) event.words[i] = read32(c->esp + 4 * i);
    REQUIRE(s_events < CALL_LIMIT);
    if (!s_pass) s_trace[s_events] = event;
    else REQUIRE(s_events < s_expected_events &&
                 memcmp(&event, &s_trace[s_events], sizeof event) == 0);
    ++s_events;
}
static void enter(CPU *c, uint32_t rva)
{
    ++s_calls; record(c, rva, 0);
    if (s_calls == s_failure) {
        guest_fault(c, rva, s_failure_text); record(c, rva, 1);
        s_stop = 1; longjmp(s_jump, 1);
    }
}
static void perturb_all_gprs(CPU *c)
{
    CPU before = *c;
    /* Separate valid guest destinations: the next caller instructions can
     * dereference EBP/ESP and ESI before the next modeled stop. These copies
     * are opaque callee effects, replayed and compared in the write trace. */
    uint8_t frame[0x400], stack[0x100];
    REQUIRE(owns(c->ebp - 0x200, sizeof frame) && owns(c->esp, sizeof stack));
    memcpy(frame, (void *)(uintptr_t)(c->ebp - 0x200), sizeof frame);
    memcpy(stack, (void *)(uintptr_t)c->esp, sizeof stack);
    write_bytes(at(0xa0000), frame, sizeof frame);
    write_bytes(at(0xc0000), stack, sizeof stack);
    c->eax = at(0x6000); c->ecx = before.ecx ^ 0x12345678U;
    c->edx = before.edx ^ 0x87654321U; c->ebx = at(0xc0800);
    c->esp = at(0xc0000); c->ebp = at(0xa0200);
    c->esi = at(0x12000); c->edi = before.edi == 23 ? 17 : 23;
    REQUIRE(c->eax != before.eax && c->ecx != before.ecx && c->edx != before.edx &&
            c->ebx != before.ebx && c->esp != before.esp && c->ebp != before.ebp &&
            c->esi != before.esi && c->edi != before.edi);
    ++s_seen_mutation;
}
static void leave(CPU *c, uint32_t rva, uint32_t pop, uint32_t eax)
{
    c->eax = eax; c->ecx = 0xecc00000U ^ s_calls ^ s_seed;
    c->edx = 0xedd00000U ^ (s_calls * 71U) ^ s_seed;
    c->esp += pop;
    SET_FLAGS(&c->fl, FLAG_ADD, s_calls, s_seed, s_calls + s_seed, 4);
    if (s_calls == s_mutation) perturb_all_gprs(c);
    record(c, rva, 1);
}
void sub_005ec152(CPU *__restrict c)
{
    enter(c, 0x5ec152); uint32_t p = read32(c->esp + 4), v = read32(c->esp + 8), n = read32(c->esp + 12);
    REQUIRE(n <= 0x10000); uint8_t block[16]; memset(block, (int)v, sizeof block);
    for (uint32_t i = 0; i < n; i += 16) write_bytes(p + i, block, n - i < 16 ? n - i : 16);
    leave(c, 0x5ec152, 4, p);
}
void sub_002c4260(CPU *__restrict c)
{
    enter(c, 0x2c4260); uint32_t p = c->ecx; uint8_t block[184] = {0};
    ++s_constructor_count; s_iteration = s_constructor_count - 1;
    write_bytes(p, block, sizeof block);
    uint32_t head = at(0x50000 + s_iteration * 32);
    reset_st32(p + 0x80, head); reset_st32(head + 4, head);
    leave(c, 0x2c4260, 4, p);
}
void sub_003063a0(CPU *__restrict c)
{
    enter(c, 0x3063a0); uint8_t block[184]; uint32_t p = read32(c->esp + 4);
    REQUIRE(owns(p, sizeof block)); memcpy(block, (void *)(uintptr_t)p, sizeof block);
    write_bytes(c->ecx, block, sizeof block); leave(c, 0x3063a0, 8, c->ecx);
}
void sub_002da0d0(CPU *__restrict c)
{ enter(c, 0x2da0d0); reset_st32(c->ecx, 0); leave(c, 0x2da0d0, 4, 0); }
void sub_00020480(CPU *__restrict c)
{ enter(c, 0x20480); leave(c, 0x20480, 12, 0); }
void sub_005eb08e(CPU *__restrict c)
{ enter(c, 0x5eb08e); leave(c, 0x5eb08e, 4, 0); }
void sub_0001ad80(CPU *__restrict c)
{ enter(c, 0x1ad80); reset_st32(c->ecx, 0); leave(c, 0x1ad80, 4, 0); }
void sub_00526dd0(CPU *__restrict c)
{ enter(c, 0x526dd0); leave(c, 0x526dd0, 4, c->ecx); }
void sub_00526d80(CPU *__restrict c)
{ enter(c, 0x526d80); leave(c, 0x526d80, 4, c->ecx); }
void sub_005eacf8(CPU *__restrict c)
{ enter(c, 0x5eacf8); leave(c, 0x5eacf8, 4, at(0x60000 + (s_calls & 127) * 128)); }
void sub_0053bdf0(CPU *__restrict c)
{ enter(c, 0x53bdf0); leave(c, 0x53bdf0, 8, c->ecx); }
void sub_0031ede0(CPU *__restrict c)
{ enter(c, 0x31ede0); leave(c, 0x31ede0, 8, c->ecx); }
void sub_005eacd7(CPU *__restrict c)
{ enter(c, 0x5eacd7); leave(c, 0x5eacd7, 4, c->eax); }

static void setup(uint32_t residue)
{
    memset(s_arena, 0, ARENA_BYTES); memset(s_image, 0, IMAGE_BYTES);
    memset(&s_cpu, 0, sizeof s_cpu);
    for (uint32_t i = 0xc8000; i < 0xf0000; ++i) s_arena[i] = (uint8_t)(i * 73 + s_seed * 17);
    memset(s_coverage_functions, 0, sizeof s_coverage_functions);
    memset(s_coverage_cases, 0, sizeof s_coverage_cases);
    s_cpu.eax = 0x11111111U ^ s_seed; s_cpu.ecx = at(0x10000);
    s_cpu.edx = 0x22222222U ^ s_seed; s_cpu.ebx = 0x33333333U ^ s_seed;
    s_cpu.esp = at(0xef000 + residue); s_cpu.ebp = 0x44444444U ^ s_seed;
    s_cpu.esi = 0x55555555U ^ s_seed; s_cpu.edi = 0x66666666U ^ s_seed;
    s_cpu.stack_owner = &s_cpu; s_cpu.stack_floor = at(0x90000);
    s_cpu.stack_ceiling = at(0xf8000); s_cpu.stack_low_water = s_cpu.stack_ceiling;
    uint32_t ret = 0xfedcba98U; memcpy((void *)(uintptr_t)s_cpu.esp, &ret, 4);
    s_calls = s_writes = s_events = s_stop = s_constructor_count = s_seen_mutation = s_checks = 0;
    g_guest_fs_base_cached = 0;
}
static void pair(uint32_t seed, uint32_t residue, uint32_t fail, uint32_t mutate)
{
    ++s_case; s_seed = seed; s_failure = fail; s_mutation = mutate;
    for (s_pass = 0; s_pass < 2; ++s_pass) {
        setup(residue);
        if (!setjmp(s_jump)) {
            if (!s_pass) room_reset_original(&s_cpu); else sub_00520160(&s_cpu);
        }
        REQUIRE(s_seen_mutation == (mutate ? 1U : 0U));
        if (!s_pass) {
            s_final = s_cpu; memcpy(s_saved_arena, s_arena, ARENA_BYTES);
            memcpy(s_saved_image, s_image, IMAGE_BYTES);
            s_expected_calls = s_calls; s_expected_writes = s_writes; s_expected_events = s_events;
            s_expected_stop = s_stop; s_expected_checks = s_checks;
        } else {
            REQUIRE(memcmp(&s_final, &s_cpu, sizeof s_cpu) == 0);
            REQUIRE(memcmp(s_saved_arena, s_arena, ARENA_BYTES) == 0);
            REQUIRE(memcmp(s_saved_image, s_image, IMAGE_BYTES) == 0);
            REQUIRE(s_calls == s_expected_calls && s_writes == s_expected_writes &&
                    s_events == s_expected_events && s_stop == s_expected_stop && s_checks == s_expected_checks);
        }
    }
    ++s_pairs;
    if (!fail) { REQUIRE(!s_stop && s_constructor_count == 525); ++s_full; }
    else { REQUIRE(s_stop == 1 && s_calls == fail); ++s_errors; }
    if (mutate) ++s_adversarial;
}
static void *mapping(uintptr_t address, size_t bytes)
{
#if defined(_WIN32)
    return VirtualAlloc((void *)address, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)address, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
int main(void)
{
    s_arena = mapping(0x10000000U, ARENA_BYTES); s_image = mapping(GUEST_IMAGE_BASE, IMAGE_BYTES);
    REQUIRE((uintptr_t)s_arena == 0x10000000U && (uintptr_t)s_image == GUEST_IMAGE_BASE);
    s_saved_arena = malloc(ARENA_BYTES); s_saved_image = malloc(IMAGE_BYTES);
    s_trace = calloc(CALL_LIMIT, sizeof *s_trace); s_write_trace = calloc(WRITE_LIMIT, sizeof *s_write_trace);
    REQUIRE(s_saved_arena && s_saved_image && s_trace && s_write_trace);
    for (uint32_t seed = 1; seed <= 4; ++seed)
        for (uint32_t residue = 0; residue <= 4; residue += 4) pair(seed, residue, 0, 0);
    /* Each of seven loop callbacks can return arbitrary changed GPRs. The
     * following original caller instructions execute, then the next actual
     * callee boundary stops after recording the complete visible snapshot. */
    for (uint32_t site = 3; site <= 9; ++site)
        for (uint32_t residue = 0; residue <= 4; residue += 4)
            pair(19, residue, site + 1, site);
    const uint32_t failures[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 179, 1839, 3671, 3677};
    for (uint32_t i = 0; i < sizeof failures / sizeof failures[0]; ++i)
        pair(41, i & 1 ? 4 : 0, failures[i], 0);
    printf("Room reset publication: PASS; pairs=%u full525=%u errors=%u all8GPR=%u; exact entry/exit/write/final-memory traces\n",
           s_pairs, s_full, s_errors, s_adversarial);
    return 0;
}
