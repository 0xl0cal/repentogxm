/* Host oracle for ISAAC_VITA_LUA_COST_PROFILE (host_vita_lua_cost.c), built
 * by recomp/test_vita_lua_cost.py with ISAAC_VITA_LUA_COST_PROFILE=1 and
 * ISAAC_VITA_LUA_COST_PROFILE_ORACLE=1.  A manual clock and scripted
 * __real_luaC_step / __real_luaM_realloc_ drive the exact accounting paths:
 * top-level and nested pcalls, callbacks (top, nested, finalizer, outside a
 * pcall), GC steps in all three contexts, Lua errors that longjmp past a
 * callback and past a GC step, allocator sampling, the 64-slot overflow, the
 * top-8 order and the 2 s window rollover.  Prints every emitted line. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_vita_lua_cost.h"

struct lua_State;
void __wrap_luaC_step(struct lua_State *L);
void *__wrap_luaM_realloc_(struct lua_State *L, void *block, size_t osize,
                           size_t nsize);

static uint64_t s_now = 1000U;
static uint32_t s_step_advance;
static uint32_t s_step_finalizer;
static int s_step_throw;
static jmp_buf s_escape;

uint64_t isaac_vita_lua_cost_oracle_clock(void)
{
    return s_now;
}

void isaac_vita_lua_cost_oracle_sink(const char *line)
{
    printf("%s\n", line);
}

void __real_luaC_step(struct lua_State *L)
{
    (void)L;
    s_now += s_step_advance;
    if (s_step_finalizer) {
        isaac_vita_lua_cost_cb_enter(s_step_finalizer);
        s_now += 2U;
        isaac_vita_lua_cost_cb_leave();
    }
    if (s_step_throw)
        longjmp(s_escape, 1);
}

void *__real_luaM_realloc_(struct lua_State *L, void *block, size_t osize,
                           size_t nsize)
{
    (void)L; (void)block; (void)osize; (void)nsize;
    s_now += 1U;
    return NULL;
}

static void step(uint32_t advance, uint32_t finalizer, int throws)
{
    s_step_advance = advance;
    s_step_finalizer = finalizer;
    s_step_throw = throws;
    __wrap_luaC_step(NULL);
}

static void callback(uint32_t target, uint32_t us)
{
    isaac_vita_lua_cost_cb_enter(target);
    s_now += us;
    isaac_vita_lua_cost_cb_leave();
}

int main(void)
{
    isaac_vita_lua_cost_mark top, inner;
    uint32_t i;

    /* S1: one top pcall with a GC step inside a callback (finalizer inside
     * it), a VM GC step, and a callback that runs a nested pcall. */
    isaac_vita_lua_cost_pcall_enter(0x0040ef41U, &top);
    s_now += 100U;
    isaac_vita_lua_cost_cb_enter(0x003f0001U);
    s_now += 30U;
    step(5U, 0x003f0002U, 0);
    isaac_vita_lua_cost_cb_leave();
    s_now += 10U;
    step(7U, 0U, 0);
    isaac_vita_lua_cost_cb_enter(0x003f0003U);
    s_now += 1U;
    isaac_vita_lua_cost_pcall_enter(0x0040d421U, &inner);
    callback(0x003f0004U, 3U);
    isaac_vita_lua_cost_pcall_leave(&inner);
    s_now += 1U;
    isaac_vita_lua_cost_cb_leave();
    isaac_vita_lua_cost_pcall_leave(&top);

    /* S2: a Lua error longjmps past a running callback. */
    isaac_vita_lua_cost_pcall_enter(0x0040ef41U, &top);
    isaac_vita_lua_cost_cb_enter(0x003f0005U);
    s_now += 4U;
    isaac_vita_lua_cost_pcall_leave(&top);

    /* S3: a Lua error longjmps out of a VM GC step. */
    isaac_vita_lua_cost_pcall_enter(0x003fd66fU, &top);
    if (setjmp(s_escape) == 0)
        step(6U, 0U, 1);
    isaac_vita_lua_cost_pcall_leave(&top);

    /* S4: outside any pcall: a paced GC step, and a callback that runs a
     * pcall of its own (not top-level: it is inside a callback). */
    step(9U, 0U, 0);
    isaac_vita_lua_cost_cb_enter(0x003f0006U);
    s_now += 1U;
    isaac_vita_lua_cost_pcall_enter(0x0040b3dfU, &inner);
    s_now += 2U;
    isaac_vita_lua_cost_pcall_leave(&inner);
    isaac_vita_lua_cost_cb_leave();

    /* S5: 130 allocator calls, every 64th timed. */
    for (i = 0U; i < 130U; ++i)
        (void)__wrap_luaM_realloc_(NULL, NULL, 0U, 16U);
    isaac_vita_lua_cost_flush();

    /* S6/S7: 72 callbacks over 64 slots (7 overflow), order by us, n, key;
     * the pcall closes exactly 2 s after the window start -> automatic line. */
    isaac_vita_lua_cost_pcall_enter(0x0040ef41U, &top);
    callback(0x200U, 1U);
    callback(0x200U, 1U);
    for (i = 0U; i < 70U; ++i)
        callback(0x100U + i, 2U);
    s_now += 2000000U - 142U;
    isaac_vita_lua_cost_pcall_leave(&top);
    return 0;
}
