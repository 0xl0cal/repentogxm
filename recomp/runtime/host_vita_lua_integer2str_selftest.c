/* ISAAC_VITA_LUA_INTEGER2STR boot self-test (profile builds only).
 *
 * With ISAAC_VITA_LUA_INTEGER2STR the Lua archive compiles a derived
 * lobject.c whose luaO_tostring formats integers with isaac_lua_integer2str
 * (recomp/vita/lua53_integer2str.py) instead of
 * snprintf(buff, 50, LUA_INTEGER_FMT, n) (luaconf.h:513-514/591, "%lld").
 * The host proof (recomp/test_vita_lua_integer2str.py) compares the two
 * on the host's C library; this unit repeats the comparison on the console's
 * own newlib snprintf and times both, so the saving per conversion is a
 * device number:
 *
 *   [isaac-lua] i2s selftest build=<id> values=<n> mismatches=<m> ...
 *       small n=<k> snprintf_ns=<a> itoa_ns=<b> wide n=<k> snprintf_ns=<c>
 *       itoa_ns=<d> passes=<p>
 *
 * "small" is 0..999 and -1..-24 (the magnitudes EID formats: ids, counts,
 * coordinates); "wide" is every digit length and sign, the 10^k and 2^k
 * edges, LLONG_MIN/LLONG_MAX and xorshift values.  Each arm is timed PASSES
 * times, interleaved, and the minimum pass is reported (ns per call, from
 * the 1 us process clock over the whole pass).  It runs once, at the first
 * LuaEngine state creation, and changes no Lua state.  Compiled only when
 * ISAAC_VITA_LUA_INTEGER2STR and ISAAC_VITA_LUA_COST_PROFILE are both ON. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"

#if defined(__vita__) && !defined(ISAAC_VITA_LUA_INTEGER2STR_ORACLE)
#include <psp2/kernel/processmgr.h>
#endif

#if !defined(ISAAC_VITA_LUA_INTEGER2STR_SELFTEST)
#error "host_vita_lua_integer2str_selftest.c is compiled only for the integer2str self-test"
#endif

#if !defined(ISAAC_VITA_LUA_INTEGER2STR_BUILD_ID)
#define ISAAC_VITA_LUA_INTEGER2STR_BUILD_ID "unknown"
#endif

/* The derived lobject.c's definition (LUAI_FUNC: hidden, static link). */
int isaac_lua_integer2str(char *buff, size_t sz, lua_Integer n);
void isaac_vita_lua_integer2str_selftest(void);

#if defined(ISAAC_VITA_LUA_INTEGER2STR_ORACLE)
uint64_t isaac_vita_lua_integer2str_oracle_clock(void);
void isaac_vita_lua_integer2str_oracle_sink(const char *line);
#else
void isaac_vita_log(const char *format, ...);
#endif

#define I2S_SMALL_COUNT 1024U   /* 0..999 and -1..-24 */
#define I2S_WIDE_COUNT 1024U
#define I2S_PASSES 3U
#define I2S_BUFFER 50U          /* lobject.c:365 MAXNUMBER2STR */

static lua_Integer s_small[I2S_SMALL_COUNT];
static lua_Integer s_wide[I2S_WIDE_COUNT];
static int s_ran;
/* Sinks the formatted bytes so neither loop is optimised away. */
static volatile unsigned s_sink;

static uint64_t i2s_now(void)
{
#if defined(ISAAC_VITA_LUA_INTEGER2STR_ORACLE)
    return isaac_vita_lua_integer2str_oracle_clock();
#elif defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
#error "the integer2str self-test needs the Vita clock or the oracle clock"
#endif
}

static void i2s_emit(const char *line)
{
#if defined(ISAAC_VITA_LUA_INTEGER2STR_ORACLE)
    isaac_vita_lua_integer2str_oracle_sink(line);
#else
    isaac_vita_log("%s", line);
#endif
}

static lua_Integer i2s_from_bits(uint64_t bits)
{
    lua_Integer value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void i2s_fill(void)
{
    unsigned i = 0U;
    unsigned k;
    uint64_t pow10 = 1U;
    uint64_t x = 0x9e3779b97f4a7c15ULL;
    for (k = 0U; k < 1000U; ++k)
        s_small[k] = (lua_Integer)k;
    for (k = 1U; k <= 24U; ++k)
        s_small[999U + k] = -(lua_Integer)k;
    /* wide: the edges first, then xorshift64 values at every magnitude */
    s_wide[i++] = 0;
    s_wide[i++] = i2s_from_bits(0x7fffffffffffffffULL);  /* LLONG_MAX */
    s_wide[i++] = i2s_from_bits(0x8000000000000000ULL);  /* LLONG_MIN */
    s_wide[i++] = i2s_from_bits(0x8000000000000001ULL);
    for (k = 0U; k < 19U; ++k) {          /* 10^0 .. 10^18, +-1 */
        lua_Integer p = (lua_Integer)pow10;
        s_wide[i++] = p;
        s_wide[i++] = p - 1;
        s_wide[i++] = p + 1;
        s_wide[i++] = -p;
        s_wide[i++] = -p + 1;
        s_wide[i++] = -p - 1;
        pow10 *= 10U;
    }
    for (k = 0U; k < 63U; ++k) {          /* 2^0 .. 2^62, +-1 */
        lua_Integer p = i2s_from_bits(1ULL << k);
        s_wide[i++] = p;
        s_wide[i++] = p - 1;
        s_wide[i++] = -p - 1;
    }
    while (i < I2S_WIDE_COUNT) {
        uint64_t v;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        v = x >> (i % 64U);               /* every magnitude */
        s_wide[i] = i2s_from_bits((i & 1U) ? v : (uint64_t)0 - v);
        ++i;
    }
}

static unsigned i2s_time_snprintf(const lua_Integer *values, unsigned count,
                                  uint64_t *us)
{
    char buff[I2S_BUFFER];
    unsigned acc = 0U;
    unsigned k;
    uint64_t start = i2s_now();
    for (k = 0U; k < count; ++k) {
        int len = snprintf(buff, sizeof(buff), LUA_INTEGER_FMT,
                           (LUAI_UACINT)values[k]);
        acc += (unsigned)len + (unsigned char)buff[0];
    }
    *us = i2s_now() - start;
    return acc;
}

static unsigned i2s_time_itoa(const lua_Integer *values, unsigned count,
                              uint64_t *us)
{
    char buff[I2S_BUFFER];
    unsigned acc = 0U;
    unsigned k;
    uint64_t start = i2s_now();
    for (k = 0U; k < count; ++k) {
        int len = isaac_lua_integer2str(buff, sizeof(buff), values[k]);
        acc += (unsigned)len + (unsigned char)buff[0];
    }
    *us = i2s_now() - start;
    return acc;
}

static unsigned i2s_compare(const lua_Integer *values, unsigned count,
                            lua_Integer *first_bad)
{
    char want[I2S_BUFFER];
    char got[I2S_BUFFER];
    unsigned bad = 0U;
    unsigned k;
    for (k = 0U; k < count; ++k) {
        int want_len, got_len;
        memset(want, 0x5a, sizeof(want));
        memset(got, 0x5a, sizeof(got));
        want_len = snprintf(want, sizeof(want), LUA_INTEGER_FMT,
                            (LUAI_UACINT)values[k]);
        got_len = isaac_lua_integer2str(got, sizeof(got), values[k]);
        /* the bytes through the NUL must agree; the rest of the buffer is
         * dead in luaO_tostring (luaS_newlstr copies len bytes) */
        if (want_len != got_len || want_len < 0 ||
            (size_t)want_len >= sizeof(want) ||
            memcmp(want, got, (size_t)want_len + 1U) != 0) {
            if (bad == 0U)
                *first_bad = values[k];
            ++bad;
        }
    }
    return bad;
}

static unsigned i2s_ns(uint64_t us, unsigned count)
{
    return (unsigned)((us * 1000U) / count);
}

void isaac_vita_lua_integer2str_selftest(void)
{
    char line[320];
    lua_Integer first_bad = 0;
    unsigned mismatches;
    uint64_t best[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    unsigned pass;
    uint64_t bad_bits;
    if (s_ran)
        return;
    s_ran = 1;
    i2s_fill();
    mismatches = i2s_compare(s_small, I2S_SMALL_COUNT, &first_bad);
    {
        lua_Integer wide_bad = 0;
        unsigned wide_mismatches = i2s_compare(s_wide, I2S_WIDE_COUNT,
                                               &wide_bad);
        if (mismatches == 0U && wide_mismatches != 0U)
            first_bad = wide_bad;
        mismatches += wide_mismatches;
    }
    /* one untimed warm-up of each arm (code and data into the caches) */
    {
        uint64_t unused;
        s_sink += i2s_time_snprintf(s_small, I2S_SMALL_COUNT, &unused);
        s_sink += i2s_time_itoa(s_small, I2S_SMALL_COUNT, &unused);
    }
    for (pass = 0U; pass < I2S_PASSES; ++pass) {
        uint64_t us;
        s_sink += i2s_time_snprintf(s_small, I2S_SMALL_COUNT, &us);
        if (us < best[0]) best[0] = us;
        s_sink += i2s_time_itoa(s_small, I2S_SMALL_COUNT, &us);
        if (us < best[1]) best[1] = us;
        s_sink += i2s_time_snprintf(s_wide, I2S_WIDE_COUNT, &us);
        if (us < best[2]) best[2] = us;
        s_sink += i2s_time_itoa(s_wide, I2S_WIDE_COUNT, &us);
        if (us < best[3]) best[3] = us;
    }
    memcpy(&bad_bits, &first_bad, sizeof(bad_bits));
    snprintf(line, sizeof(line),
             "[isaac-lua] i2s selftest build=%s values=%u mismatches=%u "
             "first_bad=0x%08x%08x small n=%u snprintf_ns=%u itoa_ns=%u "
             "wide n=%u snprintf_ns=%u itoa_ns=%u passes=%u",
             ISAAC_VITA_LUA_INTEGER2STR_BUILD_ID,
             I2S_SMALL_COUNT + I2S_WIDE_COUNT, mismatches,
             (unsigned)(bad_bits >> 32), (unsigned)bad_bits,
             I2S_SMALL_COUNT, i2s_ns(best[0], I2S_SMALL_COUNT),
             i2s_ns(best[1], I2S_SMALL_COUNT),
             I2S_WIDE_COUNT, i2s_ns(best[2], I2S_WIDE_COUNT),
             i2s_ns(best[3], I2S_WIDE_COUNT), I2S_PASSES);
    i2s_emit(line);
}
