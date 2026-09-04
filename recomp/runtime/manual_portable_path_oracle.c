#include "guest.h"
#include "manual_portable.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORACLE_PATH_BYTES 0x104u
#define ORACLE_PATH_LIMIT 0x100000u
#define ORACLE_SLOT_BYTES (ORACLE_PATH_LIMIT + 0x200u)
#define ORACLE_RANDOM_CASES 20000u

typedef struct OraclePiece {
    const unsigned char *bytes;
    uint32_t length;
} OraclePiece;

static unsigned char s_guest_arena[ORACLE_SLOT_BYTES * 2u + 0x1000u]
    __attribute__((aligned(16)));
static unsigned char s_guest_stack[16] __attribute__((aligned(16)));
static unsigned char s_coverage[4];
static const char *s_fault_message;
static uint32_t s_rng = 0x599c0a5bu;
static unsigned s_checks;

unsigned char *g_guest_coverage_functions = s_coverage;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault = message;
    c->fault_addr = address;
    s_fault_message = message;
}

static void fail(const char *label, const char *detail)
{
    fprintf(stderr, "manual portable path oracle FAIL [%s]: %s\n",
            label, detail);
    exit(1);
}

static uint32_t guest_address(void *pointer)
{
    uintptr_t value = (uintptr_t)pointer;
    if (value > UINT32_MAX)
        fail("arena", "host oracle address does not fit the guest ABI");
    return (uint32_t)value;
}

static uint32_t rng_next(void)
{
    uint32_t value = s_rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_rng = value;
    return value;
}

static uint32_t reference_split(unsigned char *bytes, uint32_t length,
                                OraclePiece *pieces)
{
    uint32_t count = 0u;
    uint32_t position = 0u;
    while (position < length) {
        uint32_t start = position;
        while (position < length && bytes[position] != (unsigned char)'/')
            ++position;
        pieces[count].bytes = bytes + start;
        pieces[count].length = position - start;
        ++count;
        if (position == length)
            break;
        ++position;
        if (position == length)
            break;
    }
    return count;
}

/* This is the removed snapshot/component implementation kept only as an
 * independent test oracle.  The production object is separately checked for
 * raw allocator imports. */
static int reference_path(const unsigned char *base_input,
                          const unsigned char *relative_input,
                          unsigned char expected[ORACLE_PATH_BYTES])
{
    size_t base_size = strlen((const char *)base_input);
    size_t relative_size = strlen((const char *)relative_input);
    unsigned char *base;
    unsigned char *relative;
    OraclePiece *base_parts;
    OraclePiece *relative_parts;
    OraclePiece leaf;
    uint32_t base_count;
    uint32_t relative_count;
    uint32_t logical = 0u;
    uint32_t i;
    int ok = 0;

    if (base_size > UINT32_MAX || relative_size > UINT32_MAX)
        return 0;
    base = (unsigned char *)malloc(base_size + 1u);
    relative = (unsigned char *)malloc(relative_size + 1u);
    base_parts = (OraclePiece *)malloc(
        (base_size + relative_size + 2u) * sizeof(*base_parts));
    relative_parts = (OraclePiece *)malloc(
        (relative_size + 1u) * sizeof(*relative_parts));
    if (!base || !relative || !base_parts || !relative_parts)
        fail("reference", "test allocator failed");

    for (i = 0u; i < (uint32_t)base_size; ++i)
        base[i] = base_input[i] == (unsigned char)'\\'
                ? (unsigned char)'/' : base_input[i];
    base[base_size] = 0u;
    for (i = 0u; i < (uint32_t)relative_size; ++i)
        relative[i] = relative_input[i] == (unsigned char)'\\'
                    ? (unsigned char)'/' : relative_input[i];
    relative[relative_size] = 0u;

    base_count = reference_split(base, (uint32_t)base_size, base_parts);
    relative_count = reference_split(
        relative, (uint32_t)relative_size, relative_parts);
    if (!base_count || !relative_count)
        goto done;

    --base_count;
    leaf = relative_parts[--relative_count];
    for (i = 0u; i < relative_count; ++i) {
        OraclePiece part = relative_parts[i];
        if (part.length == 2u && part.bytes[0] == (unsigned char)'.' &&
            part.bytes[1] == (unsigned char)'.') {
            if (base_count)
                --base_count;
            else
                base_parts[base_count++] = part;
        } else {
            base_parts[base_count++] = part;
        }
    }

    memset(expected, 0, ORACLE_PATH_BYTES);
    for (i = 0u; i < base_count && logical < ORACLE_PATH_BYTES; ++i) {
        uint32_t j;
        for (j = 0u; j < base_parts[i].length &&
                     logical < ORACLE_PATH_BYTES; ++j)
            expected[logical++] = base_parts[i].bytes[j];
        if (logical < ORACLE_PATH_BYTES)
            expected[logical++] = (unsigned char)'/';
    }
    for (i = 0u; i < leaf.length && logical < ORACLE_PATH_BYTES; ++i)
        expected[logical++] = leaf.bytes[i];
    ok = 1;

done:
    free(relative_parts);
    free(base_parts);
    free(relative);
    free(base);
    return ok;
}

static int invoke_path(uint32_t base, uint32_t relative, uint32_t output,
                       const char *label)
{
    CPU cpu;
    uint32_t stack_address = guest_address(s_guest_stack);

    memset(&cpu, 0, sizeof cpu);
    memset(s_guest_stack, 0, sizeof s_guest_stack);
    cpu.esp = stack_address;
#if GUEST_STACK_REQUIRED
    cpu.stack_owner = &cpu;
    cpu.stack_floor = stack_address;
    cpu.stack_ceiling = stack_address + (uint32_t)sizeof s_guest_stack;
    cpu.stack_low_water = cpu.stack_ceiling;
#endif
    cpu.ebp = 0x13579bdfu;
    cpu.ebx = 0x2468ace0u;
    cpu.esi = 0x89abcdefu;
    cpu.edi = 0x76543210u;
    cpu.ecx = base;
    cpu.edx = relative;
    st32(stack_address, 0xabc599c0u);
    st32(stack_address + 4u, output);
    s_fault_message = NULL;
    sub_002599c0(&cpu);

    if (!cpu.fault) {
        if (cpu.esp != stack_address + 4u ||
            cpu.ebp != 0x13579bdfu || cpu.ebx != 0x2468ace0u ||
            cpu.esi != 0x89abcdefu || cpu.edi != 0x76543210u)
            fail(label, "success changed the return ABI or saved registers");
        if (ld32(stack_address + 4u) != output)
            fail(label, "success overwrote the caller-cleaned output argument");
        return 1;
    }
    if (cpu.esp != stack_address)
        fail(label, "fault changed the guest stack");
    return 0;
}

static void run_case_at(const unsigned char *base,
                        const unsigned char *relative, uint32_t output,
                        const char *label, int guard_output)
{
    unsigned char expected[ORACLE_PATH_BYTES];
    uint32_t base_address = guest_address(s_guest_arena);
    uint32_t relative_address = guest_address(
        s_guest_arena + ORACLE_SLOT_BYTES);
    size_t base_size = strlen((const char *)base) + 1u;
    size_t relative_size = strlen((const char *)relative) + 1u;

    if (!reference_path(base, relative, expected))
        fail(label, "reference rejected a non-empty case");
    if (base_size > ORACLE_SLOT_BYTES || relative_size > ORACLE_SLOT_BYTES)
        fail(label, "test input exceeds its guest slot");
    memcpy(s_guest_arena, base, base_size);
    memcpy(s_guest_arena + ORACLE_SLOT_BYTES, relative, relative_size);
    if (guard_output) {
        memset((void *)(uintptr_t)output, 0xa5, ORACLE_PATH_BYTES + 1u);
    }
    if (!invoke_path(base_address, relative_address, output, label))
        fail(label, s_fault_message ? s_fault_message : "unexpected fault");
    if (memcmp((const void *)(uintptr_t)output,
               expected, ORACLE_PATH_BYTES) != 0)
        fail(label, "production output differs from snapshot reference");
    if (guard_output && ld8(output + ORACLE_PATH_BYTES) != 0xa5u)
        fail(label, "production wrote past the exact 260-byte output");
    ++s_checks;
}

static void run_case(const unsigned char *base,
                     const unsigned char *relative, const char *label)
{
    uint32_t output = guest_address(
        s_guest_arena + ORACLE_SLOT_BYTES * 2u);
    run_case_at(base, relative, output, label, 1);
}

static void run_alias_case(const unsigned char *base,
                           const unsigned char *relative, int alias,
                           const char *label)
{
    uint32_t base_address = guest_address(s_guest_arena);
    uint32_t relative_address = guest_address(
        s_guest_arena + ORACLE_SLOT_BYTES);
    uint32_t output;

    if (alias == 0)
        output = base_address;
    else if (alias == 1)
        output = base_address + 1u;
    else if (alias == 2)
        output = relative_address;
    else
        output = relative_address + 2u;
    run_case_at(base, relative, output, label, 0);
}

static uint32_t random_path(unsigned char *bytes, uint32_t capacity)
{
    uint32_t length = 1u + rng_next() % (capacity - 1u);
    uint32_t i;
    static const unsigned char ordinary[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";

    for (i = 0u; i < length; ++i) {
        uint32_t choice = rng_next() % 16u;
        if (choice < 2u)
            bytes[i] = (unsigned char)'/';
        else if (choice < 4u)
            bytes[i] = (unsigned char)'\\';
        else if (choice < 7u)
            bytes[i] = (unsigned char)'.';
        else
            bytes[i] = ordinary[rng_next() % (sizeof ordinary - 1u)];
    }
    bytes[length] = 0u;
    return length;
}

static void check_fixed_and_random(void)
{
    unsigned char base[4096];
    unsigned char relative[4096];
    uint32_t i;

    run_case((const unsigned char *)"resources/packed/file.anm2",
             (const unsigned char *)"../gfx/foo.png", "normal-parent");
    run_case((const unsigned char *)"root\\dir\\base.xml",
             (const unsigned char *)".\\sub//leaf.anm2", "normalization");
    run_case((const unsigned char *)"/a//base",
             (const unsigned char *)"//leaf", "empty-components");
    run_case((const unsigned char *)"basefile",
             (const unsigned char *)"../../../leaf", "alternating-parents");
    run_case((const unsigned char *)"a/b/file/",
             (const unsigned char *)"dir/", "trailing-separators");
    run_case((const unsigned char *)"a/",
             (const unsigned char *)"leaf/", "one-terminal-separator");
    run_case((const unsigned char *)"a//",
             (const unsigned char *)"leaf//", "two-terminal-separators");
    run_case((const unsigned char *)"a///",
             (const unsigned char *)"leaf///", "three-terminal-separators");
    run_case((const unsigned char *)"a/b/file",
             (const unsigned char *)"dir/..", "parent-leaf-is-literal");
    run_case((const unsigned char *)"a/b/file",
             (const unsigned char *)"dir/.", "dot-leaf-is-literal");

    run_alias_case((const unsigned char *)"base/dir/file.xml",
                   (const unsigned char *)"../gfx/leaf.png", 0,
                   "output-equals-base");
    run_alias_case((const unsigned char *)"base/dir/file.xml",
                   (const unsigned char *)"../gfx/leaf.png", 1,
                   "output-overlaps-base");
    run_alias_case((const unsigned char *)"base/dir/file.xml",
                   (const unsigned char *)"../gfx/leaf.png", 2,
                   "output-equals-relative");
    run_alias_case((const unsigned char *)"base/dir/file.xml",
                   (const unsigned char *)"../gfx/leaf.png", 3,
                   "output-overlaps-relative");

    for (i = 0u; i < ORACLE_RANDOM_CASES; ++i) {
        char label[64];
        random_path(base, 2u + rng_next() % (sizeof base - 2u));
        random_path(relative, 2u + rng_next() % (sizeof relative - 2u));
        snprintf(label, sizeof label, "random-%u", i);
        run_case(base, relative, label);
        if ((i & 1023u) == 0u)
            run_alias_case(base, relative, (int)((i >> 10) & 3u),
                           "random-alias");
    }
}

static void append_bytes(unsigned char *buffer, uint32_t *length,
                         const char *bytes)
{
    size_t amount = strlen(bytes);
    if ((size_t)*length + amount + 1u > ORACLE_SLOT_BYTES)
        fail("builder", "structured input overflow");
    memcpy(buffer + *length, bytes, amount);
    *length += (uint32_t)amount;
    buffer[*length] = 0u;
}

static void check_hostile_prefixes(void)
{
    unsigned char *base = (unsigned char *)malloc(ORACLE_SLOT_BYTES);
    unsigned char *relative = (unsigned char *)malloc(ORACLE_SLOT_BYTES);
    uint32_t base_length = 0u;
    uint32_t relative_length = 0u;
    uint32_t i;

    if (!base || !relative)
        fail("hostile", "test allocator failed");
    base[0] = 0u;
    relative[0] = 0u;

    strcpy((char *)base, "b");
    for (i = 259u; i <= 261u; ++i) {
        const char *label = i == 259u ? "leaf-259-plus-padding" :
                            i == 260u ? "leaf-exact-260-no-nul" :
                                       "leaf-261-truncated";
        memset(relative, 'x', i);
        relative[i] = 0u;
        run_case(base, relative, label);
    }

    base[0] = 0u;
    relative[0] = 0u;
    for (i = 0u; i < 340u; ++i)
        append_bytes(base, &base_length, "/");
    append_bytes(base, &base_length, "file");
    for (i = 0u; i < 87u; ++i)
        append_bytes(relative, &relative_length, "../");
    append_bytes(relative, &relative_length, "leaf");
    run_case(base, relative, "hidden-empty-components-revealed");

    base_length = 0u;
    relative_length = 0u;
    base[0] = 0u;
    relative[0] = 0u;
    append_bytes(base, &base_length, "prefix/");
    for (i = 0u; i < 400u; ++i)
        append_bytes(base, &base_length, "x");
    append_bytes(base, &base_length, "/");
    for (i = 0u; i < 300u; ++i)
        append_bytes(base, &base_length, "q/");
    append_bytes(base, &base_length, "file");
    for (i = 0u; i < 301u; ++i)
        append_bytes(relative, &relative_length, "../");
    append_bytes(relative, &relative_length, "leaf");
    run_case(base, relative, "long-saturating-component-revealed");

    memset(base, 'a', ORACLE_PATH_LIMIT);
    base[ORACLE_PATH_LIMIT] = 0u;
    strcpy((char *)relative, "leaf");
    run_case(base, relative, "exact-limit-base");
    strcpy((char *)base, "base");
    memset(relative, 'z', ORACLE_PATH_LIMIT);
    relative[ORACLE_PATH_LIMIT] = 0u;
    run_case(base, relative, "exact-limit-relative-leaf");

    free(relative);
    free(base);
}

static void check_faults(void)
{
    uint32_t base = guest_address(s_guest_arena);
    uint32_t relative = guest_address(s_guest_arena + ORACLE_SLOT_BYTES);
    uint32_t output = guest_address(
        s_guest_arena + ORACLE_SLOT_BYTES * 2u);
    unsigned char before[ORACLE_PATH_BYTES];

    strcpy((char *)(uintptr_t)base, "base");
    strcpy((char *)(uintptr_t)relative, "leaf");
    memset((void *)(uintptr_t)output, 0xa5, ORACLE_PATH_BYTES);
    memcpy(before, (const void *)(uintptr_t)output, sizeof before);
    if (invoke_path(0u, relative, output, "null-base") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: null base path") != 0)
        fail("null-base", "wrong fault result");
    if (memcmp(before, (const void *)(uintptr_t)output, sizeof before) != 0)
        fail("null-base", "fault changed output");

    if (invoke_path(base, 0u, output, "null-relative") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: null relative path") != 0)
        fail("null-relative", "wrong fault result");
    if (memcmp(before, (const void *)(uintptr_t)output, sizeof before) != 0)
        fail("null-relative", "fault changed output");

    st8(base, 0u);
    if (invoke_path(base, relative, output, "empty-base") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: empty path has no leaf component") != 0)
        fail("empty-base", "wrong fault result");
    if (memcmp(before, (const void *)(uintptr_t)output, sizeof before) != 0)
        fail("empty-base", "fault changed output");

    strcpy((char *)(uintptr_t)base, "base");
    st8(relative, 0u);
    if (invoke_path(base, relative, output, "empty-relative") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: empty path has no leaf component") != 0)
        fail("empty-relative", "wrong fault result");
    if (memcmp(before, (const void *)(uintptr_t)output, sizeof before) != 0)
        fail("empty-relative", "fault changed output");

    memset((void *)(uintptr_t)base, 'a', ORACLE_PATH_LIMIT + 1u);
    if (invoke_path(base, relative, output, "over-limit-base") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: base path exceeds snapshot limit") != 0)
        fail("over-limit-base", "wrong limit fault result");
    if (memcmp(before, (const void *)(uintptr_t)output, sizeof before) != 0)
        fail("over-limit-base", "fault changed output");

    strcpy((char *)(uintptr_t)base, "base");
    memset((void *)(uintptr_t)relative, 'r', ORACLE_PATH_LIMIT + 1u);
    if (invoke_path(base, relative, output, "over-limit-relative") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: relative path exceeds snapshot limit") != 0)
        fail("over-limit-relative", "wrong limit fault result");
    if (memcmp(before, (const void *)(uintptr_t)output, sizeof before) != 0)
        fail("over-limit-relative", "fault changed output");

    strcpy((char *)(uintptr_t)base, "base");
    strcpy((char *)(uintptr_t)relative, "leaf");
    if (invoke_path(base, relative, 0u, "null-output") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: invalid output range") != 0)
        fail("null-output", "wrong output-range fault result");
    if (invoke_path(base, relative, 0xfffffefdu, "high-output") ||
        !s_fault_message || strcmp(s_fault_message,
            "portable path: invalid output range") != 0)
        fail("high-output", "wrong output-range fault result");
    s_checks += 8u;
}

int main(void)
{
    uintptr_t arena_begin = (uintptr_t)s_guest_arena;
    uintptr_t arena_end = arena_begin + sizeof s_guest_arena;

    if (arena_end > UINT32_MAX || arena_end < arena_begin)
        fail("arena", "static guest arena is outside 32-bit address space");
    check_fixed_and_random();
    check_hostile_prefixes();
    check_faults();
    if (s_coverage[2] != 1u)
        fail("coverage", "production wrapper did not mark its coverage ID");
    printf("manual portable allocation-free path oracle: PASS "
           "(%u exact/reference checks, %u randomized)\n",
           s_checks, ORACLE_RANDOM_CASES);
    return 0;
}
