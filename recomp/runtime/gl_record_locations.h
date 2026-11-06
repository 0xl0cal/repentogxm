#ifndef ISAAC_GL_RECORD_LOCATIONS_H
#define ISAAC_GL_RECORD_LOCATIONS_H

#include <stddef.h>
#include <stdint.h>

/* Kind/name bound copied from gl_vita_backend.c:712..714. */
#define ISAAC_GL_RECORD_LOCATION_ATTRIB 1u
#define ISAAC_GL_RECORD_LOCATION_UNIFORM 2u
#define ISAAC_GL_RECORD_LOCATION_NAME_MAX 63u
#define ISAAC_GL_RECORD_LOCATION_CAPACITY 1024u

typedef struct IsaacGlRecordLocationKey {
    uint32_t hash;
    uint32_t program;
    uint8_t kind;
    uint8_t length;
    char name[ISAAC_GL_RECORD_LOCATION_NAME_MAX + 1u];
} IsaacGlRecordLocationKey;

typedef struct IsaacGlRecordLocationEntry {
    IsaacGlRecordLocationKey key; /* program zero = empty */
    int32_t location;
} IsaacGlRecordLocationEntry;

typedef struct IsaacGlRecordLocations {
    IsaacGlRecordLocationEntry entries[ISAAC_GL_RECORD_LOCATION_CAPACITY];
    uint32_t used;
} IsaacGlRecordLocations;

/* Caller provides a readable byte span, not merely a guest address. */
int isaac_gl_record_location_key(uint32_t kind, uint32_t program,
    const char *name, size_t readable_bytes, IsaacGlRecordLocationKey *key);
void isaac_gl_record_locations_reset(IsaacGlRecordLocations *memo);
int isaac_gl_record_locations_lookup(const IsaacGlRecordLocations *memo,
    const IsaacGlRecordLocationKey *key, int32_t *location);
/* No eviction. Zero on full/invalid: caller must use an exact query fallback.
 * shortcut: 1024 live pairs; size from a live-set receipt before stage 3. */
int isaac_gl_record_locations_store(IsaacGlRecordLocations *memo,
    const IsaacGlRecordLocationKey *key, int32_t location);

#endif
