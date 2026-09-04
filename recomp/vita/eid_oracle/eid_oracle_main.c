#include <stdio.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

_Static_assert(sizeof(void *) == 4, "the EID oracle must use the 32-bit ABI");

int main(int argc, char **argv) {
    lua_State *state;
    int index;
    int status;

    if (argc != 5 && argc != 6) {
        fprintf(stderr,
                "usage: %s HARNESS.LUA MOD_ROOT CORE_SCRIPTS TRANSCRIPT "
                "[dispatch]\n",
                argv[0]);
        return 2;
    }

    state = luaL_newstate();
    if (state == NULL) {
        fputs("luaL_newstate failed\n", stderr);
        return 2;
    }
    luaL_openlibs(state);

    lua_createtable(state, argc - 2, 1);
    for (index = 1; index < argc; ++index) {
        lua_pushstring(state, argv[index]);
        lua_rawseti(state, -2, index - 1);
    }
    lua_setglobal(state, "arg");

    status = luaL_loadfile(state, argv[1]);
    if (status == LUA_OK) {
        status = lua_pcall(state, 0, LUA_MULTRET, 0);
    }
    if (status != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        fprintf(stderr, "%s\n", message != NULL ? message : "Lua error");
        lua_close(state);
        return 1;
    }

    lua_close(state);
    return 0;
}
