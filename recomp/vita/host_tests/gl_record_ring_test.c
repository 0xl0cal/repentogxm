#include "gl_record_ring.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static IsaacGlRecordRing ring;
static unsigned char payload[4096];
static uint32_t replayed, expected_opcode;
static void replay(void *context, const IsaacGlRecordHeader *header,
    const void *bytes)
{
    const unsigned char *p = (const unsigned char *)bytes;
    uint32_t i;
    assert(context == &ring);
    assert(header->opcode == expected_opcode++);
    assert(header->return_rva == 0x0056d794u);
    assert(header->payload_bytes == sizeof payload);
    assert(!((uintptr_t)bytes & 7u));
    for (i = 0u; i < header->payload_bytes; ++i) assert(p[i] == 0x5au);
    assert(!isaac_gl_record_ring_append(&ring, 999u, 0u, NULL, 0u));
    ++replayed;
}
int main(void)
{
    uint32_t i, used;
    isaac_gl_record_ring_init(&ring, replay, &ring);
    for (i = 0u; i < 400u; ++i) {
        memset(payload, 0x5a, sizeof payload);
        assert(isaac_gl_record_ring_append(&ring, i, 0x0056d794u,
            payload, sizeof payload));
        memset(payload, 0xa5, sizeof payload); /* producer reuse/poison */
    }
    assert(ring.overflow_drains > 0u);
    assert(isaac_gl_record_ring_drain(&ring));
    assert(replayed == 400u && ring.records == 400u && !ring.used);
    used = ring.used;
    assert(!isaac_gl_record_ring_append(&ring, 0u, 0u, payload, UINT32_MAX));
    assert(!isaac_gl_record_ring_append(&ring, 0u, 0u, NULL, 1u));
    assert(ring.used == used);
    assert(isaac_gl_record_ring_drain(&ring) && replayed == 400u);
    printf("PASS ring transport: 400 ordered owned/poisoned payloads, overflow drain/no drop, alignment, bounds, reentry\n");
    return 0;
}
