#include "manager_sha256.h"

#include <string.h>

/*
 * Project-local SHA-256, copied from the already tested guest_pe.h loader
 * implementation.  Keeping this tiny streaming copy avoids a new library and
 * lets the manager hash a save while copying it through a bounded buffer.
 */

static uint32_t manager_sha256_rotr(uint32_t value, unsigned int count)
{
    return (value >> count) | (value << (32u - count));
}

static uint32_t manager_sha256_load_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
           ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static void manager_sha256_transform(struct manager_sha256_context *context,
                                     const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned int index;

    for (index = 0; index < 16u; ++index)
        words[index] = manager_sha256_load_be32(block + index * 4u);
    for (; index < 64u; ++index) {
        const uint32_t x = words[index - 15u];
        const uint32_t y = words[index - 2u];
        const uint32_t s0 = manager_sha256_rotr(x, 7u) ^
                            manager_sha256_rotr(x, 18u) ^ (x >> 3u);
        const uint32_t s1 = manager_sha256_rotr(y, 17u) ^
                            manager_sha256_rotr(y, 19u) ^ (y >> 10u);
        words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0; index < 64u; ++index) {
        const uint32_t sum1 = manager_sha256_rotr(e, 6u) ^
                              manager_sha256_rotr(e, 11u) ^
                              manager_sha256_rotr(e, 25u);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temporary1 = h + sum1 + choose +
                                    constants[index] + words[index];
        const uint32_t sum0 = manager_sha256_rotr(a, 2u) ^
                              manager_sha256_rotr(a, 13u) ^
                              manager_sha256_rotr(a, 22u);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void manager_sha256_init(struct manager_sha256_context *context)
{
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(context->state, initial, sizeof(initial));
    context->byte_count = 0u;
    context->block_size = 0u;
}

void manager_sha256_update(struct manager_sha256_context *context,
                           const void *data_pointer, size_t size)
{
    const unsigned char *data = (const unsigned char *)data_pointer;
    context->byte_count += size;
    while (size != 0u) {
        size_t available = sizeof(context->block) - context->block_size;
        size_t amount = size < available ? size : available;
        memcpy(context->block + context->block_size, data, amount);
        context->block_size += amount;
        data += amount;
        size -= amount;
        if (context->block_size == sizeof(context->block)) {
            manager_sha256_transform(context, context->block);
            context->block_size = 0u;
        }
    }
}

void manager_sha256_final(struct manager_sha256_context *context,
                          unsigned char digest[32])
{
    const uint64_t bit_count = context->byte_count * 8u;
    unsigned char tail[128];
    size_t padding;
    unsigned int index;

    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80u;
    padding = context->block_size < 56u ? 56u - context->block_size
                                       : 120u - context->block_size;
    for (index = 0; index < 8u; ++index)
        tail[padding + index] =
            (unsigned char)(bit_count >> (56u - index * 8u));
    manager_sha256_update(context, tail, padding + 8u);
    for (index = 0; index < 8u; ++index) {
        digest[index * 4u] = (unsigned char)(context->state[index] >> 24u);
        digest[index * 4u + 1u] =
            (unsigned char)(context->state[index] >> 16u);
        digest[index * 4u + 2u] =
            (unsigned char)(context->state[index] >> 8u);
        digest[index * 4u + 3u] = (unsigned char)context->state[index];
    }
}
