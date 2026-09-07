#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "host_vita_room_grid_init.h"
#include "host_vita_heap.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Actual fresh constructor code is linked twice. Normally opaque children are
 * modeled. REZERO_REAL_CHILDREN instead links the actual freshly generated
 * reserve/allocation/commit/empty-tree code; only the allocation boundary and
 * impossible nonempty-vector/error edges remain models. */
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "grid oracle line %d: %s (case %u)\n", __LINE__, #x, s_case); \
    exit(1); } } while (0)
enum { ARENA_BYTES = 0x20000U, IMAGE_BYTES = 0x800000U, TRACE_COUNT = 8U };
static uint8_t *s_arena, *s_image;
static uint32_t s_case, s_raw, s_grid, s_owned_bytes, s_residue, s_pattern;
static uint32_t s_lease, s_lease_calls, s_release_calls, s_fail_lease, s_fail_release;
static uint32_t s_calls, s_allocs, s_fail_at, s_null_alloc, s_logs, s_checks, s_check_hash;
static uint32_t s_paired, s_rejections;
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
static uint32_t s_grid_zero_stores, s_late_fault;
void room_grid_rezero_oracle_st32(uint32_t address, uint32_t value)
{
    if (!value && address >= s_grid && address - s_grid < 0x3800U &&
        ((address - s_grid) & 31U) == 0U)
        ++s_grid_zero_stores;
    st32(address, value);
}
#endif
static unsigned char s_coverage;
static CPU s_cpu;
static jmp_buf s_jump;
unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_cases;
uint32_t g_guest_fs_base_cached;
typedef struct trace {
    uint32_t rva, args[2]; CPU cpu; uint8_t arena[ARENA_BYTES];
} trace;
static trace s_trace[TRACE_COUNT], s_reference_trace[TRACE_COUNT];
static uint8_t s_reference_arena[ARENA_BYTES];
void sub_002c4260(CPU *__restrict c);
void grid_original_constructor(CPU *__restrict c);

static uint32_t at(uint32_t offset) { return (uint32_t)(uintptr_t)s_arena + offset; }
static int owns(uint32_t a, uint32_t n)
{
    return (a >= at(0) && n <= ARENA_BYTES && a - at(0) <= ARENA_BYTES - n) ||
           (a >= GUEST_IMAGE_BASE && n <= IMAGE_BYTES && a - GUEST_IMAGE_BASE <= IMAGE_BYTES - n);
}
void guest_check(uint32_t a, uint32_t n, int write)
{
    REQUIRE(owns(a, n)); ++s_checks;
    s_check_hash = (s_check_hash * 33U) ^ a ^ n ^ (uint32_t)write;
}
void gpush_generated(CPU *__restrict c, uint32_t v)
{
    REQUIRE(guest_stack_contains(c, c->esp - 4U, 4U));
    c->esp -= 4U; st32(c->esp, v); guest_stack_note_low(c, c->esp);
}
uint32_t gpop_generated(CPU *__restrict c)
{
    REQUIRE(guest_stack_contains(c, c->esp, 4U));
    uint32_t v = ld32(c->esp); c->esp += 4U; return v;
}
int guest_stack_set_generated(CPU *__restrict c, uint32_t v)
{
    REQUIRE(v >= c->stack_floor && v <= c->stack_ceiling);
    c->esp = v; guest_stack_note_low(c, v); return 1;
}
int guest_stack_adjust_generated(CPU *__restrict c, uint32_t n)
{
    REQUIRE(n <= c->stack_ceiling - c->esp); c->esp += n; return 1;
}
uint32_t guest_stack_address_generated(CPU *__restrict c, uint32_t a, uint32_t n)
{
    REQUIRE(guest_stack_contains(c, a, n)); guest_stack_note_low(c, a); return a;
}
uint32_t guest_fs_base(CPU *__restrict c) { (void)c; return at(0x100U); }
void guest_fault(CPU *__restrict c, uint32_t a, const char *what)
{
    c->fault_addr = a; c->fault = what;
}
void guest_int3(CPU *__restrict c, uint32_t a)
{
    guest_fault(c, a, "unexpected constructor int3"); longjmp(s_jump, 2);
}
void guest_call(CPU *__restrict c, uint32_t target)
{
    REQUIRE(s_null_alloc && target == 0x12345678U);
    guest_fault(c, 0x2c4542U, "modeled allocation failure"); longjmp(s_jump, 2);
}
uint32_t isaac_vita_guest_heap_lease_exact_range(const void *base, const void *range, size_t n)
{
    ++s_lease_calls; errno = ERANGE;
    if (s_fail_lease || s_lease || (uintptr_t)base != s_raw ||
        (uintptr_t)range < s_raw || n > s_owned_bytes ||
        (uintptr_t)range - s_raw > s_owned_bytes - n)
        return 0U;
    s_lease = 0x123U; return s_lease;
}
int isaac_vita_guest_heap_lease_release(uint32_t token)
{
    ++s_release_calls; errno = EIO;
    REQUIRE(token == s_lease && token);
    if (s_fail_release) return 0;
    s_lease = 0U; return 1;
}
void isaac_vita_log(const char *format, ...)
{
    char line[256]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof line, format, args); va_end(args);
    REQUIRE(strstr(line, "Room grid init engaged bid=") && strstr(line, "rows=447"));
    ++s_logs; errno = EINVAL;
}
static void call(CPU *c, uint32_t rva, uint32_t nargs)
{
    REQUIRE(s_calls < TRACE_COUNT);
    trace *t = &s_trace[s_calls++]; memset(t, 0, sizeof *t);
    t->rva = rva; t->cpu = *c;
    for (uint32_t i = 0U; i < nargs; ++i) t->args[i] = ld32(c->esp + 4U + i * 4U);
    memcpy(t->arena, s_arena, ARENA_BYTES);
    if (s_fail_at == s_calls) {
        guest_fault(c, rva, "modeled child failure"); longjmp(s_jump, 1);
    }
}
static void finish(CPU *c, uint32_t pop, uint32_t eax)
{
    c->eax = eax; c->ecx = 0xabc100U + s_calls; c->edx = 0xdef100U + s_calls;
    SET_FLAGS(&c->fl, FLAG_ADD, s_calls, 7U, s_calls + 7U, 4);
    c->esp += pop;
}
void sub_005eacf8(CPU *__restrict c)
{
    const uint32_t n = ld32(c->esp + 4U);
    call(c, 0x5eacf8U, 1U); ++s_allocs;
    if (s_allocs == 1U) {
        REQUIRE(n == 0x3823U);
        finish(c, 4U, s_null_alloc ? 0U : s_raw);
    } else {
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
        REQUIRE((s_allocs == 2U && n == 20U) ||
                (s_allocs == 3U && n == 16U * 104U) ||
                (s_allocs == 4U && n == 32U * 104U));
        finish(c, 4U, at(s_allocs == 2U ? 0x9000U :
                        s_allocs == 3U ? 0xa000U : 0xc000U));
        if (s_allocs == 4U && s_late_fault)
            c->fault = "modeled pre-existing late fault";
#else
        REQUIRE(s_allocs == 2U && n == 20U);
        finish(c, 4U, at(0x9000U));
#endif
    }
}
#ifndef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
void sub_002da150(CPU *__restrict c)
{
    const uint32_t vec = c->ecx, count = ld32(c->esp + 4U);
    call(c, 0x2da150U, 1U); ++s_allocs;
    REQUIRE((count == 16U && vec == at(0x1074U)) ||
            (count == 32U && vec == at(0x1088U)));
    uint32_t block = at(count == 16U ? 0xa000U : 0xc000U);
    st32(vec, block); st32(vec + 4U, block); st32(vec + 8U, block + count * 104U);
    /* A later opaque child may write the grid. The second original zeroing
     * loop must still execute at its old location; only first loop is native. */
    st32(s_grid, 0xfeed0000U | count);
    finish(c, 8U, block);
}
void sub_00020480(CPU *__restrict c)
{
    call(c, 0x20480U, 2U);
    REQUIRE(c->ecx == at(0x1080U) && ld32(c->esp + 4U) == c->ecx &&
            ld32(c->esp + 8U) == at(0x9000U));
    finish(c, 12U, 0U);
}
#else
void sub_000c0300(CPU *__restrict c)
{
    (void)c; REQUIRE(!"fresh reserve must not move any live element");
}
void sub_005eb08e(CPU *__restrict c)
{
    (void)c; REQUIRE(!"fresh reserves/empty tree must not free any allocation");
}
void sub_00001f50(CPU *__restrict c)
{
    (void)c; REQUIRE(!"fixed 16/32-element reserves must not overflow");
}
#endif
void sub_000c36d0(CPU *__restrict c)
{
    (void)c; REQUIRE(!"fresh constructor must not destroy nonempty vectors");
}
static void setup(uint32_t residue, uint32_t pattern)
{
    s_residue = residue; s_pattern = pattern;
    for (uint32_t i = 0U; i < ARENA_BYTES; ++i)
        s_arena[i] = (uint8_t)(i * 73U + i / 31U + pattern * 17U);
    memset(&s_cpu, 0, sizeof s_cpu); memset(s_trace, 0, sizeof s_trace);
    s_raw = at(0x4000U) + residue; s_grid = (s_raw + 0x23U) & ~31U;
    s_owned_bytes = 0x3823U; s_lease = s_lease_calls = s_release_calls = 0U;
    s_fail_lease = s_fail_release = s_calls = s_allocs = s_fail_at = s_null_alloc = 0U;
    s_checks = s_check_hash = 0U;
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
    s_grid_zero_stores = s_late_fault = 0U;
#endif
    g_guest_coverage_functions = g_guest_coverage_cases = NULL;
    s_cpu.stack_owner = &s_cpu; s_cpu.stack_floor = at(0x18000U);
    s_cpu.stack_ceiling = at(ARENA_BYTES); s_cpu.stack_low_water = s_cpu.stack_ceiling;
    s_cpu.esp = at(0x1f000U); s_cpu.ebp = 0x11223344U;
    s_cpu.eax = 0x123123U; s_cpu.ecx = at(0x1000U); s_cpu.edx = 0x321321U;
    s_cpu.ebx = 0xabcdefU; s_cpu.edi = 0xdeadbeefU; s_cpu.esi = 0x99887766U;
    SET_FLAGS(&s_cpu.fl, FLAG_SUB, 3U, 5U, 0xfffffffeU, 4);
    st32(s_cpu.esp, 0x1234U); st32(GUEST_IMAGE_BASE + 0x7aa3b4U, 0x98765432U);
    st32(GUEST_IMAGE_BASE + 0x6065acU, 0x12345678U);
    errno = EDOM;
}
static void mode(uint32_t kind)
{
    if (kind == 1U) g_guest_coverage_functions = &s_coverage;
    if (kind == 2U) g_guest_coverage_cases = &s_coverage;
    if (kind == 3U) s_fail_lease = 1U;
    if (kind == 4U) s_owned_bytes = s_grid - s_raw + 0x3800U - 1U;
    if (kind >= 5U && kind <= 9U) s_fail_at = kind - 4U;
    if (kind == 10U) s_null_alloc = 1U;
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
    if (kind == 11U) s_late_fault = 1U;
#endif
}
static void paired(uint32_t residue, uint32_t pattern, uint32_t kind)
{
    CPU expected; uint32_t calls, allocs, checks, check_hash; int err, stopped;
    ++s_case; setup(residue, pattern); mode(kind);
    stopped = setjmp(s_jump);
    if (!stopped) grid_original_constructor(&s_cpu);
    expected = s_cpu; calls = s_calls; allocs = s_allocs; err = errno;
    checks = s_checks; check_hash = s_check_hash;
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
    uint32_t reference_zero_stores = s_grid_zero_stores;
#endif
    memcpy(s_reference_arena, s_arena, ARENA_BYTES);
    memcpy(s_reference_trace, s_trace, sizeof s_trace);
    setup(residue, pattern); mode(kind);
    int fast_stopped = setjmp(s_jump);
    if (!fast_stopped) sub_002c4260(&s_cpu);
    REQUIRE(stopped == fast_stopped && calls == s_calls && allocs == s_allocs);
    REQUIRE(errno == err && !s_lease && memcmp(&expected, &s_cpu, sizeof expected) == 0);
    REQUIRE(memcmp(s_reference_arena, s_arena, ARENA_BYTES) == 0);
    REQUIRE(memcmp(s_reference_trace, s_trace, sizeof s_trace) == 0);
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
    if (!kind || kind == 1U || kind == 2U || kind == 3U || kind == 4U || kind == 11U) {
        REQUIRE(reference_zero_stores == 896U);
#ifdef GUEST_CHECKED_MEMORY
        REQUIRE(s_grid_zero_stores == 896U);
#else
        if (kind == 1U || kind == 2U) REQUIRE(s_grid_zero_stores == 896U);
        else {
#ifdef ROOM_GRID_REZERO_ORACLE_PREFIX_OFF
            uint32_t first_pass_stores = 448U;
#else
            uint32_t first_pass_stores = (kind == 3U || kind == 4U) ? 448U : 1U;
#endif
            REQUIRE(s_grid_zero_stores == first_pass_stores + (kind == 11U ? 448U : 1U));
        }
#endif
    }
#endif
#ifdef GUEST_CHECKED_MEMORY
    REQUIRE(checks == s_checks && check_hash == s_check_hash && !s_lease_calls);
#else
    (void)checks; (void)check_hash;
#ifdef ROOM_GRID_REZERO_ORACLE_PREFIX_OFF
    if (!kind) REQUIRE(!s_lease_calls && !s_release_calls);
#else
    if (!kind) REQUIRE(s_lease_calls == 1U && s_release_calls == 1U);
#endif
    if (kind == 1U || kind == 2U || kind == 5U || kind == 10U) REQUIRE(!s_lease_calls);
#endif
    if (!kind) {
        REQUIRE(s_cpu.eax == at(0x1000U) && s_cpu.esp == at(0x1f004U) && s_allocs == 4U);
        for (uint32_t row = 0U; row < 448U; ++row)
            for (uint32_t b = 25U; b <= 27U; ++b) {
                uint32_t i = s_grid - at(0) + row * 32U + b;
                REQUIRE(s_arena[i] == (uint8_t)(i * 73U + i / 31U + s_pattern * 17U));
            }
    }
    ++s_paired;
}
static void direct_cases(void)
{
    CPU before; uint32_t raw;
    for (uint32_t k = 0U; k < 10U; ++k) {
        ++s_case; setup(0U, 7U); s_cpu.eax = s_grid; s_cpu.ecx = 448U; raw = s_raw;
        if (k == 0U) s_cpu.fault = "existing fault";
        if (k == 1U) s_cpu.ecx = 0U;
        if (k == 2U) s_cpu.ecx = 447U;
        if (k == 3U) raw = 0U;
        if (k == 4U) raw = UINT32_MAX;
        if (k == 5U) s_cpu.eax += 1U;
        if (k == 6U) s_cpu.eax += 32U;
        if (k == 7U) s_fail_lease = 1U;
        if (k == 8U) --s_owned_bytes;
        if (k == 9U) s_cpu.eax = 0U;
        /* residue0 leaves3B requested tail slack; make exactextent1Bshort */
        if (k == 8U) s_owned_bytes = s_grid - s_raw + 0x3800U - 1U;
        before = s_cpu; memcpy(s_reference_arena, s_arena, ARENA_BYTES);
        REQUIRE(isaac_vita_room_grid_init_try(&s_cpu, raw) == 0 && errno == EDOM);
        REQUIRE(memcmp(&before, &s_cpu, sizeof before) == 0 && !s_lease);
        REQUIRE(memcmp(s_reference_arena, s_arena, ARENA_BYTES) == 0);
        ++s_rejections;
    }
    REQUIRE(isaac_vita_room_grid_init_try(NULL, 0U) == 0);
    ++s_rejections;
    setup(31U, 9U); s_cpu.eax = s_grid; s_cpu.ecx = 448U; s_fail_release = 1U;
    before = s_cpu; memcpy(s_reference_arena, s_arena, ARENA_BYTES);
    REQUIRE(isaac_vita_room_grid_init_try(&s_cpu, s_raw) == -1 && errno == EDOM);
    REQUIRE(s_cpu.fault && s_cpu.fault_addr == s_grid && s_lease && s_release_calls == 1U);
    before.eax += 447U * 32U; before.ecx = 1U;
    before.fault = s_cpu.fault; before.fault_addr = s_cpu.fault_addr;
    REQUIRE(memcmp(&before, &s_cpu, sizeof before) == 0);
    REQUIRE(memcmp(s_reference_arena + (s_grid - at(0)) + 447U * 32U,
                   (void *)(uintptr_t)(s_grid + 447U * 32U), 32U) == 0);

    /* Exercise the emitted terminal branch too, not only the helper result.
     * A broken <0 check would run the last row and later allocations. */
    setup(17U, 11U); s_fail_release = 1U;
    memcpy(s_reference_arena, s_arena, ARENA_BYTES);
    sub_002c4260(&s_cpu);
#if defined(GUEST_CHECKED_MEMORY) || defined(ROOM_GRID_REZERO_ORACLE_PREFIX_OFF)
    REQUIRE(!s_cpu.fault && !s_lease_calls && s_allocs == 4U);
#else
    REQUIRE(s_cpu.fault && s_cpu.fault_addr == s_grid && s_lease && s_release_calls == 1U);
    REQUIRE(s_calls == 1U && s_allocs == 1U && errno == EDOM);
    REQUIRE(s_cpu.eax == s_grid + 447U * 32U && s_cpu.ecx == 1U);
    REQUIRE(memcmp(s_reference_arena + (s_grid - at(0)) + 447U * 32U,
                   (void *)(uintptr_t)(s_grid + 447U * 32U), 32U) == 0);
#endif
}
static void *map(uint32_t address, size_t bytes)
{
#if defined(_WIN32)
    return VirtualAlloc((void *)(uintptr_t)address, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)address, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
int main(void)
{
    s_arena = map(0x20000000U, ARENA_BYTES); s_image = map(GUEST_IMAGE_BASE, IMAGE_BYTES);
    REQUIRE((uintptr_t)s_arena == 0x20000000U && (uintptr_t)s_image == GUEST_IMAGE_BASE);
    for (uint32_t pattern = 0U; pattern < 3U; ++pattern)
        for (uint32_t residue = 0U; residue < 32U; ++residue) paired(residue, pattern, 0U);
    for (uint32_t kind = 1U; kind <= 10U; ++kind) paired(13U, 5U, kind);
#ifdef ROOM_GRID_REZERO_ORACLE_REAL_CHILDREN
    paired(13U, 5U, 11U);
#endif
    direct_cases();
#if defined(GUEST_CHECKED_MEMORY) || defined(ROOM_GRID_REZERO_ORACLE_PREFIX_OFF)
    REQUIRE(s_logs == 0U); /* Emitted seam is absent; direct failure does not log. */
#else
    REQUIRE(s_logs == 1U);
#endif
    printf("Room grid init oracle: PASS; paired=%u direct-refusals=%u release-failure=2\n",
           s_paired, s_rejections);
#if defined(_WIN32)
    REQUIRE(VirtualFree(s_arena, 0, MEM_RELEASE) && VirtualFree(s_image, 0, MEM_RELEASE));
#else
    REQUIRE(munmap(s_arena, ARENA_BYTES) == 0 && munmap(s_image, IMAGE_BYTES) == 0);
#endif
    return 0;
}
