#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "host_vita_wav_buffered_rewind.h"
#include "host_vita_heap.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Mapping and exact-extent ledger pattern follows the existing archive MiniZ
 * guest oracle. The reset/refill below deliberately model a stateful decoded
 * block source; Seek, Read and both RIFF passes are freshly generated. */
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "WAV rewind oracle failed line %d: %s\n", __LINE__, #x); \
    exit(1); } } while (0)

enum { ARENA_BYTES = 0x20000U, IMAGE_BYTES = 0x800000U,
       OBJECT_BYTES = 0xc2cU, OUTPUT_OFFSET = 0x81cU, CURSOR = 0xc1cU,
       FILLED = 0xc20U, EOF_BYTE = 0xc28U, POSITION = 0x18U,
       VTABLE = 0x765128U, RETURN = 0x5a3b8dU };
static uint8_t *s_arena, *s_image;
static uint32_t s_object, s_stack, s_output, s_entry;
static uint32_t s_owned_bytes, s_token, s_fail_lease, s_fail_release;
static uint32_t s_lease_calls, s_release_calls, s_log_calls, s_fault_calls;
static uint8_t s_file[16384];
static uint32_t s_file_bytes, s_physical, s_stream_state, s_resets, s_refills;
static uint32_t s_success_cases, s_rejections;
unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_cases;

void sub_0059c160(CPU *__restrict c);
void sub_0059c690(CPU *__restrict c);
void sub_0059c750(CPU *__restrict c);
void sub_005bf4e0(CPU *__restrict c);
void wav_original_seek(CPU *__restrict c);

static void *map_region(uint32_t address, size_t bytes)
{
#if defined(_WIN32)
    return VirtualAlloc((void *)(uintptr_t)address, bytes,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *mapped = mmap((void *)(uintptr_t)address, bytes,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mapped == MAP_FAILED ? NULL : mapped;
#endif
}

static void unmap_region(void *address, size_t bytes)
{
#if defined(_WIN32)
    (void)bytes;
    REQUIRE(VirtualFree(address, 0, MEM_RELEASE));
#else
    REQUIRE(munmap(address, bytes) == 0);
#endif
}

static void push(CPU *c, uint32_t word)
{
    REQUIRE(guest_stack_contains(c, c->esp - 4U, 4U));
    c->esp -= 4U;
    st32(c->esp, word);
}

int guest_stack_set_generated(CPU *__restrict c, uint32_t value)
{
    REQUIRE(value >= c->stack_floor && value <= c->stack_ceiling);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

int guest_stack_adjust_generated(CPU *__restrict c, uint32_t amount)
{
    REQUIRE(amount <= c->stack_ceiling - c->esp);
    c->esp += amount;
    return 1;
}

uint32_t guest_stack_address_generated(CPU *__restrict c, uint32_t a, uint32_t n)
{
    REQUIRE(guest_stack_contains(c, a, n));
    guest_stack_note_low(c, a);
    return a;
}

void gpush_generated(CPU *__restrict c, uint32_t value) { push(c, value); }
uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value;
    REQUIRE(guest_stack_contains(c, c->esp, 4U));
    value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

void guest_fault(CPU *__restrict c, uint32_t a, const char *what)
{
    ++s_fault_calls;
    c->fault = what;
    c->fault_addr = a;
}

uint32_t isaac_vita_guest_heap_lease_exact_range(const void *base,
                                               const void *range, size_t size)
{
    ++s_lease_calls;
    errno = ERANGE; /* helper must restore even on an ownership refusal */
    if (s_fail_lease || s_token || (uintptr_t)base != s_object ||
        range != base || size > s_owned_bytes)
        return 0;
    s_token = 0xabcU;
    return s_token;
}

int isaac_vita_guest_heap_lease_release(uint32_t token)
{
    ++s_release_calls;
    errno = EIO;
    if (s_fail_release)
        return 0;
    REQUIRE(token && token == s_token);
    s_token = 0;
    return 1;
}

void isaac_vita_log(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof line, format, args);
    va_end(args);
    REQUIRE(strstr(line, "WAV buffered rewind engaged bid=") != NULL);
    REQUIRE(strstr(line, "oldpos=") && strstr(line, "filled="));
    ++s_log_calls;
    errno = EINVAL;
}

/* Reset/refill model: physical cursor and rolling stream state advance with
 * decoded blocks. A wrong rewind of either produces a wrong next block. */
void sub_0059c170(CPU *__restrict c)
{
    REQUIRE(c->ecx == s_object);
    ++s_resets;
    s_physical = 0;
    s_stream_state = 0x12345678U;
    st32(s_object + POSITION, 0);
    st32(s_object + CURSOR, 0);
    st32(s_object + FILLED, 0);
    st8(s_object + EOF_BYTE, 0);
    (void)gpop_generated(c);
}

void sub_0059c270(CPU *__restrict c)
{
    uint32_t bytes, i;
    REQUIRE(c->ecx == s_object);
    REQUIRE(ld32(s_object + POSITION) == s_physical);
    if (s_physical >= s_file_bytes) {
        st8(s_object + EOF_BYTE, 1);
        (void)gpop_generated(c);
        return;
    }
    ++s_refills;
    bytes = s_file_bytes - s_physical;
    if (bytes > 1024U) bytes = 1024U;
    memcpy((void *)(uintptr_t)(s_object + OUTPUT_OFFSET), s_file + s_physical, bytes);
    for (i = 0; i < bytes; ++i)
        s_stream_state = (s_stream_state * 33U) ^ s_file[s_physical + i];
    s_physical += bytes;
    st32(s_object + FILLED, bytes);
    st32(s_object + CURSOR, 0);
    (void)gpop_generated(c);
}

void sub_005ec14c(CPU *__restrict c)
{
    uint32_t destination = ld32(c->esp + 4U);
    memcpy((void *)(uintptr_t)destination,
           (const void *)(uintptr_t)ld32(c->esp + 8U), ld32(c->esp + 12U));
    c->eax = destination;
    (void)gpop_generated(c);
}

void sub_005eacd7(CPU *__restrict c)
{
    REQUIRE(c->ecx == ld32(GUEST_IMAGE_BASE + 0x7aa3b4U));
    (void)gpop_generated(c);
}

void sub_0055e330(CPU *__restrict c)
{
    (void)c;
    REQUIRE(!"unexpected RIFF error logger");
}

void guest_call(CPU *__restrict c, uint32_t target)
{
    if (guest_direct_translated_target(target, 0x59c160U)) sub_0059c160(c);
    else if (guest_direct_translated_target(target, 0x59c690U)) sub_0059c690(c);
    else if (guest_direct_translated_target(target, 0x59c750U)) sub_0059c750(c);
    else REQUIRE(!"unexpected guest indirect call");
}

static void put_word(uint32_t at, uint32_t value)
{
    s_file[at] = (uint8_t)value;
    s_file[at + 1U] = (uint8_t)(value >> 8);
    s_file[at + 2U] = (uint8_t)(value >> 16);
    s_file[at + 3U] = (uint8_t)(value >> 24);
}

static void setup(CPU *c, uint32_t header, uint32_t pcm, uint32_t channels)
{
    uint32_t i, base = (uint32_t)(uintptr_t)s_arena;
    memset(s_arena, 0x6d, ARENA_BYTES);
    memset(c, 0, sizeof *c);
    memset(s_file, 0, sizeof s_file);
    s_object = base + 0x1000U;
    s_entry = base + 0x3000U;
    s_output = base + 0x6000U;
    s_stack = base + 0x1f000U - 0x28U;
    s_owned_bytes = OBJECT_BYTES;
    s_token = s_fail_lease = s_fail_release = 0;
    s_lease_calls = s_release_calls = s_fault_calls = 0;
    s_resets = s_refills = s_physical = 0;
    s_stream_state = 0x12345678U;
    s_file_bytes = header + pcm;
    REQUIRE(s_file_bytes <= sizeof s_file && header >= 44U);
    put_word(0, 0x46464952U); put_word(4, s_file_bytes - 8U);
    put_word(8, 0x45564157U); put_word(12, 0x20746d66U);
    put_word(16, 16U); put_word(20, 1U | (channels << 16));
    put_word(24, 44100U); put_word(28, 44100U * channels * 2U);
    put_word(32, channels * 2U | (16U << 16));
    if (header > 44U) {
        put_word(36U, 0x4b4e554aU); put_word(40U, header - 52U);
    }
    put_word(header - 8U, 0x61746164U); put_word(header - 4U, pcm);
    for (i = 0; i < pcm; ++i) s_file[header + i] = (uint8_t)(i * 101U + i / 17U);
    memset((void *)(uintptr_t)s_object, 0, OBJECT_BYTES);
    st32(s_object, GUEST_IMAGE_BASE + VTABLE);
    st32(s_object + 12U, s_entry);
    st32(s_entry + 16U, s_file_bytes);
    c->stack_owner = c;
    c->stack_floor = base + 0x18000U;
    c->stack_ceiling = base + ARENA_BYTES;
    c->stack_low_water = c->stack_ceiling;
    c->esp = s_stack;
    c->ebp = s_stack + 0x28U;
    c->ebx = 0xabcdef01U; c->esi = s_object; c->edi = 0x12344321U;
    st32(s_stack + 12U, 0U);
    st32(s_stack + 16U, s_output);
}

static void riff(CPU *c, uint32_t metadata)
{
    uint32_t before = c->esp;
    c->ecx = s_object;
    c->edx = s_stack + 20U;
    push(c, metadata); push(c, s_stack + 12U);
    push(c, metadata ? 0x5a3b7fU : 0x5a3bbaU);
    sub_005bf4e0(c);
    REQUIRE(c->esp == before - 8U && !c->fault);
    c->esp += 8U;
}

static void prepare_rewind(CPU *c)
{
    c->eax = ld32(s_object); c->ecx = s_object;
    push(c, 0U); push(c, 0U); push(c, RETURN);
}

typedef struct final_state {
    uint32_t pos, cursor, filled, physical, codec, refills, resets, eof;
} final_state;

static final_state run_path(uint32_t header, uint32_t pcm, uint32_t channels, int fast)
{
    CPU c, before;
    final_state result;
    uint8_t object_before[OBJECT_BYTES];
    setup(&c, header, pcm, channels);
    riff(&c, 1U);
    REQUIRE(ld32(s_object + POSITION) == header);
    REQUIRE(ld32(s_stack + 12U) == pcm && s_refills == 1U);
    prepare_rewind(&c);
    before = c;
    memcpy(object_before, (const void *)(uintptr_t)s_object, OBJECT_BYTES);
    errno = EDOM;
    if (fast) {
        if (fast == 1) REQUIRE(isaac_vita_wav_buffered_rewind_try(&c) == 1);
        else sub_0059c690(&c); /* Exercise the actual emitted entry seam too. */
        REQUIRE(errno == EDOM && !c.fault && !s_token);
        before.esp += 12U;
        REQUIRE(memcmp(&c, &before, sizeof c) == 0);
        memset(object_before + POSITION, 0, 4U);
        memset(object_before + CURSOR, 0, 4U);
        REQUIRE(memcmp(object_before, (const void *)(uintptr_t)s_object, OBJECT_BYTES) == 0);
        REQUIRE(s_lease_calls == 1U && s_release_calls == 1U);
    } else wav_original_seek(&c);
    REQUIRE(c.esp == s_stack && !c.fault);
    riff(&c, 0U);
    REQUIRE(memcmp((const void *)(uintptr_t)s_output, s_file + header, pcm) == 0);
    REQUIRE(ld32(s_object + POSITION) == s_file_bytes);
    REQUIRE(ld8(s_object + EOF_BYTE) == 0U);
    /* The next read must set EOF without rereading or changing decoded data. */
    c.ecx = s_object;
    push(&c, 1U); push(&c, 1U); push(&c, s_output + pcm); push(&c, 0x1234U);
    sub_0059c750(&c);
    REQUIRE(c.eax == 0U && c.esp == s_stack && !c.fault);
    result.pos = ld32(s_object + POSITION); result.cursor = ld32(s_object + CURSOR);
    result.filled = ld32(s_object + FILLED); result.eof = ld8(s_object + EOF_BYTE);
    result.physical = s_physical; result.codec = s_stream_state;
    result.refills = s_refills; result.resets = s_resets;
    return result;
}

static void rejection_cases(void)
{
    CPU c, before;
    uint8_t snapshot[ARENA_BYTES];
    uint32_t n;
    for (n = 0; n < 26U; ++n) {
        setup(&c, 104U, 4000U, 2U); riff(&c, 1U); prepare_rewind(&c);
        switch (n) {
        case 0: st32(c.esp, RETURN + 1U); break;
        case 1: st32(c.esp + 4U, 1U); break;
        case 2: st32(c.esp + 8U, 1U); break;
        case 3: c.ebp -= 8U; break;
        case 4: c.stack_owner = NULL; break;
        case 5: c.stack_ceiling = c.esp + 0x2fU; c.stack_low_water = c.esp; break;
        case 6: c.ecx = 0; break;
        case 7: c.esi += 4U; break;
        case 8: c.eax += 4U; break;
        case 9: st32(s_object, 0U); break;
        case 10: s_owned_bytes = OBJECT_BYTES - 1U; break;
        case 11: s_fail_lease = 1U; break;
        case 12: st32(s_object + POSITION, 0U); break;
        case 13: st32(s_object + CURSOR, 103U); break;
        case 14: st32(s_object + FILLED, 103U); break;
        case 15: st32(s_object + FILLED, 1025U); break;
        case 16: st8(s_object + EOF_BYTE, 1U); break;
        case 17: st32(s_object + OUTPUT_OFFSET, 0U); break;
        case 18: st32(s_object + OUTPUT_OFFSET + 16U, 18U); break;
        case 19: st16(s_object + OUTPUT_OFFSET + 34U, 8U); break;
        case 20: st32(c.esp + 0x18U, 0U); break;
        case 21: st32(s_object + OUTPUT_OFFSET + 40U, 53U); break;
        case 22: st32(s_object + OUTPUT_OFFSET + 4U, 0xffffffffU); break;
        case 23: st16(c.esp + 0x20U, 3U); break;
        case 24: st32(s_object + POSITION, 1128U); break;
        case 25: c.fault = "already faulted"; break;
        }
        before = c;
        memcpy(snapshot, s_arena, sizeof snapshot);
        errno = EDOM;
        REQUIRE(isaac_vita_wav_buffered_rewind_try(&c) == 0);
        REQUIRE(errno == EDOM && memcmp(&c, &before, sizeof c) == 0);
        REQUIRE(memcmp(snapshot, s_arena, sizeof snapshot) == 0 && !s_token);
        REQUIRE(!s_fault_calls);
        ++s_rejections;
    }
    for (n = 0; n < 2U; ++n) {
        setup(&c, 104U, 4000U, 2U); riff(&c, 1U); prepare_rewind(&c);
        s_fail_release = 1U;
        if (n) st8(s_object + EOF_BYTE, 1U);
        errno = EDOM;
        REQUIRE(isaac_vita_wav_buffered_rewind_try(&c) == 1);
        REQUIRE(c.fault && s_fault_calls == 1U && c.esp == s_stack && errno == EDOM);
        s_token = 0U; /* test ledger cleanup after deliberately terminal failure */
    }
}

static void coverage_bypasses(void)
{
    CPU c;
    unsigned char enabled = 1U;
    uint32_t kind;
    for (kind = 0U; kind < 2U; ++kind) {
        setup(&c, 104U, 4000U, 2U); riff(&c, 1U); prepare_rewind(&c);
        if (kind) g_guest_coverage_cases = &enabled;
        else g_guest_coverage_functions = &enabled;
        sub_0059c690(&c);
        REQUIRE(!c.fault && c.esp == s_stack && s_lease_calls == 0U && s_resets == 1U);
        REQUIRE(ld32(s_object + POSITION) == 0U && ld32(s_object + FILLED) == 0U);
        g_guest_coverage_cases = g_guest_coverage_functions = NULL;
    }
}

int main(void)
{
    static const uint32_t headers[] = {44U, 104U, 1000U, 1024U};
    static const uint32_t pcms[] = {4U, 100U, 980U, 2048U, 6000U};
    size_t h, p;
    uint32_t channels;
    s_arena = map_region(0x22400000U, ARENA_BYTES);
    s_image = map_region(GUEST_IMAGE_BASE, IMAGE_BYTES);
    REQUIRE(s_arena && (uintptr_t)s_arena <= UINT32_MAX - ARENA_BYTES);
    REQUIRE((uintptr_t)s_image == GUEST_IMAGE_BASE);
    st32(GUEST_IMAGE_BASE + VTABLE + 4U, GUEST_IMAGE_BASE + 0x59c160U);
    st32(GUEST_IMAGE_BASE + VTABLE + 12U, GUEST_IMAGE_BASE + 0x59c690U);
    st32(GUEST_IMAGE_BASE + VTABLE + 20U, GUEST_IMAGE_BASE + 0x59c750U);
    for (h = 0; h < sizeof headers / sizeof headers[0]; ++h)
        for (p = 0; p < sizeof pcms / sizeof pcms[0]; ++p)
            for (channels = 1U; channels <= 2U; ++channels) {
                final_state original = run_path(headers[h], pcms[p], channels, 0);
                int mode;
                for (mode = 1; mode <= 2; ++mode) {
                    final_state fast = run_path(headers[h], pcms[p], channels, mode);
                    final_state expected = original;
                    REQUIRE(original.refills == fast.refills + 1U);
                    REQUIRE(original.resets == 1U && fast.resets == 0U);
                    expected.refills = fast.refills; expected.resets = fast.resets;
                    REQUIRE(memcmp(&expected, &fast, sizeof fast) == 0);
                    ++s_success_cases;
                }
            }
    rejection_cases();
    coverage_bypasses();
    REQUIRE(isaac_vita_wav_buffered_rewind_try(NULL) == 0);
    REQUIRE(s_log_calls == 1U);
    unmap_region(s_arena, ARENA_BYTES); unmap_region(s_image, IMAGE_BYTES);
    printf("WAV buffered rewind oracle: PASS; %u generated two-pass cases (direct+entry); %u unchanged refusals; 2 terminal lease failures; 2 coverage bypasses; 1 engagement log; refill/reset model\n",
           s_success_cases, s_rejections);
    return 0;
}
