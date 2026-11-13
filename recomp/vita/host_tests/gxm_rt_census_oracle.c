/* Host oracle for ISAAC_VITA_GXM_RT_CREATE_CENSUS
 * (recomp/runtime/kage_vita_gxm_rt_census.h), driven by
 * recomp/vita/test_gxm_rt_create_census.py.
 *
 * The two --wrap wrappers the Vita link installs are compiled here from the
 * same header and called directly; __real_sceGxm* and the process clock
 * are scripted stand-ins.  Checked: arguments and return values pass
 * through unchanged, every call reaches the real function exactly once,
 * and the record and receipt text for three windows. */
#define KAGE_VITA_GXM_RT_CENSUS_WRAPS 1
#include "kage_vita_gxm_rt_census.h"

#include <stdio.h>
#include <string.h>

static uint64_t s_clock;
static uint64_t s_cost;        /* what the next real call spends */
static int s_rc;               /* what the next real call returns */
static unsigned s_real_creates, s_real_destroys;
static const void *s_last_params;
static void **s_last_target;
static void *s_last_destroyed;
static int s_failures;

uint64_t sceKernelGetProcessTimeWide(void) { return s_clock; }

int __real_sceGxmCreateRenderTarget(const void *params, void **target)
{
    ++s_real_creates;
    s_last_params = params;
    s_last_target = target;
    s_clock += s_cost;
    if (s_rc >= 0)
        *target = (void *)(uintptr_t)(0x1000u + s_real_creates);
    return s_rc;
}

int __real_sceGxmDestroyRenderTarget(void *target)
{
    ++s_real_destroys;
    s_last_destroyed = target;
    s_clock += s_cost;
    return s_rc;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        ++s_failures; \
    } } while (0)

static void *create(int rc, uint64_t cost, int tag)
{
    void *target = NULL;
    const void *params = (const void *)(uintptr_t)(0x2000u + (unsigned)tag);
    unsigned before = s_real_creates;
    int got;

    s_rc = rc;
    s_cost = cost;
    got = __wrap_sceGxmCreateRenderTarget(params, &target);
    CHECK(got == rc);
    CHECK(s_real_creates == before + 1u);
    CHECK(s_last_params == params && s_last_target == &target);
    CHECK((rc >= 0) == (target != NULL));
    return target;
}

static void destroy(void *target, int rc, uint64_t cost)
{
    unsigned before = s_real_destroys;
    int got;

    s_rc = rc;
    s_cost = cost;
    got = __wrap_sceGxmDestroyRenderTarget(target);
    CHECK(got == rc);
    CHECK(s_real_destroys == before + 1u);
    CHECK(s_last_destroyed == target);
}

static void expect_record(unsigned window, const char *want)
{
    KageVitaGxmRtCensus take;
    char line[256];

    kage_vita_gxm_rt_census_take(&take);
    snprintf(line, sizeof line, KAGE_VITA_GXM_RT_CENSUS_FORMAT, "gxm:oracle",
             window, window * 120u, KAGE_VITA_GXM_RT_CENSUS_ARGS(&take));
    if (strcmp(line, want) != 0) {
        printf("FAIL window %u\n  want %s  got  %s", window, want, line);
        ++s_failures;
    }
}

static void expect_receipt(const char *want)
{
    uint32_t seen[3];
    char line[256];

    kage_vita_gxm_rt_census_seen(seen);
    snprintf(line, sizeof line, KAGE_VITA_GXM_RT_CENSUS_RECEIPT_FORMAT,
             seen[0], seen[1], seen[2]);
    if (strcmp(line, want) != 0) {
        printf("FAIL receipt\n  want %s  got  %s", want, line);
        ++s_failures;
    }
}

int main(void)
{
    void *a, *b, *c;

    s_clock = 1000u;
    /* Boot: the display target, before the first window. */
    a = create(0, 900u, 1);
    expect_receipt(
        "[kage-vita] KAGE VITA GXM RT CREATE CENSUS: on "
        "wrap=sceGxmCreateRenderTarget,sceGxmDestroyRenderTarget "
        "seen(c,x,d)=1,0,0 receipt=ph120.gr\n");
    /* Window 1: a size switch (destroy + create), a refused create and its
     * one-scene retry, a second destroy that fails (still one call).  The
     * boot create is part of the first window: nothing took it before
     * (c = 3 of 4 calls, us = 900 + 13300 + 250 + 15500). */
    b = create(0, 13300u, 2);
    destroy(b, 0, 410u);
    c = create((int)0x805b0006u, 250u, 3);      /* refused: negative */
    CHECK(c == NULL);
    c = create(0, 15500u, 4);
    destroy(c, -1, 90u);
    expect_record(1u,
        "[kage-vita] ph120.gr bid=gxm:oracle win=1 loops=120 "
        "create(c,x,us,max)=3,1,29950,15500 destroy(n,us)=2,500\n");
    /* Window 2: nothing happened; the take above zeroed every field. */
    expect_record(2u,
        "[kage-vita] ph120.gr bid=gxm:oracle win=2 loops=240 "
        "create(c,x,us,max)=0,0,0,0 destroy(n,us)=0,0\n");
    /* Window 3: max is the window's own, not a carried one; one call longer
     * than 32 bits of microseconds is clamped to 0xffffffff and the window
     * sum is modulo 2^32 (0xffffffff + 5 = 4). */
    b = create(0, 700u, 5);
    c = create(1, 200u, 6);                     /* positive rc is success */
    destroy(a, 0, 0x100000001ull);
    destroy(b, 0, 5u);
    expect_record(3u,
        "[kage-vita] ph120.gr bid=gxm:oracle win=3 loops=360 "
        "create(c,x,us,max)=2,0,900,700 destroy(n,us)=2,4\n");
    /* Totals since start are not reset by the takes. */
    expect_receipt(
        "[kage-vita] KAGE VITA GXM RT CREATE CENSUS: on "
        "wrap=sceGxmCreateRenderTarget,sceGxmDestroyRenderTarget "
        "seen(c,x,d)=5,1,4 receipt=ph120.gr\n");
    (void)c;
    if (s_failures) {
        printf("gxm rt census oracle: FAIL (%d)\n", s_failures);
        return 1;
    }
    printf("gxm rt census oracle: PASS (creates %u, destroys %u through the "
           "wrappers)\n", s_real_creates, s_real_destroys);
    return 0;
}
