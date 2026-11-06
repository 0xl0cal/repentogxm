#ifndef ISAAC_HOST_VITA_LUA_LOADFILE_H
#define ISAAC_HOST_VITA_LUA_LOADFILE_H

#include <stdint.h>

#if defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
/* ISAAC_VITA_LUA_LOADFILE_RETRY only:
 * one bounded "KAGE VITA LUA LOADFILE STATS" counter line (why =
 * first-load, printed once by the wrapper itself, or loading-complete from
 * kage_vita_loading.c).  The declaration is compiled out with the option. */
void isaac_vita_lua_loadfile_report(const char *why);

#if defined(ISAAC_VITA_LUA_LOADFILE_ORACLE)
/* Host oracles (recomp/test_vita_lua_loadfile_retry.py) supply the clock,
 * the sleep and the thread id, and read a few internals back. */
uint64_t isaac_vita_lua_loadfile_oracle_now_us(void);
void isaac_vita_lua_loadfile_oracle_sleep_us(uint32_t us);
int isaac_vita_lua_loadfile_oracle_thread_id(void);
uint32_t isaac_vita_lua_loadfile_oracle_delay_us(uint32_t attempt);
void isaac_vita_lua_loadfile_oracle_reset_budget(void);
uint32_t isaac_vita_lua_loadfile_oracle_arm_depth(void);
uint32_t isaac_vita_lua_loadfile_oracle_stale_clears(void);
#endif
#endif

#endif
