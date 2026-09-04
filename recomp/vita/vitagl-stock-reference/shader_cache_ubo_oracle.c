#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ISAAC_SHADER_CACHE_ORACLE 1
#define ISAAC_SHADER_CACHE_BUILD_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_VERTEX_SHADER 0x8b31u
#define GL_FRAGMENT_SHADER 0x8b30u
#define MAX_CUSTOM_SHADERS 4u
#define MAX_CG_TEXCOORD_ID 10u
#define MAX_CG_COLOR_ID 2u
#define UBOS_NUM 14u
#define SCE_SEEK_SET 0
#define SCE_SEEK_END 2

#define SCE_GXM_VERTEX_PROGRAM 1u
#define SCE_GXM_FRAGMENT_PROGRAM 2u
#define SCE_GXM_PARAMETER_CATEGORY_UNIFORM 0u
#define SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE 1u
#define SCE_GXM_PARAMETER_CATEGORY_SAMPLER 2u
#define SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER 3u

typedef uint32_t GLenum;
typedef int GLboolean;
typedef void *SceGxmShaderPatcherId;

typedef struct SceGxmProgramParameter {
    uint32_t category;
    uint32_t resource_index;
    char name[128];
} SceGxmProgramParameter;

typedef struct SceGxmProgram {
    uint32_t check_result;
    uint32_t size;
    uint32_t type;
    uint32_t default_uniform_size;
    uint32_t parameter_count;
    SceGxmProgramParameter parameters[20];
} SceGxmProgram;

typedef struct matrix_uniform {
    const SceGxmProgramParameter *ptr;
    void *chain;
} matrix_uniform;

typedef struct block_uniform {
    char name[128];
    uint8_t idx;
    void *chain;
} block_uniform;

typedef struct binds_map {
    char texcoord_names[MAX_CG_TEXCOORD_ID][64];
    char color_names[MAX_CG_COLOR_ID][64];
    GLboolean texcoord_used[MAX_CG_TEXCOORD_ID];
    GLboolean color_used[MAX_CG_COLOR_ID];
} binds_map;

typedef struct shader {
    GLenum type;
    GLboolean valid;
    GLboolean dirty;
    GLboolean is_glsl;
    binds_map semantics;
    int16_t ref_counter;
    SceGxmShaderPatcherId id;
    const SceGxmProgram *prog;
    uint32_t size;
    uint32_t unif_buf_size;
    char *source;
    matrix_uniform *mat;
    block_uniform *unif_blk;
} shader;

static shader shaders[MAX_CUSTOM_SHADERS];
static int compiler_opts;
static int compiler_fastmath;
static int compiler_fastprecision;
static int compiler_fastint;
static void *gxm_shader_patcher;

static void *vglMalloc(size_t size)
{
    return malloc(size);
}

static void vgl_free(void *pointer)
{
    free(pointer);
}

static int sceGxmProgramCheck(const SceGxmProgram *program)
{
    return program ? (int)program->check_result : -1;
}

static uint32_t sceGxmProgramGetSize(const SceGxmProgram *program)
{
    return program->size;
}

static uint32_t sceGxmProgramGetType(const SceGxmProgram *program)
{
    return program->type;
}

static uint32_t sceGxmProgramGetDefaultUniformBufferSize(
    const SceGxmProgram *program)
{
    return program->default_uniform_size;
}

static uint32_t sceGxmProgramGetParameterCount(const SceGxmProgram *program)
{
    return program->parameter_count;
}

static const SceGxmProgramParameter *sceGxmProgramGetParameter(
    const SceGxmProgram *program, uint32_t index)
{
    return index < program->parameter_count ? &program->parameters[index] : NULL;
}

static uint32_t sceGxmProgramParameterGetCategory(
    const SceGxmProgramParameter *parameter)
{
    return parameter->category;
}

static uint32_t sceGxmProgramParameterGetResourceIndex(
    const SceGxmProgramParameter *parameter)
{
    return parameter->resource_index;
}

static const SceGxmProgramParameter *sceGxmProgramFindParameterByName(
    const SceGxmProgram *program, const char *name)
{
    uint32_t index;
    for (index = 0u; index < program->parameter_count; ++index) {
        if (strcmp(program->parameters[index].name, name) == 0)
            return &program->parameters[index];
    }
    return NULL;
}

static uint32_t sceGxmProgramParameterGetIndex(
    const SceGxmProgram *program,
    const SceGxmProgramParameter *parameter)
{
    uint32_t index;
    for (index = 0u; index < program->parameter_count; ++index) {
        if (&program->parameters[index] == parameter)
            return index;
    }
    return UINT32_MAX;
}

static int sceGxmShaderPatcherRegisterProgram(
    void *patcher, const SceGxmProgram *program,
    SceGxmShaderPatcherId *registered_id)
{
    (void)patcher;
    (void)program;
    *registered_id = (void *)(uintptr_t)0x1234u;
    return 0;
}

#include "isaac_shader_cache_policy.h"
typedef isaac_shader_cache_stats vglIsaacShaderCacheStats;
#include "isaac_shader_cache_vitagl.h"

static void set_parameter(SceGxmProgram *program, uint32_t slot,
                          uint32_t category, uint32_t resource_index,
                          const char *name)
{
    size_t length = strlen(name);
    assert(slot < 20u && length < sizeof program->parameters[slot].name);
    program->parameters[slot].category = category;
    program->parameters[slot].resource_index = resource_index;
    memset(program->parameters[slot].name, 0,
        sizeof program->parameters[slot].name);
    memcpy(program->parameters[slot].name, name, length + 1u);
    if (program->parameter_count <= slot)
        program->parameter_count = slot + 1u;
}

static void set_block(block_uniform *block, uint8_t index, const char *name,
                      block_uniform *next)
{
    size_t length = strlen(name);
    assert(length < sizeof block->name);
    memset(block, 0, sizeof *block);
    memcpy(block->name, name, length + 1u);
    block->idx = index;
    block->chain = next;
}

static void prove_compiler_only_nodes_are_not_metadata(void)
{
    SceGxmProgram program;
    shader test_shader;
    block_uniform hostile;
    isaac_shader_cache_block_descriptor descriptors[
        ISAAC_SHADER_CACHE_BLOCK_MAX];
    uint32_t count = UINT32_MAX;

    memset(&program, 0, sizeof program);
    set_parameter(&program, 0u, SCE_GXM_PARAMETER_CATEGORY_UNIFORM,
        UBOS_NUM, "Transform");
    memset(&test_shader, 0, sizeof test_shader);
    test_shader.prog = &program;
    memset(&hostile, 'x', sizeof hostile);
    hostile.idx = 0u;
    hostile.chain = &hostile;
    test_shader.unif_blk = &hostile;

    /* No GXP UNIFORM_BUFFER means vitaGL never consumes unif_blk at link.
     * Even a compiler-only cyclic/malformed list is therefore excluded. */
    assert(isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));
    assert(count == 0u);
    assert(isaac_shader_cache_gxp_blocks_valid(
        &program, descriptors, count));
}

static void prove_exact_gxp_subset(void)
{
    SceGxmProgram program;
    shader test_shader;
    block_uniform needed;
    block_uniform ignored;
    isaac_shader_cache_block_descriptor descriptors[
        ISAAC_SHADER_CACHE_BLOCK_MAX];
    uint32_t count;

    memset(&program, 0, sizeof program);
    set_parameter(&program, 0u, SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE,
        0u, "Position");
    set_parameter(&program, 1u, SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER,
        3u, "Scene");
    set_parameter(&program, 2u, SCE_GXM_PARAMETER_CATEGORY_SAMPLER,
        0u, "Texture0");
    set_block(&needed, 3u, "Scene", NULL);
    memset(&ignored, 'q', sizeof ignored);
    ignored.idx = 9u;
    ignored.chain = &needed;
    memset(&test_shader, 0, sizeof test_shader);
    test_shader.prog = &program;
    test_shader.unif_blk = &ignored;

    assert(isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));
    assert(count == 1u && descriptors[0].index == 3u &&
        strcmp(descriptors[0].name, "Scene") == 0);
    assert(isaac_shader_cache_gxp_blocks_valid(
        &program, descriptors, count));

    descriptors[0].index = 4u;
    assert(!isaac_shader_cache_gxp_blocks_valid(
        &program, descriptors, count));
    descriptors[0].index = 3u;
    descriptors[0].name[0] = 's';
    assert(!isaac_shader_cache_gxp_blocks_valid(
        &program, descriptors, count));
}

static void prove_gxp_mismatches_fail_closed(void)
{
    SceGxmProgram program;
    shader test_shader;
    block_uniform first;
    block_uniform duplicate;
    block_uniform chain[ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX + 1u];
    isaac_shader_cache_block_descriptor descriptors[
        ISAAC_SHADER_CACHE_BLOCK_MAX];
    uint32_t count;
    uint32_t index;

    memset(&program, 0, sizeof program);
    set_parameter(&program, 0u, SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER,
        2u, "Lighting");
    memset(&test_shader, 0, sizeof test_shader);
    test_shader.prog = &program;

    set_block(&first, 2u, "WrongName", NULL);
    test_shader.unif_blk = &first;
    assert(!isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));
    test_shader.unif_blk = NULL;
    assert(!isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));

    set_block(&duplicate, 2u, "Lighting", NULL);
    set_block(&first, 2u, "Lighting", &duplicate);
    test_shader.unif_blk = &first;
    assert(!isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));

    for (index = 0u; index < ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX + 1u;
            ++index) {
        set_block(&chain[index], index ==
            ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX ? 2u : 12u,
            index == ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX
                ? "Lighting" : "Ignored",
            index + 1u < ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX + 1u
                ? &chain[index + 1u] : NULL);
    }
    test_shader.unif_blk = &chain[0];
    assert(!isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));

    program.parameter_count = 0u;
    set_parameter(&program, 0u, SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER,
        2u, "Lighting");
    set_parameter(&program, 1u, SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER,
        2u, "LightingAgain");
    assert(!isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));
    program.parameter_count = 0u;
    set_parameter(&program, 0u, SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER,
        ISAAC_SHADER_CACHE_BLOCK_MAX, "OutOfRange");
    assert(!isaac_shader_cache_collect_gxp_blocks(
        &test_shader, descriptors, &count));
}

static void prove_canonical_gxp_padding(void)
{
    union {
        SceGxmProgram alignment;
        unsigned char bytes[sizeof(SceGxmProgram) + 8u];
    } storage;
    SceGxmProgram *program = (SceGxmProgram *)storage.bytes;
    const uint32_t aligned_size = (uint32_t)sizeof(SceGxmProgram);
    uint32_t canonical_size;
    uint32_t padding;

    assert((aligned_size & 3u) == 0u);
    for (padding = 0u; padding <= 3u; ++padding) {
        uint32_t embedded_size = aligned_size - padding;
        memset(&storage, 0, sizeof storage);
        program->size = embedded_size;
        canonical_size = UINT32_MAX;
        assert(isaac_shader_cache_canonical_gxp_size(
            program, aligned_size, &canonical_size));
        assert(canonical_size == embedded_size);

        /* A producer may already supply the canonical, unpadded container. */
        canonical_size = UINT32_MAX;
        assert(isaac_shader_cache_canonical_gxp_size(
            program, embedded_size, &canonical_size));
        assert(canonical_size == embedded_size);

        if (padding != 0u) {
            if (padding > 1u) {
                canonical_size = UINT32_MAX;
                assert(!isaac_shader_cache_canonical_gxp_size(
                    program, embedded_size + 1u, &canonical_size));
                assert(canonical_size == UINT32_MAX);
            }
            storage.bytes[embedded_size] = 0x5au;
            canonical_size = UINT32_MAX;
            assert(!isaac_shader_cache_canonical_gxp_size(
                program, aligned_size, &canonical_size));
            assert(canonical_size == UINT32_MAX);
        }
    }

    memset(&storage, 0, sizeof storage);
    program->size = aligned_size;
    canonical_size = UINT32_MAX;
    assert(!isaac_shader_cache_canonical_gxp_size(
        program, aligned_size - 1u, &canonical_size));
    assert(canonical_size == UINT32_MAX);

    program->size = aligned_size - 4u;
    assert(!isaac_shader_cache_canonical_gxp_size(
        program, aligned_size, &canonical_size));
    assert(canonical_size == UINT32_MAX);

    program->size = 0u;
    assert(!isaac_shader_cache_canonical_gxp_size(
        program, 0u, &canonical_size));
    program->size = ISAAC_SHADER_CACHE_GXP_MAX + 1u;
    assert(!isaac_shader_cache_canonical_gxp_size(
        program, ISAAC_SHADER_CACHE_GXP_MAX + 1u, &canonical_size));
    assert(!isaac_shader_cache_canonical_gxp_size(
        NULL, aligned_size, &canonical_size));
    assert(!isaac_shader_cache_canonical_gxp_size(
        program, aligned_size, NULL));
}

int main(void)
{
    prove_compiler_only_nodes_are_not_metadata();
    prove_exact_gxp_subset();
    prove_gxp_mismatches_fail_closed();
    prove_canonical_gxp_padding();
    puts("Isaac vitaGL shader-cache GXP/UBO oracle: PASS");
    return 0;
}
