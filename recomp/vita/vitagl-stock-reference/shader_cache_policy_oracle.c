#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2
#define ISAAC_SHADER_CACHE_ORACLE 1
#define ISAAC_SHADER_CACHE_BUILD_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

#define ORACLE_FILE_MAX 32u
#define ORACLE_DESCRIPTOR_MAX 16u
#define ORACLE_PATH_MAX 192u

typedef struct oracle_file {
    int present;
    char path[ORACLE_PATH_MAX];
    unsigned char *bytes;
    uint32_t size;
    uint32_t capacity;
} oracle_file;

typedef struct oracle_descriptor {
    int open;
    int writable;
    oracle_file *file;
    uint32_t cursor;
} oracle_descriptor;

typedef struct oracle_faults {
    unsigned fail_open_read;
    unsigned fail_open_write;
    unsigned fail_read_call;
    unsigned fail_write_call;
    unsigned read_calls;
    unsigned write_calls;
    uint32_t read_chunk;
    uint32_t write_chunk;
    unsigned fail_close;
    unsigned fail_sync_fd;
    unsigned fail_rename;
    unsigned fail_sync_device;
    unsigned fail_alloc;
    unsigned fail_alloc_call;
    unsigned alloc_calls;
} oracle_faults;

typedef struct block_uniform {
    char name[128];
    uint8_t idx;
    void *chain;
} block_uniform;

static oracle_file s_files[ORACLE_FILE_MAX];
static oracle_descriptor s_descriptors[ORACLE_DESCRIPTOR_MAX];
static oracle_faults s_faults;
static uint64_t s_now;
static unsigned s_live_allocations;

#include "isaac_shader_cache_policy.h"

_Static_assert(sizeof(((block_uniform *)0)->name) ==
    ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE,
    "oracle block_uniform drifted from the production ABI");

#define vglMalloc isaac_shader_cache_oracle_alloc
#define vgl_free isaac_shader_cache_oracle_free
#include "isaac_shader_cache_block_list.h"
#undef vglMalloc
#undef vgl_free

static oracle_file *oracle_find_file(const char *path)
{
    unsigned index;
    for (index = 0u; index < ORACLE_FILE_MAX; ++index) {
        if (s_files[index].present &&
                strcmp(s_files[index].path, path) == 0)
            return &s_files[index];
    }
    return NULL;
}

static oracle_file *oracle_create_file(const char *path)
{
    unsigned index;
    for (index = 0u; index < ORACLE_FILE_MAX; ++index) {
        oracle_file *file = &s_files[index];
        if (!file->present) {
            size_t length = strlen(path);
            assert(length < sizeof file->path);
            memset(file, 0, sizeof *file);
            memcpy(file->path, path, length + 1u);
            file->present = 1;
            return file;
        }
    }
    return NULL;
}

static int oracle_descriptor_new(oracle_file *file, int writable)
{
    unsigned index;
    for (index = 1u; index < ORACLE_DESCRIPTOR_MAX; ++index) {
        if (!s_descriptors[index].open) {
            s_descriptors[index].open = 1;
            s_descriptors[index].writable = writable;
            s_descriptors[index].file = file;
            s_descriptors[index].cursor = 0u;
            return (int)index;
        }
    }
    return -1;
}

int isaac_shader_cache_oracle_open(const char *path, int write_exclusive)
{
    oracle_file *file;
    if (write_exclusive) {
        if (s_faults.fail_open_write != 0u) {
            --s_faults.fail_open_write;
            return -1;
        }
        if (oracle_find_file(path))
            return -1;
        file = oracle_create_file(path);
        if (!file)
            return -1;
        return oracle_descriptor_new(file, 1);
    }
    if (s_faults.fail_open_read != 0u) {
        --s_faults.fail_open_read;
        return -1;
    }
    file = oracle_find_file(path);
    return file ? oracle_descriptor_new(file, 0) : -1;
}

int64_t isaac_shader_cache_oracle_seek(int descriptor, int64_t offset,
                                       int origin)
{
    oracle_descriptor *entry;
    int64_t base;
    int64_t result;
    if (descriptor <= 0 || descriptor >= (int)ORACLE_DESCRIPTOR_MAX ||
            !s_descriptors[descriptor].open)
        return -1;
    entry = &s_descriptors[descriptor];
    if (origin == SCE_SEEK_SET)
        base = 0;
    else if (origin == SCE_SEEK_CUR)
        base = entry->cursor;
    else if (origin == SCE_SEEK_END)
        base = entry->file->size;
    else
        return -1;
    result = base + offset;
    if (result < 0 || result > UINT32_MAX)
        return -1;
    entry->cursor = (uint32_t)result;
    return result;
}

int isaac_shader_cache_oracle_read(int descriptor, void *buffer,
                                   uint32_t size)
{
    oracle_descriptor *entry;
    uint32_t available;
    uint32_t amount;
    if (descriptor <= 0 || descriptor >= (int)ORACLE_DESCRIPTOR_MAX ||
            !s_descriptors[descriptor].open ||
            s_descriptors[descriptor].writable)
        return -1;
    ++s_faults.read_calls;
    if (s_faults.fail_read_call != 0u &&
            s_faults.read_calls == s_faults.fail_read_call)
        return -1;
    entry = &s_descriptors[descriptor];
    available = entry->cursor < entry->file->size
        ? entry->file->size - entry->cursor : 0u;
    amount = size < available ? size : available;
    if (s_faults.read_chunk != 0u && amount > s_faults.read_chunk)
        amount = s_faults.read_chunk;
    if (amount != 0u) {
        memcpy(buffer, entry->file->bytes + entry->cursor, amount);
        entry->cursor += amount;
    }
    return (int)amount;
}

int isaac_shader_cache_oracle_write(int descriptor, const void *buffer,
                                    uint32_t size)
{
    oracle_descriptor *entry;
    oracle_file *file;
    uint32_t amount = size;
    uint32_t required;
    unsigned char *grown;
    if (descriptor <= 0 || descriptor >= (int)ORACLE_DESCRIPTOR_MAX ||
            !s_descriptors[descriptor].open ||
            !s_descriptors[descriptor].writable)
        return -1;
    ++s_faults.write_calls;
    if (s_faults.fail_write_call != 0u &&
            s_faults.write_calls == s_faults.fail_write_call)
        return -1;
    if (s_faults.write_chunk != 0u && amount > s_faults.write_chunk)
        amount = s_faults.write_chunk;
    entry = &s_descriptors[descriptor];
    file = entry->file;
    if (amount > UINT32_MAX - entry->cursor)
        return -1;
    required = entry->cursor + amount;
    if (required > file->capacity) {
        uint32_t capacity = file->capacity ? file->capacity : 512u;
        while (capacity < required) {
            if (capacity > UINT32_MAX / 2u) {
                capacity = required;
                break;
            }
            capacity *= 2u;
        }
        grown = (unsigned char *)realloc(file->bytes, capacity);
        if (!grown)
            return -1;
        file->bytes = grown;
        file->capacity = capacity;
    }
    if (amount != 0u)
        memcpy(file->bytes + entry->cursor, buffer, amount);
    entry->cursor += amount;
    if (file->size < entry->cursor)
        file->size = entry->cursor;
    return (int)amount;
}

int isaac_shader_cache_oracle_close(int descriptor)
{
    if (descriptor <= 0 || descriptor >= (int)ORACLE_DESCRIPTOR_MAX ||
            !s_descriptors[descriptor].open)
        return -1;
    memset(&s_descriptors[descriptor], 0, sizeof s_descriptors[descriptor]);
    if (s_faults.fail_close != 0u) {
        --s_faults.fail_close;
        return -1;
    }
    return 0;
}

int isaac_shader_cache_oracle_sync_fd(int descriptor)
{
    if (descriptor <= 0 || descriptor >= (int)ORACLE_DESCRIPTOR_MAX ||
            !s_descriptors[descriptor].open)
        return -1;
    if (s_faults.fail_sync_fd != 0u) {
        --s_faults.fail_sync_fd;
        return -1;
    }
    return 0;
}

int isaac_shader_cache_oracle_mkdir(const char *path)
{
    (void)path;
    return 0;
}

int isaac_shader_cache_oracle_remove(const char *path)
{
    oracle_file *file = oracle_find_file(path);
    if (!file)
        return -1;
    free(file->bytes);
    memset(file, 0, sizeof *file);
    return 0;
}

int isaac_shader_cache_oracle_rename(const char *source,
                                     const char *destination)
{
    oracle_file *file;
    size_t length;
    if (s_faults.fail_rename != 0u) {
        --s_faults.fail_rename;
        return -1;
    }
    file = oracle_find_file(source);
    if (!file || oracle_find_file(destination))
        return -1;
    length = strlen(destination);
    if (length >= sizeof file->path)
        return -1;
    memcpy(file->path, destination, length + 1u);
    return 0;
}

int isaac_shader_cache_oracle_sync_device(const char *device)
{
    assert(strcmp(device, "ux0:") == 0);
    if (s_faults.fail_sync_device != 0u) {
        --s_faults.fail_sync_device;
        return -1;
    }
    return 0;
}

void *isaac_shader_cache_oracle_alloc(uint32_t size)
{
    void *result;
    ++s_faults.alloc_calls;
    if (s_faults.fail_alloc != 0u) {
        --s_faults.fail_alloc;
        return NULL;
    }
    if (s_faults.fail_alloc_call != 0u &&
            s_faults.alloc_calls == s_faults.fail_alloc_call)
        return NULL;
    result = malloc(size ? size : 1u);
    if (result)
        ++s_live_allocations;
    return result;
}

void isaac_shader_cache_oracle_free(void *pointer)
{
    if (pointer) {
        assert(s_live_allocations != 0u);
        --s_live_allocations;
    }
    free(pointer);
}

uint64_t isaac_shader_cache_oracle_now_us(void)
{
    s_now += 7u;
    return s_now;
}

static void oracle_reset(void)
{
    unsigned index;
    assert(s_live_allocations == 0u);
    for (index = 0u; index < ORACLE_FILE_MAX; ++index)
        free(s_files[index].bytes);
    memset(s_files, 0, sizeof s_files);
    memset(s_descriptors, 0, sizeof s_descriptors);
    memset(&s_faults, 0, sizeof s_faults);
    s_now = 0u;
    isaac_shader_cache_oracle_reset_policy();
}

static void make_key(isaac_shader_cache_key *key, uint32_t type,
                     uint32_t mode, uint32_t compiler_flags,
                     uint32_t flags, const char *source, const char *peer)
{
    unsigned char source_hash[32];
    unsigned char peer_hash[32];
    uint32_t source_len = (uint32_t)strlen(source);
    uint32_t peer_len = peer ? (uint32_t)strlen(peer) : 0u;
    isaac_shader_cache_sha256(source, source_len, source_hash);
    if (peer)
        isaac_shader_cache_sha256(peer, peer_len, peer_hash);
    assert(isaac_shader_cache_make_key(
        key, type, mode, compiler_flags, flags, source_len, source_hash,
        peer_len, peer ? peer_hash : NULL));
}

static oracle_file *final_file(const isaac_shader_cache_key *key)
{
    char path[ISAAC_SHADER_CACHE_PATH_MAX];
    assert(isaac_shader_cache_path(key, path));
    return oracle_find_file(path);
}

static void restore_file(oracle_file *file,
                         const unsigned char *bytes, uint32_t size)
{
    unsigned char *copy = (unsigned char *)realloc(file->bytes, size);
    assert(copy);
    file->bytes = copy;
    file->capacity = size;
    file->size = size;
    memcpy(file->bytes, bytes, size);
}

static void prove_sha256(void)
{
    static const unsigned char expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    unsigned char actual[32];
    isaac_shader_cache_sha256("abc", 3u, actual);
    assert(memcmp(actual, expected, 32u) == 0);
}

static void prove_key_contract(void)
{
    isaac_shader_cache_key base;
    isaac_shader_cache_key changed;
    unsigned field;
    make_key(&base, ISAAC_SHADER_CACHE_VERTEX, 0x301u, 7u,
             ISAAC_SHADER_CACHE_FLAG_PAIR |
                 ISAAC_SHADER_CACHE_FLAG_BINDINGS |
                 ISAAC_SHADER_CACHE_FLAG_GLSL |
                 ISAAC_SHADER_CACHE_FLAG_PEER_GLSL,
             "vertex source", "fragment source");
    for (field = 0u; field < 10u; ++field) {
        switch (field) {
        case 0u:
            make_key(&changed, ISAAC_SHADER_CACHE_FRAGMENT, 0x301u, 7u,
                     base.flags, "vertex source", "fragment source");
            break;
        case 1u:
            make_key(&changed, base.shader_type, 0x302u, 7u,
                     base.flags, "vertex source", "fragment source");
            break;
        case 2u:
            make_key(&changed, base.shader_type, 0x301u, 6u,
                     base.flags, "vertex source", "fragment source");
            break;
        case 3u:
            make_key(&changed, base.shader_type, 0x301u, 7u,
                     ISAAC_SHADER_CACHE_FLAG_PAIR,
                     "vertex source", "fragment source");
            break;
        case 4u:
            make_key(&changed, base.shader_type, 0x301u, 7u,
                     base.flags, "vertex source!", "fragment source");
            break;
        case 5u:
            make_key(&changed, base.shader_type, 0x301u, 7u,
                     base.flags, "vertex source", "fragment source!");
            break;
        case 6u:
            changed = base;
            changed.build_sha256[0] ^= 1u;
            isaac_shader_cache_sha256(
                changed.build_sha256, 32u, changed.key_sha256);
            break;
        case 7u:
            make_key(&changed, base.shader_type, 0x301u, 7u,
                     base.flags & ~ISAAC_SHADER_CACHE_FLAG_GLSL,
                     "vertex source", "fragment source");
            break;
        case 8u:
            make_key(&changed, base.shader_type, 0x301u, 7u,
                     base.flags & ~ISAAC_SHADER_CACHE_FLAG_PEER_GLSL,
                     "vertex source", "fragment source");
            break;
        default:
            make_key(&changed, base.shader_type, 0x301u, 7u, 0u,
                     "vertex source", NULL);
            break;
        }
        assert(memcmp(base.key_sha256, changed.key_sha256, 32u) != 0);
    }
}

static void prove_metadata_v2(void)
{
    unsigned char metadata[
        ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE +
        3u * sizeof(uint32_t) +
        ISAAC_SHADER_CACHE_BLOCK_MAX * ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE +
        17u];
    unsigned char encoded[ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE];
    isaac_shader_cache_block_descriptor blocks[
        ISAAC_SHADER_CACHE_BLOCK_MAX];
    isaac_shader_cache_block_descriptor decoded;
    uint32_t metadata_size;
    uint32_t matrix_offset;
    uint32_t block_offset;
    uint32_t bindings_offset;
    uint32_t block_count;
    uint32_t valid_len;
    uint32_t index;

    memset(blocks, 0, sizeof blocks);
    for (index = 0u; index < ISAAC_SHADER_CACHE_BLOCK_MAX; ++index) {
        blocks[index].index = index;
        (void)snprintf(blocks[index].name, sizeof blocks[index].name,
            "Block_%u", index);
    }
    assert(isaac_shader_cache_blocks_unique(
        blocks, ISAAC_SHADER_CACHE_BLOCK_MAX));
    assert(isaac_shader_cache_metadata_layout(
        3u, ISAAC_SHADER_CACHE_BLOCK_MAX, 17u, &metadata_size,
        &matrix_offset, &block_offset, &bindings_offset));
    assert(matrix_offset == ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE);
    assert(block_offset == matrix_offset + 3u * sizeof(uint32_t));
    assert(bindings_offset == block_offset +
        ISAAC_SHADER_CACHE_BLOCK_MAX *
            ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE);
    assert(metadata_size == bindings_offset + 17u &&
        metadata_size == sizeof metadata);

    memset(metadata, 0xa5, sizeof metadata);
    isaac_shader_cache_metadata_encode_header(
        metadata, 3u, ISAAC_SHADER_CACHE_BLOCK_MAX);
    for (index = 0u; index < ISAAC_SHADER_CACHE_BLOCK_MAX; ++index) {
        assert(isaac_shader_cache_encode_block(
            metadata + block_offset +
                index * ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE,
            &blocks[index]));
    }
    assert(isaac_shader_cache_metadata_decode_header(
        metadata, sizeof metadata, 3u, 17u, &block_count,
        &matrix_offset, &block_offset, &bindings_offset));
    assert(block_count == ISAAC_SHADER_CACHE_BLOCK_MAX);
    for (index = 0u; index < block_count; ++index) {
        assert(isaac_shader_cache_decode_block(
            metadata + block_offset +
                index * ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE,
            &decoded));
        assert(decoded.index == blocks[index].index);
        assert(strcmp(decoded.name, blocks[index].name) == 0);
    }

    memset(&blocks[0], 0, sizeof blocks[0]);
    blocks[0].index = ISAAC_SHADER_CACHE_BLOCK_MAX - 1u;
    memset(blocks[0].name, 'z', sizeof blocks[0].name - 1u);
    blocks[0].name[sizeof blocks[0].name - 1u] = '\0';
    assert(isaac_shader_cache_encode_block(encoded, &blocks[0]));
    assert(isaac_shader_cache_decode_block(encoded, &decoded));
    assert(memcmp(&decoded, &blocks[0], sizeof decoded) == 0);
    assert(!isaac_shader_cache_decode_block(NULL, &decoded));
    assert(!isaac_shader_cache_decode_block(encoded, NULL));

    encoded[0] = ISAAC_SHADER_CACHE_BLOCK_MAX;
    encoded[1] = encoded[2] = encoded[3] = 0u;
    assert(!isaac_shader_cache_decode_block(encoded, &decoded));
    assert(isaac_shader_cache_encode_block(encoded, &blocks[0]));
    memset(encoded + sizeof(uint32_t), 0,
        ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE);
    assert(!isaac_shader_cache_decode_block(encoded, &decoded));
    assert(isaac_shader_cache_encode_block(encoded, &blocks[0]));
    memset(encoded + sizeof(uint32_t), 'q',
        ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE);
    assert(!isaac_shader_cache_decode_block(encoded, &decoded));
    assert(isaac_shader_cache_encode_block(encoded, &blocks[0]));
    encoded[sizeof(uint32_t) + 2u] = 0u;
    encoded[sizeof(uint32_t) + 3u] = 1u;
    assert(!isaac_shader_cache_decode_block(encoded, &decoded));

    blocks[0].index = ISAAC_SHADER_CACHE_BLOCK_MAX;
    assert(!isaac_shader_cache_encode_block(encoded, &blocks[0]));
    blocks[0].index = 0u;
    memset(blocks[0].name, 'n', sizeof blocks[0].name);
    assert(!isaac_shader_cache_encode_block(encoded, &blocks[0]));

    memset(blocks, 0, sizeof blocks);
    blocks[0].index = 0u;
    blocks[1].index = 1u;
    memcpy(blocks[0].name, "First", sizeof "First");
    memcpy(blocks[1].name, "Second", sizeof "Second");
    assert(isaac_shader_cache_blocks_unique(blocks, 2u));
    blocks[1].index = 0u;
    assert(!isaac_shader_cache_blocks_unique(blocks, 2u));
    blocks[1].index = 1u;
    memcpy(blocks[1].name, "First", sizeof "First");
    assert(!isaac_shader_cache_blocks_unique(blocks, 2u));
    assert(!isaac_shader_cache_blocks_unique(NULL, 1u));
    assert(!isaac_shader_cache_blocks_unique(
        blocks, ISAAC_SHADER_CACHE_BLOCK_MAX + 1u));

    assert(isaac_shader_cache_metadata_layout(
        0u, 0u, 0u, &metadata_size, NULL, NULL, NULL));
    assert(metadata_size == ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE);
    assert(!isaac_shader_cache_metadata_layout(
        ISAAC_SHADER_CACHE_MATRIX_MAX + 1u, 0u, 0u,
        NULL, NULL, NULL, NULL));
    assert(!isaac_shader_cache_metadata_layout(
        0u, ISAAC_SHADER_CACHE_BLOCK_MAX + 1u, 0u,
        NULL, NULL, NULL, NULL));
    assert(!isaac_shader_cache_metadata_layout(
        0u, 0u, UINT32_MAX, NULL, NULL, NULL, NULL));

    memset(metadata, 0, sizeof metadata);
    isaac_shader_cache_metadata_encode_header(metadata, 3u, 2u);
    valid_len = ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE +
        3u * sizeof(uint32_t) +
        2u * ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE + 17u;
    assert(isaac_shader_cache_metadata_decode_header(
        metadata, valid_len,
        3u, 17u, &block_count, NULL, NULL, NULL));
    metadata[0] ^= 1u;
    assert(!isaac_shader_cache_metadata_decode_header(
        metadata, valid_len, 3u, 17u,
        NULL, NULL, NULL, NULL));
    metadata[0] ^= 1u;
    metadata[4] = 3u;
    assert(!isaac_shader_cache_metadata_decode_header(
        metadata, valid_len, 3u, 17u,
        NULL, NULL, NULL, NULL));
    metadata[4] = 2u;
    assert(!isaac_shader_cache_metadata_decode_header(
        metadata, valid_len, 4u, 17u,
        NULL, NULL, NULL, NULL));
    isaac_shader_cache_put_u32(
        metadata + 12u, ISAAC_SHADER_CACHE_BLOCK_MAX + 1u);
    assert(!isaac_shader_cache_metadata_decode_header(
        metadata, valid_len, 3u, 17u,
        NULL, NULL, NULL, NULL));
    isaac_shader_cache_metadata_encode_header(metadata, 3u, 2u);
    assert(!isaac_shader_cache_metadata_decode_header(
        metadata, ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE - 1u,
        3u, 17u, NULL, NULL, NULL, NULL));
    assert(!isaac_shader_cache_metadata_decode_header(
        metadata, valid_len - 1u,
        3u, 17u, NULL, NULL, NULL, NULL));
}

static void prove_block_list_lifecycle(void)
{
    isaac_shader_cache_block_descriptor descriptors[3];
    block_uniform *blocks;
    block_uniform *cursor;
    uint32_t index;

    memset(descriptors, 0, sizeof descriptors);
    descriptors[0].index = 7u;
    descriptors[1].index = 1u;
    descriptors[2].index = 13u;
    memcpy(descriptors[0].name, "Scene", sizeof "Scene");
    memcpy(descriptors[1].name, "Lighting", sizeof "Lighting");
    memcpy(descriptors[2].name, "Post", sizeof "Post");

    blocks = (block_uniform *)(uintptr_t)1u;
    assert(isaac_shader_cache_rebuild_blocks(
        descriptors, 3u, &blocks));
    assert(s_live_allocations == 3u);
    cursor = blocks;
    for (index = 0u; index < 3u; ++index) {
        assert(cursor);
        assert(cursor->idx == descriptors[index].index);
        assert(memcmp(cursor->name, descriptors[index].name,
            sizeof cursor->name) == 0);
        cursor = (block_uniform *)cursor->chain;
    }
    assert(!cursor);
    isaac_shader_cache_free_blocks(blocks);
    assert(s_live_allocations == 0u);

    for (index = 1u; index <= 3u; ++index) {
        s_faults.alloc_calls = 0u;
        s_faults.fail_alloc_call = index;
        blocks = (block_uniform *)(uintptr_t)1u;
        assert(!isaac_shader_cache_rebuild_blocks(
            descriptors, 3u, &blocks));
        assert(!blocks && s_live_allocations == 0u);
        s_faults.fail_alloc_call = 0u;
    }

    blocks = (block_uniform *)(uintptr_t)1u;
    assert(isaac_shader_cache_rebuild_blocks(NULL, 0u, &blocks));
    assert(!blocks && s_live_allocations == 0u);
    descriptors[1].index = descriptors[0].index;
    s_faults.alloc_calls = 0u;
    blocks = (block_uniform *)(uintptr_t)1u;
    assert(!isaac_shader_cache_rebuild_blocks(
        descriptors, 3u, &blocks));
    assert(!blocks && s_faults.alloc_calls == 0u &&
        s_live_allocations == 0u);
    assert(!isaac_shader_cache_rebuild_blocks(
        descriptors, 3u, NULL));
}

static void prove_publish_telemetry(void)
{
    isaac_shader_cache_stats before;
    isaac_shader_cache_stats after;
    unsigned reason;

    isaac_shader_cache_get_stats(&before);
    isaac_shader_cache_note_publish_attempt(0);
    isaac_shader_cache_note_publish_attempt(1);
    isaac_shader_cache_note_publish_ready(3u);
    for (reason = ISAAC_SHADER_CACHE_PUBLISH_REJECT_SHAPE;
            reason <= ISAAC_SHADER_CACHE_PUBLISH_REJECT_ALLOC; ++reason) {
        isaac_shader_cache_note_publish_reject(
            (enum isaac_shader_cache_publish_reject_reason)reason);
    }
    isaac_shader_cache_get_stats(&after);
    assert(after.publish_attempts == before.publish_attempts + 2u);
    assert(after.publish_with_block_list ==
        before.publish_with_block_list + 1u);
    assert(after.publish_ready == before.publish_ready + 1u);
    assert(after.publish_block_records ==
        before.publish_block_records + 3u);
    assert(after.publish_reject_shape == before.publish_reject_shape + 1u);
    assert(after.publish_reject_semantics ==
        before.publish_reject_semantics + 1u);
    assert(after.publish_reject_matrices ==
        before.publish_reject_matrices + 1u);
    assert(after.publish_reject_blocks ==
        before.publish_reject_blocks + 1u);
    assert(after.publish_reject_incomplete ==
        before.publish_reject_incomplete + 1u);
    assert(after.publish_reject_alloc == before.publish_reject_alloc + 1u);
}

typedef struct program_oracle {
    int check_result;
    uint32_t size_result;
    uint32_t type_result;
    int register_result;
    unsigned register_calls;
} program_oracle;

static int program_check(void *context, const void *program)
{
    assert(program);
    return ((program_oracle *)context)->check_result;
}

static uint32_t program_size(void *context, const void *program)
{
    assert(program);
    return ((program_oracle *)context)->size_result;
}

static uint32_t program_type(void *context, const void *program)
{
    assert(program);
    return ((program_oracle *)context)->type_result;
}

static int program_register(void *context, const void *program,
                            uintptr_t *registered_id)
{
    program_oracle *oracle = (program_oracle *)context;
    assert(program && registered_id);
    ++oracle->register_calls;
    *registered_id = 0x10203040u;
    return oracle->register_result;
}

static void prove_program_transaction(void)
{
    static const isaac_shader_cache_program_ops ops = {
        program_check, program_size, program_type, program_register
    };
    unsigned char body[32] = {0};
    isaac_shader_cache_record record = {
        body, sizeof body, 24u, 8u, 2u,
        ISAAC_SHADER_CACHE_FLAG_BINDINGS
    };
    program_oracle oracle = {0, 24u, 1u, 0, 0u};
    uintptr_t registered_id = 0u;
    int source_live = 1;
    enum isaac_shader_cache_program_result result;

    result = isaac_shader_cache_validate_program(
        &record, 1u, &ops, &oracle, &registered_id);
    if (result == ISAAC_SHADER_CACHE_PROGRAM_ACCEPT)
        source_live = 0;
    assert(result == ISAAC_SHADER_CACHE_PROGRAM_ACCEPT && !source_live &&
           registered_id == 0x10203040u && oracle.register_calls == 1u);

    oracle.check_result = -1;
    source_live = 1;
    assert(isaac_shader_cache_validate_program(
               &record, 1u, &ops, &oracle, &registered_id) ==
           ISAAC_SHADER_CACHE_PROGRAM_CHECK_FAILED && source_live);
    assert(oracle.register_calls == 1u);
    oracle.check_result = 0;
    oracle.size_result = 23u;
    assert(isaac_shader_cache_validate_program(
               &record, 1u, &ops, &oracle, &registered_id) ==
           ISAAC_SHADER_CACHE_PROGRAM_SIZE_FAILED && source_live);
    assert(oracle.register_calls == 1u);
    oracle.size_result = 24u;
    oracle.type_result = 2u;
    assert(isaac_shader_cache_validate_program(
               &record, 1u, &ops, &oracle, &registered_id) ==
           ISAAC_SHADER_CACHE_PROGRAM_TYPE_FAILED && source_live);
    assert(oracle.register_calls == 1u);
    oracle.type_result = 1u;
    oracle.register_result = -1;
    assert(isaac_shader_cache_validate_program(
               &record, 1u, &ops, &oracle, &registered_id) ==
           ISAAC_SHADER_CACHE_PROGRAM_REGISTER_FAILED && source_live);
    assert(oracle.register_calls == 2u);
}

static void prove_round_trip_and_corruption(void)
{
    unsigned char gxp[127];
    unsigned char metadata[20];
    isaac_shader_cache_key key;
    isaac_shader_cache_record record;
    oracle_file *file;
    unsigned char *golden;
    uint32_t golden_size;
    unsigned index;

    for (index = 0u; index < sizeof gxp; ++index)
        gxp[index] = (unsigned char)(index * 13u + 7u);
    for (index = 0u; index < sizeof metadata; ++index)
        metadata[index] = (unsigned char)(index ^ 0xa5u);
    make_key(&key, ISAAC_SHADER_CACHE_VERTEX, 0x301u, 7u,
             ISAAC_SHADER_CACHE_FLAG_PAIR |
                 ISAAC_SHADER_CACHE_FLAG_BINDINGS,
             "vertex source", "fragment source");
    assert(isaac_shader_cache_load(&key, &record) ==
           ISAAC_SHADER_CACHE_LOAD_MISS);
    assert(isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 5u));
    file = final_file(&key);
    assert(file && file->size == ISAAC_SHADER_CACHE_HEADER_SIZE +
           sizeof gxp + sizeof metadata);
    assert(isaac_shader_cache_load(&key, &record) ==
           ISAAC_SHADER_CACHE_LOAD_CANDIDATE);
    assert(record.gxp_len == sizeof gxp &&
           record.metadata_len == sizeof metadata &&
           record.matrix_count == 5u &&
           memcmp(record.body, gxp, sizeof gxp) == 0 &&
           memcmp(record.body + sizeof gxp, metadata,
                  sizeof metadata) == 0);
    isaac_shader_cache_mark_hit();
    isaac_shader_cache_dispose(&record);

    golden_size = file->size;
    golden = (unsigned char *)malloc(golden_size);
    assert(golden);
    memcpy(golden, file->bytes, golden_size);
    (void)isaac_shader_cache_io_remove(file->path);
    /* Every persisted byte is authenticated: fixed header, reserved header
     * bytes, GXP, metadata, and all three digests. */
    for (index = 0u; index < golden_size; ++index) {
        char path[ISAAC_SHADER_CACHE_PATH_MAX];
        assert(isaac_shader_cache_path(&key, path));
        file = oracle_create_file(path);
        assert(file);
        restore_file(file, golden, golden_size);
        file->bytes[index] ^= 1u;
        assert(isaac_shader_cache_load(&key, &record) ==
               ISAAC_SHADER_CACHE_LOAD_CORRUPT);
        assert(!final_file(&key));
    }

    for (index = 0u; index < 5u; ++index) {
        static const uint32_t truncations[5] = {
            0u, 1u, 255u, 256u, UINT32_MAX
        };
        char path[ISAAC_SHADER_CACHE_PATH_MAX];
        assert(isaac_shader_cache_path(&key, path));
        file = oracle_create_file(path);
        assert(file);
        restore_file(file, golden, golden_size);
        file->size = truncations[index] < golden_size
            ? truncations[index] : golden_size - 1u;
        assert(isaac_shader_cache_load(&key, &record) ==
               ISAAC_SHADER_CACHE_LOAD_CORRUPT);
        assert(!final_file(&key));
    }
    free(golden);
}

static void prove_fault_fallbacks(void)
{
    unsigned char gxp[64];
    unsigned char metadata[8];
    isaac_shader_cache_key key;
    isaac_shader_cache_record record;
    oracle_file *file;
    unsigned char *golden;
    uint32_t golden_size;
    unsigned native_calls = 0u;

    memset(gxp, 0x3c, sizeof gxp);
    memset(metadata, 0x69, sizeof metadata);
    make_key(&key, ISAAC_SHADER_CACHE_FRAGMENT, 0x301u, 7u,
             ISAAC_SHADER_CACHE_FLAG_PAIR |
                 ISAAC_SHADER_CACHE_FLAG_BINDINGS,
             "fragment source", "vertex source");
    assert(isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    file = final_file(&key);
    assert(file);
    golden_size = file->size;
    golden = (unsigned char *)malloc(golden_size);
    assert(golden);
    memcpy(golden, file->bytes, golden_size);

    s_faults.read_chunk = 3u;
    assert(isaac_shader_cache_load(&key, &record) ==
           ISAAC_SHADER_CACHE_LOAD_CANDIDATE);
    isaac_shader_cache_dispose(&record);
    s_faults.read_chunk = 0u;

    s_faults.fail_alloc = 1u;
    assert(isaac_shader_cache_load(&key, &record) ==
           ISAAC_SHADER_CACHE_LOAD_MISS);
    assert(final_file(&key));

    s_faults.read_calls = 0u;
    s_faults.fail_read_call = 2u;
    if (isaac_shader_cache_load(&key, &record) !=
            ISAAC_SHADER_CACHE_LOAD_CANDIDATE)
        ++native_calls;
    assert(native_calls == 1u && !final_file(&key));
    s_faults.fail_read_call = 0u;

    file = oracle_create_file("placeholder");
    assert(file);
    restore_file(file, golden, golden_size);
    {
        char path[ISAAC_SHADER_CACHE_PATH_MAX];
        assert(isaac_shader_cache_path(&key, path));
        memcpy(file->path, path, strlen(path) + 1u);
    }
    s_faults.fail_close = 1u;
    assert(isaac_shader_cache_load(&key, &record) ==
           ISAAC_SHADER_CACHE_LOAD_MISS);
    assert(final_file(&key));
    free(golden);

    (void)isaac_shader_cache_io_remove(file->path);
    s_faults.write_chunk = 5u;
    assert(isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    s_faults.write_chunk = 0u;
    (void)isaac_shader_cache_io_remove(final_file(&key)->path);

    s_faults.write_calls = 0u;
    s_faults.fail_write_call = 2u;
    assert(!isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    assert(!final_file(&key));
    s_faults.fail_write_call = 0u;

    s_faults.fail_sync_fd = 1u;
    assert(!isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    assert(!final_file(&key));

    s_faults.fail_close = 1u;
    assert(!isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    assert(!final_file(&key));

    s_faults.fail_rename = 1u;
    assert(!isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    assert(!final_file(&key));

    s_faults.fail_open_write = 1u;
    assert(isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    assert(final_file(&key));
    (void)isaac_shader_cache_io_remove(final_file(&key)->path);

    s_faults.fail_open_write = ISAAC_SHADER_CACHE_TEMP_ATTEMPTS;
    assert(!isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    assert(!final_file(&key));

    s_faults.fail_sync_device = 1u;
    assert(!isaac_shader_cache_publish(
        &key, gxp, sizeof gxp, metadata, sizeof metadata, 2u));
    /* A post-rename volume-sync failure may leave a complete authenticated
     * final file.  It remains safe to consume, but publication reports fail. */
    assert(final_file(&key));
    assert(isaac_shader_cache_load(&key, &record) ==
           ISAAC_SHADER_CACHE_LOAD_CANDIDATE);
    isaac_shader_cache_dispose(&record);

    assert(!isaac_shader_cache_publish(
        &key, gxp, ISAAC_SHADER_CACHE_GXP_MAX + 1u,
        metadata, sizeof metadata, 2u));
}

int main(void)
{
    isaac_shader_cache_stats stats;
    oracle_reset();
    prove_sha256();
    prove_key_contract();
    prove_metadata_v2();
    prove_block_list_lifecycle();
    prove_publish_telemetry();
    prove_program_transaction();
    prove_round_trip_and_corruption();
    oracle_reset();
    prove_fault_fallbacks();
    isaac_shader_cache_get_stats(&stats);
    assert(stats.attempts != 0u && stats.misses != 0u &&
           stats.write_ok != 0u && stats.write_failures != 0u &&
           stats.read_failures != 0u && stats.alloc_failures != 0u &&
           stats.oversize != 0u && stats.bytes_read != 0u &&
           stats.bytes_written != 0u && stats.hash_us != 0u &&
           stats.read_us != 0u && stats.write_us != 0u);
    oracle_reset();
    puts("Isaac vitaGL shader-cache hostile oracle: PASS");
    return 0;
}
