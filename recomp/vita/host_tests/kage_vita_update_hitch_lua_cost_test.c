/* ISAAC_VITA_UPDATE_HITCH_PROBE: the Lua cost unit's lifetime accessor,
 * executed against the production host_vita_lua_cost.c (linked, built with
 * ISAAC_VITA_LUA_COST_PROFILE=1 ISAAC_VITA_LUA_COST_PROFILE_ORACLE=1
 * ISAAC_VITA_UPDATE_HITCH_PROBE=1 by test_kage_vita_update_hitch.py).
 * The point: the unit's own 2 s window may close in the middle of a
 * Game::Update; a difference of two peeks must not lose the closed part. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_lua_cost.h"

struct lua_State;
void __wrap_luaC_step(struct lua_State *L);

static uint64_t s_now = 1000U;
static char s_lines[8][512];
static unsigned s_line_count;

uint64_t isaac_vita_lua_cost_oracle_clock(void) { return s_now; }
void isaac_vita_lua_cost_oracle_sink(const char *line)
{
    assert(s_line_count < 8U && strlen(line) < 512U);
    memcpy(s_lines[s_line_count++], line, strlen(line) + 1U);
}
void isaac_vita_log(const char *format, ...) { (void)format; }
void __real_luaC_step(struct lua_State *L) { (void)L; s_now += 7U; }
void *__real_luaM_realloc_(struct lua_State *L, void *block, size_t osize,
                           size_t nsize)
{
    (void)L; (void)block; (void)osize; (void)nsize;
    return NULL;
}

static void pcall(uint32_t before, uint32_t callback, uint32_t after)
{
    isaac_vita_lua_cost_mark mark;
    isaac_vita_lua_cost_pcall_enter(0x401000U, &mark);
    s_now += before;
    if (callback) {
        isaac_vita_lua_cost_cb_enter(0x402000U);
        s_now += callback;
        isaac_vita_lua_cost_cb_leave();
    }
    s_now += after;
    isaac_vita_lua_cost_pcall_leave(&mark);
}

static void expect(const uint32_t got[6], uint32_t a, uint32_t b, uint32_t c,
                   uint32_t d, uint32_t e, uint32_t f)
{
    const uint32_t want[6] = { a, b, c, d, e, f };
    if (memcmp(got, want, sizeof want) != 0) {
        fprintf(stderr, "got %u,%u,%u,%u,%u,%u want %u,%u,%u,%u,%u,%u\n",
                got[0], got[1], got[2], got[3], got[4], got[5],
                a, b, c, d, e, f);
        assert(0);
    }
}

int main(void)
{
    uint32_t v[6];

    pcall(30U, 40U, 30U);              /* pc 1/100, cb 1/40 */
    __wrap_luaC_step(NULL);            /* outside a pcall: gcext 1/7 */
    isaac_vita_lua_cost_hitch_peek(v);
    expect(v, 1U, 100U, 1U, 40U, 1U, 7U);

    isaac_vita_lua_cost_flush();       /* the unit's window closes here */
    isaac_vita_lua_cost_hitch_peek(v);
    expect(v, 1U, 100U, 1U, 40U, 1U, 7U);

    pcall(50U, 0U, 0U);
    __wrap_luaC_step(NULL);
    isaac_vita_lua_cost_hitch_peek(v);
    expect(v, 2U, 150U, 1U, 40U, 2U, 14U);

    /* The unit's own records are what they are without the probe: window
     * totals only, the second window does not carry the first. */
    isaac_vita_lua_cost_flush();
    assert(s_line_count == 4U);
    assert(strstr(s_lines[0], " pc=1/100 cb=1/40 "));
    assert(strstr(s_lines[0], " gcext=1/7 "));
    assert(strstr(s_lines[2], " pc=1/50 cb=0/0 "));
    assert(strstr(s_lines[2], " gcext=1/7 "));
    isaac_vita_lua_cost_hitch_peek(v);
    expect(v, 2U, 150U, 1U, 40U, 2U, 14U);
    printf("update hitch lua cost: PASS lines=%u\n", s_line_count);
    return 0;
}
