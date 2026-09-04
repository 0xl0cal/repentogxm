#ifndef ISAAC_MANAGER_SHA256_H
#define ISAAC_MANAGER_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct manager_sha256_context {
    uint32_t state[8];
    uint64_t byte_count;
    unsigned char block[64];
    size_t block_size;
};

void manager_sha256_init(struct manager_sha256_context *context);
void manager_sha256_update(struct manager_sha256_context *context,
                           const void *data, size_t size);
void manager_sha256_final(struct manager_sha256_context *context,
                          unsigned char digest[32]);

#endif
