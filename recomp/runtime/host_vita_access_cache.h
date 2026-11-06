#ifndef ISAAC_VITA_ACCESS_CACHE_H
#define ISAAC_VITA_ACCESS_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact negative cache for the guest's _access probes below the mods root
 * (ISAAC_VITA_ACCESS_NEGATIVE_CACHE).  ModManager::TryRedirectPath probes
 * mods/<mod>/resources-dlc3/<path> and mods/<mod>/resources/<path> for every
 * new resource path and every enabled mod, nearly always negative.  Only
 * directories are remembered: a probe below a directory known absent cannot
 * exist, so it is answered without a stat.  Leaf files are never cached. */

/* What a directory probe found. */
enum {
    ISAAC_VITA_ACCESS_CACHE_UNKNOWN = -1, /* any other result: learn nothing */
    ISAAC_VITA_ACCESS_CACHE_ABSENT = 0,   /* ENOENT/ENOTDIR: nothing below it */
    ISAAC_VITA_ACCESS_CACHE_DIR = 1       /* exists and is a directory */
};

typedef int (*isaac_vita_access_cache_probe_fn)(const char *path);

typedef struct isaac_vita_access_cache_stats {
    uint32_t probes;        /* lookups of paths below the mods root */
    uint32_t hits;          /* answered from a remembered absent directory */
    uint32_t learned;       /* answered by probing an unknown ancestor */
    uint32_t dir_probes;    /* ancestor probes issued */
    uint32_t bypass;        /* below the root, but not a plain component path */
    uint32_t full;          /* absent directories not remembered: table full */
    uint32_t raced;         /* ... not remembered: invalidated meanwhile */
    uint32_t invalidations;
    uint32_t absent_live;   /* absent directories remembered now */
    uint32_t present_live;  /* existing directories remembered now */
} isaac_vita_access_cache_stats;

/* 1: `native_path` lies at or below a directory known (or now found) absent
 * and does not exist; the caller answers ENOENT without its own stat.
 * 0: stat the path as usual.  Unknown ancestors are probed through `probe`
 * (without the cache lock held) and remembered. */
int isaac_vita_access_cache_absent(const char *native_path,
                                   isaac_vita_access_cache_probe_fn probe);

/* After anything that may have created `native_path` (a file or a
 * directory).  Forgets every absent directory unless the path is provably
 * outside the mods root.  Call it once the creation has completed. */
void isaac_vita_access_cache_note_create(const char *native_path);

/* Forget every absent directory (a suspend: external edits are possible). */
void isaac_vita_access_cache_invalidate(void);

void isaac_vita_access_cache_snapshot(isaac_vita_access_cache_stats *stats);
void isaac_vita_access_cache_report(const char *why);

#ifdef __cplusplus
}
#endif

#endif
