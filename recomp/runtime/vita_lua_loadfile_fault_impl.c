/* Host oracle only: the definitions of vita_lua_loadfile_fault.h for the
 * bridge oracle's "loadfile-retry" variant (recomp/test_vita_lua_runtime.py),
 * whose lauxlib.c is compiled with that header force-included.  The
 * loadfile oracle (vita_lua_loadfile_retry_oracle.c) holds its own copy. */
#define VITA_LUA_LOADFILE_FAULT_IMPL 1
#include "vita_lua_loadfile_fault.h"
