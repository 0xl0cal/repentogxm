/* Representative host timing for the generated stack hot path.
 *
 * The raw loops are the exact pre-guard semantics.  The checked loops use
 * gpush_generated / guest_stack_address_generated / gpop_generated over the
 * same CPU and bytes.  A
 * volatile read prevents the compiler from deleting the synthetic-stack
 * memory traffic while keeping identical work on both sides of the ratio.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "guest.h"

#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

enum {
    BENCH_STACK_WORDS = 1026,
    BENCH_TRIALS = 7
};

static uint32_t s_stack[BENCH_STACK_WORDS];
static volatile uint32_t s_sink;

int guest_host_import(CPU *__restrict c, const char *name)
{
    (void)c;
    (void)name;
    return 0;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static BENCH_NOINLINE uint32_t run_raw_mix(CPU *__restrict c,
                                           uint32_t iterations)
{
    uint32_t i, value = 0U;
    for (i = 0U; i < iterations; ++i) {
        c->esp -= 4U;
        st32(c->esp, i);
        value ^= *(volatile uint32_t *)(uintptr_t)c->esp;
        value += ld32(c->esp);
        c->esp += 4U;
    }
    return value;
}

static BENCH_NOINLINE uint32_t run_checked_mix(CPU *__restrict c,
                                               uint32_t iterations)
{
    uint32_t i, value = 0U;
    for (i = 0U; i < iterations; ++i) {
        gpush_generated(c, i);
        GUEST_STACK_CALLSITE_BARRIER();
        value ^= *(volatile uint32_t *)(uintptr_t)
            guest_stack_address_generated(c, c->esp, 4U);
        value += gpop_generated(c);
    }
    return value;
}

static BENCH_NOINLINE uint32_t run_raw_address(CPU *__restrict c,
                                               uint32_t iterations)
{
    uint32_t i, value = 0U;
    const uint32_t address = c->esp - 4U;
    for (i = 0U; i < iterations; ++i)
        value += *(volatile uint32_t *)(uintptr_t)address;
    return value;
}

static BENCH_NOINLINE uint32_t run_checked_address(CPU *__restrict c,
                                                   uint32_t iterations)
{
    uint32_t i, value = 0U;
    const uint32_t address = c->esp - 4U;
    for (i = 0U; i < iterations; ++i)
        value += *(volatile uint32_t *)(uintptr_t)
            guest_stack_address_generated(c, address, 4U);
    return value;
}

typedef uint32_t (*bench_fn)(CPU *__restrict, uint32_t);

static double one_trial(bench_fn function, CPU *c, uint32_t iterations,
                        uint32_t ceiling)
{
    LARGE_INTEGER begin, end, frequency;
    c->esp = ceiling;
    c->stack_low_water = ceiling;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    s_sink ^= function(c, iterations);
    QueryPerformanceCounter(&end);
    return ((double)(end.QuadPart - begin.QuadPart) * 1.0e9) /
           ((double)frequency.QuadPart * (double)iterations);
}

static int compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void measure_pair(const char *name, bench_fn raw, bench_fn checked,
                         CPU *c, uint32_t iterations, uint32_t ceiling)
{
    double raw_times[BENCH_TRIALS], checked_times[BENCH_TRIALS];
    unsigned trial;
    s_sink ^= raw(c, 10000U);
    s_sink ^= checked(c, 10000U);
    for (trial = 0U; trial < BENCH_TRIALS; ++trial) {
        if (trial & 1U) {
            checked_times[trial] = one_trial(
                checked, c, iterations, ceiling);
            raw_times[trial] = one_trial(raw, c, iterations, ceiling);
        } else {
            raw_times[trial] = one_trial(raw, c, iterations, ceiling);
            checked_times[trial] = one_trial(
                checked, c, iterations, ceiling);
        }
    }
    qsort(raw_times, BENCH_TRIALS, sizeof raw_times[0], compare_double);
    qsort(checked_times, BENCH_TRIALS, sizeof checked_times[0],
          compare_double);
    printf("guest stack benchmark %s: raw=%.3f ns checked=%.3f ns "
           "ratio=%.3fx\n", name,
           raw_times[BENCH_TRIALS / 2U],
           checked_times[BENCH_TRIALS / 2U],
           checked_times[BENCH_TRIALS / 2U] /
               raw_times[BENCH_TRIALS / 2U]);
}

int main(int argc, char **argv)
{
    CPU c;
    uint32_t iterations = 5000000U;
    const uint32_t floor = pointer32(&s_stack[1]);
    const uint32_t ceiling = pointer32(&s_stack[BENCH_STACK_WORDS - 1]);

    if (argc == 2) {
        unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed == 0UL || parsed > UINT32_MAX)
            return 2;
        iterations = (uint32_t)parsed;
    }
    if (sizeof(void *) != 4U)
        return 2;
    guest_cpu_init(&c);
    if (guest_stack_bind(&c, floor, ceiling) != 0)
        return 2;
    st32(ceiling - 4U, UINT32_C(0x5a17c0de));

    measure_pair("push+address+pop", run_raw_mix, run_checked_mix,
                 &c, iterations, ceiling);
    measure_pair("address", run_raw_address, run_checked_address,
                 &c, iterations, ceiling);
    printf("guest stack benchmark: PASS sink=%08x iterations=%u\n",
           (unsigned)s_sink, (unsigned)iterations);
    return 0;
}
