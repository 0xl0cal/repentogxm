#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isaac_gpu_draw_policy.h"

typedef struct oracle_cache_entry {
    int valid;
    uintptr_t shader_id;
    uint32_t attribute_count;
    uint32_t stream_count;
    isaac_vitagl_attribute_key attributes[ISAAC_VITAGL_MAX_VERTEX_ATTRIBUTES];
    isaac_vitagl_stream_key streams[ISAAC_VITAGL_MAX_VERTEX_ATTRIBUTES];
    unsigned int program;
} oracle_cache_entry;

typedef struct oracle_cache {
    oracle_cache_entry entries[ISAAC_VITAGL_VERTEX_CACHE_CAPACITY];
    uint32_t count;
    uint32_t create_calls;
    uint32_t cache_hits;
    uint32_t full_fallbacks;
    uint32_t release_calls;
} oracle_cache;

static unsigned int oracle_get_vertex_program(
        oracle_cache *cache,
        uintptr_t shader_id,
        const isaac_vitagl_attribute_key *attributes,
        uint32_t attribute_count,
        const isaac_vitagl_stream_key *streams,
        uint32_t stream_count) {
    uint32_t index;
    unsigned int created;

    for (index = 0; index < cache->count; ++index) {
        oracle_cache_entry *entry = &cache->entries[index];
        if (entry->valid && entry->shader_id == shader_id &&
                isaac_vitagl_layout_key_equal(
                    entry->attributes, entry->attribute_count,
                    entry->streams, entry->stream_count,
                    attributes, attribute_count, streams, stream_count)) {
            ++cache->cache_hits;
            return entry->program;
        }
    }

    created = ++cache->create_calls;
    if (cache->count == ISAAC_VITAGL_VERTEX_CACHE_CAPACITY) {
        ++cache->full_fallbacks;
        return created;
    }

    {
        oracle_cache_entry *entry = &cache->entries[cache->count++];
        entry->valid = 1;
        entry->shader_id = shader_id;
        entry->attribute_count = attribute_count;
        entry->stream_count = stream_count;
        memcpy(entry->attributes, attributes,
               attribute_count * sizeof(*attributes));
        memcpy(entry->streams, streams, stream_count * sizeof(*streams));
        entry->program = created;
    }
    return created;
}

static void oracle_release_cache(oracle_cache *cache) {
    cache->release_calls += cache->count;
    cache->count = 0u;
}

static void make_physical_layout(
        isaac_vitagl_attribute_key *attributes,
        isaac_vitagl_stream_key *streams,
        uintptr_t *bases) {
    static const uint16_t offsets[8] = {
        0u, 12u, 28u, 36u, 52u, 60u, 68u, 76u
    };
    static const uint8_t components[8] = {
        3u, 4u, 2u, 4u, 2u, 2u, 2u, 3u
    };
    uint32_t index;

    for (index = 0; index < 8u; ++index) {
        attributes[index].stream_index = (uint16_t)index;
        attributes[index].offset = offsets[index];
        attributes[index].format = 9u;
        attributes[index].component_count = components[index];
        attributes[index].reg_index = (uint16_t)(index * 4u);
        streams[index].stride = 88u;
        streams[index].index_source = 0u;
        bases[index] = 0x7054709cu;
    }
}

static void prove_fetch_equivalence(void) {
    isaac_vitagl_attribute_key attributes[8];
    isaac_vitagl_stream_key streams[8];
    uintptr_t bases[8];
    uint32_t attribute;
    uint32_t vertex;

    make_physical_layout(attributes, streams, bases);
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 8u) ==
           ISAAC_VITAGL_COALESCE_OK);

    for (attribute = 0; attribute < 8u; ++attribute) {
        for (vertex = 0; vertex < 4u; ++vertex) {
            uintptr_t old_address = bases[attribute] +
                vertex * streams[attribute].stride +
                attributes[attribute].offset;
            uintptr_t new_address = bases[0] +
                vertex * streams[0].stride + attributes[attribute].offset;
            assert(old_address == new_address);
        }
    }

    bases[7] += 4u;
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 8u) ==
           ISAAC_VITAGL_COALESCE_REJECT_BASE);
    bases[7] -= 4u;
    streams[6].stride = 84u;
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 8u) ==
           ISAAC_VITAGL_COALESCE_REJECT_DESCRIPTOR);
    streams[6].stride = 88u;
    streams[5].index_source = 1u;
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 8u) ==
           ISAAC_VITAGL_COALESCE_REJECT_DESCRIPTOR);
    streams[5].index_source = 0u;
    attributes[4].stream_index = 3u;
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 8u) ==
           ISAAC_VITAGL_COALESCE_REJECT_STREAM_INDEX);
    attributes[4].stream_index = 4u;
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 1u) ==
           ISAAC_VITAGL_COALESCE_REJECT_COUNT);
    assert(isaac_vitagl_single_stream_policy(
               attributes, streams, bases, 17u) ==
           ISAAC_VITAGL_COALESCE_REJECT_COUNT);
}

static void assert_cache_miss_for_every_field(
        const isaac_vitagl_attribute_key *attributes,
        const isaac_vitagl_stream_key *streams) {
    isaac_vitagl_attribute_key changed_attributes[8];
    isaac_vitagl_stream_key changed_streams[1];
    oracle_cache cache = {0};
    unsigned int first;
    uint32_t field;

    first = oracle_get_vertex_program(
        &cache, 0x8354cca8u, attributes, 8u, streams, 1u);
    for (field = 0; field < 5u; ++field) {
        memcpy(changed_attributes, attributes, sizeof(changed_attributes));
        switch (field) {
        case 0u: changed_attributes[3].stream_index ^= 1u; break;
        case 1u: changed_attributes[3].offset += 4u; break;
        case 2u: changed_attributes[3].format ^= 1u; break;
        case 3u: changed_attributes[3].component_count ^= 1u; break;
        default: changed_attributes[3].reg_index += 1u; break;
        }
        assert(oracle_get_vertex_program(
                   &cache, 0x8354cca8u, changed_attributes, 8u,
                   streams, 1u) != first);
        oracle_release_cache(&cache);
        memset(&cache, 0, sizeof(cache));
        first = oracle_get_vertex_program(
            &cache, 0x8354cca8u, attributes, 8u, streams, 1u);
    }

    changed_streams[0] = streams[0];
    changed_streams[0].stride += 4u;
    assert(oracle_get_vertex_program(
               &cache, 0x8354cca8u, attributes, 8u,
               changed_streams, 1u) != first);
    oracle_release_cache(&cache);
    memset(&cache, 0, sizeof(cache));
    first = oracle_get_vertex_program(
        &cache, 0x8354cca8u, attributes, 8u, streams, 1u);
    changed_streams[0] = streams[0];
    changed_streams[0].index_source ^= 1u;
    assert(oracle_get_vertex_program(
               &cache, 0x8354cca8u, attributes, 8u,
               changed_streams, 1u) != first);
    assert(oracle_get_vertex_program(
               &cache, 0x8354cca9u, attributes, 8u,
               streams, 1u) != first);
    assert(oracle_get_vertex_program(
               &cache, 0x8354cca8u, attributes, 7u,
               streams, 1u) != first);
    assert(oracle_get_vertex_program(
               &cache, 0x8354cca8u, attributes, 8u,
               streams, 0u) != first);
}

static void prove_exact_layout_cache(void) {
    isaac_vitagl_attribute_key attributes[8];
    isaac_vitagl_attribute_key changed_attributes[8];
    isaac_vitagl_stream_key streams[8];
    uintptr_t bases[8];
    oracle_cache cache = {0};
    unsigned int first;
    unsigned int repeated;
    uint32_t index;

    make_physical_layout(attributes, streams, bases);
    for (index = 0; index < 8u; ++index)
        attributes[index].stream_index = 0u;

    first = oracle_get_vertex_program(
        &cache, 0x8354cca8u, attributes, 8u, streams, 1u);
    repeated = oracle_get_vertex_program(
        &cache, 0x8354cca8u, attributes, 8u, streams, 1u);
    assert(first == repeated);
    assert(cache.create_calls == 1u && cache.cache_hits == 1u);

    memcpy(changed_attributes, attributes, sizeof(attributes));
    for (index = 0; index < ISAAC_VITAGL_VERTEX_CACHE_CAPACITY - 1u;
            ++index) {
        changed_attributes[3].offset += 4u;
        assert(oracle_get_vertex_program(
                   &cache, 0x8354cca8u, changed_attributes, 8u,
                   streams, 1u) != first);
    }
    assert(cache.count == ISAAC_VITAGL_VERTEX_CACHE_CAPACITY);
    changed_attributes[3].offset += 4u;
    (void)oracle_get_vertex_program(
        &cache, 0x8354cca8u, changed_attributes, 8u, streams, 1u);
    assert(cache.full_fallbacks == 1u);
    oracle_release_cache(&cache);
    assert(cache.count == 0u);
    assert(cache.release_calls == ISAAC_VITAGL_VERTEX_CACHE_CAPACITY);

    assert_cache_miss_for_every_field(attributes, streams);
}

int main(void) {
    _Static_assert(sizeof(isaac_vitagl_attribute_key) == 8u,
                   "attribute key ABI drifted");
    _Static_assert(sizeof(isaac_vitagl_stream_key) == 4u,
                   "stream key ABI drifted");
    prove_fetch_equivalence();
    prove_exact_layout_cache();
    puts("vitaGL exact GPU draw policy oracle: PASS (8->1 fetch equivalence; hostile fallbacks; exact-layout cache/lifetime model)");
    return 0;
}
