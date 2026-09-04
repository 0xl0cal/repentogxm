/* How much slower is recompiled code than native code?
 *
 * Nobody on this project has measured this.  Every statement about the 444 MHz
 * target so far has been static -- 4.68 ARM instructions per guest x86
 * instruction, 13.31 bytes of .text -- and the journals say plainly that a
 * static count is a lower bound, not a prediction.
 *
 * What makes a measurement possible now is the oracle ladder: for RNG::Next we
 * have the same algorithm twice in one process, once as recompiled C and once
 * as a native reference, and they are already proven to produce identical
 * output over 200,000 draws.  Timing both is therefore comparing two
 * implementations of the same work, not two different programs.
 *
 * WHAT THIS MEASURES
 *   The per-call cost of one small, integer-only, branch-light translated
 *   function against the same algorithm written in C, on an x86 host, at a
 *   stated optimisation level.
 *
 * WHAT IT DOES NOT MEASURE, and these matter more than the number
 *   * ARM.  A different register count, no partial-register writes, and a
 *     different compiler.  This is not a Vita prediction.
 *   * Cache.  A 28-instruction function in a hot loop is the best case; the
 *     real program is 51.8 MB of translated code with terrible locality.
 *   * Dispatch.  Real calls go through guest_lookup's binary search over
 *     14,814 entries on the ~10% of calls that are indirect.  Not exercised.
 *   * Flags.  This function emits ZERO flag stores and answers both its
 *     conditions directly.  Functions that cannot pair will be worse.
 *   * x87/SSE.  Integer only here.
 *
 * So: a lower bound on translated-code speed for the friendliest possible
 * function.  Reported as such.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "guest.h"

#ifndef EXE_PATH
#define EXE_PATH "isaac-ng.exe.unpacked.exe"
#endif
#define RNG_NEXT 0x003a7e20u

void sub_003a7e20(CPU *__restrict c);

/* COPIED from ladder_test.c, not reinvented: KAGE's logger, the one callee
 * this function has.  It must pop the return address our translated `call`
 * pushed, or the guest stack drifts by four bytes per invocation. */
unsigned g_log_calls;
void sub_0055e330(CPU *__restrict c)
{
    g_log_calls++;
    (void)gpop(c);
}

/* guest.c's dispatcher offers every indirect call to the host import layer.
 * This benchmark links no import layer and makes no indirect calls, so the
 * honest answer is "not handled" -- and if that ever becomes false, the
 * unhandled path faults loudly in guest_call rather than silently here. */
unsigned g_host_import_calls;
int guest_host_import(CPU *__restrict c, const char *name)
{
    (void)c; (void)name;
    return 0;
}

/* COPIED from ladder_test.c.  The shift amounts live in the OBJECT, and the
 * order is shr, shl, shr -- writing this from memory once already produced a
 * "translator bug" that was this reference being wrong. */
typedef struct { uint32_t seed, s1, s2, s3; } RNG;

static uint32_t ref_next(RNG *r)
{
    uint32_t s = r->seed;
    s ^= s >> (r->s1 & 31);
    s ^= s << (r->s2 & 31);
    s ^= s >> (r->s3 & 31);
    return r->seed = s;
}

static CPU cpu;

static double now_seconds(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
    /* The RNG object lives in host memory; guest address == host address in
     * this backend, so the translated code can read its shift amounts. */
    static RNG native_obj, guest_obj;
    unsigned long long i, n = 20000000ULL;
    double t0, t_native, t_recomp;
    uint32_t h_native = 2166136261u, h_recomp = 2166136261u;
    uint32_t saved_esp;

    if (argc > 1) n = strtoull(argv[1], NULL, 10);

    printf("=== recompiled vs native, same algorithm ===\n");
    printf("iterations                  : %llu\n", n);

    if (guest_image_load(EXE_PATH)) return 2;
    if (guest_stack_init(&cpu)) return 2;

    native_obj.seed = 12345u; native_obj.s1 = 13; native_obj.s2 = 17;
    native_obj.s3 = 5;
    guest_obj = native_obj;

    /* Interleave and repeat, then take the minimum of each.
     *
     * The first version ran native once, then recompiled once.  Its native
     * figure came out at 6.94 ns in one process and 2.11 ns in another, from
     * identical /O2 code -- clock ramp on a cold CPU, measured as if it were a
     * property of the code.  The ratio built on it would have been wrong by
     * 3x.  Minimum-of-repeats is the standard answer: noise only ever adds
     * time, so the smallest observation is the closest to the real cost. */
    {
        const int rounds = 5;
        int round;
        double best_native = 1e18, best_recomp = 1e18;

        saved_esp = cpu.esp;
        for (round = 0; round < rounds; ++round) {
            double dt;

            h_native = 2166136261u;
            native_obj.seed = 12345u;
            t0 = now_seconds();
            for (i = 0; i < n; ++i)
                h_native = (h_native ^ ref_next(&native_obj)) * 16777619u;
            dt = now_seconds() - t0;
            if (dt < best_native) best_native = dt;

            /* thiscall: object in ECX, one pushed return address that the
             * translated RET consumes.  ESP is restored outside the loop so
             * the per-iteration cost is the call, not stack bookkeeping. */
            h_recomp = 2166136261u;
            guest_obj.seed = 12345u;
            t0 = now_seconds();
            for (i = 0; i < n; ++i) {
                cpu.ecx = (uint32_t)(uintptr_t)&guest_obj;
                gpush(&cpu, 0xDEADBEEFu);
                sub_003a7e20(&cpu);
                h_recomp = (h_recomp ^ cpu.eax) * 16777619u;
            }
            dt = now_seconds() - t0;
            if (dt < best_recomp) best_recomp = dt;
            cpu.esp = saved_esp;

            printf("   round %d: native %6.3f s   recompiled %6.3f s\n",
                   round + 1, best_native, best_recomp);
        }
        t_native = best_native;
        t_recomp = best_recomp;
    }

    printf("native   %8.3f s   %7.2f ns/call\n",
           t_native, t_native * 1e9 / (double)n);
    printf("recompiled %6.3f s   %7.2f ns/call\n",
           t_recomp, t_recomp * 1e9 / (double)n);
    printf("slowdown factor             : %.2fx\n",
           t_native > 0.0 ? t_recomp / t_native : 0.0);

    /* The hashes must agree, or one of the two loops was optimised away or is
     * computing something else -- either way the timing would be a lie. */
    printf("hash native/recompiled      : %08X / %08X  %s\n",
           h_native, h_recomp,
           h_native == h_recomp ? "EQUAL" : "DIFFERENT -- timing is invalid");
    printf("logger calls (should be 0)  : %u\n", g_log_calls);

    return h_native == h_recomp ? 0 : 1;
}
