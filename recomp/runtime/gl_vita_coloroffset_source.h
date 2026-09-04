#ifndef GL_VITA_COLOROFFSET_SOURCE_H
#define GL_VITA_COLOROFFSET_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#define ISAAC_COLOROFFSET_SOURCE_SIZE 2052u
#define ISAAC_COLOROFFSET_SOURCE_FNV1A 0x2b3ccf4au
#define ISAAC_COLOROFFSET_SOURCE_FIRST512_FNV1A 0x75ef2322u
#define ISAAC_COLOROFFSET_SOURCE_PART_LIMIT 32u

typedef struct IsaacColorOffsetSourceSignature {
    uint32_t size;
    uint32_t full_fnv1a;
    uint32_t first512_fnv1a;
} IsaacColorOffsetSourceSignature;

typedef struct IsaacColorOffsetSourceMatch {
    uint32_t source_fnv1a;
    uint32_t source_first512_fnv1a;
    uint32_t source_size;
    uint32_t selected;
} IsaacColorOffsetSourceMatch;

uint32_t isaac_coloroffset_fnv1a(const void *data, size_t size);

/* Reproduce OpenGL's count/length rules into a bounded buffer and authenticate
 * the complete CRLF asset.  This is selection only: the stock source is passed
 * to vitaGL byte-for-byte and no source-pointer identity participates. */
int isaac_coloroffset_match_source(
    uint32_t shader_type, int32_t count, const char *const *strings,
    const int32_t *lengths, const IsaacColorOffsetSourceSignature *signature,
    IsaacColorOffsetSourceMatch *result);

static inline IsaacColorOffsetSourceSignature
isaac_coloroffset_production_signature(void)
{
    IsaacColorOffsetSourceSignature value;
    value.size = ISAAC_COLOROFFSET_SOURCE_SIZE;
    value.full_fnv1a = ISAAC_COLOROFFSET_SOURCE_FNV1A;
    value.first512_fnv1a = ISAAC_COLOROFFSET_SOURCE_FIRST512_FNV1A;
    return value;
}

#endif
