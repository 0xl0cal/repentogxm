#include "gl_record_locations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Freshly extracted ORIGINAL direct-mapped cache bodies, not a test clone. */
#include "gl_record_original_locations.inc"

static IsaacGlRecordLocations memo;
static IsaacGlRecordLocationKey collisions[24];
static IsaacGlRecordLocationKey keys[ISAAC_GL_RECORD_LOCATION_CAPACITY + 1u];

static void check_collision_pattern(void)
{
    uint32_t candidate, count = 0u, round, i, original_misses = 0u, memo_misses = 0u;
    int32_t answer;
    char name[64];
    gl_vita_location_cache_reset();
    isaac_gl_record_locations_reset(&memo);
    for (candidate = 0u; candidate < 100000u && count < 24u; ++candidate) {
        IsaacGlRecordLocationKey key;
        gl_vita_location_cache_key original;
        sprintf_s(name, sizeof name, "collision_%u", candidate);
        assert(isaac_gl_record_location_key(1u, 17u, name, strlen(name) + 1u, &key));
        (void)gl_vita_location_cache_lookup(1u, 17u,
            (uint32_t)(uintptr_t)name, &original, &answer);
        assert(original.cacheable && original.hash == key.hash);
        if (original.slot == 0u) collisions[count++] = key;
    }
    assert(count == 24u);
    gl_vita_location_cache_reset();
    for (round = 0u; round < 9u; ++round) {
        for (i = 0u; i < count; ++i) {
            gl_vita_location_cache_key original;
            int32_t value = i == 7u ? -1 : (int32_t)i;
            /* Poisoned/reused name address, same bytes as the recorded key. */
            memcpy(name, collisions[i].name, sizeof name);
            if (!gl_vita_location_cache_lookup(1u, 17u,
                    (uint32_t)(uintptr_t)name, &original, &answer)) {
                ++original_misses;
                gl_vita_location_cache_store(&original, value);
                answer = value;
            }
            assert(answer == value);
            if (!isaac_gl_record_locations_lookup(&memo, &collisions[i], &answer)) {
                ++memo_misses;
                assert(isaac_gl_record_locations_store(&memo, &collisions[i], value));
                answer = value;
            }
            assert(answer == value);
            memset(name, 0xa5, sizeof name);
        }
    }
    assert(original_misses == 9u * 24u && memo_misses == 24u);
    printf("PASS exact ORIGINAL 256-slot eviction pattern: %u direct misses, %u memo cold misses, 0 repeat misses\n",
        original_misses, memo_misses);
    gl_vita_location_cache_invalidate();
    isaac_gl_record_locations_reset(&memo);
    assert(!gl_vita_location_cache_lookup(1u, 17u,
        (uint32_t)(uintptr_t)collisions[0].name, &(gl_vita_location_cache_key){0}, &answer));
    assert(!isaac_gl_record_locations_lookup(&memo, &collisions[0], &answer));
}

static void check_capacity_and_mutation(void)
{
    uint32_t i;
    int32_t answer;
    char name[65];
    IsaacGlRecordLocationKey key;
    isaac_gl_record_locations_reset(&memo);
    for (i = 0u; i <= ISAAC_GL_RECORD_LOCATION_CAPACITY; ++i) {
        sprintf_s(name, sizeof name, "pair_%u", i);
        assert(isaac_gl_record_location_key((i & 1u) + 1u, (i % 32u) + 1u,
            name, strlen(name) + 1u, &keys[i]));
        assert(isaac_gl_record_locations_store(&memo, &keys[i], (int32_t)i) ==
            (i < ISAAC_GL_RECORD_LOCATION_CAPACITY));
    }
    assert(memo.used == ISAAC_GL_RECORD_LOCATION_CAPACITY);
    for (i = 0u; i < ISAAC_GL_RECORD_LOCATION_CAPACITY; ++i) {
        assert(isaac_gl_record_locations_lookup(&memo, &keys[i], &answer));
        assert(answer == (int32_t)i);
    }
    assert(isaac_gl_record_locations_store(&memo, &keys[0], -1));
    assert(memo.used == ISAAC_GL_RECORD_LOCATION_CAPACITY);
    assert(isaac_gl_record_locations_lookup(&memo, &keys[0], &answer) && answer == -1);
    assert(!isaac_gl_record_locations_lookup(&memo, &keys[ISAAC_GL_RECORD_LOCATION_CAPACITY], &answer));
    isaac_gl_record_locations_reset(&memo); /* attach/compile/link/source/delete fence */
    assert(memo.used == 0u && !isaac_gl_record_locations_lookup(&memo, &keys[0], &answer));
    strcpy_s(name, sizeof name, "Transform");
    assert(isaac_gl_record_location_key(2u, 9u, name, 10u, &key));
    assert(isaac_gl_record_locations_store(&memo, &key, 123));
    strcpy_s(name, sizeof name, "Texture0");
    assert(isaac_gl_record_location_key(2u, 9u, name, 9u, &key));
    assert(!isaac_gl_record_locations_lookup(&memo, &key, &answer));
    strcpy_s(name, sizeof name, "Transform");
    assert(isaac_gl_record_location_key(1u, 9u, name, 10u, &key));
    assert(!isaac_gl_record_locations_lookup(&memo, &key, &answer));
    assert(isaac_gl_record_location_key(2u, 10u, name, 10u, &key));
    assert(!isaac_gl_record_locations_lookup(&memo, &key, &answer));
    assert(!isaac_gl_record_location_key(2u, 0u, name, sizeof name, &key));
    assert(!isaac_gl_record_location_key(3u, 1u, name, sizeof name, &key));
    assert(!isaac_gl_record_location_key(1u, 1u, NULL, 0u, &key));
    assert(!isaac_gl_record_location_key(1u, 1u, "", 1u, &key));
    assert(!isaac_gl_record_location_key(1u, 1u, "a\x80", 3u, &key));
    assert(!isaac_gl_record_location_key(1u, 1u, "no terminator", 2u, &key));
    memset(name, 'a', 63u); name[63] = 0;
    assert(isaac_gl_record_location_key(1u, 1u, name, 64u, &key));
    name[63] = 'a'; name[64] = 0;
    assert(!isaac_gl_record_location_key(1u, 1u, name, sizeof name, &key));
    printf("PASS location ownership/kind/program/-1/mutation/reset/bounds/full-no-eviction; memo=%zu bytes\n",
        sizeof memo);
}

int main(void)
{
    check_collision_pattern();
    check_capacity_and_mutation();
    return 0;
}
