/* Host oracle for ISAAC_VITA_ACCESS_NEGATIVE_CACHE (host_vita_access_cache.c).
 *
 * Includes the module so its tables can be reset between cases.  The pinned
 * cases drive a fake directory set; the randomized case runs against a real
 * directory tree (the data root mapped below a temporary directory) and
 * compares every probe answer with stat(), across random file and directory
 * creations inside and outside mods/ that are reported through
 * isaac_vita_access_cache_note_create exactly as the CRT and
 * CreateDirectoryA do. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "host_vita_access_cache.c"

#if defined(ACCESS_ORACLE_MUTANT_NO_INVALIDATE)
/* Proof that the randomized run can fail: creations are never reported. */
#define isaac_vita_access_cache_note_create(path) ((void)(path))
#endif

#define ROOT ISAAC_VITA_NATIVE_DATA_ROOT
#define MODS ROOT "/mods/"

static char s_log[512];
static unsigned s_probe_calls;
static const char *const *s_dirs;
static const char *const *s_unknown;
static int s_invalidate_during_probe;

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "access cache oracle failed at line %u: %s\n", line,
            expression);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_log, sizeof s_log, format, arguments);
    va_end(arguments);
}

static int listed(const char *const *list, const char *path)
{
    for (; list && *list; ++list)
        if (strcmp(*list, path) == 0)
            return 1;
    return 0;
}

/* Directories in s_dirs exist; s_unknown answer "other error"; the rest is
 * absent. */
static int fake_probe(const char *path)
{
    ++s_probe_calls;
    if (s_invalidate_during_probe) {
        s_invalidate_during_probe = 0;
        isaac_vita_access_cache_invalidate();
    }
    if (listed(s_unknown, path))
        return ISAAC_VITA_ACCESS_CACHE_UNKNOWN;
    return listed(s_dirs, path) ? ISAAC_VITA_ACCESS_CACHE_DIR
                                : ISAAC_VITA_ACCESS_CACHE_ABSENT;
}

static void reset_cache(void)
{
    memset(s_absent, 0, sizeof s_absent);
    memset(s_present, 0, sizeof s_present);
    s_absent_count = 0U;
    s_present_count = 0U;
    s_generation = 0U;
    memset(&s_stats, 0, sizeof s_stats);
    s_probe_calls = 0U;
    s_dirs = NULL;
    s_unknown = NULL;
    s_invalidate_during_probe = 0;
}

static int absent(const char *path)
{
    return isaac_vita_access_cache_absent(path, fake_probe);
}

static const char *const s_tree[] = {
    MODS "repro_a", MODS "eid", MODS "eid/resources",
    MODS "eid/resources/gfx", MODS "eid/resources/font", NULL
};

/* A mod without resources directories collapses to one remembered fact;
 * the equality case and a sibling with a longer name are kept apart. */
static int test_absent_directory(void)
{
    reset_cache();
    s_dirs = s_tree;
    CHECK(!absent(ROOT "/resources/gfx/a.png") && s_stats.probes == 0U);
    CHECK(absent(MODS "repro_a/resources-dlc3/gfx/shadow.png"));
    CHECK(s_probe_calls == 2U && s_stats.learned == 1U &&
          s_absent_count == 1U && s_present_count == 1U);
    CHECK(absent(MODS "repro_a/resources-dlc3/gfx/grid/lava.png"));
    CHECK(absent(MODS "repro_a/resources-dlc3"));
    CHECK(s_probe_calls == 2U && s_stats.hits == 2U);
    CHECK(absent(MODS "repro_a/resources-dlc3x/a.png"));
    CHECK(s_probe_calls == 3U && s_absent_count == 2U);
    CHECK(absent(MODS "repro_a/resources/gfx/a.png"));
    CHECK(s_probe_calls == 4U && s_absent_count == 3U);
    return 0;
}

/* Existing directories are probed once; leaves in them are always the
 * caller's stat; an absent subdirectory is remembered. */
static int test_existing_directories(void)
{
    reset_cache();
    s_dirs = s_tree;
    CHECK(!absent(MODS "eid/resources/gfx/shadow.png"));
    CHECK(s_probe_calls == 3U && s_present_count == 3U);
    CHECK(!absent(MODS "eid/resources/gfx/eid_icons.png"));
    CHECK(!absent(MODS "eid/resources/backdrops.xml"));
    CHECK(!absent(MODS "eid/resources"));
    CHECK(!absent(MODS "eid"));
    CHECK(s_probe_calls == 3U && s_absent_count == 0U);
    CHECK(absent(MODS "eid/resources/gfx/grid/lava.png"));
    CHECK(absent(MODS "eid/resources/gfx/grid/lava_cold.png"));
    CHECK(s_probe_calls == 4U && s_stats.hits == 1U);
    CHECK(!absent(MODS "eid/resources/gfx/gridx.png"));
    CHECK(s_probe_calls == 4U);
    return 0;
}

/* Other errors teach nothing; odd path forms are not cached at all. */
static int test_unknown_and_bypass(void)
{
    static const char *const unknown[] = { MODS "eid/locked", NULL };

    reset_cache();
    s_dirs = s_tree;
    s_unknown = unknown;
    CHECK(!absent(MODS "eid/locked/a.png"));
    CHECK(!absent(MODS "eid/locked/a.png"));
    CHECK(s_probe_calls == 3U && s_absent_count == 0U);
    s_probe_calls = 0U;
    CHECK(!absent(MODS "eid//metadata.xml"));
    CHECK(!absent(MODS "eid/./x/a.png"));
    CHECK(!absent(MODS "eid/../x/a.png"));
    CHECK(!absent(MODS "eid/x/"));
    CHECK(!absent(MODS "eid\\x/a.png"));
    CHECK(!absent(MODS));
    CHECK(s_probe_calls == 0U && s_stats.bypass == 6U);
    return 0;
}

/* Only a creation that may reach mods/ forgets; existing directories stay. */
static int test_invalidation(void)
{
    static const char *const created[] = {
        MODS "repro_a", MODS "repro_a/resources-dlc3",
        MODS "repro_a/resources-dlc3/gfx", NULL
    };

    reset_cache();
    s_dirs = s_tree;
    CHECK(absent(MODS "repro_a/resources-dlc3/gfx/a.png"));
    isaac_vita_access_cache_note_create(
        ROOT "/Documents/My Games/Binding of Isaac Repentance/gamestate1.dat");
    isaac_vita_access_cache_note_create(ROOT "/data/repro_console/save1.dat");
    isaac_vita_access_cache_note_create(ROOT "/modsx/a");
    isaac_vita_access_cache_note_create(ROOT "/mod/a");
    isaac_vita_access_cache_note_create(ROOT "//Documents/x");
    CHECK(s_absent_count == 1U && s_stats.invalidations == 0U);
    CHECK(absent(MODS "repro_a/resources-dlc3/gfx/b.png") &&
          s_stats.hits == 1U);

    /* The directory now exists and is reported: the next probe sees it. */
    s_dirs = created;
    isaac_vita_access_cache_note_create(MODS "repro_a/resources-dlc3");
    CHECK(s_absent_count == 0U && s_stats.invalidations == 1U &&
          s_present_count == 1U);
    s_probe_calls = 0U;
    CHECK(!absent(MODS "repro_a/resources-dlc3/gfx/b.png"));
    CHECK(s_probe_calls == 2U && s_present_count == 3U);

    /* Spellings that might reach mods/ all forget. */
    {
        static const char *const reach[] = {
            ROOT "/MODS/x", ROOT "/Mods/x/y.dat", ROOT "/./mods/x",
            ROOT "//mods", ROOT "/mods", ROOT "/", ROOT,
            ISAAC_VITA_NATIVE_DATA_PARENT, "app0:/x", NULL
        };
        const char *const *p;
        uint32_t before = s_stats.invalidations;

        for (p = reach; *p; ++p)
            isaac_vita_access_cache_note_create(*p);
        isaac_vita_access_cache_note_create(NULL);
        CHECK(s_stats.invalidations == before + 10U);
    }
    return 0;
}

/* A creation noted while an ancestor probe ran: the answer stands (as of
 * the probe), the fact is not remembered. */
static int test_race(void)
{
    reset_cache();
    s_dirs = s_tree;
    CHECK(!absent(MODS "repro_a/x"));
    s_invalidate_during_probe = 1;
    CHECK(absent(MODS "repro_a/resources-dlc3/a.png"));
    CHECK(s_stats.raced == 1U && s_absent_count == 0U);
    CHECK(absent(MODS "repro_a/resources-dlc3/a.png"));
    CHECK(s_absent_count == 1U && s_probe_calls == 3U);
    return 0;
}

/* Full tables stay exact: an absent directory past the bound is answered
 * but not remembered, an unknown ancestor past the existing-directory bound
 * is not probed, and a directory longer than the entry bound is the
 * caller's. */
static int test_bounds(void)
{
    static const char *const one[] = { MODS "m", NULL };
    char path[256];
    char longname[ACCESS_SUFFIX_CAPACITY + 2U];
    unsigned i;

    reset_cache();
    s_dirs = one;
    for (i = 0U; i < ACCESS_ABSENT_SLOTS; ++i) {
        (void)snprintf(path, sizeof path, MODS "m/d%03u/a.png", i);
        CHECK(absent(path));
    }
    CHECK(s_absent_count == ACCESS_ABSENT_SLOTS && s_stats.full == 0U);
    CHECK(absent(MODS "m/extra/a.png") && s_stats.full == 1U);
    s_probe_calls = 0U;
    CHECK(absent(MODS "m/extra/b.png") && s_probe_calls == 1U);
    CHECK(absent(MODS "m/d127/b.png") && s_probe_calls == 1U);

    reset_cache();
    s_dirs = one;
    for (i = 0U; i < ACCESS_PRESENT_SLOTS; ++i)
        access_entry_set(&s_present[s_present_count++], "pp", 2U);
    CHECK(!absent(MODS "m/x/a.png") && s_probe_calls == 0U);

    reset_cache();
    memset(longname, 'n', sizeof longname - 1U);
    longname[sizeof longname - 1U] = '\0';
    (void)snprintf(path, sizeof path, MODS "%s/a.png", longname);
    CHECK(!absent(path) && s_probe_calls == 0U);
    return 0;
}

static int test_report(void)
{
    reset_cache();
    s_dirs = s_tree;
    CHECK(absent(MODS "repro_a/resources-dlc3/a.png"));
    CHECK(absent(MODS "repro_a/resources-dlc3/b.png"));
    isaac_vita_access_cache_report("oracle");
    CHECK(strcmp(s_log,
                 "KAGE VITA ACCESS CACHE: why=oracle probes=2 hits=1 "
                 "learned=1 dir_probes=2 absent=1 present=1 bypass=0 "
                 "full=0 raced=0 inval=0") == 0);
    return 0;
}

/* ---- randomized exactness against a real tree ------------------------- */

static char s_host_root[1024];

static void host_path(const char *native, char *out, size_t capacity)
{
    (void)snprintf(out, capacity, "%s%s", s_host_root,
                   native + strlen(ROOT));
}

static int real_probe(const char *path)
{
    char host[1400];
    struct stat status;

    ++s_probe_calls;
    host_path(path, host, sizeof host);
    if (stat(host, &status) == 0)
        return S_ISDIR(status.st_mode) ? ISAAC_VITA_ACCESS_CACHE_DIR
                                       : ISAAC_VITA_ACCESS_CACHE_UNKNOWN;
    return errno == ENOENT || errno == ENOTDIR
        ? ISAAC_VITA_ACCESS_CACHE_ABSENT : ISAAC_VITA_ACCESS_CACHE_UNKNOWN;
}

static uint32_t s_random = 0x5eed0003U;

static uint32_t next_random(void)
{
    uint32_t x = s_random;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random = x ? x : 0x9e3779b9U;
    return s_random;
}

static void random_native(char *out, size_t capacity)
{
    static const char *const tops[] = { "mods", "mods", "mods", "Documents", "data" };
    static const char *const names[] = {
        "m0", "m1", "resources", "resources-dlc3", "gfx", "grid", "a.png",
        "b.xml"
    };
    unsigned depth = 1U + next_random() % 5U;
    size_t used;
    unsigned i;

    used = (size_t)snprintf(out, capacity, "%s/%s", ROOT,
                            tops[next_random() % 5U]);
    for (i = 0U; i < depth && used < capacity; ++i)
        used += (size_t)snprintf(out + used, capacity - used, "/%s",
                                 names[next_random() % 8U]);
}

static int test_random_exact(const char *directory, unsigned operations)
{
    char native[256];
    char host[1400];
    unsigned op;
    unsigned answered = 0U;
    unsigned creations = 0U;

    reset_cache();
    (void)snprintf(s_host_root, sizeof s_host_root, "%s/access-root",
                   directory);
    if (mkdir(s_host_root, 0777) != 0 && errno != EEXIST)
        return fail(__LINE__, "mkdir root");
    host_path(MODS, host, sizeof host);
    (void)mkdir(host, 0777);
    for (op = 0U; op < operations; ++op) {
        unsigned kind = next_random() % 20U;
        struct stat status;

        random_native(native, sizeof native);
        host_path(native, host, sizeof host);
        if (kind < 17U) {
            int cached = isaac_vita_access_cache_absent(native, real_probe);
            int exists = stat(host, &status) == 0;

            if (cached && exists)
                return fail(__LINE__, native);
            answered += (unsigned)cached;
        } else if (kind < 19U) {
            /* A directory, as CreateDirectoryA: noted after success. */
            if (mkdir(host, 0777) == 0) {
                isaac_vita_access_cache_note_create(native);
                ++creations;
            }
        } else {
            /* A file, as a writing fopen: noted after the open. */
            FILE *stream = fopen(host, "w");

            if (stream) {
                (void)fclose(stream);
                isaac_vita_access_cache_note_create(native);
                ++creations;
            }
        }
        if (next_random() % 997U == 0U)
            isaac_vita_access_cache_invalidate();
    }
    printf("access cache oracle random: ops=%u answered=%u creations=%u "
           "hits=%u learned=%u dir_probes=%u inval=%u\n",
           operations, answered, creations, (unsigned)s_stats.hits,
           (unsigned)s_stats.learned, (unsigned)s_stats.dir_probes,
           (unsigned)s_stats.invalidations);
    /* Exercised, not tuned: exactness is the per-probe check above. */
    CHECK(answered > 0U && s_stats.hits > 0U && creations > 0U);
    return 0;
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    unsigned operations = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10)
                                   : 20000U;

    if (argc > 3 && strcmp(argv[3], "random") == 0)
        return test_random_exact(directory, operations);
    if (test_absent_directory() || test_existing_directories() ||
        test_unknown_and_bypass() || test_invalidation() || test_race() ||
        test_bounds() || test_report() ||
        test_random_exact(directory, operations))
        return 1;
    puts("access cache oracle: PASS");
    return 0;
}
