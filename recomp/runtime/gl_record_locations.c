#include "gl_record_locations.h"
#include <string.h>

_Static_assert((ISAAC_GL_RECORD_LOCATION_CAPACITY &
    (ISAAC_GL_RECORD_LOCATION_CAPACITY - 1u)) == 0u,
    "location capacity must be a power of two");

int isaac_gl_record_location_key(uint32_t kind, uint32_t program,
    const char *name, size_t readable_bytes, IsaacGlRecordLocationKey *key)
{
    uint32_t length, hash;
    if (!key) return 0;
    memset(key, 0, sizeof *key);
    if (!program || !name ||
        (kind != ISAAC_GL_RECORD_LOCATION_ATTRIB &&
         kind != ISAAC_GL_RECORD_LOCATION_UNIFORM)) return 0;
    /* Same hash and byte admission as gl_vita_backend.c:845..865. */
    hash = UINT32_C(2166136261) ^ (program * UINT32_C(0x9e3779b9));
    hash = (hash ^ kind) * UINT32_C(16777619);
    for (length = 0u; length < readable_bytes &&
            length <= ISAAC_GL_RECORD_LOCATION_NAME_MAX; ++length) {
        uint8_t byte = (uint8_t)name[length];
        if (!byte) {
            if (!length) return 0;
            key->hash = hash;
            key->program = program;
            key->kind = (uint8_t)kind;
            key->length = (uint8_t)length;
            return 1;
        }
        if (byte < 0x20u || byte > 0x7eu ||
                length == ISAAC_GL_RECORD_LOCATION_NAME_MAX) return 0;
        key->name[length] = (char)byte;
        hash = (hash ^ byte) * UINT32_C(16777619);
    }
    return 0;
}

void isaac_gl_record_locations_reset(IsaacGlRecordLocations *memo)
{
    memset(memo, 0, sizeof *memo);
}

static int location_key_valid(const IsaacGlRecordLocationKey *key)
{
    return key && key->program && key->length &&
        key->length <= ISAAC_GL_RECORD_LOCATION_NAME_MAX &&
        !key->name[key->length] &&
        (key->kind == ISAAC_GL_RECORD_LOCATION_ATTRIB ||
         key->kind == ISAAC_GL_RECORD_LOCATION_UNIFORM);
}

static const IsaacGlRecordLocationEntry *location_slot(
    const IsaacGlRecordLocations *memo, const IsaacGlRecordLocationKey *key)
{
    uint32_t slot, probe;
    if (!memo || !location_key_valid(key)) return NULL;
    slot = (key->hash ^ (key->hash >> 16)) &
        (ISAAC_GL_RECORD_LOCATION_CAPACITY - 1u);
    for (probe = 0u; probe < ISAAC_GL_RECORD_LOCATION_CAPACITY; ++probe) {
        const IsaacGlRecordLocationEntry *entry = &memo->entries[slot];
        if (!entry->key.program ||
            (entry->key.hash == key->hash &&
             entry->key.program == key->program && entry->key.kind == key->kind &&
             entry->key.length == key->length &&
             !memcmp(entry->key.name, key->name, (size_t)key->length + 1u)))
            return entry;
        slot = (slot + 1u) & (ISAAC_GL_RECORD_LOCATION_CAPACITY - 1u);
    }
    return NULL;
}

int isaac_gl_record_locations_lookup(const IsaacGlRecordLocations *memo,
    const IsaacGlRecordLocationKey *key, int32_t *location)
{
    const IsaacGlRecordLocationEntry *entry = location_slot(memo, key);
    if (!entry || !entry->key.program || !location) return 0;
    *location = entry->location;
    return 1;
}

int isaac_gl_record_locations_store(IsaacGlRecordLocations *memo,
    const IsaacGlRecordLocationKey *key, int32_t location)
{
    const IsaacGlRecordLocationEntry *found = location_slot(memo, key);
    IsaacGlRecordLocationEntry *entry;
    if (!found) return 0;
    entry = &memo->entries[found - memo->entries];
    if (!entry->key.program) ++memo->used;
    entry->key = *key;
    entry->location = location;
    return 1;
}
