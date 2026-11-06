/* Exact negative cache for _access probes below the mods root.
 *
 * Measured (hotprof 2026-10-12): 769 of 1,708 stall samples were
 * _access probes from ModManager::TryRedirectPath -> GetExpandedPath, two per
 * enabled mod for every new resource path.  The game already caches each
 * resolved path, so a leaf is probed about once per session and a leaf cache
 * would never hit; what repeats is the directory: mods/<mod>/resources-dlc3
 * that does not exist, or mods/<mod>/resources/gfx/<subdir> that does not.
 *
 * Exactness.  A remembered fact "D does not exist" (sceIoGetstat ENOENT or
 * ENOTDIR) implies that D itself and every path textually below D + "/" do
 * not exist, which the guest observes as -1/ENOENT whatever native code a
 * real stat would return (host_vita_filesystem.c maps ENOENT, ENOTDIR,
 * EINVAL, ENAMETOOLONG and unproven codes to ENOENT).  The fact only turns
 * false when something creates D; every in-process creator under the data
 * root (guest fopen in a writing mode, CreateDirectoryA) calls
 * isaac_vita_access_cache_note_create after the creation, which forgets
 * every absent directory unless the path is provably outside the mods root.
 * A descriptor recovery (a suspend) forgets them too.  Facts learned while an
 * invalidation raced are not remembered.  Existing directories are only an
 * optimisation for the ancestor walk: the leaf below them is always statted,
 * so a stale one can only cost a cache hit, never an answer.  Not covered:
 * edits by another process (FTP plugin) while the game runs, which the
 * game's own per-path redirect cache does not observe either. */
#include "host_vita_access_cache.h"

#include <stddef.h>
#include <string.h>

#include "platform.h"

#define ACCESS_DATA_ROOT ISAAC_VITA_NATIVE_DATA_ROOT "/"
#define ACCESS_MODS_ROOT ISAAC_VITA_NATIVE_DATA_ROOT "/mods/"

enum {
    ACCESS_ABSENT_SLOTS = 128U,
    ACCESS_PRESENT_SLOTS = 64U,
    /* Longest remembered directory below the mods root, in bytes. */
    ACCESS_SUFFIX_CAPACITY = 80U
};

typedef struct access_entry {
    uint32_t length;
    char suffix[ACCESS_SUFFIX_CAPACITY];
} access_entry;

static access_entry s_absent[ACCESS_ABSENT_SLOTS];
static access_entry s_present[ACCESS_PRESENT_SLOTS];
static uint32_t s_absent_count;
static uint32_t s_present_count;
static uint32_t s_generation;
static isaac_vita_access_cache_stats s_stats;
static volatile uint32_t s_lock;

static void access_lock(void)
{
    while (__sync_lock_test_and_set(&s_lock, 1U)) {
        while (s_lock) {
        }
    }
    __sync_synchronize();
}

static void access_unlock(void)
{
    __sync_synchronize();
    __sync_lock_release(&s_lock);
}

/* The part below the mods root when it is a plain component path (no empty,
 * "." or ".." component, no backslash, no trailing slash): only then is
 * every ancestor a textual prefix ending before a '/'. */
static int access_plain_suffix(const char *suffix)
{
    const char *p = suffix;

    for (;;) {
        const char *start = p;
        size_t length;

        while (*p && *p != '/') {
            if (*p == '\\')
                return 0;
            ++p;
        }
        length = (size_t)(p - start);
        if (length == 0U || (length == 1U && start[0] == '.') ||
            (length == 2U && start[0] == '.' && start[1] == '.'))
            return 0;
        if (!*p)
            return 1;
        ++p;
    }
}

/* Caller owns s_lock.  A remembered absent directory equal to the first
 * `length` bytes of `suffix` or to a component prefix of them. */
static int access_absent_covers(const char *suffix, size_t length)
{
    uint32_t i;

    for (i = 0U; i < s_absent_count; ++i) {
        const access_entry *entry = &s_absent[i];

        if (entry->length <= length &&
            memcmp(entry->suffix, suffix, entry->length) == 0 &&
            (entry->length == length || suffix[entry->length] == '/'))
            return 1;
    }
    return 0;
}

/* Caller owns s_lock. */
static int access_table_has(const access_entry *table, uint32_t count,
                            const char *suffix, size_t length)
{
    uint32_t i;

    for (i = 0U; i < count; ++i) {
        if (table[i].length == length &&
            memcmp(table[i].suffix, suffix, length) == 0)
            return 1;
    }
    return 0;
}

static void access_entry_set(access_entry *entry, const char *suffix,
                             size_t length)
{
    memcpy(entry->suffix, suffix, length);
    entry->length = (uint32_t)length;
}

int isaac_vita_access_cache_absent(const char *native_path,
                                   isaac_vita_access_cache_probe_fn probe)
{
    static const char root[] = ACCESS_MODS_ROOT;
    char ancestor[sizeof root - 1U + ACCESS_SUFFIX_CAPACITY + 1U];
    const char *suffix;
    size_t length;
    size_t cut = 0U;
    uint32_t generation;

    if (!native_path || strncmp(native_path, root, sizeof root - 1U) != 0)
        return 0;
    suffix = native_path + sizeof root - 1U;
    length = strlen(suffix);
    access_lock();
    ++s_stats.probes;
    if (!access_plain_suffix(suffix)) {
        ++s_stats.bypass;
        access_unlock();
        return 0;
    }
    if (access_absent_covers(suffix, length)) {
        ++s_stats.hits;
        access_unlock();
        return 1;
    }
    generation = s_generation;
    access_unlock();

    memcpy(ancestor, root, sizeof root - 1U);
    /* Proper ancestors only, from the mod directory down; the leaf is the
     * caller's stat. */
    for (;;) {
        const char *slash = memchr(suffix + cut, '/', length - cut);
        int known;
        int state;

        if (!slash)
            return 0;
        cut = (size_t)(slash - suffix);
        if (cut >= ACCESS_SUFFIX_CAPACITY)
            return 0;
        access_lock();
        known = access_table_has(s_present, s_present_count, suffix, cut);
        /* No room to remember another existing directory: probing it now
         * would be repeated on every later path below it. */
        if (!known && s_present_count == ACCESS_PRESENT_SLOTS) {
            access_unlock();
            return 0;
        }
        if (!known)
            ++s_stats.dir_probes;
        access_unlock();
        if (!known) {
            memcpy(ancestor + sizeof root - 1U, suffix, cut);
            ancestor[sizeof root - 1U + cut] = '\0';
            state = probe(ancestor);
            access_lock();
            if (state == ISAAC_VITA_ACCESS_CACHE_DIR) {
                if (s_present_count < ACCESS_PRESENT_SLOTS &&
                    !access_table_has(s_present, s_present_count, suffix, cut))
                    access_entry_set(&s_present[s_present_count++],
                                     suffix, cut);
            } else if (state == ISAAC_VITA_ACCESS_CACHE_ABSENT) {
                /* Answered as of the probe either way; remembered only if
                 * no creation was noted since the walk began. */
                ++s_stats.learned;
                if (generation != s_generation)
                    ++s_stats.raced;
                else if (s_absent_count == ACCESS_ABSENT_SLOTS)
                    ++s_stats.full;
                else if (!access_absent_covers(suffix, cut))
                    access_entry_set(&s_absent[s_absent_count++],
                                     suffix, cut);
                access_unlock();
                return 1;
            } else {
                access_unlock();
                return 0;
            }
            access_unlock();
        }
        ++cut;
    }
}

/* A path below the data root whose first real component is not "mods"
 * (ASCII case-insensitive: ux0: is case-insensitive) cannot create
 * anything below the mods root.  Anything else might. */
static int access_outside_mods(const char *path)
{
    static const char root[] = ACCESS_DATA_ROOT;
    static const char mods[] = "mods";
    const char *p;
    size_t length;
    size_t i;

    if (strncmp(path, root, sizeof root - 1U) != 0)
        return 0;
    p = path + sizeof root - 1U;
    for (;;) {
        while (*p == '/')
            ++p;
        if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) {
            ++p;
            continue;
        }
        break;
    }
    length = strcspn(p, "/");
    if (length != sizeof mods - 1U)
        return length != 0U;
    for (i = 0U; i < length; ++i) {
        char c = p[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != mods[i])
            return 1;
    }
    return 0;
}

void isaac_vita_access_cache_invalidate(void)
{
    access_lock();
    ++s_generation;
    s_absent_count = 0U;
    ++s_stats.invalidations;
    access_unlock();
}

void isaac_vita_access_cache_note_create(const char *native_path)
{
    if (native_path && access_outside_mods(native_path))
        return;
    isaac_vita_access_cache_invalidate();
}

void isaac_vita_access_cache_snapshot(isaac_vita_access_cache_stats *stats)
{
    access_lock();
    *stats = s_stats;
    stats->absent_live = s_absent_count;
    stats->present_live = s_present_count;
    access_unlock();
}

/* One bounded counter line; never under s_lock while logging. */
void isaac_vita_access_cache_report(const char *why)
{
    isaac_vita_access_cache_stats stats;

    isaac_vita_access_cache_snapshot(&stats);
    isaac_vita_log(
        "KAGE VITA ACCESS CACHE: why=%s probes=%u hits=%u learned=%u "
        "dir_probes=%u absent=%u present=%u bypass=%u full=%u raced=%u "
        "inval=%u",
        why, (unsigned)stats.probes, (unsigned)stats.hits,
        (unsigned)stats.learned, (unsigned)stats.dir_probes,
        (unsigned)stats.absent_live, (unsigned)stats.present_live,
        (unsigned)stats.bypass, (unsigned)stats.full, (unsigned)stats.raced,
        (unsigned)stats.invalidations);
}
