/*
 * Manager UI2 mod display names from mods/<dir>/metadata.xml (the MODS
 * page): first 4 KiB, plain substring scan, entity decode.  Mirrors the
 * PC companion's rule (tools/isaac_vita_sync_gui.py metadata_title):
 * <name> first, then <title>; collapse whitespace; drop control
 * characters; cut at 90 characters; the text is display-only and never
 * used as a path.  Without a usable name the directory name is shown.
 * <version> is read the same way (cut at 16 characters).  SDK-free.
 */
#ifndef ISAAC_MANAGER_MODS_META_H
#define ISAAC_MANAGER_MODS_META_H

#if defined(MANAGER_UI2)

#include "manager_storage.h"

#include <stddef.h>
#include <stdint.h>

#define MANAGER_MODS_META_HEAD_BYTES 4096u
#define MANAGER_MODS_META_NAME_CHARS 90u
#define MANAGER_MODS_META_NAME_CAPACITY 192u
#define MANAGER_MODS_META_VERSION_CHARS 16u
#define MANAGER_MODS_META_VERSION_CAPACITY 40u

struct manager_mod_meta {
    char name[MANAGER_MODS_META_NAME_CAPACITY];       /* never empty after read */
    char version[MANAGER_MODS_META_VERSION_CAPACITY]; /* "" when absent */
    uint8_t from_metadata; /* 1: name from <name>/<title>; 0: directory name */
};

/* Scans an XML head; fills name ("" when none) and version; returns 1 when
 * a non-empty name was found. */
int manager_mods_meta_scan(const char *xml, size_t length,
                           struct manager_mod_meta *meta);
/* Reads mods/<dir>/metadata.xml (first 4 KiB) and scans it; the name falls
 * back to `dir`.  Returns the enum manager_storage_result of the read
 * (MANAGER_STORAGE_ABSENT when there is no metadata.xml). */
int manager_mods_meta_read(const struct manager_storage_io *io,
                           const char *dir, struct manager_mod_meta *meta);

#endif /* MANAGER_UI2 */

#endif
