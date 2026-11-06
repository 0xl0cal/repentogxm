/*
 * Manager UI2 game skin (design variant B: the game's own menu art;
 * knob ISAAC_VITA_MANAGER_UI2_GAME_SKIN ->
 * MANAGER_UI2_GAME_SKIN=1).
 *
 * The game theme of manager_theme.h, drawn with the game's own menu art and
 * Team Meat fonts read at run time from the user's
 * ux0:data/isaacr001/resources/packed archives (nothing of the game is
 * packaged).  manager_skin_game_init() is the hook manager_theme_select()
 * calls the first time `game` is selected; on any failure (archives
 * missing, a row absent on another game revision, a decode error, a sheet
 * of an unexpected size, the arena too small) it releases everything,
 * leaves *theme untouched and returns nonzero, and the theme set falls back
 * to `light` with a status line (never an error dialog).
 *
 * The context is what the caller provides: the blob I/O the archives are
 * read through, the packed directory, and one memory block requested once
 * per init through acquire() and handed back through release() when the
 * init fails (on success the skin keeps it for the life of the process).
 * The skin carves every buffer out of that block - persistent data from its
 * top, transient decode buffers as a stack from its bottom - and never
 * calls an allocator itself.  manager_main.c backs it with a USER_RW
 * memblock; the host tests back it with malloc plus guard words.
 *
 * SDK-free.
 */
#ifndef ISAAC_MANAGER_SKIN_GAME_H
#define ISAAC_MANAGER_SKIN_GAME_H

#if defined(MANAGER_UI2) && defined(MANAGER_UI2_GAME_SKIN)

#include "manager_blob.h"
#include "manager_theme.h"

#include <stddef.h>
#include <stdint.h>

/* Arena the skin requests when the context leaves arena_bytes at 0.  The
 * measured high-water mark on the stock Repentance archives is reported in
 * manager_skin_game_context.arena_peak; the rest is
 * headroom for larger archive indexes of other game revisions. */
#define MANAGER_SKIN_GAME_ARENA_BYTES (3u * 1024u * 1024u)

/* Failure stages (manager_skin_game_context.stage). */
enum manager_skin_game_stage {
    MANAGER_SKIN_GAME_OK = 0,
    MANAGER_SKIN_GAME_STAGE_CONTEXT,  /* NULL context / io / acquire */
    MANAGER_SKIN_GAME_STAGE_MEMORY,   /* acquire refused, arena too small */
    MANAGER_SKIN_GAME_STAGE_ARCHIVE,  /* no archive opened */
    MANAGER_SKIN_GAME_STAGE_FIND,     /* a file is not in any archive */
    MANAGER_SKIN_GAME_STAGE_READ,     /* manager_arch_read failed */
    MANAGER_SKIN_GAME_STAGE_PNG,      /* probe/decode failed */
    MANAGER_SKIN_GAME_STAGE_GEOMETRY, /* sheet or page of an unexpected size */
    MANAGER_SKIN_GAME_STAGE_FONT      /* .fnt parse / page attach failed */
};

struct manager_skin_game_context {
    /* inputs */
    const struct manager_blob_io *io;
    const char *packed_dir; /* NULL: MANAGER_ARCH_PACKED_DIR */
    void *(*acquire)(void *opaque, size_t bytes);
    void (*release)(void *opaque, void *block, size_t bytes);
    void *opaque;
    uint64_t (*clock_us)(void *opaque); /* optional, for decode_us */
    size_t arena_bytes;                 /* 0: MANAGER_SKIN_GAME_ARENA_BYTES */
    /* results of the last manager_skin_game_init() */
    int stage;            /* enum manager_skin_game_stage */
    int status;           /* the decoder's status code at that stage */
    uint32_t decode_us;   /* whole init, when clock_us is set */
    size_t arena_peak;    /* bytes of the arena in use at the high-water mark */
    size_t arena_resident; /* bytes kept after a successful init */
};

/* Stable short name of a stage ("ok", "memory", "find", ...). */
const char *manager_skin_game_stage_text(int stage);

#endif /* MANAGER_UI2 && MANAGER_UI2_GAME_SKIN */

#endif
