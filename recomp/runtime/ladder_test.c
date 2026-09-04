/* Re-run the ladder's oracles through the real recompiler.
 *
 * The two hand-grown spikes proved 5,396 checks with 0 mismatches on
 * RNG::Next and RNG::RandomFloat. Those functions are themselves pinned to
 * the shipped game -- 4,443 and 2,084 live calls with zero mismatches inside
 * the running process -- so agreement here is agreement with the program, not
 * with a test written alongside the translator.
 *
 * A rewrite that does not re-run its predecessor's checks has lost a proven
 * result whether or not anything says so.
 */
#include <stdio.h>
#include <string.h>

#include "guest.h"

#ifndef EXE_PATH
#define EXE_PATH "isaac-ng.exe.unpacked.exe"
#endif

/* The image is rebased (see guest.c), so addresses are base + RVA rather than
 * the 0x400000 + RVA that every note in the journals uses. Keep the RVAs
 * visible so the two can still be matched up by eye. */
#define VA_SHIFTS   (GUEST_IMAGE_BASE + 0x608aa0u) /* 81 x 12-byte triples */
#define VA_SCALE    (GUEST_IMAGE_BASE + 0x76a198u) /* 0x2F7FFFFE scale     */
#define VA_ISA_AVAILABLE (GUEST_IMAGE_BASE + 0x7fd61cu)
#define VA_SIMD_MULTIPLIERS (GUEST_IMAGE_BASE + 0x76b4a0u)
#define VA_PANDN_SOURCE (GUEST_IMAGE_BASE + 0x76caf0u)
#define VA_CHARACTER_SORT_VECTOR (GUEST_IMAGE_BASE + 0x806f64u)
#define VA_IAT_TIMEGETTIME (GUEST_IMAGE_BASE + 0x6064c0u)

void rng_next(CPU *__restrict c);
void rng_randomint(CPU *__restrict c);
void rng_randomfloat(CPU *__restrict c);
void sub_00089420(CPU *__restrict c);      /* WeightedOutcomePicker::PickOutcome */
void sub_005ea140(CPU *__restrict c);      /* mimalloc atomic fetch-add64 */
void sub_005ec2c0(CPU *__restrict c);      /* MSVC _ftol2/_ftol2_sse */
void sub_004a1f20(CPU *__restrict c);      /* SortCharRenders partition */
void x87_fstp_st0(CPU *__restrict c);      /* real DD D8 instruction */
void x87_fstp_st1(CPU *__restrict c);      /* real DD D9 instruction */
void x87_fstp_st2(CPU *__restrict c);      /* real DD DA instruction */
void x87_fstp_st3(CPU *__restrict c);      /* real DD DB instruction */
void x87_fstp_st4(CPU *__restrict c);      /* real DD DC instruction */
void x87_fstp_st5(CPU *__restrict c);      /* real DD DD instruction */
void x87_fstp_st6(CPU *__restrict c);      /* real DD DE instruction */
void x87_fstp_st7(CPU *__restrict c);      /* real DD DF instruction */
void simd_pmulld_reg(CPU *__restrict c);   /* real 00008683 instruction */
void simd_pmulld_mem(CPU *__restrict c);   /* real 000950c3 instruction */
void simd_pcmpgtd_reg(CPU *__restrict c);  /* real 000950da instruction */
void simd_pmulld_self(CPU *__restrict c);  /* real 005d1a76 instruction */
void simd_pshuflw_self(CPU *__restrict c); /* real 005c6e0e instruction */
void simd_pshufhw_self(CPU *__restrict c); /* real 005c6e13 instruction */
void simd_packuswb_self(CPU *__restrict c); /* real 005c6e29 instruction */
void simd_pandn_reg(CPU *__restrict c);    /* real 005b2ed6 instruction */
void simd_pandn_mem(CPU *__restrict c);    /* real 005d4266 instruction */
void comis_flags_comiss(CPU *__restrict c);  /* real 0004e3d0 instruction */
void comis_flags_ucomiss(CPU *__restrict c); /* real 005340ce instruction */
void comis_flags_comisd(CPU *__restrict c);  /* real 0029eef0 instruction */
void comis_flags_ucomisd(CPU *__restrict c); /* real 0017866f instruction */
void indirect_call_stack_probe(CPU *__restrict c); /* real 002d118c instruction */

/* 8 bytes, confirmed on both builds: the Switch code indexes base + i*8 for
 * the value and +4 for the weight, the Windows build stores the pair with a
 * single 8-byte write. */
typedef struct { int32_t value, weight; } Outcome;

/* Exact 16-byte Menu_Character::CharacterRender sort record.  Only the key
 * at +8 has comparison semantics; unique payload words make an accidental
 * equal-key swap observable rather than harmless. */
typedef struct {
    uint32_t identity;
    uint32_t payload0;
    float key;
    uint32_t payload1;
} CharacterSortRecord;

typedef struct {
    uint32_t before[4];
    CharacterSortRecord records[14];
    uint32_t after[4];
} CharacterSortGuarded;

extern unsigned g_int3_count, g_fault_count;

/* Callees of get_chaos_pool that are not translated yet. A recompiler has to
 * translate the transitive closure of everything reachable; until it does,
 * an untranslated callee must fault loudly rather than link to nothing. */
#define UNTRANSLATED(addr)                                                   \
    void sub_##addr(CPU *__restrict c)                                       \
    { guest_fault(c, 0x##addr##U, "callee not translated yet"); }
UNTRANSLATED(0008d900)
UNTRANSLATED(005eb08e)

/* The one callee all four functions share: KAGE's logger. Counted so the
 * zero-seed branch can be shown to have actually run -- an untaken branch
 * that reports "0 mismatches" proves nothing. */
unsigned g_log_calls;
void sub_0055e330(CPU *__restrict c)
{
    g_log_calls++;
    (void)gpop(c);           /* the return address our `call` pushed */
}

/* --------------------------------------------------------------- model -- */
/* The reference implementation. Three lines, and deliberately written from
 * the algorithm rather than from the generated C. */
typedef struct { uint32_t seed, s1, s2, s3; } RNG;

static uint32_t ref_next(RNG *r)
{
    uint32_t s = r->seed;
    s ^= s >> (r->s1 & 31);
    s ^= s << (r->s2 & 31);
    s ^= s >> (r->s3 & 31);
    return r->seed = s;
}

static float ref_float(RNG *r)
{
    uint32_t v = ref_next(r);
    return (float)v * 2.3283062e-10f;   /* the literal 0x2F7FFFFE */
}

static uint32_t ref_int(RNG *r, uint32_t max)
{
    uint32_t v = ref_next(r);
    return max ? v % max : 0u;
}

/* -------------------------------------------------------------- driver -- */

static CPU cpu;
__declspec(align(8)) static volatile uint64_t atomic_word;
static uint8_t f80_storage[12];
__declspec(align(16)) static CharacterSortGuarded character_sort_guarded;
static const guest_import ladder_imports[] = {
    { 0x006064c0U, "WINMM.dll!timeGetTime" },
};

static uint64_t call_atomic_add64(uint64_t delta, int *bad_stack)
{
    uint32_t saved = cpu.esp;
    uint64_t old;
    gpush(&cpu, (uint32_t)(delta >> 32));
    gpush(&cpu, (uint32_t)delta);
    gpush(&cpu, (uint32_t)(uintptr_t)&atomic_word);
    gpush(&cpu, 0xA70C1C64u);
    sub_005ea140(&cpu);
    old = ((uint64_t)cpu.edx << 32) | cpu.eax;
    if (cpu.esp != saved - 12U) (*bad_stack)++;
    cpu.esp = saved;
    return old;
}

static double double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint8_t compare_lahf_byte(const CPU *state)
{
    return (uint8_t)((fl_sf(&state->fl) ? 0x80U : 0U) |
                     (fl_zf(&state->fl) ? 0x40U : 0U) |
                     (fl_pf(&state->fl) ? 0x04U : 0U) | 0x02U |
                     (fl_cf(&state->fl) ? 0x01U : 0U));
}

static int run_comis_flag_probe(void (*probe)(CPU *__restrict), int is_double,
                                const char *name, int *checks)
{
    static const struct {
        double left;
        double right;
        uint8_t expected_ah;
    } finite_cases[] = {
        { 2.0, 1.0, 0x02 }, /* greater   CF/ZF/PF = 0/0/0 */
        { 1.0, 2.0, 0x03 }, /* less      CF/ZF/PF = 1/0/0 */
        { 1.0, 1.0, 0x42 }, /* equal     CF/ZF/PF = 0/1/0 */
    };
    const double quiet_nan = double_from_bits(UINT64_C(0x7ff8000000001234));
    int index;
    int failures = 0;

    for (index = 0; index < 4; ++index) {
        double left = index == 3 ? quiet_nan : finite_cases[index].left;
        double right = index == 3 ? 1.0 : finite_cases[index].right;
        uint8_t expected = index == 3 ? 0x47 : finite_cases[index].expected_ah;
        uint8_t actual;

        memset(cpu.x, 0, sizeof cpu.x);
        if (is_double) {
            cpu.x[0].d[0] = left;
            cpu.x[1].d[0] = right;
        } else {
            cpu.x[0].f[0] = (float)left;
            cpu.x[1].f[0] = (float)right;
        }
        cpu.f_op = FLAG_NONE;
        cpu.f_r = 0x00000000U; /* catches accidental generic parity reuse */
        cpu.f_of = 1;
        probe(&cpu);
        actual = compare_lahf_byte(&cpu);
        *checks += 2;
        if (actual != expected || fl_of(&cpu.fl) != 0 ||
                cpu.f_op != FLAG_PARTIAL) {
            printf("  MISMATCH %s case=%d AH=%02X want=%02X OF=%d op=%u\n",
                   name, index, actual, expected, fl_of(&cpu.fl), cpu.f_op);
            failures++;
        }
        /* This is the exact native partition discriminator.  Equal is the
         * sole ordered outcome for which TEST AH,44h has odd parity. */
        if ((((actual & 0x44U) == 0x40U) ? 1 : 0) !=
                (index == 2 ? 1 : 0))
            failures++;
    }
    return failures;
}

static uint32_t call_ftol(double value, uint32_t isa_available,
                          int *bad_stack, int *bad_x87)
{
    uint32_t saved_esp = cpu.esp;
    const int saved_top = 3;

    st32(VA_ISA_AVAILABLE, isa_available);
    cpu.st_top = saved_top;
    cpu.st[saved_top] = value;
    gpush(&cpu, 0xF8702C0u);
    sub_005ec2c0(&cpu);
    if (cpu.esp != saved_esp) (*bad_stack)++;
    if (cpu.st_top != ((saved_top + 1) & 7)) (*bad_x87)++;
    cpu.esp = saved_esp;
    return cpu.eax;
}

static int run_character_equal_sort(int *checks)
{
    CharacterSortGuarded expected;
    uint32_t old_base = ld32(VA_CHARACTER_SORT_VECTOR);
    uint32_t saved_esp = cpu.esp;
    uint32_t saved_ebp = cpu.ebp;
    uint32_t saved_ebx = cpu.ebx;
    uint32_t saved_esi = cpu.esi;
    uint32_t saved_edi = cpu.edi;
    int index;
    int failures = 0;

    memset(&character_sort_guarded, 0, sizeof character_sort_guarded);
    for (index = 0; index < 4; ++index) {
        character_sort_guarded.before[index] = 0xBA5E0000U + (uint32_t)index;
        character_sort_guarded.after[index] = 0xAF7E0000U + (uint32_t)index;
    }
    for (index = 0; index < 14; ++index) {
        CharacterSortRecord *record = &character_sort_guarded.records[index];
        record->identity = 0xC0010000U + (uint32_t)index;
        record->payload0 = 0x12340000U ^ ((uint32_t)index * 0x10203U);
        record->key = 1.0f;
        record->payload1 = 0xFEDC0000U ^ ((uint32_t)index * 0x30405U);
    }
    expected = character_sort_guarded;
    st32(VA_CHARACTER_SORT_VECTOR,
         (uint32_t)(uintptr_t)&character_sort_guarded.records[0]);

    printf("SORT 004a1f20 equal BEGIN n=14\n");
    fflush(stdout);
    gpush(&cpu, 13U);          /* inclusive high index, caller-owned */
    gpush(&cpu, 0xA1F20E0U);  /* synthetic return */
    cpu.edx = 0U;              /* inclusive low index */
    cpu.ecx = VA_CHARACTER_SORT_VECTOR;
    sub_004a1f20(&cpu);
    printf("SORT 004a1f20 equal RETURN\n");
    fflush(stdout);

    *checks += 3;
    if (cpu.esp != saved_esp - 4U) {
        printf("  MISMATCH SortCharRenders ESP=%08X want=%08X\n",
               cpu.esp, saved_esp - 4U);
        failures++;
    }
    if (cpu.ebp != saved_ebp || cpu.ebx != saved_ebx ||
            cpu.esi != saved_esi || cpu.edi != saved_edi) {
        printf("  MISMATCH SortCharRenders nonvolatile registers\n");
        failures++;
    }
    if (memcmp(&character_sort_guarded, &expected, sizeof expected) != 0) {
        printf("  MISMATCH SortCharRenders equal records/canaries changed\n");
        failures++;
    }

    st32(VA_CHARACTER_SORT_VECTOR, old_base);
    cpu.esp = saved_esp;
    cpu.ebp = saved_ebp;
    cpu.ebx = saved_ebx;
    cpu.esi = saved_esi;
    cpu.edi = saved_edi;
    return failures;
}

static int run_indirect_call_stack_probe(int *checks)
{
    uint32_t initial_esp = cpu.esp;
    uint32_t call_esp;
    uint32_t target;
    unsigned calls_before = g_host_import_calls;
    unsigned faults_before = g_fault_count;
    int stopped;
    int index;
    int failures = 0;

    /* Leave mapped headroom above ESP, then reproduce the two adjacent live
     * slots.  Correct x86 reads old [ESP+20]=timeGetTime.  The former emitter
     * pushed first and read new [ESP+20]=old [ESP+1c]=Entity type 1000. */
    for (index = 0; index < 12; ++index)
        gpush(&cpu, 0xC0110000U + (uint32_t)index);
    call_esp = cpu.esp;
    target = ld32(VA_IAT_TIMEGETTIME);
    st32(call_esp + 0x1cU, 0x000003e8U);
    st32(call_esp + 0x20U, target);

    /* Keep the negative regression observable.  The old push-before-read
     * lowering dispatches to 0x000003e8 and therefore faults; without the
     * guarded run boundary that fault aborts the whole ladder before any of
     * the exact assertions below can report what changed. */
    stopped = guest_run_until_stop(&cpu, indirect_call_stack_probe);
    *checks += 6;
    if (stopped != GUEST_RUN_RETURNED) {
        printf("  MISMATCH indirect CALL stop=%d want=%d\n",
               stopped, GUEST_RUN_RETURNED);
        failures++;
    }
    if (target != VA_IAT_TIMEGETTIME) {
        printf("  MISMATCH indirect CALL IAT token=%08X\n", target);
        failures++;
    }
    if (cpu.esp != call_esp) {
        printf("  MISMATCH indirect CALL ESP=%08X want=%08X\n",
               cpu.esp, call_esp);
        failures++;
    }
    if (g_host_import_calls != calls_before + 1U) {
        printf("  MISMATCH indirect CALL import delta=%u\n",
               g_host_import_calls - calls_before);
        failures++;
    }
    if (g_fault_count != faults_before || cpu.fault) {
        printf("  MISMATCH indirect CALL faulted at %08X: %s\n",
               cpu.fault_addr, cpu.fault ? cpu.fault : "none");
        failures++;
    }
    if (ld32(call_esp + 0x1cU) != 0x000003e8U) {
        printf("  MISMATCH indirect CALL adjacent slot changed\n");
        failures++;
    }

    cpu.esp = initial_esp;
    return failures;
}

/* Invoke a translated __thiscall function: `this` in ecx, stack arguments
 * pushed right to left, then a return address because the callee's `ret`
 * pops one. */
static void call_this(void (*fn)(CPU *__restrict), uint32_t self,
                      const uint32_t *args, int nargs)
{
    int i;
    uint32_t save = cpu.esp;
    for (i = nargs - 1; i >= 0; i--) gpush(&cpu, args[i]);
    gpush(&cpu, 0xDEADBEEFu);
    cpu.ecx = self;
    fn(&cpu);
    cpu.esp = save;            /* the caller owns its own frame either way */
}

static uint32_t fnv(uint32_t h, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

int main(void)
{
    uint32_t *shifts;
    int idx, k, fail = 0, checks = 0;
    uint32_t h_ref, h_got;
    int signbit_hits = 0;

    guest_register_imports(ladder_imports, 1U);
    if (guest_image_load(EXE_PATH)) return 2;
    if (guest_stack_init(&cpu))     return 2;

    shifts = (uint32_t *)(uintptr_t)VA_SHIFTS;
    printf("shift table [0..2]          : %u, %u, %u   (expect 1, 3, 10)\n",
           shifts[0], shifts[1], shifts[2]);
    printf("float scale word at %08x: %08X   (expect 2F7FFFFE)\n",
           VA_SCALE, *(uint32_t *)(uintptr_t)VA_SCALE);
    if (shifts[0] != 1 || shifts[1] != 3 || shifts[2] != 10) {
        printf("IMAGE NOT MAPPED CORRECTLY -- refusing to report a pass\n");
        return 2;
    }

    /* ---- indirect CALL: target is read before architectural push ------ */
    {
        int call_fail = run_indirect_call_stack_probe(&checks);
        fail += call_fail;
        printf("indirect CALL [esp+20]      : pre-push target, %d mismatches\n",
               call_fail);
    }

    /* ---- scalar SSE compares: all architectural flag outcomes -------- */
    {
        int compare_fail = 0;
        compare_fail += run_comis_flag_probe(
            comis_flags_comiss, 0, "COMISS", &checks);
        compare_fail += run_comis_flag_probe(
            comis_flags_ucomiss, 0, "UCOMISS", &checks);
        compare_fail += run_comis_flag_probe(
            comis_flags_comisd, 1, "COMISD", &checks);
        compare_fail += run_comis_flag_probe(
            comis_flags_ucomisd, 1, "UCOMISD", &checks);
        fail += compare_fail;
        printf("flags   COMIS*              : 16 outcomes, %d mismatches\n",
               compare_fail);
    }

    /* The exact live failure: fourteen distinct records with the same key.
     * Native x86 advances the low partition index; the old PF model swapped
     * equal records forever. build_test.py runs this executable as a child
     * with a hard timeout, so a regression fails instead of hanging CI. */
    {
        int sort_fail = run_character_equal_sort(&checks);
        fail += sort_fail;
        printf("sort    004a1f20 equal keys : 14 records, %d mismatches\n",
               sort_fail);
    }

    /* ---- x87: explicit binary64 -> extended80 + real _ftol2 root ------ */
    {
        static const struct {
            uint64_t bits, significand;
            uint16_t sign_exponent;
        } encodings[] = {
            { UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), 0x0000 },
            { UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000000), 0x8000 },
            { UINT64_C(0x3ff0000000000000), UINT64_C(0x8000000000000000), 0x3fff },
            { UINT64_C(0xbff0000000000000), UINT64_C(0x8000000000000000), 0xbfff },
            { UINT64_C(0x3ff8000000000000), UINT64_C(0xc000000000000000), 0x3fff },
            { UINT64_C(0x0010000000000000), UINT64_C(0x8000000000000000), 0x3c01 },
            { UINT64_C(0x0000000000000001), UINT64_C(0x8000000000000000), 0x3bcd },
            { UINT64_C(0x7fefffffffffffff), UINT64_C(0xfffffffffffff800), 0x43fe },
            { UINT64_C(0x7ff0000000000000), UINT64_C(0x8000000000000000), 0x7fff },
            { UINT64_C(0xfff0000000000000), UINT64_C(0x8000000000000000), 0xffff },
            { UINT64_C(0x7ff8000000001234), UINT64_C(0xc00000000091a000), 0x7fff },
            { UINT64_C(0x7ff0000000001234), UINT64_C(0x800000000091a000), 0x7fff },
        };
        static const struct { uint64_t bits; uint32_t result; } fallback[] = {
            { UINT64_C(0x0000000000000000), 0x00000000U }, /* +0 */
            { UINT64_C(0x8000000000000000), 0x00000000U }, /* -0 */
            { UINT64_C(0x3fe8000000000000), 0x00000000U }, /* +0.75 */
            { UINT64_C(0xbfe8000000000000), 0x00000000U }, /* -0.75 */
            { UINT64_C(0x3ff8000000000000), 0x00000001U }, /* +1.5 */
            { UINT64_C(0xbff8000000000000), 0xffffffffU }, /* -1.5 */
            { UINT64_C(0x41dfffffffc00000), 0x7fffffffU }, /* INT_MAX */
            { UINT64_C(0xc1e0000000000000), 0x80000000U }, /* INT_MIN */
            { UINT64_C(0x41e0000000000000), 0x80000000U }, /* +overflow */
            { UINT64_C(0x7ff0000000000000), 0x80000000U }, /* +inf */
            { UINT64_C(0xfff0000000000000), 0x80000000U }, /* -inf */
            { UINT64_C(0x7ff8000000001234), 0x80000000U }, /* qNaN */
        };
        static const struct { uint64_t bits; uint32_t result; } fast[] = {
            { UINT64_C(0x0000000000000000), 0x00000000U },
            { UINT64_C(0x8000000000000000), 0x00000000U },
            { UINT64_C(0x3fe8000000000000), 0x00000000U },
            { UINT64_C(0xbfe8000000000000), 0x00000000U },
            { UINT64_C(0x3ff8000000000000), 0x00000001U },
            { UINT64_C(0xbff8000000000000), 0xffffffffU },
            { UINT64_C(0x41dfffffffc00000), 0x7fffffffU },
            { UINT64_C(0xc1e0000000000000), 0x80000000U },
        };
        uint32_t saved_isa = ld32(VA_ISA_AVAILABLE);
        int vi, bi, bad_stack = 0, bad_x87 = 0, x87_fail = 0;

        for (vi = 0; vi < (int)(sizeof encodings / sizeof encodings[0]); ++vi) {
            uint8_t expected[10];
            unsigned j;
            for (j = 0; j < 8; ++j)
                expected[j] = (uint8_t)(encodings[vi].significand >> (j * 8));
            expected[8] = (uint8_t)encodings[vi].sign_exponent;
            expected[9] = (uint8_t)(encodings[vi].sign_exponent >> 8);
            memset(f80_storage, 0xa5, sizeof f80_storage);
            st80d((uint32_t)(uintptr_t)&f80_storage[1],
                  double_from_bits(encodings[vi].bits));
            checks += 2;
            if (memcmp(&f80_storage[1], expected, sizeof expected) != 0)
                x87_fail++;
            if (f80_storage[0] != 0xa5 || f80_storage[11] != 0xa5)
                x87_fail++;
        }

        for (bi = 0; bi < (int)(sizeof fallback / sizeof fallback[0]); ++bi) {
            int stack0 = bad_stack, x870 = bad_x87;
            uint32_t got = call_ftol(double_from_bits(fallback[bi].bits), 0,
                                     &bad_stack, &bad_x87);
            checks += 3;
            if (got != fallback[bi].result || bad_stack != stack0 ||
                    bad_x87 != x870)
                x87_fail++;
        }
        for (bi = 0; bi < (int)(sizeof fast / sizeof fast[0]); ++bi) {
            int stack0 = bad_stack, x870 = bad_x87;
            uint32_t got = call_ftol(double_from_bits(fast[bi].bits), 2,
                                     &bad_stack, &bad_x87);
            checks += 3;
            if (got != fast[bi].result || bad_stack != stack0 ||
                    bad_x87 != x870)
                x87_fail++;
        }
        st32(VA_ISA_AVAILABLE, saved_isa);
        fail += x87_fail;
        printf("x87     m80/_ftol2       : %d encodings, %d conversions,"
               " %d mismatches  (stack %d, x87-top %d)\n",
               (int)(sizeof encodings / sizeof encodings[0]),
               (int)(sizeof fallback / sizeof fallback[0]) +
               (int)(sizeof fast / sizeof fast[0]), x87_fail,
               bad_stack, bad_x87);
    }

    /* ---- register FSTP: old-relative destination, then one pop -------- */
    {
        static void (*const probes[8])(CPU *__restrict) = {
            x87_fstp_st0, x87_fstp_st1, x87_fstp_st2, x87_fstp_st3,
            x87_fstp_st4, x87_fstp_st5, x87_fstp_st6, x87_fstp_st7,
        };
        double saved_st[8], before[8];
        int saved_top = cpu.st_top;
        int index, physical, fstp_checks = 0, fstp_fail = 0;

        memcpy(saved_st, cpu.st, sizeof saved_st);
        for (index = 0; index < 8; index++) {
            double source = -123.5 - (double)index;
            double old_next;
            int destination = (7 + index) & 7;

            cpu.st_top = 7;       /* forces ST(i) physical-index wrapping */
            for (physical = 0; physical < 8; physical++)
                cpu.st[physical] = 1000.0 + (double)(index * 10 + physical);
            cpu.st[7] = source;
            memcpy(before, cpu.st, sizeof before);
            old_next = before[0];

            probes[index](&cpu);
            checks++; fstp_checks++;
            if (cpu.st_top != 0) fstp_fail++;
            for (physical = 0; physical < 8; physical++) {
                double expected = (index > 0 && physical == destination)
                                  ? source : before[physical];
                checks++; fstp_checks++;
                if (cpu.st[physical] != expected) fstp_fail++;
            }
            checks++; fstp_checks++;
            if (*fst(&cpu, 0) != (index == 1 ? source : old_next))
                fstp_fail++;
        }
        memcpy(cpu.st, saved_st, sizeof saved_st);
        cpu.st_top = saved_top;
        fail += fstp_fail;
        printf("x87     FSTP ST(i)       : %d checks, %d mismatches"
               "  (all 8 destinations, wrapped top)\n",
               fstp_checks, fstp_fail);
    }

    /* ---- packed integer SSE4.1: actual emitted instruction bodies ----- */
    {
        static const uint32_t reg_a[4] = {
            0x00000000U, 0x00000001U, 0xffffffffU, 0x80000000U
        };
        static const uint32_t reg_b[4] = {
            0xffffffffU, 0xffffffffU, 0x00000002U, 0x00000003U
        };
        static const uint32_t reg_product[4] = {
            0x00000000U, 0xffffffffU, 0xfffffffeU, 0x80000000U
        };
        static const uint32_t mem_a[4] = {
            0x7fffffffU, 0x80000000U, 0xffffffffU, 0x12345678U
        };
        static const uint32_t mem_b[4] = {
            0x00000002U, 0x00000003U, 0x80000000U, 0x9abcdef0U
        };
        static const uint32_t mem_product[4] = {
            0xfffffffeU, 0x80000000U, 0x80000000U, 0x242d2080U
        };
        static const uint32_t self_product[4] = {
            0x00000001U, 0x00000000U, 0x00000001U, 0x1df4d840U
        };
        static const uint32_t cmp_a[2][4] = {
            { 0x80000000U, 0xffffffffU, 0x00000000U, 0x7fffffffU },
            { 0x80000000U, 0x7fffffffU, 0xffffffffU, 0x00000001U },
        };
        static const uint32_t cmp_b[2][4] = {
            { 0x7fffffffU, 0xfffffffeU, 0x00000000U, 0x80000000U },
            { 0x80000000U, 0x7fffffffU, 0x00000000U, 0xffffffffU },
        };
        static const uint32_t cmp_mask[2][4] = {
            { 0x00000000U, 0xffffffffU, 0x00000000U, 0xffffffffU },
            { 0x00000000U, 0x00000000U, 0x00000000U, 0xffffffffU },
        };
        xmm_t saved_multipliers = ldx(VA_SIMD_MULTIPLIERS);
        xmm_t memory_operand;
        int lane, vector, simd_checks = 0, simd_fail = 0;

        for (lane = 0; lane < 4; lane++) {
            cpu.x[3].u32[lane] = reg_a[lane];
            cpu.x[6].u32[lane] = reg_b[lane];
        }
        simd_pmulld_reg(&cpu);
        for (lane = 0; lane < 4; lane++) {
            checks++; simd_checks++;
            if (cpu.x[3].u32[lane] != reg_product[lane]) simd_fail++;
        }

        for (lane = 0; lane < 4; lane++) {
            cpu.x[1].u32[lane] = mem_a[lane];
            memory_operand.u32[lane] = mem_b[lane];
        }
        stx(VA_SIMD_MULTIPLIERS, memory_operand);
        simd_pmulld_mem(&cpu);
        stx(VA_SIMD_MULTIPLIERS, saved_multipliers);
        for (lane = 0; lane < 4; lane++) {
            checks++; simd_checks++;
            if (cpu.x[1].u32[lane] != mem_product[lane]) simd_fail++;
        }

        for (lane = 0; lane < 4; lane++) cpu.x[1].u32[lane] = mem_a[lane];
        simd_pmulld_self(&cpu);
        for (lane = 0; lane < 4; lane++) {
            checks++; simd_checks++;
            if (cpu.x[1].u32[lane] != self_product[lane]) simd_fail++;
        }

        for (vector = 0; vector < 2; vector++) {
            for (lane = 0; lane < 4; lane++) {
                cpu.x[0].u32[lane] = cmp_a[vector][lane];
                cpu.x[1].u32[lane] = cmp_b[vector][lane];
            }
            simd_pcmpgtd_reg(&cpu);
            for (lane = 0; lane < 4; lane++) {
                checks++; simd_checks++;
                if (cpu.x[0].u32[lane] != cmp_mask[vector][lane]) simd_fail++;
            }
        }
        fail += simd_fail;
        printf("simd    PMULLD/PCMPGTD  : %d checks, %d mismatches"
               "  (reg/self/m128 + signed masks)\n",
               simd_checks, simd_fail);
    }

    /* ---- libpng packed-integer SIMD: generated from exact game bytes -- */
    {
        static const uint16_t shuffle_input[8] = {
            0x0000U, 0x1111U, 0x2222U, 0x3333U,
            0x4444U, 0x5555U, 0x6666U, 0x7777U
        };
        static const uint16_t shuffle_low[8] = {
            0x0000U, 0x2222U, 0x1111U, 0x3333U,
            0x4444U, 0x5555U, 0x6666U, 0x7777U
        };
        static const uint16_t shuffle_high[8] = {
            0x0000U, 0x1111U, 0x2222U, 0x3333U,
            0x4444U, 0x6666U, 0x5555U, 0x7777U
        };
        static const int16_t pack_input[8] = {
            INT16_MIN, -1, 0, 1, 254, 255, 256, INT16_MAX
        };
        static const uint8_t pack_expected[16] = {
            0, 0, 0, 1, 254, 255, 255, 255,
            0, 0, 0, 1, 254, 255, 255, 255
        };
        static const uint32_t pandn_reg_d[4] = {
            0x00000000U, 0xffffffffU, 0xaaaaaaaaU, 0x12345678U
        };
        static const uint32_t pandn_reg_s[4] = {
            0xffffffffU, 0xffffffffU, 0x55555555U, 0xf0f0f0f0U
        };
        static const uint32_t pandn_reg_expected[4] = {
            0xffffffffU, 0x00000000U, 0x55555555U, 0xe0c0a080U
        };
        static const uint32_t pandn_mem_d[4] = {
            0x0f0f0f0fU, 0xf0f0f0f0U, 0xaaaaaaaaU, 0x55555555U
        };
        static const uint32_t pandn_mem_s[4] = {
            0xffffffffU, 0xffffffffU, 0x0f0f0f0fU, 0xf0f0f0f0U
        };
        static const uint32_t pandn_mem_expected[4] = {
            0xf0f0f0f0U, 0x0f0f0f0fU, 0x05050505U, 0xa0a0a0a0U
        };
        xmm_t saved_pandn_source = ldx(VA_PANDN_SOURCE);
        xmm_t memory_operand;
        int lane, png_simd_checks = 0, png_simd_fail = 0;

        for (lane = 0; lane < 8; lane++)
            cpu.x[0].u16[lane] = shuffle_input[lane];
        simd_pshuflw_self(&cpu);
        for (lane = 0; lane < 8; lane++) {
            checks++; png_simd_checks++;
            if (cpu.x[0].u16[lane] != shuffle_low[lane]) png_simd_fail++;
        }

        for (lane = 0; lane < 8; lane++)
            cpu.x[0].u16[lane] = shuffle_input[lane];
        simd_pshufhw_self(&cpu);
        for (lane = 0; lane < 8; lane++) {
            checks++; png_simd_checks++;
            if (cpu.x[0].u16[lane] != shuffle_high[lane]) png_simd_fail++;
        }

        for (lane = 0; lane < 8; lane++)
            cpu.x[1].i16[lane] = pack_input[lane];
        simd_packuswb_self(&cpu);
        for (lane = 0; lane < 16; lane++) {
            checks++; png_simd_checks++;
            if (cpu.x[1].u8[lane] != pack_expected[lane]) png_simd_fail++;
        }

        for (lane = 0; lane < 4; lane++) {
            cpu.x[0].u32[lane] = pandn_reg_d[lane];
            cpu.x[1].u32[lane] = pandn_reg_s[lane];
        }
        simd_pandn_reg(&cpu);
        for (lane = 0; lane < 4; lane++) {
            checks++; png_simd_checks++;
            if (cpu.x[0].u32[lane] != pandn_reg_expected[lane])
                png_simd_fail++;
        }

        for (lane = 0; lane < 4; lane++) {
            cpu.x[3].u32[lane] = pandn_mem_d[lane];
            memory_operand.u32[lane] = pandn_mem_s[lane];
        }
        stx(VA_PANDN_SOURCE, memory_operand);
        simd_pandn_mem(&cpu);
        stx(VA_PANDN_SOURCE, saved_pandn_source);
        for (lane = 0; lane < 4; lane++) {
            checks++; png_simd_checks++;
            if (cpu.x[3].u32[lane] != pandn_mem_expected[lane])
                png_simd_fail++;
        }

        fail += png_simd_fail;
        printf("simd    PNG packed ops   : %d checks, %d mismatches"
               "  (shuffle/pack/PANDN reg+m128)\n",
               png_simd_checks, png_simd_fail);
    }

    /* ---- LOCK CMPXCHG8B: helper branches + real fetch-add loop -------- */
    {
        static const struct {
            uint64_t initial, delta, old, final;
        } vectors[] = {
            { UINT64_C(0x1122334455667788), UINT64_C(0),
              UINT64_C(0x1122334455667788), UINT64_C(0x1122334455667788) },
            { UINT64_C(0x1122334455667788), UINT64_C(1),
              UINT64_C(0x1122334455667788), UINT64_C(0x1122334455667789) },
            { UINT64_C(0x00000000ffffffff), UINT64_C(1),
              UINT64_C(0x00000000ffffffff), UINT64_C(0x0000000100000000) },
            { UINT64_C(5), UINT64_C(0xfffffffffffffff9),
              UINT64_C(5), UINT64_C(0xfffffffffffffffe) },
            { UINT64_C(0xffffffffffffffff), UINT64_C(1),
              UINT64_C(0xffffffffffffffff), UINT64_C(0) },
        };
        const uint64_t a = UINT64_C(0x1122334455667788);
        const uint64_t b = UINT64_C(0x8877665544332211);
        const uint64_t d = UINT64_C(0x0102030405060708);
        uint64_t observed, old;
        int vi, bad_stack = 0, atomic_fail = 0;

        checks++;
        if (((uintptr_t)&atomic_word & 7U) != 0) atomic_fail++;
        atomic_word = a;
        observed = guest_atomic_cmpxchg64(
            (uint32_t)(uintptr_t)&atomic_word, a, b);
        checks += 2;
        if (observed != a || atomic_word != b) atomic_fail++;
        observed = guest_atomic_cmpxchg64(
            (uint32_t)(uintptr_t)&atomic_word, a, d);
        checks += 2;
        if (observed != b || atomic_word != b) atomic_fail++;

        for (vi = 0; vi < (int)(sizeof vectors / sizeof vectors[0]); vi++) {
            int stack0 = bad_stack;
            atomic_word = vectors[vi].initial;
            old = call_atomic_add64(vectors[vi].delta, &bad_stack);
            checks += 3;
            if (old != vectors[vi].old ||
                    atomic_word != vectors[vi].final || bad_stack != stack0)
                atomic_fail++;
        }
        fail += atomic_fail;
        printf("atomic  CMPXCHG8B       : %d checks, %d mismatches"
               "  (cdecl stack errors %d)\n",
               5 + 3 * (int)(sizeof vectors / sizeof vectors[0]),
               atomic_fail, bad_stack);
    }

    /* ---- rung 1: RNG::Next, all 81 Marsaglia triples x 8 draws --------- */
    for (idx = 0; idx < 81; idx++) {
        RNG got, ref;
        got.seed = ref.seed = 1u;
        got.s1 = ref.s1 = shifts[idx * 3 + 0];
        got.s2 = ref.s2 = shifts[idx * 3 + 1];
        got.s3 = ref.s3 = shifts[idx * 3 + 2];
        for (k = 0; k < 8; k++) {
            uint32_t want = ref_next(&ref);
            call_this(rng_next, (uint32_t)(uintptr_t)&got, NULL, 0);
            checks++;
            if (cpu.eax != want || got.seed != ref.seed) {
                if (fail < 8)
                    printf("  MISMATCH Next idx=%d k=%d got=%08X want=%08X\n",
                           idx, k, cpu.eax, want);
                fail++;
            }
        }
    }
    printf("rung 1  RNG::Next           : %d checks, %d mismatches\n",
           checks, fail);

    /* ---- 200,000 draws on the default seed, compared as a stream ------- */
    {
        RNG got, ref;
        got.seed = ref.seed = 0xAA17414Fu;
        got.s1 = ref.s1 = shifts[35 * 3 + 0];
        got.s2 = ref.s2 = shifts[35 * 3 + 1];
        got.s3 = ref.s3 = shifts[35 * 3 + 2];
        h_ref = h_got = 2166136261u;
        for (k = 0; k < 200000; k++) {
            h_ref = fnv(h_ref, ref_next(&ref));
            call_this(rng_next, (uint32_t)(uintptr_t)&got, NULL, 0);
            h_got = fnv(h_got, cpu.eax);
        }
        checks += 200000;
        printf("        200,000-draw FNV    : ref %08X  recompiled %08X  %s\n",
               h_ref, h_got, h_ref == h_got ? "equal" : "DIFFER");
        if (h_ref != h_got) fail++;
    }

    /* ---- the zero-seed guard: prove the branch ran -------------------- */
    {
        RNG got;
        unsigned log0 = g_log_calls, i3 = g_int3_count;
        got.seed = 0u; got.s1 = 5; got.s2 = 9; got.s3 = 7;
        call_this(rng_next, (uint32_t)(uintptr_t)&got, NULL, 0);
        printf("        zero-seed guard     : log stub +%u, int3 stub +%u"
               "  (both must be 1)\n",
               g_log_calls - log0, g_int3_count - i3);
        if (g_log_calls - log0 != 1 || g_int3_count - i3 != 1) fail++;
    }

    /* ---- rung 2: RandomFloat, compared as RAW BITS -------------------- */
    {
        int c2 = 0, f2 = 0;
        for (idx = 0; idx < 81; idx++) {
            RNG got, ref;
            got.seed = ref.seed = 1u;
            got.s1 = ref.s1 = shifts[idx * 3 + 0];
            got.s2 = ref.s2 = shifts[idx * 3 + 1];
            got.s3 = ref.s3 = shifts[idx * 3 + 2];
            for (k = 0; k < 8; k++) {
                float want = ref_float(&ref), have;
                uint32_t wb, hb;
                call_this(rng_randomfloat, (uint32_t)(uintptr_t)&got, NULL, 0);
                have = (float)*fst(&cpu, 0);
                memcpy(&wb, &want, 4); memcpy(&hb, &have, 4);
                c2++;
                if (wb != hb) {
                    if (f2 < 8)
                        printf("  MISMATCH Float idx=%d k=%d got=%08X want=%08X\n",
                               idx, k, hb, wb);
                    f2++;
                }
            }
        }
        /* deliberately hit the top half of the range: the unsigned convert
         * goes through a table indexed by the sign bit, and a naive signed
         * conversion is wrong for exactly these values. */
        {
            RNG got, ref;
            got.seed = ref.seed = 0x80000001u;
            got.s1 = ref.s1 = shifts[35 * 3 + 0];
            got.s2 = ref.s2 = shifts[35 * 3 + 1];
            got.s3 = ref.s3 = shifts[35 * 3 + 2];
            for (k = 0; k < 4096; k++) {
                float want, have; uint32_t wb, hb;
                if (ref.seed & 0x80000000u) signbit_hits++;
                want = ref_float(&ref);
                call_this(rng_randomfloat, (uint32_t)(uintptr_t)&got, NULL, 0);
                have = (float)*fst(&cpu, 0);
                memcpy(&wb, &want, 4); memcpy(&hb, &have, 4);
                c2++;
                if (wb != hb) f2++;
            }
        }
        checks += c2; fail += f2;
        printf("rung 2  RNG::RandomFloat    : %d checks, %d mismatches"
               "  (%d draws with the sign bit set)\n", c2, f2, signbit_hits);
    }

    /* ---- RandomInt: a stack argument and a modulo --------------------- */
    {
        int c3 = 0, f3 = 0;
        static const uint32_t maxes[] = {1, 2, 3, 6, 7, 100, 1000, 65535,
                                         0x7FFFFFFFu, 0xFFFFFFFFu, 0};
        int mi;
        for (mi = 0; maxes[mi] || mi == 10; mi++) {
            RNG got, ref;
            got.seed = ref.seed = 12345u;
            got.s1 = ref.s1 = shifts[35 * 3 + 0];
            got.s2 = ref.s2 = shifts[35 * 3 + 1];
            got.s3 = ref.s3 = shifts[35 * 3 + 2];
            for (k = 0; k < 64; k++) {
                uint32_t arg = maxes[mi];
                uint32_t want = ref_int(&ref, arg);
                call_this(rng_randomint, (uint32_t)(uintptr_t)&got, &arg, 1);
                c3++;
                if (cpu.eax != want || got.seed != ref.seed) {
                    if (f3 < 8)
                        printf("  MISMATCH Int max=%u k=%d got=%u want=%u"
                               " (seed %08X vs %08X)\n",
                               arg, k, cpu.eax, want, got.seed, ref.seed);
                    f3++;
                }
            }
            if (mi == 10) break;
        }
        checks += c3; fail += f3;
        printf("        RNG::RandomInt      : %d checks, %d mismatches\n", c3, f3);
    }

    /* ---- rung 3: WeightedOutcomePicker::PickOutcome ------------------- */
    /* The rung that stopped the hand-grown translator: unsigned compares
     * needing CF, an auto-vectorised packed-SSE weight sum consuming eight
     * elements per iteration with a horizontal reduction, and nested loops
     * with remainder handling.
     *
     * The object is a single MSVC std::vector<Outcome> at +0x00, i.e. three
     * pointers {first, last, end}; Outcome is {int value; int weight;}. The
     * RNG is passed BY VALUE -- `ret 0x10` -- so the caller's generator must
     * NOT advance, and that is checked explicitly below. */
    {
        int c4 = 0, f4 = 0, counts[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31,
                                        32, 33, 40, 64, 100};
        int ci, no_adv_fail = 0, zero_weight_cases = 0;
        unsigned log_before = g_log_calls;
        static Outcome pool[128];
        static uint32_t vec[3];

        for (ci = 0; ci < (int)(sizeof counts / sizeof counts[0]); ci++) {
            int n = counts[ci], t;
            for (t = 0; t < 24; t++) {
                RNG caller, tmp;
                uint32_t args[4];
                int i, total = 0, cum = 0, want = 0;
                int32_t roll;
                uint32_t seed_before;

                /* A deterministic but varied pool. Every sixth iteration is
                 * ALL-ZERO with n > 0, which is the only way to reach the
                 * "No weights were added" path with a non-empty vector --
                 * at n == 0 both sides answer 0 trivially and the check
                 * would be green without proving anything. */
                for (i = 0; i < n; i++) {
                    pool[i].value  = 1000 + i;
                    pool[i].weight = (t % 6 == 5) ? 0 : ((i * 7 + t * 13) % 11);
                }
                if (n > 0 && t % 6 == 5) zero_weight_cases++;
                vec[0] = (uint32_t)(uintptr_t)pool;
                vec[1] = (uint32_t)(uintptr_t)(pool + n);
                vec[2] = (uint32_t)(uintptr_t)(pool + 128);

                caller.seed = 0x1234567u + (uint32_t)(ci * 977 + t);
                caller.s1 = shifts[35 * 3 + 0];
                caller.s2 = shifts[35 * 3 + 1];
                caller.s3 = shifts[35 * 3 + 2];
                seed_before = caller.seed;

                /* reference, on a copy -- by value */
                tmp = caller;
                for (i = 0; i < n; i++) total += pool[i].weight;
                roll = (int32_t)ref_int(&tmp, (uint32_t)total);
                want = 0;
                for (i = 0; i < n; i++) {
                    cum += pool[i].weight;
                    if (roll < cum) { want = pool[i].value; break; }
                }

                args[0] = caller.seed; args[1] = caller.s1;
                args[2] = caller.s2;   args[3] = caller.s3;
                call_this(sub_00089420, (uint32_t)(uintptr_t)vec, args, 4);
                c4++;
                if (cpu.eax != (uint32_t)want) {
                    if (f4 < 8)
                        printf("  MISMATCH Pick n=%d t=%d got=%u want=%d"
                               " (total=%d roll=%d)\n",
                               n, t, cpu.eax, want, total, roll);
                    f4++;
                }
                if (caller.seed != seed_before) no_adv_fail++;
            }
        }
        checks += c4; fail += f4 + no_adv_fail;
        printf("rung 3  PickOutcome         : %d checks, %d mismatches\n",
               c4, f4);
        printf("        RNG passed by value : caller's seed moved %d times"
               "  (must be 0)\n", no_adv_fail);
        printf("        no-weight path      : %d non-empty all-zero pools,"
               " log stub called %u times\n",
               zero_weight_cases, g_log_calls - log_before);
        if (zero_weight_cases == 0) {
            printf("        the no-weight path was never entered -- "
                   "this run does not test it\n");
            fail++;
        }
    }

    printf("\nTOTAL                       : %d checks, %d mismatches\n",
           checks, fail);
    printf("guest faults                : %u   (must be 0)\n", g_fault_count);
    printf("VERDICT                     : %s\n",
           (fail == 0 && g_fault_count == 0) ? "PASS" : "FAIL");
    return (fail == 0 && g_fault_count == 0) ? 0 : 1;
}
