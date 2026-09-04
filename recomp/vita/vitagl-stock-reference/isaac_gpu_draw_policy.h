#ifndef ISAAC_VITAGL_GPU_DRAW_POLICY_H
#define ISAAC_VITAGL_GPU_DRAW_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define ISAAC_VITAGL_MAX_VERTEX_ATTRIBUTES 16u
#define ISAAC_VITAGL_VERTEX_CACHE_CAPACITY 4u

typedef struct isaac_vitagl_attribute_key {
    uint16_t stream_index;
    uint16_t offset;
    uint8_t format;
    uint8_t component_count;
    uint16_t reg_index;
} isaac_vitagl_attribute_key;

typedef struct isaac_vitagl_stream_key {
    uint16_t stride;
    uint16_t index_source;
} isaac_vitagl_stream_key;

enum isaac_vitagl_coalesce_result {
    ISAAC_VITAGL_COALESCE_OK = 0,
    ISAAC_VITAGL_COALESCE_REJECT_COUNT = 1,
    ISAAC_VITAGL_COALESCE_REJECT_STREAM_INDEX = 2,
    ISAAC_VITAGL_COALESCE_REJECT_BASE = 3,
    ISAAC_VITAGL_COALESCE_REJECT_DESCRIPTOR = 4
};

static inline int isaac_vitagl_attribute_key_equal(
        const isaac_vitagl_attribute_key *left,
        const isaac_vitagl_attribute_key *right) {
    return left->stream_index == right->stream_index &&
           left->offset == right->offset &&
           left->format == right->format &&
           left->component_count == right->component_count &&
           left->reg_index == right->reg_index;
}

static inline int isaac_vitagl_stream_key_equal(
        const isaac_vitagl_stream_key *left,
        const isaac_vitagl_stream_key *right) {
    return left->stride == right->stride &&
           left->index_source == right->index_source;
}

static inline int isaac_vitagl_layout_key_equal(
        const isaac_vitagl_attribute_key *left_attributes,
        uint32_t left_attribute_count,
        const isaac_vitagl_stream_key *left_streams,
        uint32_t left_stream_count,
        const isaac_vitagl_attribute_key *right_attributes,
        uint32_t right_attribute_count,
        const isaac_vitagl_stream_key *right_streams,
        uint32_t right_stream_count) {
    uint32_t index;

    if (left_attribute_count != right_attribute_count ||
            left_stream_count != right_stream_count ||
            left_attribute_count > ISAAC_VITAGL_MAX_VERTEX_ATTRIBUTES ||
            left_stream_count > ISAAC_VITAGL_MAX_VERTEX_ATTRIBUTES)
        return 0;

    for (index = 0; index < left_attribute_count; ++index) {
        if (!isaac_vitagl_attribute_key_equal(
                &left_attributes[index], &right_attributes[index]))
            return 0;
    }
    for (index = 0; index < left_stream_count; ++index) {
        if (!isaac_vitagl_stream_key_equal(
                &left_streams[index], &right_streams[index]))
            return 0;
    }
    return 1;
}

static inline enum isaac_vitagl_coalesce_result
isaac_vitagl_single_stream_policy(
        const isaac_vitagl_attribute_key *attributes,
        const isaac_vitagl_stream_key *streams,
        const uintptr_t *stream_bases,
        uint32_t attribute_count) {
    uint32_t index;

    if (attribute_count < 2u ||
            attribute_count > ISAAC_VITAGL_MAX_VERTEX_ATTRIBUTES)
        return ISAAC_VITAGL_COALESCE_REJECT_COUNT;

    for (index = 0; index < attribute_count; ++index) {
        if (attributes[index].stream_index != index)
            return ISAAC_VITAGL_COALESCE_REJECT_STREAM_INDEX;
        if (stream_bases[index] != stream_bases[0])
            return ISAAC_VITAGL_COALESCE_REJECT_BASE;
        if (!isaac_vitagl_stream_key_equal(&streams[index], &streams[0]))
            return ISAAC_VITAGL_COALESCE_REJECT_DESCRIPTOR;
    }
    return ISAAC_VITAGL_COALESCE_OK;
}

#endif
