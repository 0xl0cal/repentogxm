#ifndef ISAAC_VITAGL_SHADER_CACHE_POLICY_H
#define ISAAC_VITAGL_SHADER_CACHE_POLICY_H

/* Project-local persistent custom-shader cache for the exact 73dd57a
 * vitaGL build.  This header deliberately does not reuse vitaGL's
 * HAVE_SHADER_CACHE implementation: that path trusts lengths and frees the
 * source before it knows whether a cached program is usable.
 *
 * The policy is header-only so the same byte parser and publication code is
 * compiled by custom_shaders.c and by the hostile host oracle.  Production
 * gets its I/O and allocation surface from vitaGL/VitaSDK; the oracle supplies
 * deterministic fault-injection adapters below.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ISAAC_SHADER_CACHE_BUILD_HEX
#error "ISAAC_SHADER_CACHE_BUILD_HEX must be the exact 64-digit recipe SHA-256"
#endif

#define ISAAC_SHADER_CACHE_ROOT \
    "ux0:data/isaacr001/shader-cache"
#define ISAAC_SHADER_CACHE_VERSION       2u
#define ISAAC_SHADER_CACHE_HEADER_SIZE   256u
#define ISAAC_SHADER_CACHE_GXP_MAX       (1024u * 1024u)
#define ISAAC_SHADER_CACHE_METADATA_MAX  (64u * 1024u)
#define ISAAC_SHADER_CACHE_MATRIX_MAX    1024u
#define ISAAC_SHADER_CACHE_BLOCK_MAX     14u
#define ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE 128u
#define ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE 16u
#define ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE \
    (sizeof(uint32_t) + ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE)
#define ISAAC_SHADER_CACHE_PATH_MAX      192u
#define ISAAC_SHADER_CACHE_TEMP_ATTEMPTS 8u

#define ISAAC_SHADER_CACHE_FLAG_BINDINGS 0x00000001u
#define ISAAC_SHADER_CACHE_FLAG_PAIR     0x00000002u
#define ISAAC_SHADER_CACHE_FLAG_GLSL     0x00000004u
#define ISAAC_SHADER_CACHE_FLAG_PEER_GLSL 0x00000008u
#define ISAAC_SHADER_CACHE_FLAG_MASK     0x0000000fu

#define ISAAC_SHADER_CACHE_VERTEX   1u
#define ISAAC_SHADER_CACHE_FRAGMENT 2u

enum isaac_shader_cache_load_result {
    ISAAC_SHADER_CACHE_LOAD_MISS = 0,
    ISAAC_SHADER_CACHE_LOAD_CANDIDATE = 1,
    ISAAC_SHADER_CACHE_LOAD_CORRUPT = 2
};

enum isaac_shader_cache_program_result {
    ISAAC_SHADER_CACHE_PROGRAM_ACCEPT = 0,
    ISAAC_SHADER_CACHE_PROGRAM_CHECK_FAILED = 1,
    ISAAC_SHADER_CACHE_PROGRAM_SIZE_FAILED = 2,
    ISAAC_SHADER_CACHE_PROGRAM_TYPE_FAILED = 3,
    ISAAC_SHADER_CACHE_PROGRAM_REGISTER_FAILED = 4
};

typedef struct isaac_shader_cache_key {
    uint32_t shader_type;
    uint32_t translator_mode;
    uint32_t compiler_flags;
    uint32_t flags;
    uint32_t source_len;
    uint32_t peer_len;
    unsigned char build_sha256[32];
    unsigned char source_sha256[32];
    unsigned char peer_sha256[32];
    unsigned char key_sha256[32];
} isaac_shader_cache_key;

typedef struct isaac_shader_cache_record {
    unsigned char *body;
    uint32_t body_size;
    uint32_t gxp_len;
    uint32_t metadata_len;
    uint32_t matrix_count;
    uint32_t flags;
} isaac_shader_cache_record;

typedef struct isaac_shader_cache_block_descriptor {
    uint32_t index;
    char name[ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE];
} isaac_shader_cache_block_descriptor;

typedef struct isaac_shader_cache_stats {
    uint64_t serial;
    uint64_t attempts;
    uint64_t hits;
    uint64_t misses;
    uint64_t corrupt;
    uint64_t removes;
    uint64_t remove_failures;
    uint64_t read_failures;
    uint64_t alloc_failures;
    uint64_t oversize;
    uint64_t native_compiles;
    uint64_t register_failures;
    uint64_t write_ok;
    uint64_t write_failures;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t read_us;
    uint64_t hash_us;
    uint64_t register_us;
    uint64_t compile_us;
    uint64_t write_us;
    uint64_t publish_attempts;
    uint64_t publish_with_block_list;
    uint64_t publish_ready;
    uint64_t publish_block_records;
    uint64_t publish_reject_shape;
    uint64_t publish_reject_semantics;
    uint64_t publish_reject_matrices;
    uint64_t publish_reject_blocks;
    uint64_t publish_reject_incomplete;
    uint64_t publish_reject_alloc;
} isaac_shader_cache_stats;

typedef struct isaac_shader_cache_program_ops {
    int (*check)(void *context, const void *program);
    uint32_t (*size)(void *context, const void *program);
    uint32_t (*type)(void *context, const void *program);
    int (*register_program)(void *context, const void *program,
                            uintptr_t *registered_id);
} isaac_shader_cache_program_ops;

#if defined(ISAAC_SHADER_CACHE_ORACLE)
int isaac_shader_cache_oracle_open(const char *path, int write_exclusive);
int64_t isaac_shader_cache_oracle_seek(int descriptor, int64_t offset,
                                       int origin);
int isaac_shader_cache_oracle_read(int descriptor, void *buffer,
                                   uint32_t size);
int isaac_shader_cache_oracle_write(int descriptor, const void *buffer,
                                    uint32_t size);
int isaac_shader_cache_oracle_close(int descriptor);
int isaac_shader_cache_oracle_sync_fd(int descriptor);
int isaac_shader_cache_oracle_mkdir(const char *path);
int isaac_shader_cache_oracle_remove(const char *path);
int isaac_shader_cache_oracle_rename(const char *source,
                                     const char *destination);
int isaac_shader_cache_oracle_sync_device(const char *device);
void *isaac_shader_cache_oracle_alloc(uint32_t size);
void isaac_shader_cache_oracle_free(void *pointer);
uint64_t isaac_shader_cache_oracle_now_us(void);
#define isaac_shader_cache_io_open   isaac_shader_cache_oracle_open
#define isaac_shader_cache_io_seek   isaac_shader_cache_oracle_seek
#define isaac_shader_cache_io_read   isaac_shader_cache_oracle_read
#define isaac_shader_cache_io_write  isaac_shader_cache_oracle_write
#define isaac_shader_cache_io_close  isaac_shader_cache_oracle_close
#define isaac_shader_cache_io_sync   isaac_shader_cache_oracle_sync_fd
#define isaac_shader_cache_io_mkdir  isaac_shader_cache_oracle_mkdir
#define isaac_shader_cache_io_remove isaac_shader_cache_oracle_remove
#define isaac_shader_cache_io_rename isaac_shader_cache_oracle_rename
#define isaac_shader_cache_io_sync_device \
    isaac_shader_cache_oracle_sync_device
#define isaac_shader_cache_alloc isaac_shader_cache_oracle_alloc
#define isaac_shader_cache_free  isaac_shader_cache_oracle_free
#define isaac_shader_cache_now_us isaac_shader_cache_oracle_now_us
#else
static inline int isaac_shader_cache_io_open(const char *path,
                                              int write_exclusive)
{
    int flags = write_exclusive
        ? (SCE_O_CREAT | SCE_O_EXCL | SCE_O_WRONLY) : SCE_O_RDONLY;
    return sceIoOpen(path, flags, 0666);
}
static inline int64_t isaac_shader_cache_io_seek(int descriptor,
                                                  int64_t offset,
                                                  int origin)
{
    return (int64_t)sceIoLseek(descriptor, (SceOff)offset, origin);
}
static inline int isaac_shader_cache_io_read(int descriptor, void *buffer,
                                              uint32_t size)
{
    return sceIoRead(descriptor, buffer, size);
}
static inline int isaac_shader_cache_io_write(int descriptor,
                                               const void *buffer,
                                               uint32_t size)
{
    return sceIoWrite(descriptor, buffer, size);
}
static inline int isaac_shader_cache_io_close(int descriptor)
{
    return sceIoClose(descriptor);
}
static inline int isaac_shader_cache_io_sync(int descriptor)
{
    return sceIoSyncByFd(descriptor, 0);
}
static inline int isaac_shader_cache_io_mkdir(const char *path)
{
    return sceIoMkdir(path, 0777);
}
static inline int isaac_shader_cache_io_remove(const char *path)
{
    return sceIoRemove(path);
}
static inline int isaac_shader_cache_io_rename(const char *source,
                                                const char *destination)
{
    return sceIoRename(source, destination);
}
static inline int isaac_shader_cache_io_sync_device(const char *device)
{
    return sceIoSync(device, 0);
}
static inline void *isaac_shader_cache_alloc(uint32_t size)
{
    return vglMalloc(size);
}
static inline void isaac_shader_cache_free(void *pointer)
{
    vgl_free(pointer);
}
static inline uint64_t isaac_shader_cache_now_us(void)
{
    return sceKernelGetProcessTimeWide();
}
#endif

typedef struct isaac_shader_cache_sha256_context {
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    uint32_t used;
} isaac_shader_cache_sha256_context;

static isaac_shader_cache_stats s_isaac_shader_cache_stats;
static uint32_t s_isaac_shader_cache_temp_serial;

static inline uint32_t isaac_shader_cache_rotr32(uint32_t value,
                                                 unsigned amount)
{
    return (value >> amount) | (value << (32u - amount));
}

static void isaac_shader_cache_sha256_transform(
    isaac_shader_cache_sha256_context *context,
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
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t index;

    for (index = 0; index < 16u; ++index) {
        const unsigned char *source = block + index * 4u;
        words[index] = ((uint32_t)source[0] << 24u) |
                       ((uint32_t)source[1] << 16u) |
                       ((uint32_t)source[2] << 8u) |
                       (uint32_t)source[3];
    }
    for (index = 16u; index < 64u; ++index) {
        uint32_t left = words[index - 15u];
        uint32_t right = words[index - 2u];
        uint32_t s0 = isaac_shader_cache_rotr32(left, 7u) ^
                      isaac_shader_cache_rotr32(left, 18u) ^ (left >> 3u);
        uint32_t s1 = isaac_shader_cache_rotr32(right, 17u) ^
                      isaac_shader_cache_rotr32(right, 19u) ^ (right >> 10u);
        words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
    }

    a = context->state[0]; b = context->state[1];
    c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5];
    g = context->state[6]; h = context->state[7];
    for (index = 0; index < 64u; ++index) {
        uint32_t sum1 = isaac_shader_cache_rotr32(e, 6u) ^
                        isaac_shader_cache_rotr32(e, 11u) ^
                        isaac_shader_cache_rotr32(e, 25u);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + sum1 + choice + constants[index] + words[index];
        uint32_t sum0 = isaac_shader_cache_rotr32(a, 2u) ^
                        isaac_shader_cache_rotr32(a, 13u) ^
                        isaac_shader_cache_rotr32(a, 22u);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}

static void isaac_shader_cache_sha256_init(
    isaac_shader_cache_sha256_context *context)
{
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(context->state, initial, sizeof initial);
    context->bytes = 0u;
    context->used = 0u;
}

static void isaac_shader_cache_sha256_update(
    isaac_shader_cache_sha256_context *context,
    const void *data_pointer, size_t size)
{
    const unsigned char *data = (const unsigned char *)data_pointer;
    context->bytes += size;
    while (size != 0u) {
        size_t available = sizeof context->block - context->used;
        size_t amount = size < available ? size : available;
        memcpy(context->block + context->used, data, amount);
        context->used += (uint32_t)amount;
        data += amount;
        size -= amount;
        if (context->used == sizeof context->block) {
            isaac_shader_cache_sha256_transform(context, context->block);
            context->used = 0u;
        }
    }
}

static void isaac_shader_cache_sha256_final(
    isaac_shader_cache_sha256_context *context,
    unsigned char digest[32])
{
    unsigned char tail[128];
    uint64_t bits = context->bytes * UINT64_C(8);
    size_t padding = context->used < 56u
        ? 56u - context->used : 120u - context->used;
    uint32_t index;

    memset(tail, 0, sizeof tail);
    tail[0] = 0x80u;
    for (index = 0; index < 8u; ++index)
        tail[padding + index] = (unsigned char)(bits >> (56u - index * 8u));
    isaac_shader_cache_sha256_update(context, tail, padding + 8u);
    for (index = 0; index < 8u; ++index) {
        digest[index * 4u] = (unsigned char)(context->state[index] >> 24u);
        digest[index * 4u + 1u] =
            (unsigned char)(context->state[index] >> 16u);
        digest[index * 4u + 2u] =
            (unsigned char)(context->state[index] >> 8u);
        digest[index * 4u + 3u] = (unsigned char)context->state[index];
    }
}

static void isaac_shader_cache_sha256(const void *data, size_t size,
                                      unsigned char digest[32])
{
    isaac_shader_cache_sha256_context context;
    isaac_shader_cache_sha256_init(&context);
    isaac_shader_cache_sha256_update(&context, data, size);
    isaac_shader_cache_sha256_final(&context, digest);
}

static inline void isaac_shader_cache_put_u32(unsigned char *output,
                                               uint32_t value)
{
    output[0] = (unsigned char)value;
    output[1] = (unsigned char)(value >> 8u);
    output[2] = (unsigned char)(value >> 16u);
    output[3] = (unsigned char)(value >> 24u);
}

static inline uint32_t isaac_shader_cache_get_u32(
    const unsigned char *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8u) |
           ((uint32_t)input[2] << 16u) | ((uint32_t)input[3] << 24u);
}

static int isaac_shader_cache_metadata_layout(
    uint32_t matrix_count, uint32_t block_count, uint32_t bindings_size,
    uint32_t *metadata_size, uint32_t *matrix_offset,
    uint32_t *block_offset, uint32_t *bindings_offset)
{
    uint32_t matrices_size;
    uint32_t blocks_size;
    uint32_t size = ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE;

    if (matrix_count > ISAAC_SHADER_CACHE_MATRIX_MAX ||
            block_count > ISAAC_SHADER_CACHE_BLOCK_MAX ||
            matrix_count > UINT32_MAX / sizeof(uint32_t) ||
            block_count > UINT32_MAX / ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE)
        return 0;
    matrices_size = matrix_count * (uint32_t)sizeof(uint32_t);
    blocks_size = block_count * (uint32_t)ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE;
    if (size > UINT32_MAX - matrices_size)
        return 0;
    if (matrix_offset)
        *matrix_offset = size;
    size += matrices_size;
    if (size > UINT32_MAX - blocks_size)
        return 0;
    if (block_offset)
        *block_offset = size;
    size += blocks_size;
    if (size > UINT32_MAX - bindings_size)
        return 0;
    if (bindings_offset)
        *bindings_offset = size;
    size += bindings_size;
    if (size > ISAAC_SHADER_CACHE_METADATA_MAX)
        return 0;
    if (metadata_size)
        *metadata_size = size;
    return 1;
}

static void isaac_shader_cache_metadata_encode_header(
    unsigned char metadata[ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE],
    uint32_t matrix_count, uint32_t block_count)
{
    static const unsigned char magic[4] = {'I', 'S', 'M', '2'};
    memset(metadata, 0, ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE);
    memcpy(metadata, magic, sizeof magic);
    isaac_shader_cache_put_u32(metadata + 4u, 2u);
    isaac_shader_cache_put_u32(metadata + 8u, matrix_count);
    isaac_shader_cache_put_u32(metadata + 12u, block_count);
}

static int isaac_shader_cache_metadata_decode_header(
    const unsigned char *metadata, uint32_t metadata_len,
    uint32_t expected_matrix_count, uint32_t bindings_size,
    uint32_t *block_count, uint32_t *matrix_offset,
    uint32_t *block_offset, uint32_t *bindings_offset)
{
    static const unsigned char magic[4] = {'I', 'S', 'M', '2'};
    uint32_t encoded_matrix_count;
    uint32_t encoded_block_count;
    uint32_t expected_size;

    if (!metadata || metadata_len < ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE ||
            memcmp(metadata, magic, sizeof magic) != 0 ||
            isaac_shader_cache_get_u32(metadata + 4u) != 2u)
        return 0;
    encoded_matrix_count = isaac_shader_cache_get_u32(metadata + 8u);
    encoded_block_count = isaac_shader_cache_get_u32(metadata + 12u);
    if (encoded_matrix_count != expected_matrix_count ||
            !isaac_shader_cache_metadata_layout(
                encoded_matrix_count, encoded_block_count, bindings_size,
                &expected_size, matrix_offset, block_offset,
                bindings_offset) ||
            expected_size != metadata_len)
        return 0;
    if (block_count)
        *block_count = encoded_block_count;
    return 1;
}

static int isaac_shader_cache_block_name_valid(
    const char name[ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE], size_t *length)
{
    const char *terminator = (const char *)memchr(
        name, '\0', ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE);
    size_t amount;
    if (!terminator || terminator == name)
        return 0;
    amount = (size_t)(terminator - name);
    if (length)
        *length = amount;
    return 1;
}

static int isaac_shader_cache_encode_block(
    unsigned char output[ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE],
    const isaac_shader_cache_block_descriptor *descriptor)
{
    size_t length;
    if (!descriptor || descriptor->index >= ISAAC_SHADER_CACHE_BLOCK_MAX ||
            !isaac_shader_cache_block_name_valid(descriptor->name, &length))
        return 0;
    memset(output, 0, ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE);
    isaac_shader_cache_put_u32(output, descriptor->index);
    memcpy(output + sizeof(uint32_t), descriptor->name, length + 1u);
    return 1;
}

static int isaac_shader_cache_decode_block(
    const unsigned char input[ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE],
    isaac_shader_cache_block_descriptor *descriptor)
{
    const unsigned char *encoded_name;
    const unsigned char *terminator;
    const unsigned char *cursor;
    uint32_t index;
    if (!input || !descriptor)
        return 0;
    encoded_name = input + sizeof(uint32_t);
    index = isaac_shader_cache_get_u32(input);
    terminator = (const unsigned char *)memchr(
        encoded_name, 0, ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE);
    if (index >= ISAAC_SHADER_CACHE_BLOCK_MAX ||
            !terminator || terminator == encoded_name)
        return 0;
    for (cursor = terminator + 1u;
            cursor < encoded_name + ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE;
            ++cursor) {
        if (*cursor != 0u)
            return 0;
    }
    memset(descriptor, 0, sizeof *descriptor);
    descriptor->index = index;
    memcpy(descriptor->name, encoded_name,
        (size_t)(terminator - encoded_name) + 1u);
    return 1;
}

static int isaac_shader_cache_blocks_unique(
    const isaac_shader_cache_block_descriptor *blocks, uint32_t block_count)
{
    uint32_t left;
    uint32_t right;
    if (block_count > ISAAC_SHADER_CACHE_BLOCK_MAX ||
            (block_count != 0u && !blocks))
        return 0;
    for (left = 0u; left < block_count; ++left) {
        if (blocks[left].index >= ISAAC_SHADER_CACHE_BLOCK_MAX ||
                !isaac_shader_cache_block_name_valid(
                    blocks[left].name, NULL))
            return 0;
        for (right = left + 1u; right < block_count; ++right) {
            if (blocks[left].index == blocks[right].index ||
                    strncmp(blocks[left].name, blocks[right].name,
                        ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE) == 0)
                return 0;
        }
    }
    return 1;
}

static void isaac_shader_cache_touch(void)
{
    ++s_isaac_shader_cache_stats.serial;
}

static int isaac_shader_cache_hex_nibble(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int isaac_shader_cache_build_digest(unsigned char digest[32])
{
    static const char build_hex[] = ISAAC_SHADER_CACHE_BUILD_HEX;
    uint32_t index;
    if (sizeof build_hex != 65u)
        return 0;
    for (index = 0u; index < 32u; ++index) {
        int high = isaac_shader_cache_hex_nibble(build_hex[index * 2u]);
        int low = isaac_shader_cache_hex_nibble(build_hex[index * 2u + 1u]);
        if (high < 0 || low < 0)
            return 0;
        digest[index] = (unsigned char)((unsigned)high << 4u | (unsigned)low);
    }
    return 1;
}

static inline void isaac_shader_cache_hash_timed(
    const void *data, size_t size, unsigned char digest[32])
{
    uint64_t start = isaac_shader_cache_now_us();
    isaac_shader_cache_sha256(data, size, digest);
    s_isaac_shader_cache_stats.hash_us +=
        isaac_shader_cache_now_us() - start;
}

static int isaac_shader_cache_make_key(
    isaac_shader_cache_key *key,
    uint32_t shader_type, uint32_t translator_mode,
    uint32_t compiler_flags, uint32_t flags,
    uint32_t source_len, const unsigned char source_sha256[32],
    uint32_t peer_len, const unsigned char peer_sha256[32])
{
    static const unsigned char domain[] = "ISAAC-VITA-SHADER-KEY-V2";
    isaac_shader_cache_sha256_context context;
    unsigned char numbers[24];
    uint64_t start;

    if (!key || !source_sha256 ||
            (shader_type != ISAAC_SHADER_CACHE_VERTEX &&
             shader_type != ISAAC_SHADER_CACHE_FRAGMENT) ||
            (flags & ~ISAAC_SHADER_CACHE_FLAG_MASK) != 0u ||
            source_len == 0u ||
            (((flags & ISAAC_SHADER_CACHE_FLAG_PAIR) != 0u) !=
             (peer_len != 0u)) ||
            (peer_len != 0u && !peer_sha256))
        return 0;
    memset(key, 0, sizeof *key);
    key->shader_type = shader_type;
    key->translator_mode = translator_mode;
    key->compiler_flags = compiler_flags;
    key->flags = flags;
    key->source_len = source_len;
    key->peer_len = peer_len;
    if (!isaac_shader_cache_build_digest(key->build_sha256))
        return 0;
    memcpy(key->source_sha256, source_sha256, 32u);
    if (peer_len != 0u)
        memcpy(key->peer_sha256, peer_sha256, 32u);

    isaac_shader_cache_put_u32(numbers + 0u, shader_type);
    isaac_shader_cache_put_u32(numbers + 4u, translator_mode);
    isaac_shader_cache_put_u32(numbers + 8u, compiler_flags);
    isaac_shader_cache_put_u32(numbers + 12u, flags);
    isaac_shader_cache_put_u32(numbers + 16u, source_len);
    isaac_shader_cache_put_u32(numbers + 20u, peer_len);
    start = isaac_shader_cache_now_us();
    isaac_shader_cache_sha256_init(&context);
    isaac_shader_cache_sha256_update(&context, domain, sizeof domain - 1u);
    isaac_shader_cache_sha256_update(&context, numbers, sizeof numbers);
    isaac_shader_cache_sha256_update(&context, key->build_sha256, 32u);
    isaac_shader_cache_sha256_update(&context, key->source_sha256, 32u);
    isaac_shader_cache_sha256_update(&context, key->peer_sha256, 32u);
    isaac_shader_cache_sha256_final(&context, key->key_sha256);
    s_isaac_shader_cache_stats.hash_us +=
        isaac_shader_cache_now_us() - start;
    return 1;
}

static int isaac_shader_cache_append(char *output, size_t capacity,
                                     size_t *length, const char *text)
{
    size_t amount = strlen(text);
    if (*length > capacity || amount >= capacity - *length)
        return 0;
    memcpy(output + *length, text, amount);
    *length += amount;
    output[*length] = '\0';
    return 1;
}

static int isaac_shader_cache_path(const isaac_shader_cache_key *key,
                                   char output[ISAAC_SHADER_CACHE_PATH_MAX])
{
    static const char digits[] = "0123456789abcdef";
    size_t length = 0u;
    uint32_t index;
    output[0] = '\0';
    if (!isaac_shader_cache_append(output, ISAAC_SHADER_CACHE_PATH_MAX,
            &length, ISAAC_SHADER_CACHE_ROOT "/v2/") ||
        !isaac_shader_cache_append(output, ISAAC_SHADER_CACHE_PATH_MAX,
            &length, key->shader_type == ISAAC_SHADER_CACHE_VERTEX
                ? "v/" : "f/"))
        return 0;
    if (length + 64u + 4u >= ISAAC_SHADER_CACHE_PATH_MAX)
        return 0;
    for (index = 0u; index < 32u; ++index) {
        output[length++] = digits[key->key_sha256[index] >> 4u];
        output[length++] = digits[key->key_sha256[index] & 15u];
    }
    memcpy(output + length, ".bin", 5u);
    return 1;
}

static int isaac_shader_cache_temp_path(
    const isaac_shader_cache_key *key,
    char output[ISAAC_SHADER_CACHE_PATH_MAX], uint32_t serial)
{
    static const char digits[] = "0123456789abcdef";
    size_t length;
    unsigned shift;
    if (!isaac_shader_cache_path(key, output))
        return 0;
    length = strlen(output);
    if (length + 13u >= ISAAC_SHADER_CACHE_PATH_MAX)
        return 0;
    memcpy(output + length, ".tmp.", 5u);
    length += 5u;
    for (shift = 0u; shift < 32u; shift += 4u)
        output[length++] = digits[(serial >> (28u - shift)) & 15u];
    output[length] = '\0';
    return 1;
}

static void isaac_shader_cache_remove_corrupt(const char *path)
{
    ++s_isaac_shader_cache_stats.corrupt;
    if (isaac_shader_cache_io_remove(path) == 0)
        ++s_isaac_shader_cache_stats.removes;
    else
        ++s_isaac_shader_cache_stats.remove_failures;
    isaac_shader_cache_touch();
}

static int isaac_shader_cache_read_exact(int descriptor, void *buffer,
                                         uint32_t size)
{
    unsigned char *cursor = (unsigned char *)buffer;
    uint32_t remaining = size;
    while (remaining != 0u) {
        int result = isaac_shader_cache_io_read(descriptor, cursor, remaining);
        if (result <= 0 || (uint32_t)result > remaining)
            return 0;
        cursor += result;
        remaining -= (uint32_t)result;
        s_isaac_shader_cache_stats.bytes_read += (uint32_t)result;
    }
    return 1;
}

static int isaac_shader_cache_write_exact(int descriptor, const void *buffer,
                                          uint32_t size)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    uint32_t remaining = size;
    while (remaining != 0u) {
        int result = isaac_shader_cache_io_write(
            descriptor, cursor, remaining);
        if (result <= 0 || (uint32_t)result > remaining)
            return 0;
        cursor += result;
        remaining -= (uint32_t)result;
        s_isaac_shader_cache_stats.bytes_written += (uint32_t)result;
    }
    return 1;
}

static void isaac_shader_cache_record_digest(
    const unsigned char header[ISAAC_SHADER_CACHE_HEADER_SIZE],
    const void *gxp, uint32_t gxp_len,
    const void *metadata, uint32_t metadata_len,
    unsigned char digest[32])
{
    isaac_shader_cache_sha256_context context;
    isaac_shader_cache_sha256_init(&context);
    /* The digest field occupies [224,256); every preceding header byte and
     * every payload byte is authenticated. */
    isaac_shader_cache_sha256_update(&context, header, 224u);
    isaac_shader_cache_sha256_update(&context, gxp, gxp_len);
    isaac_shader_cache_sha256_update(&context, metadata, metadata_len);
    isaac_shader_cache_sha256_final(&context, digest);
}

static void isaac_shader_cache_encode_header(
    unsigned char header[ISAAC_SHADER_CACHE_HEADER_SIZE],
    const isaac_shader_cache_key *key,
    const void *gxp, uint32_t gxp_len,
    const void *metadata, uint32_t metadata_len, uint32_t matrix_count)
{
    static const unsigned char magic[16] = {
        'I','S','A','A','C','V','S','C','G','X','P','V','2',0,0,0
    };
    uint64_t start = isaac_shader_cache_now_us();
    memset(header, 0, ISAAC_SHADER_CACHE_HEADER_SIZE);
    memcpy(header, magic, sizeof magic);
    isaac_shader_cache_put_u32(header + 16u, ISAAC_SHADER_CACHE_VERSION);
    isaac_shader_cache_put_u32(header + 20u,
                               ISAAC_SHADER_CACHE_HEADER_SIZE);
    isaac_shader_cache_put_u32(header + 24u,
        ISAAC_SHADER_CACHE_HEADER_SIZE + gxp_len + metadata_len);
    isaac_shader_cache_put_u32(header + 28u, key->flags);
    isaac_shader_cache_put_u32(header + 32u, key->shader_type);
    isaac_shader_cache_put_u32(header + 36u, key->translator_mode);
    isaac_shader_cache_put_u32(header + 40u, key->compiler_flags);
    isaac_shader_cache_put_u32(header + 44u, key->source_len);
    isaac_shader_cache_put_u32(header + 48u, key->peer_len);
    isaac_shader_cache_put_u32(header + 52u, gxp_len);
    isaac_shader_cache_put_u32(header + 56u, metadata_len);
    isaac_shader_cache_put_u32(header + 60u, matrix_count);
    memcpy(header + 64u, key->build_sha256, 32u);
    memcpy(header + 96u, key->source_sha256, 32u);
    memcpy(header + 128u, key->peer_sha256, 32u);
    isaac_shader_cache_sha256(gxp, gxp_len, header + 160u);
    isaac_shader_cache_sha256(metadata, metadata_len, header + 192u);
    isaac_shader_cache_record_digest(
        header, gxp, gxp_len, metadata, metadata_len, header + 224u);
    s_isaac_shader_cache_stats.hash_us +=
        isaac_shader_cache_now_us() - start;
}

static int isaac_shader_cache_header_valid(
    const unsigned char header[ISAAC_SHADER_CACHE_HEADER_SIZE],
    uint32_t file_size, const isaac_shader_cache_key *key,
    uint32_t *gxp_len, uint32_t *metadata_len, uint32_t *matrix_count)
{
    static const unsigned char magic[16] = {
        'I','S','A','A','C','V','S','C','G','X','P','V','2',0,0,0
    };
    uint32_t record_size = isaac_shader_cache_get_u32(header + 24u);
    uint32_t gxp = isaac_shader_cache_get_u32(header + 52u);
    uint32_t metadata = isaac_shader_cache_get_u32(header + 56u);
    uint32_t matrices = isaac_shader_cache_get_u32(header + 60u);

    if (memcmp(header, magic, sizeof magic) != 0 ||
        isaac_shader_cache_get_u32(header + 16u) !=
            ISAAC_SHADER_CACHE_VERSION ||
        isaac_shader_cache_get_u32(header + 20u) !=
            ISAAC_SHADER_CACHE_HEADER_SIZE ||
        record_size != file_size ||
        isaac_shader_cache_get_u32(header + 28u) != key->flags ||
        isaac_shader_cache_get_u32(header + 32u) != key->shader_type ||
        isaac_shader_cache_get_u32(header + 36u) != key->translator_mode ||
        isaac_shader_cache_get_u32(header + 40u) != key->compiler_flags ||
        isaac_shader_cache_get_u32(header + 44u) != key->source_len ||
        isaac_shader_cache_get_u32(header + 48u) != key->peer_len ||
        memcmp(header + 64u, key->build_sha256, 32u) != 0 ||
        memcmp(header + 96u, key->source_sha256, 32u) != 0 ||
        memcmp(header + 128u, key->peer_sha256, 32u) != 0 ||
        gxp == 0u || gxp > ISAAC_SHADER_CACHE_GXP_MAX ||
        metadata > ISAAC_SHADER_CACHE_METADATA_MAX ||
        matrices > ISAAC_SHADER_CACHE_MATRIX_MAX ||
        gxp > UINT32_MAX - metadata ||
        ISAAC_SHADER_CACHE_HEADER_SIZE > UINT32_MAX - gxp - metadata ||
        ISAAC_SHADER_CACHE_HEADER_SIZE + gxp + metadata != record_size)
        return 0;
    *gxp_len = gxp;
    *metadata_len = metadata;
    *matrix_count = matrices;
    return 1;
}

static int isaac_shader_cache_validate_body(
    const unsigned char header[ISAAC_SHADER_CACHE_HEADER_SIZE],
    const unsigned char *body, uint32_t gxp_len, uint32_t metadata_len)
{
    unsigned char digest[32];
    uint64_t start = isaac_shader_cache_now_us();
    isaac_shader_cache_sha256(body, gxp_len, digest);
    if (memcmp(digest, header + 160u, 32u) != 0)
        goto invalid;
    isaac_shader_cache_sha256(body + gxp_len, metadata_len, digest);
    if (memcmp(digest, header + 192u, 32u) != 0)
        goto invalid;
    isaac_shader_cache_record_digest(
        header, body, gxp_len, body + gxp_len, metadata_len, digest);
    if (memcmp(digest, header + 224u, 32u) != 0)
        goto invalid;
    s_isaac_shader_cache_stats.hash_us +=
        isaac_shader_cache_now_us() - start;
    return 1;
invalid:
    s_isaac_shader_cache_stats.hash_us +=
        isaac_shader_cache_now_us() - start;
    return 0;
}

static enum isaac_shader_cache_load_result isaac_shader_cache_read_path(
    const char *path, const isaac_shader_cache_key *key,
    isaac_shader_cache_record *record, int delete_corrupt)
{
    unsigned char header[ISAAC_SHADER_CACHE_HEADER_SIZE];
    unsigned char extra;
    unsigned char *body = NULL;
    int descriptor = -1;
    int64_t file_size64;
    uint32_t file_size, gxp_len, metadata_len, matrix_count, body_size;
    int read_result;
    uint64_t start = isaac_shader_cache_now_us();

    memset(record, 0, sizeof *record);
    descriptor = isaac_shader_cache_io_open(path, 0);
    if (descriptor < 0)
        goto miss;
    file_size64 = isaac_shader_cache_io_seek(descriptor, 0, SCE_SEEK_END);
    if (file_size64 < (int64_t)ISAAC_SHADER_CACHE_HEADER_SIZE ||
            file_size64 > (int64_t)(ISAAC_SHADER_CACHE_HEADER_SIZE +
                ISAAC_SHADER_CACHE_GXP_MAX +
                ISAAC_SHADER_CACHE_METADATA_MAX) ||
            isaac_shader_cache_io_seek(descriptor, 0, SCE_SEEK_SET) != 0)
        goto corrupt;
    file_size = (uint32_t)file_size64;
    if (!isaac_shader_cache_read_exact(
            descriptor, header, ISAAC_SHADER_CACHE_HEADER_SIZE) ||
        !isaac_shader_cache_header_valid(
            header, file_size, key, &gxp_len, &metadata_len, &matrix_count))
        goto corrupt;
    body_size = gxp_len + metadata_len;
    body = (unsigned char *)isaac_shader_cache_alloc(body_size);
    if (!body) {
        ++s_isaac_shader_cache_stats.alloc_failures;
        goto miss;
    }
    if (!isaac_shader_cache_read_exact(descriptor, body, body_size))
        goto corrupt;
    read_result = isaac_shader_cache_io_read(descriptor, &extra, 1u);
    if (read_result != 0 ||
            !isaac_shader_cache_validate_body(
                header, body, gxp_len, metadata_len))
        goto corrupt;
    if (isaac_shader_cache_io_close(descriptor) < 0) {
        descriptor = -1;
        ++s_isaac_shader_cache_stats.read_failures;
        goto miss;
    }
    descriptor = -1;
    record->body = body;
    record->body_size = body_size;
    record->gxp_len = gxp_len;
    record->metadata_len = metadata_len;
    record->matrix_count = matrix_count;
    record->flags = isaac_shader_cache_get_u32(header + 28u);
    s_isaac_shader_cache_stats.read_us +=
        isaac_shader_cache_now_us() - start;
    return ISAAC_SHADER_CACHE_LOAD_CANDIDATE;

corrupt:
    if (descriptor >= 0)
        (void)isaac_shader_cache_io_close(descriptor);
    if (body)
        isaac_shader_cache_free(body);
    ++s_isaac_shader_cache_stats.read_failures;
    if (delete_corrupt)
        isaac_shader_cache_remove_corrupt(path);
    s_isaac_shader_cache_stats.read_us +=
        isaac_shader_cache_now_us() - start;
    return ISAAC_SHADER_CACHE_LOAD_CORRUPT;
miss:
    if (descriptor >= 0)
        (void)isaac_shader_cache_io_close(descriptor);
    if (body)
        isaac_shader_cache_free(body);
    s_isaac_shader_cache_stats.read_us +=
        isaac_shader_cache_now_us() - start;
    return ISAAC_SHADER_CACHE_LOAD_MISS;
}

static enum isaac_shader_cache_load_result isaac_shader_cache_load(
    const isaac_shader_cache_key *key, isaac_shader_cache_record *record)
{
    char path[ISAAC_SHADER_CACHE_PATH_MAX];
    enum isaac_shader_cache_load_result result;
    ++s_isaac_shader_cache_stats.attempts;
    if (!isaac_shader_cache_path(key, path)) {
        ++s_isaac_shader_cache_stats.misses;
        isaac_shader_cache_touch();
        memset(record, 0, sizeof *record);
        return ISAAC_SHADER_CACHE_LOAD_MISS;
    }
    result = isaac_shader_cache_read_path(path, key, record, 1);
    if (result != ISAAC_SHADER_CACHE_LOAD_CANDIDATE)
        ++s_isaac_shader_cache_stats.misses;
    isaac_shader_cache_touch();
    return result;
}

static void isaac_shader_cache_dispose(isaac_shader_cache_record *record)
{
    if (record && record->body)
        isaac_shader_cache_free(record->body);
    if (record)
        memset(record, 0, sizeof *record);
}

static inline void isaac_shader_cache_reject_corrupt(
    const isaac_shader_cache_key *key, isaac_shader_cache_record *record)
{
    char path[ISAAC_SHADER_CACHE_PATH_MAX];
    isaac_shader_cache_dispose(record);
    if (isaac_shader_cache_path(key, path))
        isaac_shader_cache_remove_corrupt(path);
}

static void isaac_shader_cache_mark_hit(void)
{
    ++s_isaac_shader_cache_stats.hits;
    isaac_shader_cache_touch();
}

static enum isaac_shader_cache_program_result
isaac_shader_cache_check_program(
    const isaac_shader_cache_record *record, uint32_t expected_type,
    const isaac_shader_cache_program_ops *ops, void *context)
{
    if (!record || !record->body || !ops || !ops->check || !ops->size ||
            !ops->type || ops->check(context, record->body) != 0)
        return ISAAC_SHADER_CACHE_PROGRAM_CHECK_FAILED;
    if (ops->size(context, record->body) != record->gxp_len)
        return ISAAC_SHADER_CACHE_PROGRAM_SIZE_FAILED;
    if (ops->type(context, record->body) != expected_type)
        return ISAAC_SHADER_CACHE_PROGRAM_TYPE_FAILED;
    return ISAAC_SHADER_CACHE_PROGRAM_ACCEPT;
}

static enum isaac_shader_cache_program_result
isaac_shader_cache_register_program(
    const isaac_shader_cache_record *record,
    const isaac_shader_cache_program_ops *ops, void *context,
    uintptr_t *registered_id)
{
    if (!record || !record->body || !ops || !ops->register_program ||
            !registered_id ||
            ops->register_program(context, record->body, registered_id) != 0)
        return ISAAC_SHADER_CACHE_PROGRAM_REGISTER_FAILED;
    return ISAAC_SHADER_CACHE_PROGRAM_ACCEPT;
}

#if defined(ISAAC_SHADER_CACHE_ORACLE)
static enum isaac_shader_cache_program_result
isaac_shader_cache_validate_program(
    const isaac_shader_cache_record *record, uint32_t expected_type,
    const isaac_shader_cache_program_ops *ops, void *context,
    uintptr_t *registered_id)
{
    enum isaac_shader_cache_program_result result =
        isaac_shader_cache_check_program(record, expected_type, ops, context);
    if (result != ISAAC_SHADER_CACHE_PROGRAM_ACCEPT)
        return result;
    return isaac_shader_cache_register_program(
        record, ops, context, registered_id);
}
#endif

static int isaac_shader_cache_prepare_directories(void)
{
    /* mkdir is idempotent on the intended filesystem.  Individual return
     * codes are not interpreted as EEXIST; the exclusive temp open below is
     * the authoritative availability check. */
    (void)isaac_shader_cache_io_mkdir("ux0:data/isaacr001");
    (void)isaac_shader_cache_io_mkdir(ISAAC_SHADER_CACHE_ROOT);
    (void)isaac_shader_cache_io_mkdir(ISAAC_SHADER_CACHE_ROOT "/v2");
    (void)isaac_shader_cache_io_mkdir(ISAAC_SHADER_CACHE_ROOT "/v2/v");
    (void)isaac_shader_cache_io_mkdir(ISAAC_SHADER_CACHE_ROOT "/v2/f");
    return 1;
}

static int isaac_shader_cache_publish(
    const isaac_shader_cache_key *key,
    const void *gxp, uint32_t gxp_len,
    const void *metadata, uint32_t metadata_len, uint32_t matrix_count)
{
    static const unsigned char empty_metadata;
    unsigned char header[ISAAC_SHADER_CACHE_HEADER_SIZE];
    isaac_shader_cache_record verified;
    char final_path[ISAAC_SHADER_CACHE_PATH_MAX];
    char temp_path[ISAAC_SHADER_CACHE_PATH_MAX];
    int descriptor = -1;
    uint32_t attempt;
    int success = 0;
    uint64_t start = isaac_shader_cache_now_us();

    if (!key || !gxp || gxp_len == 0u ||
        gxp_len > ISAAC_SHADER_CACHE_GXP_MAX ||
        metadata_len > ISAAC_SHADER_CACHE_METADATA_MAX ||
        matrix_count > ISAAC_SHADER_CACHE_MATRIX_MAX ||
        (metadata_len != 0u && !metadata)) {
        ++s_isaac_shader_cache_stats.oversize;
        ++s_isaac_shader_cache_stats.write_failures;
        goto done;
    }
    if (metadata_len == 0u)
        metadata = &empty_metadata;
    isaac_shader_cache_encode_header(
        header, key, gxp, gxp_len, metadata, metadata_len, matrix_count);
    if (!isaac_shader_cache_path(key, final_path) ||
            !isaac_shader_cache_prepare_directories()) {
        ++s_isaac_shader_cache_stats.write_failures;
        goto done;
    }
    for (attempt = 0u; attempt < ISAAC_SHADER_CACHE_TEMP_ATTEMPTS; ++attempt) {
        uint32_t serial = ++s_isaac_shader_cache_temp_serial ^
                          (uint32_t)isaac_shader_cache_now_us();
        if (!isaac_shader_cache_temp_path(key, temp_path, serial))
            break;
        descriptor = isaac_shader_cache_io_open(temp_path, 1);
        if (descriptor >= 0)
            break;
    }
    if (descriptor < 0) {
        ++s_isaac_shader_cache_stats.write_failures;
        goto done;
    }
    if (!isaac_shader_cache_write_exact(
            descriptor, header, ISAAC_SHADER_CACHE_HEADER_SIZE) ||
        !isaac_shader_cache_write_exact(descriptor, gxp, gxp_len) ||
        !isaac_shader_cache_write_exact(
            descriptor, metadata, metadata_len) ||
        isaac_shader_cache_io_sync(descriptor) < 0) {
        ++s_isaac_shader_cache_stats.write_failures;
        goto cleanup_open;
    }
    if (isaac_shader_cache_io_close(descriptor) < 0) {
        descriptor = -1;
        ++s_isaac_shader_cache_stats.write_failures;
        goto cleanup_temp;
    }
    descriptor = -1;
    if (isaac_shader_cache_read_path(
            temp_path, key, &verified, 0) !=
            ISAAC_SHADER_CACHE_LOAD_CANDIDATE ||
        verified.gxp_len != gxp_len ||
        verified.metadata_len != metadata_len ||
        memcmp(verified.body, gxp, gxp_len) != 0 ||
        memcmp(verified.body + gxp_len, metadata, metadata_len) != 0) {
        isaac_shader_cache_dispose(&verified);
        ++s_isaac_shader_cache_stats.write_failures;
        goto cleanup_temp;
    }
    isaac_shader_cache_dispose(&verified);
    /* Vita's rename does not promise replacement.  Losing an old cache file
     * between remove and rename can only force a later native compile; a
     * partial file is never accepted because every read is authenticated. */
    (void)isaac_shader_cache_io_remove(final_path);
    if (isaac_shader_cache_io_rename(temp_path, final_path) < 0 ||
        isaac_shader_cache_io_sync_device("ux0:") < 0) {
        ++s_isaac_shader_cache_stats.write_failures;
        goto cleanup_temp;
    }
    ++s_isaac_shader_cache_stats.write_ok;
    success = 1;
    goto done;

cleanup_open:
    (void)isaac_shader_cache_io_close(descriptor);
    descriptor = -1;
cleanup_temp:
    (void)isaac_shader_cache_io_remove(temp_path);
done:
    s_isaac_shader_cache_stats.write_us +=
        isaac_shader_cache_now_us() - start;
    isaac_shader_cache_touch();
    return success;
}

static inline void isaac_shader_cache_note_native_compile(uint64_t elapsed_us)
{
    ++s_isaac_shader_cache_stats.native_compiles;
    s_isaac_shader_cache_stats.compile_us += elapsed_us;
    isaac_shader_cache_touch();
}

static inline void isaac_shader_cache_note_register(
    uint64_t elapsed_us, int failed)
{
    s_isaac_shader_cache_stats.register_us += elapsed_us;
    if (failed)
        ++s_isaac_shader_cache_stats.register_failures;
    isaac_shader_cache_touch();
}

enum isaac_shader_cache_publish_reject_reason {
    ISAAC_SHADER_CACHE_PUBLISH_REJECT_SHAPE = 0,
    ISAAC_SHADER_CACHE_PUBLISH_REJECT_SEMANTICS = 1,
    ISAAC_SHADER_CACHE_PUBLISH_REJECT_MATRICES = 2,
    ISAAC_SHADER_CACHE_PUBLISH_REJECT_BLOCKS = 3,
    ISAAC_SHADER_CACHE_PUBLISH_REJECT_INCOMPLETE = 4,
    ISAAC_SHADER_CACHE_PUBLISH_REJECT_ALLOC = 5
};

static inline void isaac_shader_cache_note_publish_attempt(int has_block_list)
{
    ++s_isaac_shader_cache_stats.publish_attempts;
    if (has_block_list)
        ++s_isaac_shader_cache_stats.publish_with_block_list;
    isaac_shader_cache_touch();
}

static inline void isaac_shader_cache_note_publish_ready(uint32_t block_count)
{
    ++s_isaac_shader_cache_stats.publish_ready;
    s_isaac_shader_cache_stats.publish_block_records += block_count;
    isaac_shader_cache_touch();
}

static inline void isaac_shader_cache_note_publish_reject(
    enum isaac_shader_cache_publish_reject_reason reason)
{
    switch (reason) {
    case ISAAC_SHADER_CACHE_PUBLISH_REJECT_SHAPE:
        ++s_isaac_shader_cache_stats.publish_reject_shape;
        break;
    case ISAAC_SHADER_CACHE_PUBLISH_REJECT_SEMANTICS:
        ++s_isaac_shader_cache_stats.publish_reject_semantics;
        break;
    case ISAAC_SHADER_CACHE_PUBLISH_REJECT_MATRICES:
        ++s_isaac_shader_cache_stats.publish_reject_matrices;
        break;
    case ISAAC_SHADER_CACHE_PUBLISH_REJECT_BLOCKS:
        ++s_isaac_shader_cache_stats.publish_reject_blocks;
        break;
    case ISAAC_SHADER_CACHE_PUBLISH_REJECT_INCOMPLETE:
        ++s_isaac_shader_cache_stats.publish_reject_incomplete;
        break;
    case ISAAC_SHADER_CACHE_PUBLISH_REJECT_ALLOC:
        ++s_isaac_shader_cache_stats.publish_reject_alloc;
        break;
    }
    isaac_shader_cache_touch();
}

static void isaac_shader_cache_get_stats(isaac_shader_cache_stats *stats)
{
    if (stats)
        *stats = s_isaac_shader_cache_stats;
}

#if defined(ISAAC_SHADER_CACHE_ORACLE)
static void isaac_shader_cache_oracle_reset_policy(void)
{
    memset(&s_isaac_shader_cache_stats, 0,
           sizeof s_isaac_shader_cache_stats);
    s_isaac_shader_cache_temp_serial = 0u;
}
#endif

#endif
