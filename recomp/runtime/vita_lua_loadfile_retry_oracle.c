/* Host oracle for ISAAC_VITA_LUA_LOADFILE_RETRY (recomp/test_vita_lua_loadfile_retry.py).
 *
 *   vita-lua-loadfile-oracle <fixture-dir> <group>
 *
 * lauxlib.c is compiled with vita_lua_loadfile_fault.h force-included, so
 * luaL_loadfilex's FILE calls run a newlib-shaped stdio over a fake sceIo
 * kernel that injects the faults each case asks for.  ON builds rename
 * luaL_loadfilex to __real_ in lauxlib.c and to __wrap_ here, in lbaselib.c
 * and in loadlib.c (GNU ld --wrap semantics); OFF builds link it unrenamed.
 * Every load runs on a fresh lua_State and prints one line
 *
 *   case <id> fired=<faults that fired> opens=<sceIoOpen calls>
 *        delays=<retry sleeps> :: status= delta= errno= dump= source= ret= msg=
 *
 * (one line; the part after "::" is what an OFF load and an ON load must
 * agree on).  ON builds follow each case with a STATS line why=<id>; the
 * log sink sets errno = EINVAL, so a wrapper that forgot to restore errno
 * changes the transcript. */
#define VITA_LUA_LOADFILE_FAULT_IMPL 1
#include "vita_lua_loadfile_fault.h"

#include <stdarg.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
#include "host_vita_lua_loadfile.h"
#endif

static const char *s_dir;
static char s_delays[256];
static size_t s_delays_used;
static uint64_t s_sleep_total;
static int s_oom_left;

void isaac_vita_log(const char *format, ...);

void isaac_vita_log(const char *format, ...)
{
    va_list args;

    fputs("[log] ", stdout);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fputc('\n', stdout);
    errno = EINVAL; /* a sink that clobbers errno (spec 3.8) */
}

#if defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
static uint64_t s_clock;

uint64_t isaac_vita_lua_loadfile_oracle_now_us(void)
{
    s_clock += 3U;
    return s_clock;
}

void isaac_vita_lua_loadfile_oracle_sleep_us(uint32_t us)
{
    int n = snprintf(s_delays + s_delays_used,
                     sizeof s_delays - s_delays_used, "%s%u",
                     s_delays_used ? "," : "", (unsigned)us);
    if (n > 0 && (size_t)n < sizeof s_delays - s_delays_used)
        s_delays_used += (size_t)n;
    s_clock += us;
    s_sleep_total += us;
}

int isaac_vita_lua_loadfile_oracle_thread_id(void)
{
    return 0x40010003;
}
#endif

static void report(const char *id)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
    isaac_vita_lua_loadfile_report(id);
#else
    (void)id;
#endif
}

static void reset_budget(void)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
    isaac_vita_lua_loadfile_oracle_reset_budget();
#endif
}

static void *oracle_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0U) {
        free(ptr);
        return NULL;
    }
    if (s_oom_left > 0) {
        --s_oom_left;
        return NULL;
    }
    return realloc(ptr, nsize);
}

static lua_State *new_state(int libs)
{
    lua_State *L = lua_newstate(oracle_alloc, NULL);

    if (!L)
        abort();
    if (libs)
        luaL_openlibs(L);
    return L;
}

static void fixture(char *out, size_t size, const char *name)
{
    if ((size_t)snprintf(out, size, "%s/%s", s_dir, name) >= size)
        abort();
}

static long file_size(const char *path)
{
    struct stat status;

    if (stat(path, &status) != 0)
        abort();
    return (long)status.st_size;
}

/* One line: \n, \r, \t and \\ escaped. */
static void put_text(const char *text)
{
    for (; *text; ++text) {
        switch (*text) {
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        default: fputc(*text, stdout); break;
        }
    }
}

typedef struct dump_sum {
    uint32_t hash;
    size_t length;
} dump_sum;

static int dump_writer(lua_State *L, const void *p, size_t size, void *ud)
{
    dump_sum *sum = (dump_sum *)ud;
    const unsigned char *bytes = (const unsigned char *)p;
    size_t index;

    (void)L;
    for (index = 0U; index < size; ++index) {
        sum->hash ^= bytes[index];
        sum->hash *= 16777619U;
    }
    sum->length += size;
    return 0;
}

/* Values from the index-th slot to the top, comma-joined. */
static void put_values(lua_State *L, int first)
{
    int index, top = lua_gettop(L);

    for (index = first; index <= top; ++index) {
        if (index > first)
            fputc(',', stdout);
        put_text(luaL_tolstring(L, index, NULL));
        lua_pop(L, 1);
    }
}

static void case_head(const char *id, unsigned fired0, unsigned opens0)
{
    printf("case %s fired=%u opens=%u delays=%s :: ", id,
           lfo_fault_fired() - fired0, lfo_kernel_opens() - opens0,
           s_delays_used ? s_delays : "-");
}

/* Load path (NULL = stdin) on a fresh state and print the case line. */
static void observe(const char *id, const char *path)
{
    lua_State *L = new_state(0);
    unsigned fired0 = lfo_fault_fired();
    unsigned opens0 = lfo_kernel_opens();
    dump_sum sum = { 2166136261U, 0U };
    char source[512] = "-";
    int top0 = lua_gettop(L);
    int status;
    int error;
    int delta;

    s_delays_used = 0U;
    s_delays[0] = '\0';
    errno = 0;
    status = luaL_loadfilex(L, path, NULL);
    error = errno;
    delta = lua_gettop(L) - top0;
    case_head(id, fired0, opens0);
    printf("status=%d delta=%d errno=%d ", status, delta, error);
    if (status == LUA_OK && lua_type(L, -1) == LUA_TFUNCTION) {
        lua_Debug ar;

        lua_dump(L, dump_writer, &sum, 0);
        lua_pushvalue(L, -1);
        lua_getinfo(L, ">S", &ar);
        snprintf(source, sizeof source, "%s", ar.source);
        printf("dump=%08x/%u source=", (unsigned)sum.hash,
               (unsigned)sum.length);
        put_text(source);
        fputs(" ret=", stdout);
        lua_pushvalue(L, -1);
        if (lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK) {
            put_values(L, top0 + delta + 1);
        } else {
            fputs("error:", stdout);
            put_text(lua_tostring(L, -1));
        }
        fputs(" msg=-\n", stdout);
    } else {
        fputs("dump=-/0 source=- ret=- msg=", stdout);
        put_text(lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                                : "(not a string)");
        fputc('\n', stdout);
    }
    lua_close(L);
    if (lfo_kernel_live() != 0U) {
        printf("FAIL %s leaked %u kernel handles\n", id, lfo_kernel_live());
        exit(2);
    }
    report(id);
}

static void group_clean(void)
{
    static const char *const names[] = {
        "f1.lua", "f2.lua", "f3.lua", "f4.luac", "f5.lua", "f6.lua"
    };
    char path[512], id[64];
    size_t index;

    for (index = 0U; index < sizeof names / sizeof names[0]; ++index) {
        fixture(path, sizeof path, names[index]);
        snprintf(id, sizeof id, "clean/%s", names[index]);
        observe(id, path);
    }
}

static void group_transient(void)
{
    static const char *const names[] = {
        "f1.lua", "f2.lua", "f3.lua", "f4.luac"
    };
    char path[512], id[96];
    size_t index;

    for (index = 0U; index < sizeof names / sizeof names[0]; ++index) {
        long size, ks[10], seen[10];
        size_t count = 0U, k, j;

        fixture(path, sizeof path, names[index]);
        size = file_size(path);
        ks[0] = 0; ks[1] = 1; ks[2] = 2; ks[3] = 3;
        ks[4] = BUFSIZ - 1; ks[5] = BUFSIZ; ks[6] = BUFSIZ + 1;
        ks[7] = 2L * BUFSIZ + 7; ks[8] = size - 1; ks[9] = size;
        printf("kset %s size=%ld", names[index], size);
        for (k = 0U; k < 10U; ++k) {
            int dup = 0;
            for (j = 0U; j < count; ++j)
                dup |= seen[j] == ks[k];
            if (dup || ks[k] < 0 || ks[k] > size)
                continue;
            seen[count++] = ks[k];
            printf(" %ld", ks[k]);
        }
        fputc('\n', stdout);
        for (k = 0U; k < count; ++k) {
            reset_budget();
            lfo_fault_clear();
            lfo_fault_add(names[index], LFO_READ_AT, seen[k], EIO, 1U, 1);
            snprintf(id, sizeof id, "transient/%s@%ld", names[index], seen[k]);
            observe(id, path);
            lfo_fault_clear();
        }
    }
}

static void group_persistent(void)
{
    char path[512];

    fixture(path, sizeof path, "f1.lua");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f1.lua", LFO_READ_AT, BUFSIZ + 1, EIO, 1U, -1);
    observe("persistent/f1.lua", path);
    lfo_fault_clear();
}

static void group_budget(void)
{
    char path[512], id[64];
    int load;

    fixture(path, sizeof path, "f1.lua");
    reset_budget();
    s_sleep_total = 0U;
    for (load = 1; load <= 7; ++load) {
        lfo_fault_clear();
        lfo_fault_add("f1.lua", LFO_READ_AT, BUFSIZ + 1, EIO, 1U, -1);
        snprintf(id, sizeof id, "budget/%d", load);
        observe(id, path);
    }
    lfo_fault_clear();
    printf("budget sleep_total=%llu\n", (unsigned long long)s_sleep_total);
}

static void group_open(void)
{
    char path[512];

    fixture(path, sizeof path, "missing.lua");
    reset_budget();
    observe("open/missing", path);

    fixture(path, sizeof path, "f1.lua");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f1.lua", LFO_OPEN_FAIL, 0, EIO, 1U, 1);
    observe("open/transient-eio", path);

    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f1.lua", LFO_OPEN_FAIL, 0, EIO, 1U, -1);
    observe("open/persistent-eio", path);

    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f1.lua", LFO_OPEN_FAIL, 0, EACCES, 1U, 1);
    observe("open/eacces", path);

    fixture(path, sizeof path, "f4.luac");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f4.luac", LFO_OPEN_FAIL, 0, EIO, 2U, 1);
    observe("open/reopen-f4", path);

    fixture(path, sizeof path, "f7dir");
    reset_budget();
    lfo_fault_clear();
    observe("open/isdir", path);
}

static void group_changed(void)
{
    char path[512];

    fixture(path, sizeof path, "f1.lua");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f1.lua", LFO_READ_AT, BUFSIZ + 1, EIO, 1U, 1);
    lfo_fault_add("f1.lua", LFO_TRUNCATE_AT, 2L * BUFSIZ, 0, 2U, 1);
    observe("changed/syntax", path);

    fixture(path, sizeof path, "f4.luac");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f4.luac", LFO_READ_AT, BUFSIZ + 1, EIO, 2U, 1);
    lfo_fault_add("f4.luac", LFO_OPEN_FAIL, 0, EIO, 4U, 1);
    observe("changed/reopen", path);
    lfo_fault_clear();
}

static void group_stdin(void)
{
    observe("stdin/null", NULL);
}

/* loadfile, dofile and require (package.searchers[2] -> searcher_Lua). */
static void group_callers(void)
{
    static const struct {
        const char *name;
        const char *code;
    } callers[] = {
        { "loadfile", "local f, e = loadfile(P) if not f then return 'nil', e end return f()" },
        { "dofile", "return dofile(P)" },
        { "require", "package.loaded.f1 = nil return require('f1')" },
    };
    char path[512], pattern[600], id[64];
    size_t index;
    int faulted;

    fixture(path, sizeof path, "f1.lua");
    snprintf(pattern, sizeof pattern, "%s/?.lua", s_dir);
    for (index = 0U; index < sizeof callers / sizeof callers[0]; ++index) {
        for (faulted = 0; faulted <= 1; ++faulted) {
            lua_State *L = new_state(1);
            unsigned fired0 = lfo_fault_fired();
            unsigned opens0 = lfo_kernel_opens();
            int status;

            lua_pushstring(L, path);
            lua_setglobal(L, "P");
            lua_getglobal(L, "package");
            lua_pushstring(L, pattern);
            lua_setfield(L, -2, "path");
            lua_pop(L, 1);
            reset_budget();
            lfo_fault_clear();
            if (faulted)
                lfo_fault_add("f1.lua", LFO_READ_AT, BUFSIZ + 1, EIO, 1U, 1);
            s_delays_used = 0U;
            s_delays[0] = '\0';
            if (luaL_loadstring(L, callers[index].code) != LUA_OK)
                abort();
            status = lua_pcall(L, 0, LUA_MULTRET, 0);
            snprintf(id, sizeof id, "callers/%s-%s", callers[index].name,
                     faulted ? "fault" : "clean");
            case_head(id, fired0, opens0);
            printf("status=%d ret=", status);
            if (status == LUA_OK) {
                put_values(L, 1);
                fputs(" msg=-\n", stdout);
            } else {
                fputs("- msg=", stdout);
                put_text(lua_tostring(L, -1));
                fputc('\n', stdout);
            }
            lua_close(L);
            lfo_fault_clear();
            report(id);
        }
    }
}

#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
static int s_oom_on_close;

static void oom_after_faulted_close(void)
{
    if (s_oom_on_close) {
        s_oom_on_close = 0;
        s_oom_left = 2; /* luaM_realloc_: the call, then after the emergency GC */
    }
}

static int stale_loader(lua_State *L)
{
    lua_pushinteger(L, luaL_loadfilex(L, lua_tostring(L, 1), NULL));
    return 1;
}

/* Runs the pcall a few KiB below the caller, so the next wrapped load (from
 * group_probe -> observe) is shallower than the dead arming frame -- the
 * case the frame-marker check detects.  A later load from a deeper frame
 * nests on the stale arm instead (see the README risk). */
static int stale_pcall_deep(lua_State *L, int depth)
{
    volatile char burn[2048];
    int status;

    burn[0] = (char)depth;
    status = depth > 0 ? stale_pcall_deep(L, depth - 1)
                       : lua_pcall(L, 1, 1, 0);
    return status + burn[0] - (char)depth;
}

/* The IO_INJECT build injects f1.lua's first sceIoRead on attempt 1. */
static void group_probe(void)
{
    char path[512];
    lua_State *L;
    int status;
    int round;
    int same_depth[2];
    uint32_t depth_after[2];

    fixture(path, sizeof path, "f1.lua");
    reset_budget();
    observe("probe/inject", path);

    fixture(path, sizeof path, "f2.lua");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f2.lua", LFO_READ_AT, BUFSIZ + 1, ENODEV, 1U, 1);
    observe("probe/kernel-enodev", path);
    lfo_fault_clear();

    fixture(path, sizeof path, "f7dir");
    reset_budget();
    observe("probe/isdir", path);

    /* A longjmp out of __real_luaL_loadfilex (LUA_ERRMEM in errfile's
     * lua_pushfstring) leaves the arm set; the next wrapped load, from a
     * shallower frame, must clear it. */
    fixture(path, sizeof path, "f3.lua");
    reset_budget();
    lfo_fault_clear();
    lfo_fault_add("f3.lua", LFO_READ_AT, 0, EIO, 1U, -1);
    lfo_set_close_hook(oom_after_faulted_close);
    L = new_state(0);
    lua_pushcfunction(L, stale_loader);
    lua_pushstring(L, path);
    s_oom_on_close = 1;
    status = stale_pcall_deep(L, 2);
    s_oom_left = 0;
    s_oom_on_close = 0;
    lfo_set_close_hook(NULL);
    lfo_fault_clear();
    printf("stale pcall=%d arm_depth=%u stale_clears=%u live=%u\n", status,
           (unsigned)isaac_vita_lua_loadfile_oracle_arm_depth(),
           (unsigned)isaac_vita_lua_loadfile_oracle_stale_clears(),
           lfo_kernel_live());
    lua_close(L);
    fixture(path, sizeof path, "f6.lua");
    observe("probe/after-stale", path);
    printf("stale after arm_depth=%u stale_clears=%u\n",
           (unsigned)isaac_vita_lua_loadfile_oracle_arm_depth(),
           (unsigned)isaac_vita_lua_loadfile_oracle_stale_clears());

    /* The same longjmp, then a clean load through the very same lua_pcall
     * call site: the second wrapper frame lands exactly in the dead arming
     * frame's slot (an address equal to the marker), as EID's next
     * pcall(require) does, and must clear the stale arm instead of nesting
     * on it. */
    L = new_state(0);
    for (round = 0; round < 2; ++round) {
        fixture(path, sizeof path, round == 0 ? "f3.lua" : "f2.lua");
        reset_budget();
        lfo_fault_clear();
        if (round == 0) {
            lfo_fault_add("f3.lua", LFO_READ_AT, 0, EIO, 1U, -1);
            lfo_set_close_hook(oom_after_faulted_close);
            s_oom_on_close = 1;
        }
        lua_pushcfunction(L, stale_loader);
        lua_pushstring(L, path);
        status = lua_pcall(L, 1, 1, 0);
        s_oom_left = 0;
        s_oom_on_close = 0;
        lfo_set_close_hook(NULL);
        lfo_fault_clear();
        same_depth[round] = status == LUA_OK ? (int)lua_tointeger(L, -1) : -status;
        lua_pop(L, 1);
        depth_after[round] = isaac_vita_lua_loadfile_oracle_arm_depth();
    }
    printf("stale same-depth first=%d arm_depth=%u second=%d arm_depth=%u "
           "stale_clears=%u live=%u\n", same_depth[0], (unsigned)depth_after[0],
           same_depth[1], (unsigned)depth_after[1],
           (unsigned)isaac_vita_lua_loadfile_oracle_stale_clears(),
           lfo_kernel_live());
    lua_close(L);
}
#endif

/* Writes f4.luac: f1.lua's chunk dumped with lua_dump (binary, strip=0). */
static int dump_file_writer(lua_State *L, const void *p, size_t size, void *ud)
{
    (void)L;
    return fwrite(p, 1, size, (FILE *)ud) == size ? 0 : 1;
}

static void group_prep(void)
{
    char path[512], out_path[512];
    lua_State *L = new_state(0);
    long size;
    char *text;
    FILE *in;
    FILE *out;

    fixture(path, sizeof path, "f1.lua");
    fixture(out_path, sizeof out_path, "f4.luac");
    size = file_size(path);
    text = (char *)malloc((size_t)size + 1U);
    in = fopen(path, "rb");
    if (!text || !in || fread(text, 1, (size_t)size, in) != (size_t)size)
        abort();
    fclose(in);
    if (luaL_loadbufferx(L, text, (size_t)size, "=f4", "t") != LUA_OK)
        abort();
    out = fopen(out_path, "wb");
    if (!out || lua_dump(L, dump_file_writer, out, 0) != 0 || fclose(out))
        abort();
    free(text);
    lua_close(L);
    printf("prep f4.luac size=%ld\n", file_size(out_path));
}

static void group_unit(void)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
    uint32_t attempt;

    fputs("unit delay_us=", stdout);
    for (attempt = 1U; attempt <= 8U; ++attempt)
        printf("%s%u", attempt == 1U ? "" : ",",
               (unsigned)isaac_vita_lua_loadfile_oracle_delay_us(attempt));
    fputc('\n', stdout);
#endif
}

int main(int argc, char **argv)
{
    const char *group;

    if (argc != 3)
        return 64;
    s_dir = argv[1];
    group = argv[2];
    printf("oracle bufsiz=%d eio=%d enodev=%d strerror0=", BUFSIZ, EIO,
           ENODEV);
    put_text(strerror(0));
    fputc('\n', stdout);
    if (strcmp(group, "prep") == 0)
        group_prep();
    else if (strcmp(group, "clean") == 0)
        group_clean();
    else if (strcmp(group, "transient") == 0)
        group_transient();
    else if (strcmp(group, "persistent") == 0)
        group_persistent();
    else if (strcmp(group, "budget") == 0)
        group_budget();
    else if (strcmp(group, "open") == 0)
        group_open();
    else if (strcmp(group, "changed") == 0)
        group_changed();
    else if (strcmp(group, "stdin") == 0)
        group_stdin();
    else if (strcmp(group, "callers") == 0)
        group_callers();
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
    else if (strcmp(group, "probe") == 0)
        group_probe();
#endif
    else if (strcmp(group, "unit") == 0)
        group_unit();
    else
        return 64;
    fflush(stdout);
    return 0;
}
