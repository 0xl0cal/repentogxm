#include "gl_vita_coloroffset_source.h"

#include <string.h>

#define ISAAC_GL_FRAGMENT_SHADER 0x00008b30u

uint32_t isaac_coloroffset_fnv1a(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t value = 0x811c9dc5u;
    size_t index;

    if (!bytes && size)
        return 0u;
    for (index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= 0x01000193u;
    }
    return value;
}

static size_t coloroffset_bounded_strlen(const char *text, size_t limit)
{
    size_t size;

    if (!text)
        return SIZE_MAX;
    for (size = 0; size < limit; ++size) {
        if (!text[size])
            return size;
    }
    return SIZE_MAX;
}

static const char *coloroffset_find_bytes(
    const char *haystack, size_t haystack_size,
    const char *needle, size_t needle_size)
{
    size_t offset;

    if (!haystack || !needle || !needle_size || needle_size > haystack_size)
        return NULL;
    for (offset = 0; offset <= haystack_size - needle_size; ++offset) {
        if (!memcmp(haystack + offset, needle, needle_size))
            return haystack + offset;
    }
    return NULL;
}

static int coloroffset_has_required_structure(const char *source, size_t size)
{
    static const char *const required[] = {
        "Color0", "ColorizeOut", "ColorOffsetOut", "TextureSizeOut",
        "PixelationAmountOut", "ClipPlaneOut", "TexCoord0", "fragColor",
        "uniform sampler2D Texture0", "void main", "discard;", "vec2 pa",
        "texture(Texture0"
    };
    size_t index;
    int saw_crlf = 0;

    for (index = 0; index < size; ++index) {
        if (source[index] == '\r') {
            if (index + 1u >= size || source[index + 1u] != '\n')
                return 0;
            saw_crlf = 1;
        } else if (source[index] == '\n' &&
                   (index == 0u || source[index - 1u] != '\r')) {
            return 0;
        }
    }
    if (!saw_crlf)
        return 0;
    for (index = 0; index < sizeof required / sizeof required[0]; ++index) {
        size_t token_size = strlen(required[index]);
        if (!coloroffset_find_bytes(source, size, required[index], token_size))
            return 0;
    }
    return 1;
}

int isaac_coloroffset_match_source(
    uint32_t shader_type, int32_t count, const char *const *strings,
    const int32_t *lengths, const IsaacColorOffsetSourceSignature *signature,
    IsaacColorOffsetSourceMatch *result)
{
    char source[ISAAC_COLOROFFSET_SOURCE_SIZE];
    size_t source_size = 0u;
    size_t index;
    uint32_t full_hash;
    uint32_t prefix_hash;

    if (result)
        memset(result, 0, sizeof *result);
    if (shader_type != ISAAC_GL_FRAGMENT_SHADER || !signature || !strings ||
            count <= 0 ||
            (uint32_t)count > ISAAC_COLOROFFSET_SOURCE_PART_LIMIT ||
            signature->size != ISAAC_COLOROFFSET_SOURCE_SIZE)
        return 0;

    for (index = 0; index < (size_t)count; ++index) {
        size_t part_size;
        size_t remaining = signature->size - source_size;

        if (!strings[index])
            return 0;
        if (lengths && lengths[index] >= 0) {
            part_size = (size_t)(uint32_t)lengths[index];
            if (part_size > remaining)
                return 0;
        } else {
            part_size = coloroffset_bounded_strlen(strings[index], remaining + 1u);
            if (part_size == SIZE_MAX || part_size > remaining)
                return 0;
        }
        /* vitaGL 73dd concatenates with strncat, so an explicit interior NUL
         * would not represent the authenticated byte sequence it stores. */
        if (part_size && memchr(strings[index], 0, part_size))
            return 0;
        if (part_size)
            memcpy(source + source_size, strings[index], part_size);
        source_size += part_size;
    }
    if (source_size != signature->size)
        return 0;

    full_hash = isaac_coloroffset_fnv1a(source, source_size);
    prefix_hash = isaac_coloroffset_fnv1a(source, 512u);
    if (full_hash != signature->full_fnv1a ||
            prefix_hash != signature->first512_fnv1a ||
            !coloroffset_has_required_structure(source, source_size))
        return 0;
    if (result) {
        result->source_fnv1a = full_hash;
        result->source_first512_fnv1a = prefix_hash;
        result->source_size = (uint32_t)source_size;
        result->selected = 1u;
    }
    return 1;
}
