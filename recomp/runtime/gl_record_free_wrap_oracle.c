/* Host oracle for gl_record_free_wrap.c.
 *
 * The production TU is #included with the oracle defines, so the wrap under
 * test is the text the device compiles.  Fakes for the two calls it makes
 * append events to one log; the checks are:
 *   1. every call is BUMP then REAL, exactly one of each;
 *   2. the uid reaches the real free unchanged;
 *   3. the real free's result is returned unchanged;
 *   4. 1000 calls make 1000 bumps and 1000 frees, interleaved.
 * The uid and result values are arbitrary test inputs, not SCE error codes.
 * Built with -DISAAC_GL_RECORD_FREE_WRAP_ORACLE_MUTATE_LATE_BUMP=1 it must
 * fail check 1 (test_gl_record_free_wrap.py asserts that it does). */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "gl_record_free_wrap.c"

enum { EV_BUMP = 1, EV_REAL = 2 };
#define ORACLE_EVENTS 4096u
static struct { int kind; SceUID uid; } s_ev[ORACLE_EVENTS];
static unsigned s_n, s_overflow;
static int s_real_result;

static void oracle_event(int kind, SceUID uid)
{
    if (s_n < ORACLE_EVENTS) {
        s_ev[s_n].kind = kind;
        s_ev[s_n].uid = uid;
        ++s_n;
    } else {
        ++s_overflow;
    }
}

void isaac_gl_record_memblock_freeing(void) { oracle_event(EV_BUMP, 0); }
int __real_sceKernelFreeMemBlock(SceUID uid)
{
    oracle_event(EV_REAL, uid);
    return s_real_result;
}

static unsigned s_failures, s_checks;
/* Branch coverage of the checks themselves: how many cases took each arm. */
static unsigned s_cov_order_ok, s_cov_order_bad, s_cov_uid, s_cov_result;

static void expect(int ok, const char *what, long long a, long long b)
{
    ++s_checks;
    if (!ok) {
        ++s_failures;
        if (s_failures <= 8u)
            printf("FAIL %s: got %lld want %lld\n", what, a, b);
    }
}

/* One call: the log must read exactly {BUMP, REAL(uid)} and the result pass through. */
static void one_call(SceUID uid, int result)
{
    int got;
    s_n = 0u;
    s_real_result = result;
    got = __wrap_sceKernelFreeMemBlock(uid);
    expect(s_n == 2u, "events per call", (long long)s_n, 2);
    if (s_n == 2u && s_ev[0].kind == EV_BUMP && s_ev[1].kind == EV_REAL) {
        ++s_cov_order_ok;
        expect(1, "order bump-then-real", 1, 1);
    } else {
        ++s_cov_order_bad;
        expect(0, "order bump-then-real", s_n ? s_ev[0].kind : 0, EV_BUMP);
    }
    if (s_n == 2u) {
        const SceUID seen = s_ev[0].kind == EV_REAL ? s_ev[0].uid : s_ev[1].uid;
        ++s_cov_uid;
        expect(seen == uid, "uid passes through", (long long)seen, (long long)uid);
    }
    ++s_cov_result;
    expect(got == result, "result passes through", (long long)got, (long long)result);
}

int main(void)
{
    static const SceUID uids[] = {0, 1, 0x7fffffff, -1};
    static const int results[] = {0, -1, INT_MIN, 1};
    unsigned i, j, bumps = 0u, reals = 0u, alternating = 1u;

    for (i = 0u; i < sizeof uids / sizeof uids[0]; ++i)
        for (j = 0u; j < sizeof results / sizeof results[0]; ++j)
            one_call(uids[i], results[j]);

    /* 1000 calls into one log: one bump per call, each before its free. */
    s_n = 0u;
    s_real_result = 0;
    for (i = 0u; i < 1000u; ++i)
        (void)__wrap_sceKernelFreeMemBlock((SceUID)(i + 2u));
    for (i = 0u; i < s_n; ++i) {
        if (s_ev[i].kind == EV_BUMP) ++bumps;
        if (s_ev[i].kind == EV_REAL) ++reals;
        if (s_ev[i].kind != ((i & 1u) ? EV_REAL : EV_BUMP)) alternating = 0u;
        if (s_ev[i].kind == EV_REAL && s_ev[i].uid != (SceUID)(i / 2u + 2u)) alternating = 0u;
    }
    expect(bumps == 1000u, "bumps over 1000 calls", bumps, 1000);
    expect(reals == 1000u, "frees over 1000 calls", reals, 1000);
    expect(alternating == 1u, "bump/real alternation with uids", alternating, 1);
    expect(s_overflow == 0u, "event log overflow", s_overflow, 0);

    printf("gl_record_free_wrap oracle: checks=%u failures=%u "
           "cov(order_ok,order_bad,uid,result)=%u,%u,%u,%u long(bumps,frees)=%u,%u mutant=%d\n",
           s_checks, s_failures, s_cov_order_ok, s_cov_order_bad, s_cov_uid, s_cov_result,
           bumps, reals,
#if defined(ISAAC_GL_RECORD_FREE_WRAP_ORACLE_MUTATE_LATE_BUMP)
           1
#else
           0
#endif
           );
    printf("%s\n", s_failures ? "FAIL" : "PASS");
    return s_failures ? 1 : 0;
}
