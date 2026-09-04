#include "gl_vita_coloroffset_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "coloroffset-source oracle: %s\n", message);
        exit(1);
    }
}

static void make_fixture(char source[ISAAC_COLOROFFSET_SOURCE_SIZE + 1u])
{
    static const char body[] =
        "#ifdef GL_ES\r\n"
        "precision highp float;\r\n"
        "#endif\r\n"
        "varying vec4 Color0;\r\n"
        "varying vec2 TexCoord0;\r\n"
        "varying vec4 ColorizeOut;\r\n"
        "varying vec3 ColorOffsetOut;\r\n"
        "varying vec2 TextureSizeOut;\r\n"
        "varying float PixelationAmountOut;\r\n"
        "varying vec3 ClipPlaneOut;\r\n"
        "#define fragColor gl_FragColor\r\n"
        "#define texture texture2D\r\n"
        "uniform sampler2D Texture0;\r\n"
        "void main(void) {\r\n"
        "\tif (dot(gl_FragCoord.xy, ClipPlaneOut.xy) < ClipPlaneOut.z)\r\n"
        "\t\tdiscard;\r\n"
        "\tvec2 pa = TexCoord0;\r\n"
        "\tfragColor = Color0 * texture(Texture0, pa);\r\n"
        "}\r\n"
        "/*";
    size_t body_size = sizeof body - 1u;
    size_t index;

    expect(body_size + 2u <= ISAAC_COLOROFFSET_SOURCE_SIZE,
           "fixture body exceeds exact size");
    memcpy(source, body, body_size);
    for (index = body_size;
         index < ISAAC_COLOROFFSET_SOURCE_SIZE - 2u; ++index)
        source[index] = (char)('a' + index % 23u);
    source[ISAAC_COLOROFFSET_SOURCE_SIZE - 2u] = '*';
    source[ISAAC_COLOROFFSET_SOURCE_SIZE - 1u] = '/';
    source[ISAAC_COLOROFFSET_SOURCE_SIZE] = 0;
}

static IsaacColorOffsetSourceSignature fixture_signature(const char *source)
{
    IsaacColorOffsetSourceSignature signature;
    signature.size = ISAAC_COLOROFFSET_SOURCE_SIZE;
    signature.full_fnv1a = isaac_coloroffset_fnv1a(
        source, ISAAC_COLOROFFSET_SOURCE_SIZE);
    signature.first512_fnv1a = isaac_coloroffset_fnv1a(source, 512u);
    return signature;
}

static void verify_owned_production_asset(const char *path)
{
    char source[ISAAC_COLOROFFSET_SOURCE_SIZE + 1u];
    const char *parts[3];
    int32_t lengths[3];
    IsaacColorOffsetSourceSignature signature =
        isaac_coloroffset_production_signature();
    IsaacColorOffsetSourceMatch result;
    FILE *stream = fopen(path, "rb");

    expect(stream != NULL, "owned production asset could not be opened");
    expect(fread(source, 1u, sizeof source, stream) ==
               ISAAC_COLOROFFSET_SOURCE_SIZE,
           "owned production asset does not have the exact size");
    expect(fgetc(stream) == EOF,
           "owned production asset has trailing bytes");
    expect(!ferror(stream), "owned production asset read failed");
    expect(fclose(stream) == 0, "owned production asset close failed");
    source[ISAAC_COLOROFFSET_SOURCE_SIZE] = 0;

    parts[0] = source;
    parts[1] = source + 993u;
    parts[2] = "";
    lengths[0] = 993;
    lengths[1] = -1;
    lengths[2] = -1;
    expect(isaac_coloroffset_match_source(
               0x8b30u, 3, parts, lengths, &signature, &result),
           "owned production asset failed exact split-source selection");
    expect(result.selected &&
               result.source_size == ISAAC_COLOROFFSET_SOURCE_SIZE &&
               result.source_fnv1a == ISAAC_COLOROFFSET_SOURCE_FNV1A &&
               result.source_first512_fnv1a ==
                   ISAAC_COLOROFFSET_SOURCE_FIRST512_FNV1A,
           "owned production asset match receipt is inconsistent");
}

int main(int argc, char **argv)
{
    char source[ISAAC_COLOROFFSET_SOURCE_SIZE + 1u];
    char mutated[ISAAC_COLOROFFSET_SOURCE_SIZE + 1u];
    const char *parts[4];
    int32_t lengths[4];
    IsaacColorOffsetSourceSignature signature;
    IsaacColorOffsetSourceSignature production =
        isaac_coloroffset_production_signature();
    IsaacColorOffsetSourceMatch result;
    size_t index;

    make_fixture(source);
    signature = fixture_signature(source);
    parts[0] = source;
    expect(isaac_coloroffset_match_source(
               0x8b30u, 1, parts, NULL, &signature, &result),
           "complete NUL-terminated source was not selected");
    expect(result.selected &&
               result.source_size == ISAAC_COLOROFFSET_SOURCE_SIZE &&
               result.source_fnv1a == signature.full_fnv1a &&
               result.source_first512_fnv1a == signature.first512_fnv1a,
           "match receipt is inconsistent");

    parts[0] = source;
    parts[1] = source + 211u;
    parts[2] = source + 999u;
    parts[3] = "";
    lengths[0] = 211;
    lengths[1] = 788;
    lengths[2] = (int32_t)ISAAC_COLOROFFSET_SOURCE_SIZE - 999;
    lengths[3] = -1;
    expect(isaac_coloroffset_match_source(
               0x8b30u, 4, parts, lengths, &signature, &result),
           "split source with explicit lengths/trailing empty part failed");

    memcpy(mutated, source, sizeof source);
    parts[0] = mutated;
    for (index = 0; index < ISAAC_COLOROFFSET_SOURCE_SIZE; ++index) {
        mutated[index] ^= 0x01;
        expect(!isaac_coloroffset_match_source(
                   0x8b30u, 1, parts, NULL, &signature, &result),
               "single-byte hostile mutation passed the full-source gate");
        mutated[index] ^= 0x01;
    }

    parts[0] = source;
    expect(!isaac_coloroffset_match_source(
               0x8b31u, 1, parts, NULL, &signature, &result),
           "vertex shader passed fragment-only gate");
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 0, parts, NULL, &signature, &result),
           "zero source count passed");
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 33, parts, NULL, &signature, &result),
           "unbounded source count passed");
    lengths[0] = (int32_t)ISAAC_COLOROFFSET_SOURCE_SIZE - 1;
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 1, parts, lengths, &signature, &result),
           "short explicit source length passed");
    lengths[0] = (int32_t)ISAAC_COLOROFFSET_SOURCE_SIZE + 1;
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 1, parts, lengths, &signature, &result),
           "oversized explicit source length passed");
    lengths[0] = 4;
    parts[0] = "a\0bc";
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 1, parts, lengths, &signature, &result),
           "explicit interior NUL passed");
    parts[0] = NULL;
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 1, parts, NULL, &signature, &result),
           "NULL source pointer passed");
    parts[0] = source;
    expect(!isaac_coloroffset_match_source(
               0x8b30u, 1, parts, NULL, &production, &result),
           "non-stock fixture passed production hashes");

    if (argc == 2)
        verify_owned_production_asset(argv[1]);
    else
        expect(argc == 1, "usage: oracle [owned-coloroffset.fs]");

    puts("coloroffset-source oracle: PASS");
    return 0;
}
