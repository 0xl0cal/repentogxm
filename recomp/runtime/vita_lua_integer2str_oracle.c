/* Host oracle for ISAAC_VITA_LUA_INTEGER2STR (recomp/test_vita_lua_integer2str.py).
 *
 * Built twice from this one file against pristine Lua 5.3.3:
 *   I2S_DERIVED=1: lobject.c is the derived file (lua53_integer2str.py);
 *                  runs the direct check, the transcript and the boot
 *                  self-test unit with a fake clock;
 *   I2S_DERIVED=0: the pristine lobject.c; runs the transcript only.
 * The harness requires the two transcript lines to be identical.
 *
 * direct: isaac_lua_integer2str(buff, 50, v) against
 *   snprintf(buff, 50, LUA_INTEGER_FMT, v) -- return value and every byte
 *   through the NUL -- for every value of the classes below, with coverage
 *   per sign x digit count and per number of 64-bit loop rounds (0/1/2).
 * transcript: the strings Lua itself produces through luaO_tostring
 *   (lua_tolstring of an integer, lua_pushfstring %d/%I, a Lua chunk with
 *   tostring/../table.concat/error positions and float formatting), hashed
 *   (FNV-1a 64 over length + bytes).
 *
 * Value classes (shared by both): dense -200000..200000; 10^k +- 2000 and
 * 2^k +- 2000 for every k, both signs (two's-complement wrap at 2^63);
 * LLONG_MIN + 0..100000 and LLONG_MAX - 0..100000; the 2^32 and
 * 0xffffffff * 10^9 loop boundaries; xorshift64* values shifted right by
 * every 0..63 with a random sign.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#if !defined(I2S_DERIVED)
#error "define I2S_DERIVED to 0 (pristine lobject.c) or 1 (derived)"
#endif

#define I2S_BUFFER 50
#define I2S_RANDOM_PER_SHIFT 300000U

#if I2S_DERIVED
int isaac_lua_integer2str(char *buff, size_t sz, lua_Integer n);
void isaac_vita_lua_integer2str_selftest(void);

static uint64_t s_fake_clock;
static unsigned s_selftest_lines;
static char s_selftest_line[512];

uint64_t isaac_vita_lua_integer2str_oracle_clock(void);
void isaac_vita_lua_integer2str_oracle_sink(const char *line);

uint64_t isaac_vita_lua_integer2str_oracle_clock(void)
{
    s_fake_clock += 1000U;   /* deterministic: every timed pass costs 1 ms */
    return s_fake_clock;
}

void isaac_vita_lua_integer2str_oracle_sink(const char *line)
{
    ++s_selftest_lines;
    snprintf(s_selftest_line, sizeof(s_selftest_line), "%s", line);
}
#endif

static lua_Integer from_bits(uint64_t bits)
{
    lua_Integer value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t to_bits(lua_Integer value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* ---- value classes ---------------------------------------------------- */

typedef void (*value_fn)(lua_Integer value, void *context);

static uint64_t s_rng = 0x2545f4914f6cdd1dULL;

static uint64_t rng_next(void)
{
    uint64_t x = s_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_rng = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static void each_value(value_fn fn, void *context, unsigned random_stride)
{
    int64_t d;
    unsigned k;
    uint64_t pow10 = 1U;
    static const uint64_t boundaries[] = {
        0xffffffffULL, 0x100000000ULL, 0xfffffffeULL, 0x100000001ULL,
        4294967295999999999ULL, 4294967296000000000ULL,
        4294967296000000001ULL, 4294967295999999998ULL,
        999999999ULL, 1000000000ULL, 999999999999999999ULL,
        1000000000000000000ULL, 9999999999ULL, 10000000000ULL,
    };
    for (d = -200000; d <= 200000; ++d)
        fn((lua_Integer)d, context);
    for (k = 0U; k < 19U; ++k) {
        for (d = -2000; d <= 2000; ++d) {
            uint64_t v = pow10 + (uint64_t)d;
            fn(from_bits(v), context);
            fn(from_bits((uint64_t)0 - v), context);
        }
        pow10 *= 10U;
    }
    for (k = 0U; k < 64U; ++k) {
        for (d = -2000; d <= 2000; ++d) {
            uint64_t v = (1ULL << k) + (uint64_t)d;
            fn(from_bits(v), context);
            fn(from_bits((uint64_t)0 - v), context);
        }
    }
    for (d = 0; d <= 100000; ++d) {
        fn(from_bits(0x8000000000000000ULL + (uint64_t)d), context);
        fn(from_bits(0x7fffffffffffffffULL - (uint64_t)d), context);
    }
    for (k = 0U; k < sizeof(boundaries) / sizeof(boundaries[0]); ++k) {
        fn(from_bits(boundaries[k]), context);
        fn(from_bits((uint64_t)0 - boundaries[k]), context);
    }
    s_rng = 0x2545f4914f6cdd1dULL;
    for (k = 0U; k < 64U; ++k) {
        unsigned n;
        for (n = 0U; n < I2S_RANDOM_PER_SHIFT; ++n) {
            uint64_t r = rng_next();
            uint64_t v = r >> k;
            if (n % random_stride != 0U)
                continue;
            fn(from_bits((r & 1U) ? (uint64_t)0 - v : v), context);
        }
    }
}

/* ---- direct check (derived only) ---------------------------------------- */

#if I2S_DERIVED
typedef struct direct_state {
    uint64_t checked;
    uint64_t mismatches;
    lua_Integer first_bad;
    uint64_t digits[2][21];     /* [negative][digit count] */
    uint64_t rounds[3];         /* 64-bit loop rounds */
    uint64_t llong_min;
} direct_state;

static void direct_one(lua_Integer value, void *context)
{
    direct_state *state = (direct_state *)context;
    char want[I2S_BUFFER];
    char got[I2S_BUFFER];
    int want_len, got_len;
    uint64_t u = value < 0 ? (uint64_t)0 - to_bits(value) : to_bits(value);
    unsigned digits = 1U;
    uint64_t t = u;
    memset(want, 0x5a, sizeof(want));
    memset(got, 0xa5, sizeof(got));
    want_len = snprintf(want, sizeof(want), LUA_INTEGER_FMT,
                        (LUAI_UACINT)value);
    got_len = isaac_lua_integer2str(got, sizeof(got), value);
    ++state->checked;
    if (want_len != got_len || want_len < 0 || want_len >= I2S_BUFFER ||
        memcmp(want, got, (size_t)want_len + 1U) != 0) {
        if (state->mismatches == 0U)
            state->first_bad = value;
        ++state->mismatches;
    }
    while (t >= 10U) {
        t /= 10U;
        ++digits;
    }
    state->digits[value < 0][digits]++;
    if (u <= 0xffffffffULL)
        state->rounds[0]++;
    else if (u / 1000000000U <= 0xffffffffULL)
        state->rounds[1]++;
    else
        state->rounds[2]++;
    if (to_bits(value) == 0x8000000000000000ULL)
        state->llong_min++;
}

static int run_direct(void)
{
    direct_state state;
    unsigned sign, d;
    memset(&state, 0, sizeof(state));
    each_value(direct_one, &state, 1U);
    printf("direct: checked=%llu mismatches=%llu first_bad=0x%016llx\n",
           (unsigned long long)state.checked,
           (unsigned long long)state.mismatches,
           (unsigned long long)to_bits(state.first_bad));
    for (sign = 0U; sign < 2U; ++sign) {
        printf("coverage digits %s:", sign ? "neg" : "pos");
        for (d = 1U; d <= 19U; ++d)
            printf(" %u=%llu", d, (unsigned long long)state.digits[sign][d]);
        printf("\n");
    }
    printf("coverage loop64 rounds: 0=%llu 1=%llu 2=%llu llong_min=%llu\n",
           (unsigned long long)state.rounds[0],
           (unsigned long long)state.rounds[1],
           (unsigned long long)state.rounds[2],
           (unsigned long long)state.llong_min);
    if (state.mismatches != 0U)
        return 1;
    for (sign = 0U; sign < 2U; ++sign)
        for (d = 1U; d <= 19U; ++d)
            if (state.digits[sign][d] == 0U)
                return 1;
    if (state.rounds[0] == 0U || state.rounds[1] == 0U ||
        state.rounds[2] == 0U || state.llong_min == 0U)
        return 1;
    return 0;
}
#endif

/* ---- transcript (both builds) -------------------------------------------- */

typedef struct transcript_state {
    lua_State *L;
    uint64_t hash;
    uint64_t strings;
    uint64_t bytes;
    uint64_t values;
} transcript_state;

static void hash_bytes(transcript_state *state, const char *s, size_t len)
{
    size_t i;
    uint64_t h = state->hash;
    uint64_t n = (uint64_t)len;
    for (i = 0U; i < sizeof(n); ++i) {
        h ^= (uint8_t)(n >> (8U * i));
        h *= 0x100000001b3ULL;
    }
    for (i = 0U; i < len; ++i) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    state->hash = h;
    state->strings++;
    state->bytes += len;
}

static void transcript_one(lua_Integer value, void *context)
{
    transcript_state *state = (transcript_state *)context;
    lua_State *L = state->L;
    size_t len;
    const char *s;
    lua_pushinteger(L, value);
    s = lua_tolstring(L, -1, &len);     /* luaO_tostring in place */
    hash_bytes(state, s, len);
    lua_pop(L, 1);
    if ((state->values & 63U) == 0U) {
        /* %d takes an int: the low 32 bits as a signed int (INT_MIN..MAX) */
        uint32_t low = (uint32_t)to_bits(value);
        int32_t as_int;
        memcpy(&as_int, &low, sizeof(as_int));
        s = lua_pushfstring(L, "%d|%I", (int)as_int, (LUAI_UACINT)value);
        hash_bytes(state, s, strlen(s));
        lua_pop(L, 1);
    }
    if ((state->values & 4095U) == 0U)
        lua_gc(L, LUA_GCSTEP, 0);
    state->values++;
}

static const char *const k_chunk =
    "local out = {}\n"
    "local function add(s) out[#out + 1] = s end\n"
    "for i = -3000, 3000 do add(tostring(i)); add(i .. ''); "
    "add(('%d'):format(i)) end\n"
    "add(tostring(math.maxinteger)); add(tostring(math.mininteger))\n"
    "add(math.mininteger .. ''); add(math.maxinteger .. '|' .. -1)\n"
    "for _, f in ipairs({0.0, -0.0, 1.5, -1.5, 2^53, 2^63, -2^63, 1e300, "
    "1/0, -1/0, 3.0, 1e15, 1e16, 0.1, -7.0, 123456789012.0}) do\n"
    "  add(tostring(f)); add(f .. '')\n"
    "end\n"
    "local t = {}\n"
    "for i = 1, 1000 do t[i] = i * 7919 - 500000 end\n"
    "add(table.concat(t, ','))\n"
    "local ok, e = pcall(function() error('position') end); add(tostring(e))\n"
    "ok, e = pcall(function() local x = nil; return x.y end); add(tostring(e))\n"
    "add(tostring(1 << 62) .. tostring(-(1 << 62)) .. tostring(7 // 2))\n"
    "add(string.format('%5.1f|%d|%s', 2.25, -42, 99))\n"
    "return table.concat(out, '\\n')\n";

static int run_transcript(void)
{
    transcript_state state;
    size_t len;
    const char *s;
    memset(&state, 0, sizeof(state));
    state.hash = 0xcbf29ce484222325ULL;
    state.L = luaL_newstate();
    if (state.L == NULL)
        return 1;
    luaL_openlibs(state.L);
    each_value(transcript_one, &state, 4U);
    if (luaL_loadstring(state.L, k_chunk) != LUA_OK ||
        lua_pcall(state.L, 0, 1, 0) != LUA_OK) {
        printf("chunk failed: %s\n", lua_tostring(state.L, -1));
        lua_close(state.L);
        return 1;
    }
    s = lua_tolstring(state.L, -1, &len);
    hash_bytes(&state, s, len);
    printf("chunk: bytes=%llu last=%s\n", (unsigned long long)len,
           strrchr(s, '\n') != NULL ? strrchr(s, '\n') + 1 : s);
    lua_close(state.L);
    printf("transcript: values=%llu strings=%llu bytes=%llu fnv1a64=%016llx\n",
           (unsigned long long)state.values,
           (unsigned long long)state.strings,
           (unsigned long long)state.bytes,
           (unsigned long long)state.hash);
    return 0;
}

int main(void)
{
    int failed = 0;
#if I2S_DERIVED
    failed |= run_direct();
#endif
    failed |= run_transcript();
#if I2S_DERIVED
    isaac_vita_lua_integer2str_selftest();
    isaac_vita_lua_integer2str_selftest();   /* runs once */
    printf("selftest lines=%u: %s\n", s_selftest_lines, s_selftest_line);
    if (s_selftest_lines != 1U ||
        strstr(s_selftest_line, " mismatches=0 ") == NULL)
        failed = 1;
#endif
    printf("integer2str oracle (%s lobject.c): %s\n",
           I2S_DERIVED ? "derived" : "pristine", failed ? "FAIL" : "PASS");
    return failed;
}
