#ifndef ISAAC_GUEST_PE_H
#define ISAAC_GUEST_PE_H

/* Frozen, bounded loader for the one PE revision translated by this tree.
 *
 * This is deliberately header-only.  guest.c is compiled directly by several
 * independent PC oracles as well as by the Vita CMake edge; keeping the pure
 * parser here makes every one of those build graphs consume the same code
 * without teaching generator/build_test scripts about another object file.
 * No function below reads global state or performs platform allocation.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GUEST_PE_EXPECTED_FILE_SIZE       8650240U
#define GUEST_PE_EXPECTED_ORIGINAL_BASE   0x00400000U
#define GUEST_PE_PC_TARGET_BASE           0x30000000U
#define GUEST_PE_VITA_TARGET_BASE         0x98000000U
#define GUEST_PE_EXPECTED_IMAGE_SIZE      0x0085f000U
#define GUEST_PE_EXPECTED_ENTRY_RVA       0x005eb83eU
#define GUEST_PE_EXPECTED_HEADER_SIZE     0x00000400U
#define GUEST_PE_EXPECTED_SECTION_COUNT   5U
#define GUEST_PE_EXPECTED_RELOC_RVA       0x0080f000U
#define GUEST_PE_EXPECTED_RELOC_SIZE      0x0004f3d8U
#define GUEST_PE_EXPECTED_TLS_RVA         0x0076cc40U
#define GUEST_PE_EXPECTED_TLS_SIZE        0x00000018U
#define GUEST_PE_EXPECTED_RELOC_BLOCKS    1632U
#define GUEST_PE_EXPECTED_HIGHLOW_COUNT   154959U
#define GUEST_PE_EXPECTED_ABSOLUTE_COUNT  797U
#define GUEST_PE_MAX_SECTIONS             16U
#define GUEST_PE_DIRECTORY_COUNT          16U

typedef enum guest_pe_error {
    GUEST_PE_OK = 0,
    GUEST_PE_ERR_ARGUMENT,
    GUEST_PE_ERR_OPEN,
    GUEST_PE_ERR_SIZE_IO,
    GUEST_PE_ERR_FILE_SIZE,
    GUEST_PE_ERR_REWIND,
    GUEST_PE_ERR_RAW_ALLOC,
    GUEST_PE_ERR_READ,
    GUEST_PE_ERR_CLOSE,
    GUEST_PE_ERR_FILE_HASH,
    GUEST_PE_ERR_DOS_HEADER,
    GUEST_PE_ERR_PE_OFFSET,
    GUEST_PE_ERR_PE_SIGNATURE,
    GUEST_PE_ERR_COFF_HEADER,
    GUEST_PE_ERR_OPTIONAL_HEADER,
    GUEST_PE_ERR_CONTRACT,
    GUEST_PE_ERR_SECTION_TABLE,
    GUEST_PE_ERR_SECTION_SOURCE,
    GUEST_PE_ERR_SECTION_DESTINATION,
    GUEST_PE_ERR_SECTION_OVERLAP,
    GUEST_PE_ERR_DIRECTORY,
    GUEST_PE_ERR_DIRECTORY_RAW,
    GUEST_PE_ERR_ENTRY_POINT,
    GUEST_PE_ERR_RELOC_DIRECTORY,
    GUEST_PE_ERR_RELOC_BLOCK,
    GUEST_PE_ERR_RELOC_TYPE,
    GUEST_PE_ERR_RELOC_TARGET,
    GUEST_PE_ERR_RELOC_COUNT,
    GUEST_PE_ERR_IMAGE_ALLOC,
    GUEST_PE_ERR_MAP_HASH
} guest_pe_error;

typedef struct guest_pe_section_plan {
    char name[9];
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t copy_size;
    uint32_t mapped_size;
    uint32_t characteristics;
} guest_pe_section_plan;

typedef struct guest_pe_directory_plan {
    uint32_t rva;
    uint32_t size;
} guest_pe_directory_plan;

typedef struct guest_pe_plan {
    uint32_t original_base;
    uint32_t target_base;
    uint32_t image_size;
    uint32_t entry_rva;
    uint32_t header_size;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint32_t relocation_blocks;
    uint32_t highlow_count;
    uint32_t absolute_count;
    uint16_t section_count;
    guest_pe_section_plan sections[GUEST_PE_MAX_SECTIONS];
    guest_pe_directory_plan directories[GUEST_PE_DIRECTORY_COUNT];
} guest_pe_plan;

typedef struct guest_pe_loader_ops {
    void *context;
    void *(*open_file)(void *context, const char *path);
    int (*file_size)(void *context, void *file, uint64_t *size_out);
    int (*rewind_file)(void *context, void *file);
    size_t (*read_file)(void *context, void *file,
                        unsigned char *destination, size_t size);
    int (*close_file)(void *context, void *file);
    void *(*raw_alloc)(void *context, size_t size);
    void (*raw_free)(void *context, void *allocation);
    void *(*image_alloc)(void *context, uint32_t address, uint32_t size);
    void (*image_free)(void *context, void *allocation);
} guest_pe_loader_ops;

typedef struct guest_pe_loaded_image {
    void *image;
    guest_pe_plan plan;
} guest_pe_loaded_image;

typedef struct guest_pe_sha256_context {
    uint32_t state[8];
    uint64_t total_size;
    unsigned char block[64];
} guest_pe_sha256_context;

static const unsigned char guest_pe_expected_file_sha256[32] = {
    0x31, 0x84, 0x64, 0x86, 0x97, 0x9c, 0xfa, 0x07,
    0xc8, 0xc9, 0x68, 0x22, 0x15, 0x53, 0x05, 0x2c,
    0x3f, 0xf6, 0x03, 0x51, 0x86, 0x81, 0xca, 0x11,
    0x91, 0x2d, 0x44, 0x5f, 0x96, 0xca, 0x94, 0x04
};

static const unsigned char guest_pe_expected_map_sha256[32] = {
    0x0c, 0xd8, 0x9a, 0x59, 0x56, 0x20, 0x94, 0xed,
    0xb5, 0xcc, 0x19, 0x01, 0x66, 0xf0, 0x15, 0xe6,
    0x11, 0x85, 0xbf, 0x38, 0xae, 0x66, 0xb7, 0x37,
    0x45, 0xb0, 0xe4, 0xe8, 0xb0, 0xf3, 0x88, 0x37
};

static const unsigned char guest_pe_expected_pc_map_sha256[32] = {
    0x8c, 0x7b, 0xca, 0xf3, 0x2e, 0xd9, 0x96, 0xde,
    0xcb, 0x67, 0x7b, 0x21, 0xe3, 0x7d, 0x1b, 0x1d,
    0x4d, 0x0e, 0x13, 0x8f, 0x4f, 0xf4, 0xb0, 0x8b,
    0x34, 0x85, 0x80, 0x5d, 0xaf, 0x20, 0x8b, 0xcb
};

static inline int guest_pe_target_supported(uint32_t target_base)
{
    return target_base == GUEST_PE_PC_TARGET_BASE ||
           target_base == GUEST_PE_VITA_TARGET_BASE;
}

static inline uint32_t guest_pe_rotr32(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32U - count));
}

static inline void guest_pe_sha256_transform(
    guest_pe_sha256_context *context, const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        size_t offset = (size_t)i * 4U;
        words[i] = ((uint32_t)block[offset] << 24) |
                   ((uint32_t)block[offset + 1U] << 16) |
                   ((uint32_t)block[offset + 2U] << 8) |
                   (uint32_t)block[offset + 3U];
    }
    for (i = 16U; i < 64U; ++i) {
        uint32_t s0 = guest_pe_rotr32(words[i - 15U], 7U) ^
                      guest_pe_rotr32(words[i - 15U], 18U) ^
                      (words[i - 15U] >> 3);
        uint32_t s1 = guest_pe_rotr32(words[i - 2U], 17U) ^
                      guest_pe_rotr32(words[i - 2U], 19U) ^
                      (words[i - 2U] >> 10);
        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }

    a = context->state[0]; b = context->state[1];
    c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5];
    g = context->state[6]; h = context->state[7];
    for (i = 0U; i < 64U; ++i) {
        uint32_t s1 = guest_pe_rotr32(e, 6U) ^ guest_pe_rotr32(e, 11U) ^
                      guest_pe_rotr32(e, 25U);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
        uint32_t s0 = guest_pe_rotr32(a, 2U) ^ guest_pe_rotr32(a, 13U) ^
                      guest_pe_rotr32(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}

static inline void guest_pe_sha256_init(guest_pe_sha256_context *context)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memcpy(context->state, initial, sizeof initial);
    context->total_size = 0U;
    memset(context->block, 0, sizeof context->block);
}

static inline void guest_pe_sha256_update(
    guest_pe_sha256_context *context, const void *data_pointer, size_t size)
{
    const unsigned char *data = (const unsigned char *)data_pointer;
    size_t used = (size_t)(context->total_size & 63U);

    context->total_size += (uint64_t)size;
    if (used) {
        size_t available = 64U - used;
        size_t take = size < available ? size : available;
        memcpy(context->block + used, data, take);
        data += take;
        size -= take;
        used += take;
        if (used == 64U)
            guest_pe_sha256_transform(context, context->block);
    }
    while (size >= 64U) {
        guest_pe_sha256_transform(context, data);
        data += 64U;
        size -= 64U;
    }
    if (size)
        memcpy(context->block, data, size);
}

static inline void guest_pe_sha256_final(
    guest_pe_sha256_context *context, unsigned char digest[32])
{
    unsigned char padding[128];
    unsigned char length_bytes[8];
    uint64_t bit_size = context->total_size * 8U;
    size_t used = (size_t)(context->total_size & 63U);
    size_t padding_size = used < 56U ? 56U - used : 120U - used;
    unsigned i;

    memset(padding, 0, sizeof padding);
    padding[0] = 0x80U;
    for (i = 0U; i < 8U; ++i)
        length_bytes[7U - i] = (unsigned char)(bit_size >> (i * 8U));
    guest_pe_sha256_update(context, padding, padding_size);
    guest_pe_sha256_update(context, length_bytes, sizeof length_bytes);
    for (i = 0U; i < 8U; ++i) {
        digest[i * 4U] = (unsigned char)(context->state[i] >> 24);
        digest[i * 4U + 1U] = (unsigned char)(context->state[i] >> 16);
        digest[i * 4U + 2U] = (unsigned char)(context->state[i] >> 8);
        digest[i * 4U + 3U] = (unsigned char)context->state[i];
    }
}

static inline void guest_pe_sha256(
    const void *data, size_t size, unsigned char digest[32])
{
    guest_pe_sha256_context context;
    guest_pe_sha256_init(&context);
    guest_pe_sha256_update(&context, data, size);
    guest_pe_sha256_final(&context, digest);
}

static inline int guest_pe_span_size(size_t total, size_t offset, size_t size)
{
    return offset <= total && size <= total - offset;
}

static inline int guest_pe_span_u32(
    uint32_t total, uint32_t offset, uint32_t size)
{
    return offset <= total && size <= total - offset;
}

static inline int guest_pe_read_u16(
    const unsigned char *data, size_t size, size_t offset, uint16_t *value)
{
    if (!data || !value || !guest_pe_span_size(size, offset, 2U))
        return 0;
    *value = (uint16_t)((uint16_t)data[offset] |
                       ((uint16_t)data[offset + 1U] << 8));
    return 1;
}

static inline int guest_pe_read_u32(
    const unsigned char *data, size_t size, size_t offset, uint32_t *value)
{
    if (!data || !value || !guest_pe_span_size(size, offset, 4U))
        return 0;
    *value = (uint32_t)data[offset] |
             ((uint32_t)data[offset + 1U] << 8) |
             ((uint32_t)data[offset + 2U] << 16) |
             ((uint32_t)data[offset + 3U] << 24);
    return 1;
}

static inline int guest_pe_write_u32(
    unsigned char *data, size_t size, size_t offset, uint32_t value)
{
    if (!data || !guest_pe_span_size(size, offset, 4U))
        return 0;
    data[offset] = (unsigned char)value;
    data[offset + 1U] = (unsigned char)(value >> 8);
    data[offset + 2U] = (unsigned char)(value >> 16);
    data[offset + 3U] = (unsigned char)(value >> 24);
    return 1;
}

static inline guest_pe_error guest_pe_authenticate(
    const unsigned char *raw, size_t raw_size)
{
    unsigned char digest[32];
    if (!raw)
        return GUEST_PE_ERR_ARGUMENT;
    if (raw_size != (size_t)GUEST_PE_EXPECTED_FILE_SIZE)
        return GUEST_PE_ERR_FILE_SIZE;
    guest_pe_sha256(raw, raw_size, digest);
    return memcmp(digest, guest_pe_expected_file_sha256, sizeof digest) == 0
        ? GUEST_PE_OK : GUEST_PE_ERR_FILE_HASH;
}

static inline int guest_pe_rva_to_raw(
    const guest_pe_plan *plan, uint32_t rva, uint32_t size,
    uint32_t *raw_offset)
{
    uint16_t i;
    if (!plan || !raw_offset)
        return 0;
    if (rva < plan->header_size &&
        guest_pe_span_u32(plan->header_size, rva, size)) {
        *raw_offset = rva;
        return 1;
    }
    for (i = 0U; i < plan->section_count; ++i) {
        const guest_pe_section_plan *section = &plan->sections[i];
        uint32_t within;
        if (rva < section->virtual_address)
            continue;
        within = rva - section->virtual_address;
        if (within <= section->copy_size &&
            size <= section->copy_size - within &&
            section->raw_offset <= UINT32_MAX - within) {
            *raw_offset = section->raw_offset + within;
            return 1;
        }
    }
    return 0;
}

static inline guest_pe_error guest_pe_validate_relocations_raw(
    const unsigned char *raw, size_t raw_size, guest_pe_plan *plan)
{
    const guest_pe_directory_plan *directory;
    uint32_t raw_offset, offset = 0U;
    uint32_t blocks = 0U, highlow = 0U, absolute = 0U;

    if (!raw || !plan)
        return GUEST_PE_ERR_ARGUMENT;
    directory = &plan->directories[5];
    if (!directory->rva || !directory->size ||
        !guest_pe_rva_to_raw(plan, directory->rva, directory->size,
                             &raw_offset) ||
        !guest_pe_span_size(raw_size, raw_offset, directory->size))
        return GUEST_PE_ERR_RELOC_DIRECTORY;

    while (offset < directory->size) {
        uint32_t page, block_size, entry_offset;
        uint32_t remaining = directory->size - offset;
        if (remaining < 8U ||
            !guest_pe_read_u32(raw, raw_size,
                               (size_t)raw_offset + offset, &page) ||
            !guest_pe_read_u32(raw, raw_size,
                               (size_t)raw_offset + offset + 4U,
                               &block_size))
            return GUEST_PE_ERR_RELOC_BLOCK;
        if (block_size < 8U || (block_size & 1U) != 0U ||
            block_size > remaining || (page & 0xfffU) != 0U ||
            page >= plan->image_size)
            return GUEST_PE_ERR_RELOC_BLOCK;
        for (entry_offset = 8U; entry_offset < block_size;
             entry_offset += 2U) {
            uint16_t entry;
            uint32_t type, within, target;
            if (!guest_pe_read_u16(raw, raw_size,
                                   (size_t)raw_offset + offset + entry_offset,
                                   &entry))
                return GUEST_PE_ERR_RELOC_BLOCK;
            type = (uint32_t)entry >> 12;
            within = (uint32_t)entry & 0xfffU;
            if (type == 0U) {
                ++absolute;
                continue;
            }
            if (type != 3U)
                return GUEST_PE_ERR_RELOC_TYPE;
            if (page > UINT32_MAX - within)
                return GUEST_PE_ERR_RELOC_TARGET;
            target = page + within;
            if (!guest_pe_span_u32(plan->image_size, target, 4U))
                return GUEST_PE_ERR_RELOC_TARGET;
            ++highlow;
        }
        offset += block_size;
        ++blocks;
    }
    if (offset != directory->size)
        return GUEST_PE_ERR_RELOC_BLOCK;
    if (blocks != GUEST_PE_EXPECTED_RELOC_BLOCKS ||
        highlow != GUEST_PE_EXPECTED_HIGHLOW_COUNT ||
        absolute != GUEST_PE_EXPECTED_ABSOLUTE_COUNT)
        return GUEST_PE_ERR_RELOC_COUNT;
    plan->relocation_blocks = blocks;
    plan->highlow_count = highlow;
    plan->absolute_count = absolute;
    return GUEST_PE_OK;
}

static inline guest_pe_error guest_pe_parse_bounded(
    const unsigned char *raw, size_t raw_size, uint32_t target_base,
    guest_pe_plan *plan)
{
    uint32_t pe_offset, signature, optional_offset, section_table;
    uint32_t image_size, image_base, entry_rva, header_size;
    uint32_t section_alignment, file_alignment, directory_count;
    uint16_t machine, section_count, optional_size, optional_magic;
    uint16_t i, j;

    if (!raw || !plan)
        return GUEST_PE_ERR_ARGUMENT;
    memset(plan, 0, sizeof *plan);
    if (!guest_pe_target_supported(target_base) ||
        target_base > UINT32_MAX - GUEST_PE_EXPECTED_IMAGE_SIZE)
        return GUEST_PE_ERR_CONTRACT;
    if (raw_size > UINT32_MAX)
        return GUEST_PE_ERR_FILE_SIZE;
    if (raw_size < 64U || raw[0] != 'M' || raw[1] != 'Z')
        return GUEST_PE_ERR_DOS_HEADER;
    if (!guest_pe_read_u32(raw, raw_size, 0x3cU, &pe_offset) ||
        !guest_pe_span_size(raw_size, pe_offset, 24U))
        return GUEST_PE_ERR_PE_OFFSET;
    if (!guest_pe_read_u32(raw, raw_size, pe_offset, &signature) ||
        signature != 0x00004550U)
        return GUEST_PE_ERR_PE_SIGNATURE;
    if (!guest_pe_read_u16(raw, raw_size, (size_t)pe_offset + 4U,
                           &machine) ||
        !guest_pe_read_u16(raw, raw_size, (size_t)pe_offset + 6U,
                           &section_count) ||
        !guest_pe_read_u16(raw, raw_size, (size_t)pe_offset + 20U,
                           &optional_size))
        return GUEST_PE_ERR_COFF_HEADER;
    if (machine != 0x014cU ||
        section_count != GUEST_PE_EXPECTED_SECTION_COUNT ||
        section_count > GUEST_PE_MAX_SECTIONS || optional_size != 0x00e0U)
        return GUEST_PE_ERR_COFF_HEADER;
    optional_offset = pe_offset + 24U;
    if (optional_offset < pe_offset ||
        !guest_pe_span_size(raw_size, optional_offset, optional_size) ||
        !guest_pe_read_u16(raw, raw_size, optional_offset, &optional_magic) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 16U,
                           &entry_rva) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 28U,
                           &image_base) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 32U,
                           &section_alignment) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 36U,
                           &file_alignment) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 56U,
                           &image_size) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 60U,
                           &header_size) ||
        !guest_pe_read_u32(raw, raw_size, optional_offset + 92U,
                           &directory_count))
        return GUEST_PE_ERR_OPTIONAL_HEADER;
    if (optional_magic != 0x010bU || directory_count != 16U ||
        image_base != GUEST_PE_EXPECTED_ORIGINAL_BASE ||
        image_size != GUEST_PE_EXPECTED_IMAGE_SIZE ||
        entry_rva != GUEST_PE_EXPECTED_ENTRY_RVA ||
        header_size != GUEST_PE_EXPECTED_HEADER_SIZE ||
        section_alignment != 0x1000U || file_alignment != 0x200U ||
        !guest_pe_span_size(raw_size, 0U, header_size))
        return GUEST_PE_ERR_CONTRACT;

    section_table = optional_offset + optional_size;
    if (section_table < optional_offset ||
        !guest_pe_span_size(raw_size, section_table,
                            (size_t)section_count * 40U) ||
        section_table > header_size ||
        (size_t)section_count * 40U > header_size - section_table)
        return GUEST_PE_ERR_SECTION_TABLE;

    plan->original_base = image_base;
    plan->target_base = target_base;
    plan->image_size = image_size;
    plan->entry_rva = entry_rva;
    plan->header_size = header_size;
    plan->section_alignment = section_alignment;
    plan->file_alignment = file_alignment;
    plan->section_count = section_count;

    for (i = 0U; i < section_count; ++i) {
        guest_pe_section_plan *section = &plan->sections[i];
        size_t offset = (size_t)section_table + (size_t)i * 40U;
        uint32_t mapped_size;
        memcpy(section->name, raw + offset, 8U);
        section->name[8] = '\0';
        if (!guest_pe_read_u32(raw, raw_size, offset + 8U,
                               &section->virtual_size) ||
            !guest_pe_read_u32(raw, raw_size, offset + 12U,
                               &section->virtual_address) ||
            !guest_pe_read_u32(raw, raw_size, offset + 16U,
                               &section->raw_size) ||
            !guest_pe_read_u32(raw, raw_size, offset + 20U,
                               &section->raw_offset) ||
            !guest_pe_read_u32(raw, raw_size, offset + 36U,
                               &section->characteristics))
            return GUEST_PE_ERR_SECTION_TABLE;
        mapped_size = section->virtual_size
            ? section->virtual_size : section->raw_size;
        section->mapped_size = mapped_size;
        section->copy_size = section->virtual_size &&
                             section->raw_size > section->virtual_size
            ? section->virtual_size : section->raw_size;
        if ((section->virtual_address & (section_alignment - 1U)) != 0U ||
            section->virtual_address < header_size ||
            !guest_pe_span_u32(image_size, section->virtual_address,
                               mapped_size) ||
            !guest_pe_span_u32(image_size, section->virtual_address,
                               section->copy_size))
            return GUEST_PE_ERR_SECTION_DESTINATION;
        if (section->raw_size &&
            ((section->raw_offset & (file_alignment - 1U)) != 0U ||
             section->raw_offset < header_size ||
             !guest_pe_span_size(raw_size, section->raw_offset,
                                 section->raw_size)))
            return GUEST_PE_ERR_SECTION_SOURCE;
        for (j = 0U; j < i; ++j) {
            const guest_pe_section_plan *previous = &plan->sections[j];
            uint32_t section_end = section->virtual_address + mapped_size;
            uint32_t previous_end = previous->virtual_address +
                                    previous->mapped_size;
            if (mapped_size && previous->mapped_size &&
                section->virtual_address < previous_end &&
                previous->virtual_address < section_end)
                return GUEST_PE_ERR_SECTION_OVERLAP;
            if (section->raw_size && previous->raw_size) {
                uint32_t raw_end = section->raw_offset + section->raw_size;
                uint32_t previous_raw_end = previous->raw_offset +
                                            previous->raw_size;
                if (section->raw_offset < previous_raw_end &&
                    previous->raw_offset < raw_end)
                    return GUEST_PE_ERR_SECTION_OVERLAP;
            }
        }
    }

    for (i = 0U; i < GUEST_PE_DIRECTORY_COUNT; ++i) {
        guest_pe_directory_plan *directory = &plan->directories[i];
        uint32_t raw_offset;
        size_t offset = (size_t)optional_offset + 96U + (size_t)i * 8U;
        if (!guest_pe_read_u32(raw, raw_size, offset, &directory->rva) ||
            !guest_pe_read_u32(raw, raw_size, offset + 4U,
                               &directory->size))
            return GUEST_PE_ERR_DIRECTORY;
        if (!directory->rva && !directory->size)
            continue;
        if (!directory->rva || !directory->size || i == 4U ||
            !guest_pe_span_u32(image_size, directory->rva, directory->size))
            return GUEST_PE_ERR_DIRECTORY;
        if (!guest_pe_rva_to_raw(plan, directory->rva, directory->size,
                                 &raw_offset) ||
            !guest_pe_span_size(raw_size, raw_offset, directory->size))
            return GUEST_PE_ERR_DIRECTORY_RAW;
    }
    if (plan->directories[5].rva != GUEST_PE_EXPECTED_RELOC_RVA ||
        plan->directories[5].size != GUEST_PE_EXPECTED_RELOC_SIZE ||
        plan->directories[9].rva != GUEST_PE_EXPECTED_TLS_RVA ||
        plan->directories[9].size != GUEST_PE_EXPECTED_TLS_SIZE)
        return GUEST_PE_ERR_CONTRACT;

    for (i = 0U; i < section_count; ++i) {
        const guest_pe_section_plan *section = &plan->sections[i];
        if ((section->characteristics & 0x20000000U) != 0U &&
            entry_rva >= section->virtual_address &&
            entry_rva - section->virtual_address < section->mapped_size)
            break;
    }
    if (i == section_count)
        return GUEST_PE_ERR_ENTRY_POINT;
    return guest_pe_validate_relocations_raw(raw, raw_size, plan);
}

static inline guest_pe_error guest_pe_map_image(
    const unsigned char *raw, size_t raw_size, const guest_pe_plan *plan,
    unsigned char *image, size_t image_capacity)
{
    const guest_pe_directory_plan *directory;
    uint32_t offset = 0U, blocks = 0U, highlow = 0U, absolute = 0U;
    uint32_t delta;
    unsigned char digest[32];
    uint16_t i;

    if (!raw || !plan || !image)
        return GUEST_PE_ERR_ARGUMENT;
    if (!guest_pe_target_supported(plan->target_base) ||
        plan->original_base != GUEST_PE_EXPECTED_ORIGINAL_BASE ||
        plan->image_size != GUEST_PE_EXPECTED_IMAGE_SIZE ||
        plan->header_size != GUEST_PE_EXPECTED_HEADER_SIZE ||
        plan->section_count != GUEST_PE_EXPECTED_SECTION_COUNT ||
        image_capacity < plan->image_size ||
        !guest_pe_span_size(raw_size, 0U, plan->header_size))
        return GUEST_PE_ERR_CONTRACT;

    memset(image, 0, plan->image_size);
    memcpy(image, raw, plan->header_size);
    for (i = 0U; i < plan->section_count; ++i) {
        const guest_pe_section_plan *section = &plan->sections[i];
        if (!guest_pe_span_size(raw_size, section->raw_offset,
                                section->copy_size))
            return GUEST_PE_ERR_SECTION_SOURCE;
        if (!guest_pe_span_u32(plan->image_size, section->virtual_address,
                               section->copy_size))
            return GUEST_PE_ERR_SECTION_DESTINATION;
        if (section->copy_size)
            memcpy(image + section->virtual_address,
                   raw + section->raw_offset, section->copy_size);
    }

    directory = &plan->directories[5];
    if (directory->rva != GUEST_PE_EXPECTED_RELOC_RVA ||
        directory->size != GUEST_PE_EXPECTED_RELOC_SIZE ||
        !guest_pe_span_u32(plan->image_size, directory->rva,
                           directory->size))
        return GUEST_PE_ERR_RELOC_DIRECTORY;
    delta = plan->target_base - plan->original_base;
    while (offset < directory->size) {
        uint32_t page, block_size, entry_offset;
        uint32_t remaining = directory->size - offset;
        size_t block = (size_t)directory->rva + offset;
        if (remaining < 8U ||
            !guest_pe_read_u32(image, plan->image_size, block, &page) ||
            !guest_pe_read_u32(image, plan->image_size, block + 4U,
                               &block_size) ||
            block_size < 8U || (block_size & 1U) != 0U ||
            block_size > remaining || (page & 0xfffU) != 0U ||
            page >= plan->image_size)
            return GUEST_PE_ERR_RELOC_BLOCK;
        for (entry_offset = 8U; entry_offset < block_size;
             entry_offset += 2U) {
            uint16_t entry;
            uint32_t type, within, target, value;
            if (!guest_pe_read_u16(image, plan->image_size,
                                   block + entry_offset, &entry))
                return GUEST_PE_ERR_RELOC_BLOCK;
            type = (uint32_t)entry >> 12;
            within = (uint32_t)entry & 0xfffU;
            if (type == 0U) {
                ++absolute;
                continue;
            }
            if (type != 3U)
                return GUEST_PE_ERR_RELOC_TYPE;
            if (page > UINT32_MAX - within)
                return GUEST_PE_ERR_RELOC_TARGET;
            target = page + within;
            if (!guest_pe_span_u32(plan->image_size, target, 4U) ||
                !guest_pe_read_u32(image, plan->image_size, target, &value) ||
                !guest_pe_write_u32(image, plan->image_size, target,
                                    value + delta))
                return GUEST_PE_ERR_RELOC_TARGET;
            ++highlow;
        }
        offset += block_size;
        ++blocks;
    }
    if (offset != directory->size)
        return GUEST_PE_ERR_RELOC_BLOCK;
    if (blocks != plan->relocation_blocks ||
        highlow != plan->highlow_count ||
        absolute != plan->absolute_count ||
        blocks != GUEST_PE_EXPECTED_RELOC_BLOCKS ||
        highlow != GUEST_PE_EXPECTED_HIGHLOW_COUNT ||
        absolute != GUEST_PE_EXPECTED_ABSOLUTE_COUNT)
        return GUEST_PE_ERR_RELOC_COUNT;
    guest_pe_sha256(image, plan->image_size, digest);
    if (memcmp(digest,
               plan->target_base == GUEST_PE_VITA_TARGET_BASE
                   ? guest_pe_expected_map_sha256
                   : guest_pe_expected_pc_map_sha256,
               sizeof digest) != 0)
        return GUEST_PE_ERR_MAP_HASH;
    return GUEST_PE_OK;
}

static inline guest_pe_error guest_pe_load_file(
    const guest_pe_loader_ops *ops, const char *path, uint32_t target_base,
    guest_pe_loaded_image *loaded)
{
    guest_pe_error error = GUEST_PE_OK;
    void *file = NULL;
    unsigned char *raw = NULL;
    void *image = NULL;
    uint64_t file_size = 0U;

    if (!loaded)
        return GUEST_PE_ERR_ARGUMENT;
    memset(loaded, 0, sizeof *loaded);
    if (!ops || !path || !ops->open_file || !ops->file_size ||
        !ops->rewind_file || !ops->read_file || !ops->close_file ||
        !ops->raw_alloc || !ops->raw_free || !ops->image_alloc ||
        !ops->image_free)
        return GUEST_PE_ERR_ARGUMENT;
    file = ops->open_file(ops->context, path);
    if (!file)
        return GUEST_PE_ERR_OPEN;
    if (ops->file_size(ops->context, file, &file_size) != 0) {
        error = GUEST_PE_ERR_SIZE_IO;
        goto cleanup;
    }
    if (file_size != GUEST_PE_EXPECTED_FILE_SIZE) {
        error = GUEST_PE_ERR_FILE_SIZE;
        goto cleanup;
    }
    if (ops->rewind_file(ops->context, file) != 0) {
        error = GUEST_PE_ERR_REWIND;
        goto cleanup;
    }
    raw = (unsigned char *)ops->raw_alloc(
        ops->context, GUEST_PE_EXPECTED_FILE_SIZE);
    if (!raw) {
        error = GUEST_PE_ERR_RAW_ALLOC;
        goto cleanup;
    }
    if (ops->read_file(ops->context, file, raw,
                       GUEST_PE_EXPECTED_FILE_SIZE) !=
        GUEST_PE_EXPECTED_FILE_SIZE) {
        error = GUEST_PE_ERR_READ;
        goto cleanup;
    }
    if (ops->close_file(ops->context, file) != 0) {
        file = NULL;
        error = GUEST_PE_ERR_CLOSE;
        goto cleanup;
    }
    file = NULL;
    error = guest_pe_authenticate(raw, GUEST_PE_EXPECTED_FILE_SIZE);
    if (error != GUEST_PE_OK)
        goto cleanup;
    error = guest_pe_parse_bounded(raw, GUEST_PE_EXPECTED_FILE_SIZE,
                                   target_base, &loaded->plan);
    if (error != GUEST_PE_OK)
        goto cleanup;
    image = ops->image_alloc(ops->context, loaded->plan.target_base,
                             loaded->plan.image_size);
    if (!image) {
        error = GUEST_PE_ERR_IMAGE_ALLOC;
        goto cleanup;
    }
    error = guest_pe_map_image(raw, GUEST_PE_EXPECTED_FILE_SIZE,
                               &loaded->plan, (unsigned char *)image,
                               loaded->plan.image_size);
    if (error != GUEST_PE_OK)
        goto cleanup;
    loaded->image = image;
    image = NULL;

cleanup:
    if (file) {
        int close_error = ops->close_file(ops->context, file);
        if (error == GUEST_PE_OK && close_error != 0)
            error = GUEST_PE_ERR_CLOSE;
    }
    if (raw)
        ops->raw_free(ops->context, raw);
    if (image)
        ops->image_free(ops->context, image);
    if (error != GUEST_PE_OK)
        memset(loaded, 0, sizeof *loaded);
    return error;
}

static inline void guest_pe_release_loaded(
    const guest_pe_loader_ops *ops, guest_pe_loaded_image *loaded)
{
    if (!ops || !loaded)
        return;
    if (loaded->image && ops->image_free)
        ops->image_free(ops->context, loaded->image);
    memset(loaded, 0, sizeof *loaded);
}

static inline const char *guest_pe_error_string(guest_pe_error error)
{
    switch (error) {
    case GUEST_PE_OK: return "success";
    case GUEST_PE_ERR_ARGUMENT: return "invalid loader argument";
    case GUEST_PE_ERR_OPEN: return "cannot open PE";
    case GUEST_PE_ERR_SIZE_IO: return "cannot measure PE";
    case GUEST_PE_ERR_FILE_SIZE: return "unsupported PE file size";
    case GUEST_PE_ERR_REWIND: return "cannot rewind PE";
    case GUEST_PE_ERR_RAW_ALLOC: return "cannot allocate PE input buffer";
    case GUEST_PE_ERR_READ: return "short PE read";
    case GUEST_PE_ERR_CLOSE: return "cannot close PE";
    case GUEST_PE_ERR_FILE_HASH: return "unsupported PE SHA-256";
    case GUEST_PE_ERR_DOS_HEADER: return "invalid DOS header";
    case GUEST_PE_ERR_PE_OFFSET: return "invalid PE header offset";
    case GUEST_PE_ERR_PE_SIGNATURE: return "invalid PE signature";
    case GUEST_PE_ERR_COFF_HEADER: return "invalid COFF header";
    case GUEST_PE_ERR_OPTIONAL_HEADER: return "invalid PE32 optional header";
    case GUEST_PE_ERR_CONTRACT: return "PE differs from frozen contract";
    case GUEST_PE_ERR_SECTION_TABLE: return "invalid PE section table";
    case GUEST_PE_ERR_SECTION_SOURCE: return "PE section source is out of file";
    case GUEST_PE_ERR_SECTION_DESTINATION: return "PE section is out of image";
    case GUEST_PE_ERR_SECTION_OVERLAP: return "overlapping PE sections";
    case GUEST_PE_ERR_DIRECTORY: return "invalid PE data directory";
    case GUEST_PE_ERR_DIRECTORY_RAW: return "PE directory is not file-backed";
    case GUEST_PE_ERR_ENTRY_POINT: return "PE entry point is not executable";
    case GUEST_PE_ERR_RELOC_DIRECTORY: return "invalid relocation directory";
    case GUEST_PE_ERR_RELOC_BLOCK: return "invalid relocation block";
    case GUEST_PE_ERR_RELOC_TYPE: return "unsupported relocation type";
    case GUEST_PE_ERR_RELOC_TARGET: return "relocation target is out of image";
    case GUEST_PE_ERR_RELOC_COUNT: return "relocation census differs from contract";
    case GUEST_PE_ERR_IMAGE_ALLOC: return "cannot allocate fixed guest image";
    case GUEST_PE_ERR_MAP_HASH: return "mapped image differs from translator";
    default: return "unknown PE loader error";
    }
}

#endif
