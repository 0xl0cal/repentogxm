/* Host oracle for host_vita_launch_args.c: splitter cases, the loader's
 * receipts over a fake file (absent, io error, short reads, too large, too
 * many tokens, comments, CRLF), the --vita-save-slot= rule (accepted, no-slot,
 * bad-slot, duplicates, order, stripping) and the Manager+0x10 store over a
 * fake guest memory (waiting for both pointers, first store, idempotence,
 * restore after a foreign write, the restore log cap, Manager teardown).
 * Prints the outcome classes it reached; every class must be reached. */
#include "host_vita_launch_args.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_file;     /* NULL = absent */
static int g_open_error;       /* nonzero: open returns it */
static int g_read_error;       /* nonzero: second read returns it */
static unsigned g_chunk = 3u;  /* short reads */
static unsigned g_pos;
static int g_reads;
static char g_log[2048];
static unsigned g_logs;        /* isaac_vita_log calls */
static int g_failures;

/* Fake guest memory: a handful of 32-bit words by address. */
#define MEM_WORDS 16u
static uint32_t g_mem_addr[MEM_WORDS];
static uint32_t g_mem_val[MEM_WORDS];
static unsigned g_mem_used;
static unsigned g_loads;
static unsigned g_stores;
static uint32_t g_store_addr;
static uint32_t g_store_val;

/* Outcome classes (branch coverage of the code under test). */
enum {
    C_SPLIT_COMMENT, C_SPLIT_OVERCAP, C_ABSENT, C_IO_OPEN, C_IO_READ,
    C_TOO_LARGE, C_TOO_MANY, C_OK_NO_SLOT, C_OK_SLOT, C_NO_SLOT,
    C_BAD_VALUE, C_BAD_DUP, C_APPLY_NONE, C_APPLY_WAIT_MANAGER,
    C_APPLY_WAIT_GAME, C_APPLY_FIRST, C_APPLY_SAME, C_APPLY_RESTORE_LOGGED,
    C_APPLY_RESTORE_QUIET, C_APPLY_TEARDOWN, C_COUNT
};
static const char *const g_class_name[C_COUNT] = {
    "split-comment", "split-over-cap", "absent", "io-open", "io-read",
    "too-large", "too-many", "ok-no-slot", "ok-slot", "no-slot",
    "bad-slot-value", "bad-slot-dup", "apply-no-slot", "apply-wait-manager",
    "apply-wait-game", "apply-first", "apply-unchanged",
    "apply-restore-logged", "apply-restore-quiet", "apply-teardown"
};
static unsigned g_class[C_COUNT];

void isaac_vita_log(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsnprintf(g_log, sizeof g_log, format, ap);
    va_end(ap);
    ++g_logs;
}

int isaac_vita_launch_args_oracle_open(const char *path)
{
    if (strcmp(path, "ux0:/data/isaacr001/launch-args.txt") != 0)
        return (int)0x80010013u;
    if (g_open_error)
        return g_open_error;
    if (!g_file)
        return (int)0x80010002u;
    g_pos = 0u;
    g_reads = 0;
    return 7;
}

int isaac_vita_launch_args_oracle_read(int descriptor, void *buffer,
                                       unsigned size)
{
    unsigned left = (unsigned)strlen(g_file) - g_pos;
    unsigned n = left < g_chunk ? left : g_chunk;

    if (descriptor != 7)
        return -1;
    if (++g_reads == 2 && g_read_error)
        return g_read_error;
    if (n > size)
        n = size;
    memcpy(buffer, g_file + g_pos, n);
    g_pos += n;
    return (int)n;
}

int isaac_vita_launch_args_oracle_close(int descriptor)
{
    return descriptor == 7 ? 0 : -1;
}

static unsigned mem_find(uint32_t address)
{
    unsigned i;

    for (i = 0u; i < g_mem_used; ++i)
        if (g_mem_addr[i] == address)
            return i;
    return MEM_WORDS;
}

static void mem_set(uint32_t address, uint32_t value)
{
    unsigned i = mem_find(address);

    if (i == MEM_WORDS) {
        if (g_mem_used == MEM_WORDS) {
            printf("FAIL fake memory full\n");
            exit(2);
        }
        i = g_mem_used++;
        g_mem_addr[i] = address;
    }
    g_mem_val[i] = value;
}

static uint32_t mem_get(uint32_t address)
{
    unsigned i = mem_find(address);

    return i == MEM_WORDS ? 0u : g_mem_val[i];
}

uint32_t isaac_vita_launch_args_oracle_ld32(uint32_t address)
{
    ++g_loads;
    if (mem_find(address) == MEM_WORDS) {
        /* Only the two globals and Manager+0x10 may ever be read. */
        printf("FAIL load of unmapped guest word 0x%08x\n", (unsigned)address);
        ++g_failures;
    }
    return mem_get(address);
}

void isaac_vita_launch_args_oracle_st32(uint32_t address, uint32_t value)
{
    ++g_stores;
    g_store_addr = address;
    g_store_val = value;
    mem_set(address, value);
}

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n  log: %s\n", what, g_log);
        ++g_failures;
    }
}

static unsigned load(const char *file)
{
    char *out[ISAAC_VITA_LAUNCH_ARGS_MAX];

    g_file = file;
    return isaac_vita_launch_args_load("isaac-ng.exe", out);
}

/* load() with the tokens copied out for inspection. */
static unsigned load_tokens(const char *file, char tokens[][64])
{
    char *out[ISAAC_VITA_LAUNCH_ARGS_MAX];
    unsigned n, i;

    g_file = file;
    n = isaac_vita_launch_args_load("isaac-ng.exe", out);
    for (i = 0u; i < n; ++i) {
        size_t len = strlen(out[i]);

        if (len > 63u)
            len = 63u;
        memcpy(tokens[i], out[i], len);
        tokens[i][len] = '\0';
    }
    return n;
}

static int logged(const char *needle)
{
    return strstr(g_log, needle) != NULL;
}

#define MANAGER_GLOBAL (0x98000000u + ISAAC_VITA_LAUNCH_ARGS_MANAGER_RVA)
#define GAME_GLOBAL (0x98000000u + ISAAC_VITA_LAUNCH_ARGS_GAME_RVA)
#define MANAGER 0x81234000u
#define SLOT_FIELD (MANAGER + ISAAC_VITA_LAUNCH_ARGS_MANAGER_SLOT)
#define TRIGGER "ux0:/data/isaacr001/resources/stringtable.sta"

static void loader_cases(void)
{
    char text[] = "  --set-stage=1\t--set-stage-type=4 # tail comment --x\r\n"
                  "#full line\n--luadebug#glued";
    char *out[4];
    char big[ISAAC_VITA_LAUNCH_ARGS_BYTES + 8u];
    char tok[ISAAC_VITA_LAUNCH_ARGS_MAX][64];
    unsigned n;

    n = isaac_vita_launch_args_split(text, (unsigned)strlen(text), out, 4u);
    check(n == 3u, "split count");
    check(n == 3u && strcmp(out[0], "--set-stage=1") == 0 &&
              strcmp(out[1], "--set-stage-type=4") == 0 &&
              strcmp(out[2], "--luadebug") == 0, "split tokens");
    ++g_class[C_SPLIT_COMMENT];
    {
        char t2[] = "a b c d e";
        n = isaac_vita_launch_args_split(t2, (unsigned)strlen(t2), out, 2u);
        check(n == 5u && strcmp(out[1], "b") == 0, "split over cap");
        ++g_class[C_SPLIT_OVERCAP];
    }

    check(load(NULL) == 0u, "absent count");
    check(logged("status=absent") &&
              logged("slot=0 argc=1 cmdline=isaac-ng.exe"), "absent receipt");
    check(isaac_vita_launch_args_save_slot() == 0u, "absent slot");
    ++g_class[C_ABSENT];

    /* Without --set-stage= no slot is needed. */
    check(load("--luadebug --set-stage-type=4\n") == 2u, "ok count");
    check(logged("status=ok") &&
              logged("tokens=2 slot=0 argc=3 cmdline=isaac-ng.exe --luadebug "
                     "--set-stage-type=4"), "ok receipt");
    ++g_class[C_OK_NO_SLOT];

    g_chunk = 1000u;
    check(load("\xef\xbb\xbf") == 1u, "bom is not special (one token)");
    g_chunk = 3u;

    check(load("# nothing\n\n") == 0u, "comment-only count");
    check(logged("status=ok") && logged("tokens=0 slot=0 argc=1"),
          "comment-only receipt");

    check(load("a b c d e f g h i") == 0u, "too many count");
    check(logged("status=too-many") && logged("tokens=9 slot=0 argc=1"),
          "too many receipt");
    ++g_class[C_TOO_MANY];
    check(load("a b c d e f g h") == 8u, "exactly max");
    /* The slot token counts against the cap and is not honoured when the
     * file is rejected. */
    check(load("a b c d e f g h --vita-save-slot=2") == 0u &&
              logged("status=too-many") &&
              isaac_vita_launch_args_save_slot() == 0u, "too many with slot");

    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';
    check(load(big) == 0u, "too large count");
    check(logged("status=too-large"), "too large receipt");
    ++g_class[C_TOO_LARGE];
    big[ISAAC_VITA_LAUNCH_ARGS_BYTES] = '\0';
    check(load(big) == 1u, "exactly cap bytes");

    g_read_error = (int)0x80010005u;
    check(load("--set-stage=1 --set-stage-type=4 --vita-save-slot=2") == 0u,
          "read error count");
    check(logged("status=io-error io=0x80010005") &&
              isaac_vita_launch_args_save_slot() == 0u, "read error receipt");
    ++g_class[C_IO_READ];
    g_read_error = 0;

    g_open_error = (int)0x80010013u;
    check(load("--set-stage=1 --vita-save-slot=2") == 0u, "open error count");
    check(logged("status=io-error io=0x80010013"), "open error receipt");
    ++g_class[C_IO_OPEN];
    g_open_error = 0;

    /* The bench file: the slot token is consumed, the rest reaches argv. */
    n = load_tokens("# bench\n--set-stage=1 --set-stage-type=4 "
                    "--vita-save-slot=2\n", tok);
    check(n == 2u && strcmp(tok[0], "--set-stage=1") == 0 &&
              strcmp(tok[1], "--set-stage-type=4") == 0, "slot stripped");
    check(logged("status=ok") &&
              logged("tokens=3 slot=2 argc=3 cmdline=isaac-ng.exe "
                     "--set-stage=1 --set-stage-type=4") &&
              !logged("vita-save-slot"), "slot receipt");
    check(isaac_vita_launch_args_save_slot() == 2u, "slot value");
    ++g_class[C_OK_SLOT];

    /* Order does not matter; 1 and 3 are accepted too. */
    n = load_tokens("--vita-save-slot=3 --set-stage=1", tok);
    check(n == 1u && strcmp(tok[0], "--set-stage=1") == 0 &&
              isaac_vita_launch_args_save_slot() == 3u, "slot first");
    n = load_tokens("--set-stage=1 --vita-save-slot=1 --set-stage-type=4",
                    tok);
    check(n == 2u && strcmp(tok[1], "--set-stage-type=4") == 0 &&
              isaac_vita_launch_args_save_slot() == 1u, "slot middle");
    /* A slot alone (no run start) is accepted and harmless. */
    check(load("--vita-save-slot=2") == 0u && logged("status=ok") &&
              logged("tokens=1 slot=2 argc=1") &&
              isaac_vita_launch_args_save_slot() == 2u, "slot alone");
    /* The next load forgets the previous slot, whatever its outcome. */
    check(load("--luadebug") == 1u && isaac_vita_launch_args_save_slot() == 0u,
          "slot reset per load");
    (void)load("--vita-save-slot=2");
    check(load(NULL) == 0u && isaac_vita_launch_args_save_slot() == 0u,
          "slot reset by an absent file");
    (void)load("--vita-save-slot=2");
    check(load("a b c d e f g h i") == 0u &&
              isaac_vita_launch_args_save_slot() == 0u,
          "slot reset by a rejected file");

    /* Fail-safe: --set-stage= without a slot = menu boot. */
    check(load("--set-stage=1 --set-stage-type=4") == 0u &&
              logged("status=no-slot") && logged("tokens=2 slot=0 argc=1 "
                                                 "cmdline=isaac-ng.exe") &&
              isaac_vita_launch_args_save_slot() == 0u, "no slot");
    check(load("--luadebug --set-stage=7") == 0u && logged("status=no-slot"),
          "no slot, other value");
    ++g_class[C_NO_SLOT];

    {
        static const char *const bad[] = {
            "--set-stage=1 --vita-save-slot=0",
            "--set-stage=1 --vita-save-slot=4",
            "--set-stage=1 --vita-save-slot=22",
            "--set-stage=1 --vita-save-slot=",
            "--set-stage=1 --vita-save-slot=2x",
            "--vita-save-slot=-1",
            "--vita-save-slot=9",
        };
        unsigned i;

        for (i = 0u; i < sizeof bad / sizeof bad[0]; ++i) {
            check(load(bad[i]) == 0u && logged("status=bad-slot") &&
                      logged("slot=0 argc=1 cmdline=isaac-ng.exe") &&
                      isaac_vita_launch_args_save_slot() == 0u, bad[i]);
            ++g_class[C_BAD_VALUE];
        }
    }
    check(load("--set-stage=1 --vita-save-slot=2 --vita-save-slot=2") == 0u &&
              logged("status=bad-slot") &&
              isaac_vita_launch_args_save_slot() == 0u, "duplicate slot");
    check(load("--vita-save-slot=2 --vita-save-slot=3 --set-stage=1") == 0u &&
              logged("status=bad-slot"), "conflicting slots");
    ++g_class[C_BAD_DUP];
    /* A prefix lookalike is an ordinary guest token. */
    check(load("--vita-save-slotx=2 --luadebug") == 2u && logged("status=ok"),
          "lookalike token");
}

static void apply_cases(void)
{
    unsigned i, before_logs, before_loads;

    /* No slot: nothing is even read. */
    isaac_vita_launch_args_oracle_reset();
    (void)load("--set-stage-type=4");
    g_mem_used = 0u;
    g_loads = g_stores = 0u;
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            TRIGGER) == 0 &&
              g_loads == 0u && g_stores == 0u, "apply without slot");
    ++g_class[C_APPLY_NONE];

    /* Slot 2, Manager not constructed yet. */
    isaac_vita_launch_args_oracle_reset();
    check(load("--set-stage=1 --set-stage-type=4 --vita-save-slot=2") == 2u,
          "bench load");
    mem_set(MANAGER_GLOBAL, 0u);
    mem_set(GAME_GLOBAL, 0u);
    mem_set(SLOT_FIELD, 0xdeadbeefu);
    g_stores = 0u;
    before_logs = g_logs;
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            TRIGGER) == 0 &&
              g_stores == 0u && g_logs == before_logs, "wait for manager");
    ++g_class[C_APPLY_WAIT_MANAGER];

    /* Manager stored (0050b973), Manager::Init not past 004ad841 yet: the
     * slot is still to be cleared, so writing now would be undone. */
    mem_set(MANAGER_GLOBAL, MANAGER);
    for (i = 0u; i < 3u; ++i) {
        check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                                TRIGGER) == 0 &&
                  g_stores == 0u, "wait for game");
        ++g_class[C_APPLY_WAIT_GAME];
    }
    /* Init cleared it (004ad7fa) and stored the Game pointer (004ad841). */
    mem_set(SLOT_FIELD, 0u);
    mem_set(GAME_GLOBAL, 0x82000000u);
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            TRIGGER) == 1 &&
              g_stores == 1u && g_store_addr == SLOT_FIELD &&
              g_store_val == 2u && mem_get(SLOT_FIELD) == 2u, "first store");
    check(logged("ISAAC VITA LAUNCH SAVE SLOT: event=applied slot=2 "
                 "was=0x00000000 manager=0x81234000 waited=4 trigger="
                 TRIGGER), "applied receipt");
    ++g_class[C_APPLY_FIRST];

    /* Unchanged: one load of each word, no store, no log. */
    before_logs = g_logs;
    for (i = 0u; i < 5u; ++i) {
        before_loads = g_loads;
        check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                                "x") == 0 &&
                  g_stores == 1u && g_loads - before_loads == 2u,
              "unchanged slot");
        ++g_class[C_APPLY_SAME];
    }
    check(g_logs == before_logs, "unchanged is silent");

    /* A foreign write (e.g. a menu loading File 1) is undone, loudly. */
    mem_set(SLOT_FIELD, 1u);
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            "ux0:/x/save1.dat") == 1 &&
              mem_get(SLOT_FIELD) == 2u && g_stores == 2u, "restore");
    check(logged("event=restored slot=2 was=0x00000001 manager=0x81234000 "
                 "restored=1 trigger=ux0:/x/save1.dat"), "restored receipt");
    ++g_class[C_APPLY_RESTORE_LOGGED];
    /* i = the restore count; the stored value never equals the slot. */
    for (i = 2u; i <= ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX + 3u; ++i) {
        before_logs = g_logs;
        mem_set(SLOT_FIELD, 100u + i);
        check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                                "y") == 1 &&
                  mem_get(SLOT_FIELD) == 2u, "repeated restore");
        if (i <= ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX) {
            check(g_logs == before_logs + 1u, "restore logged");
            ++g_class[C_APPLY_RESTORE_LOGGED];
        } else {
            check(g_logs == before_logs, "restore log capped");
            ++g_class[C_APPLY_RESTORE_QUIET];
        }
    }

    mem_set(SLOT_FIELD, 0u);
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            NULL) == 1 &&
              mem_get(SLOT_FIELD) == 2u, "restore without trigger");
    ++g_class[C_APPLY_RESTORE_QUIET];

    /* Teardown (0050c3a6 stores 0 into the Manager pointer). */
    mem_set(MANAGER_GLOBAL, 0u);
    before_loads = g_stores;
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            "z") == 0 &&
              g_stores == before_loads, "teardown");
    ++g_class[C_APPLY_TEARDOWN];

    /* The first store reports a garbage previous value verbatim. */
    isaac_vita_launch_args_oracle_reset();
    (void)load("--set-stage=1 --vita-save-slot=3");
    mem_set(MANAGER_GLOBAL, MANAGER);
    mem_set(GAME_GLOBAL, 0x82000000u);
    mem_set(SLOT_FIELD, 0xdeadbeefu);
    check(isaac_vita_launch_args_slot_apply(MANAGER_GLOBAL, GAME_GLOBAL,
                                            NULL) == 1 &&
              mem_get(SLOT_FIELD) == 3u &&
              logged("slot=3 was=0xdeadbeef manager=0x81234000 waited=0 "
                     "trigger=(null)"), "garbage before first store");
    ++g_class[C_APPLY_FIRST];
}

int main(void)
{
    unsigned i;
    int unreached = 0;

    loader_cases();
    apply_cases();
    printf("coverage:");
    for (i = 0u; i < C_COUNT; ++i) {
        printf(" %s=%u", g_class_name[i], g_class[i]);
        if (g_class[i] == 0u)
            unreached = 1;
    }
    printf("\n");
    if (unreached) {
        printf("FAIL an outcome class was not reached\n");
        ++g_failures;
    }
    if (g_failures) {
        printf("launch args oracle: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("launch args oracle: PASS\n");
    return 0;
}
